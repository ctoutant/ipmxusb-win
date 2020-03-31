#include "threaded_csq.h"

VOID _threaded_csq_complete_cancelled_irp(_In_ PIO_CSQ csq, _In_ PIRP irp)
{
	UNREFERENCED_PARAMETER(csq);
	irp->IoStatus.Status = STATUS_CANCELLED;
	irp->IoStatus.Information = 0;

	IoCompleteRequest(irp, IO_NO_INCREMENT);
}

_Acquires_lock_(CONTAINING_RECORD(csq, threaded_csq, irp_csq)->irp_csq_lock)
VOID _threaded_csq_acquire_lock(_In_ PIO_CSQ csq, _Out_ PKIRQL irql)
{
	pthreaded_csq ctx = CONTAINING_RECORD(csq, threaded_csq, irp_csq);
	KeAcquireSpinLock(&ctx->irp_csq_lock, irql);
}

_Releases_lock_(CONTAINING_RECORD(csq, threaded_csq, irp_csq)->irp_csq_lock)
VOID _threaded_csq_release_lock(_In_ PIO_CSQ csq, _In_ KIRQL irql)
{
	pthreaded_csq ctx = CONTAINING_RECORD(csq, threaded_csq, irp_csq);
	KeReleaseSpinLock(&ctx->irp_csq_lock, irql);
}

VOID _threaded_csq_insert_irp(_In_ PIO_CSQ csq, _In_ PIRP irp)
{
	pthreaded_csq ctx = CONTAINING_RECORD(csq, threaded_csq, irp_csq);
	InsertTailList(&ctx->irp_csq_list, &irp->Tail.Overlay.ListEntry);
}

VOID _threaded_csq_remove_irp(_In_ PIO_CSQ csq, _In_ PIRP irp)
{
	UNREFERENCED_PARAMETER(csq);
	RemoveEntryList(&irp->Tail.Overlay.ListEntry);
}

PIRP _threaded_csq_peek_next_irp(PIO_CSQ csq, PIRP irp, PVOID peek_context)
{
	UNREFERENCED_PARAMETER(peek_context);
	pthreaded_csq ctx = CONTAINING_RECORD(csq, threaded_csq, irp_csq);
	PLIST_ENTRY next_entry = NULL;

	if (irp == NULL) {
		next_entry = ctx->irp_csq_list.Flink;
	}
	else {
		next_entry = irp->Tail.Overlay.ListEntry.Flink;
	}

	// we explicitly set context to NULL, so there lookup of first matching
	return (next_entry != &ctx->irp_csq_list)
		? CONTAINING_RECORD(next_entry, IRP, Tail.Overlay.ListEntry)
		: NULL;
}

VOID threaded_csq_main(_In_ PVOID context)
{
	pthreaded_csq ctx = (pthreaded_csq)context;
	PIRP irp;

	KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

	while (TRUE)
	{
		KeWaitForSingleObject(&ctx->thread_semaphore, Executive, KernelMode, FALSE, NULL);
		if (ctx->thread_stop) {
			PsTerminateSystemThread(STATUS_SUCCESS);
		}

		irp = IoCsqRemoveNextIrp(&ctx->irp_csq, NULL);
		if (irp == NULL) {
			continue;
		}

		// processing must be done inside this method and result in IoCompleteRequest
		ctx->process_irp_fn(ctx->process_irp_ctx, irp);
	}
}
