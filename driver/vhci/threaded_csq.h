#pragma once

#include <ntddk.h>

typedef VOID CSQ_WITH_THREAD_PROCESS_IRP(_In_ PVOID context, _In_ PIRP irp);
typedef CSQ_WITH_THREAD_PROCESS_IRP * PCSQ_WITH_THREAD_PROCESS_IRP;

// this structures abstracts csq and thread so this pattern can be reused if needed elsewhere
typedef struct _threaded_csq {
	PETHREAD thread;
	// Something to wait on, so we avoid busy-wait in thread
	KSEMAPHORE thread_semaphore;
	BOOLEAN thread_stop;

	IO_CSQ irp_csq;
	LIST_ENTRY irp_csq_list;
	KSPIN_LOCK	irp_csq_lock;
	// The spin lock that protects access to  the queue

	PCSQ_WITH_THREAD_PROCESS_IRP process_irp_fn;
	PVOID process_irp_ctx;
} threaded_csq, *pthreaded_csq;

IO_CSQ_COMPLETE_CANCELED_IRP _threaded_csq_complete_cancelled_irp;
IO_CSQ_ACQUIRE_LOCK _threaded_csq_acquire_lock;
IO_CSQ_RELEASE_LOCK _threaded_csq_release_lock;
IO_CSQ_INSERT_IRP _threaded_csq_insert_irp;
IO_CSQ_REMOVE_IRP _threaded_csq_remove_irp;
IO_CSQ_PEEK_NEXT_IRP _threaded_csq_peek_next_irp;
VOID threaded_csq_main(_In_ PVOID context);

inline NTSTATUS threaded_csq_init(_In_ pthreaded_csq ctx,
	_In_ PCSQ_WITH_THREAD_PROCESS_IRP process_irp_fn,
	_In_ PVOID process_irp_ctx)
{
	InitializeListHead(&ctx->irp_csq_list);
	KeInitializeSpinLock(&ctx->irp_csq_lock);
	KeInitializeSemaphore(&ctx->thread_semaphore, 0, MAXLONG);
	IoCsqInitialize(&ctx->irp_csq,
		_threaded_csq_insert_irp,
		_threaded_csq_remove_irp,
		_threaded_csq_peek_next_irp,
		_threaded_csq_acquire_lock,
		_threaded_csq_release_lock,
		_threaded_csq_complete_cancelled_irp);

	ctx->thread_stop = FALSE;
	ctx->process_irp_fn = process_irp_fn;
	ctx->process_irp_ctx = process_irp_ctx;

	if (process_irp_fn == NULL)
	{
		return STATUS_INVALID_PARAMETER_2;
	}

	HANDLE handle;
	NTSTATUS status = PsCreateSystemThread(&handle,
		(ACCESS_MASK)0,
		NULL,
		(HANDLE)0,
		NULL,
		threaded_csq_main,
		ctx);

	if (!NT_SUCCESS(status))
	{
		//nth log error
		return status;
	}

	ObReferenceObjectByHandle(handle,
		THREAD_ALL_ACCESS,
		NULL,
		KernelMode,
		&ctx->thread,
		NULL);

	ZwClose(handle);
	return status;
}

inline VOID threaded_csq_cleanup(_In_ pthreaded_csq ctx)
{
	ctx->thread_stop = TRUE;
	KeReleaseSemaphore(&ctx->thread_semaphore, 0, 1, TRUE);
	KeWaitForSingleObject(ctx->thread, Executive, KernelMode, FALSE, NULL);
	ObDereferenceObject(ctx->thread);

	PIRP irp = IoCsqRemoveNextIrp(&ctx->irp_csq, NULL);
	while (irp)
	{
		irp->IoStatus.Information = 0;
		irp->IoStatus.Status = STATUS_CANCELLED;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		irp = IoCsqRemoveNextIrp(&ctx->irp_csq, NULL);
	}
}

inline VOID threaded_csq_insert_irp(_In_ pthreaded_csq ctx, _In_ PIRP irp)
{
	IoCsqInsertIrp(&ctx->irp_csq, irp, NULL);
	KeReleaseSemaphore(&ctx->thread_semaphore, 0, 1, FALSE);
}

#define USING_CSQ_WITH_THREAD 1
