#include "vhci.h"

#include "usbip_proto.h"
#include "usbreq.h"
#include "usbd_helper.h"

extern struct urb_req *
find_pending_urbr(pusbip_vpdo_dev_t vpdo);

extern void
set_cmd_submit_usbip_header(struct usbip_header *h, unsigned long seqnum, unsigned int devid,
	unsigned int direct, USBD_PIPE_HANDLE pipe, unsigned int flags, unsigned int len);

void
set_cmd_unlink_usbip_header(struct usbip_header *h, unsigned long seqnum, unsigned int devid, unsigned long seqnum_unlink);

static struct usbip_header *
get_usbip_hdr_from_read_irp(PIRP irp)
{
	PIO_STACK_LOCATION	irpstack;

	irp->IoStatus.Information = 0;

	irpstack = IoGetCurrentIrpStackLocation(irp);
	if (irpstack->Parameters.Read.Length < sizeof(struct usbip_header)) {
		return NULL;
	}
	return (struct usbip_header *)irp->AssociatedIrp.SystemBuffer;
}

static PVOID
get_read_irp_data(PIRP irp, ULONG length)
{
	PIO_STACK_LOCATION	irpstack;

	irp->IoStatus.Information = 0;

	irpstack = IoGetCurrentIrpStackLocation(irp);
	if (irpstack->Parameters.Read.Length < length) {
		return NULL;
	}
	return (PVOID)irp->AssociatedIrp.SystemBuffer;
}

static ULONG
get_read_payload_length(PIRP irp)
{
	PIO_STACK_LOCATION	irpstack;

	irpstack = IoGetCurrentIrpStackLocation(irp);
	return irpstack->Parameters.Read.Length - sizeof(struct usbip_header);
}

static NTSTATUS
store_urb_reset_dev(PIRP irp, struct urb_req *urbr)
{
	struct usbip_header	*hdr;
	usb_cspkt_t	*csp;

	hdr = get_usbip_hdr_from_read_irp(irp);
	if (hdr == NULL) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	csp = (usb_cspkt_t *)hdr->u.cmd_submit.setup;

	set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, 0, 0, 0, 0);

	build_setup_packet(csp, 0, BMREQUEST_CLASS, BMREQUEST_TO_OTHER, USB_REQUEST_SET_FEATURE);
	csp->wLength = 0;
	csp->wValue.LowByte = 4; // Reset
	csp->wIndex.W = 0;

	irp->IoStatus.Information = sizeof(struct usbip_header);

	return STATUS_SUCCESS;
}

extern NTSTATUS
process_urb_abort_pipe(pusbip_vpdo_dev_t vpdo, USBD_PIPE_HANDLE pipe_handle);

static NTSTATUS
store_urb_reset_pipe(PIRP irp, PURB urb, struct urb_req *urbr)
{
	struct _URB_PIPE_REQUEST	*urb_rp = &urb->UrbPipeRequest;
	struct usbip_header	*hdr;
	int type;
	pusbip_vpdo_dev_t vpdo;

	vpdo = urbr->vpdo;
	urbr->pipe_handle = urb_rp->PipeHandle;

	hdr = get_usbip_hdr_from_read_irp(irp);
	if (hdr == NULL) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	type = PIPE2TYPE(urb_rp->PipeHandle);
	if (type != USB_ENDPOINT_TYPE_BULK && type != USB_ENDPOINT_TYPE_INTERRUPT) {
		DBGE(DBG_READ, "Error, not a bulk pipe\n");
		return STATUS_INVALID_PARAMETER;
	}

	//set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, in, 0, 0, 0);
	set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, 0, 0, 0, 0);
	RtlZeroMemory(hdr->u.cmd_submit.setup, 8);

	usb_cspkt_t *csp = (usb_cspkt_t *)hdr->u.cmd_submit.setup;
	build_setup_packet(csp, 0, BMREQUEST_STANDARD, BMREQUEST_TO_ENDPOINT, USB_REQUEST_CLEAR_FEATURE);
	csp->wIndex.W = 0;
	csp->wValue.W = 0;

	irp->IoStatus.Information = sizeof(struct usbip_header);

	process_urb_abort_pipe(vpdo, urb_rp->PipeHandle);

	return STATUS_SUCCESS;
}

static PVOID
get_buf(PVOID buf, PMDL bufMDL)
{
	if (buf == NULL) {
		if (bufMDL != NULL)
			buf = MmGetSystemAddressForMdlSafe(bufMDL, LowPagePriority);
		if (buf == NULL) {
			DBGE(DBG_READ, "No transfer buffer\n");
		}
	}
	return buf;
}

static NTSTATUS
store_urb_get_dev_desc(PIRP irp, PURB urb, struct urb_req *urbr)
{
	struct _URB_CONTROL_DESCRIPTOR_REQUEST	*urb_desc = &urb->UrbControlDescriptorRequest;
	struct usbip_header	*hdr;
	usb_cspkt_t	*csp;

	hdr = get_usbip_hdr_from_read_irp(irp);
	if (hdr == NULL) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	csp = (usb_cspkt_t *)hdr->u.cmd_submit.setup;

	set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, USBIP_DIR_IN, 0,
				    USBD_SHORT_TRANSFER_OK, urb_desc->TransferBufferLength);
	build_setup_packet(csp, USBIP_DIR_IN, BMREQUEST_STANDARD, BMREQUEST_TO_DEVICE, USB_REQUEST_GET_DESCRIPTOR);

	csp->wLength = (unsigned short)urb_desc->TransferBufferLength;
	csp->wValue.HiByte = urb_desc->DescriptorType;
	csp->wValue.LowByte = urb_desc->Index;

	switch (urb_desc->DescriptorType) {
	case USB_DEVICE_DESCRIPTOR_TYPE:
	case USB_CONFIGURATION_DESCRIPTOR_TYPE:
		csp->wIndex.W = 0;
		break;
	case USB_INTERFACE_DESCRIPTOR_TYPE:
		csp->wIndex.W = urb_desc->Index;
		break;
	case USB_STRING_DESCRIPTOR_TYPE:
		csp->wIndex.W = urb_desc->LanguageId;
		break;
	default:
		return STATUS_INVALID_PARAMETER;
	}

	irp->IoStatus.Information = sizeof(struct usbip_header);
	return STATUS_SUCCESS;
}

static NTSTATUS
store_urb_get_intf_desc(PIRP irp, PURB urb, struct urb_req *urbr)
{
	struct _URB_CONTROL_DESCRIPTOR_REQUEST	*urb_desc = &urb->UrbControlDescriptorRequest;
	struct usbip_header	*hdr;
	usb_cspkt_t	*csp;

	hdr = get_usbip_hdr_from_read_irp(irp);
	if (hdr == NULL) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	csp = (usb_cspkt_t *)hdr->u.cmd_submit.setup;

	set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, USBIP_DIR_IN, 0,
				    USBD_SHORT_TRANSFER_OK, urb_desc->TransferBufferLength);
	build_setup_packet(csp, USBIP_DIR_IN, BMREQUEST_STANDARD, BMREQUEST_TO_INTERFACE, USB_REQUEST_GET_DESCRIPTOR);

	csp->wLength = (unsigned short)urb_desc->TransferBufferLength;
	csp->wValue.HiByte = urb_desc->DescriptorType;
	csp->wValue.LowByte = urb_desc->Index;
	csp->wIndex.W = urb_desc->LanguageId;

	irp->IoStatus.Information = sizeof(struct usbip_header);
	return STATUS_SUCCESS;
}

static NTSTATUS
store_urb_class_vendor_partial(pusbip_vpdo_dev_t vpdo, PIRP irp, PURB urb)
{
	struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST	*urb_vc = &urb->UrbControlVendorClassRequest;
	PVOID	dst;
	char    *buf;

	dst = get_read_irp_data(irp, urb_vc->TransferBufferLength);
	if (dst == NULL)
		return STATUS_BUFFER_TOO_SMALL;

	/*
	 * reading from TransferBuffer or TransferBufferMDL,
	 * whichever of them is not null
	 */
	buf = get_buf(urb_vc->TransferBuffer, urb_vc->TransferBufferMDL);
	if (buf == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;

	RtlCopyMemory(dst, buf, urb_vc->TransferBufferLength);
	irp->IoStatus.Information = urb_vc->TransferBufferLength;
	vpdo->len_sent_partial = 0;

	return STATUS_SUCCESS;
}

static NTSTATUS
store_urb_class_vendor(PIRP irp, PURB urb, struct urb_req *urbr)
{
	struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST	*urb_vc = &urb->UrbControlVendorClassRequest;
	struct usbip_header	*hdr;
	usb_cspkt_t	*csp;
	char	type, recip;
	int	in = urb_vc->TransferFlags & USBD_TRANSFER_DIRECTION_IN ? 1: 0;

	hdr = get_usbip_hdr_from_read_irp(irp);
	if (hdr == NULL) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	switch (urb_vc->Hdr.Function) {
	case URB_FUNCTION_CLASS_DEVICE:
		type = BMREQUEST_CLASS;
		recip = BMREQUEST_TO_DEVICE;
		break;
	case URB_FUNCTION_CLASS_INTERFACE:
		type = BMREQUEST_CLASS;
		recip = BMREQUEST_TO_INTERFACE;
		break;
	case URB_FUNCTION_CLASS_ENDPOINT:
		type = BMREQUEST_CLASS;
		recip = BMREQUEST_TO_ENDPOINT;
		break;
	case URB_FUNCTION_CLASS_OTHER:
		type = BMREQUEST_CLASS;
		recip = BMREQUEST_TO_OTHER;
		break;
	case URB_FUNCTION_VENDOR_DEVICE:
		type = BMREQUEST_VENDOR;
		recip = BMREQUEST_TO_DEVICE;
		break;
	case URB_FUNCTION_VENDOR_INTERFACE:
		type = BMREQUEST_VENDOR;
		recip = BMREQUEST_TO_INTERFACE;
		break;
	case URB_FUNCTION_VENDOR_ENDPOINT:
		type = BMREQUEST_VENDOR;
		recip = BMREQUEST_TO_ENDPOINT;
		break;
	case URB_FUNCTION_VENDOR_OTHER:
		type = BMREQUEST_VENDOR;
		recip = BMREQUEST_TO_OTHER;
		break;
	default:
		return STATUS_INVALID_PARAMETER;
	}

	csp = (usb_cspkt_t *)hdr->u.cmd_submit.setup;

	set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, in, 0,
				    urb_vc->TransferFlags | USBD_SHORT_TRANSFER_OK, urb_vc->TransferBufferLength);
	build_setup_packet(csp, (unsigned char)in, type, recip, urb_vc->Request);
	//FIXME what is the usage of RequestTypeReservedBits?
	csp->wLength = (unsigned short)urb_vc->TransferBufferLength;
	csp->wValue.W = urb_vc->Value;
	csp->wIndex.W = urb_vc->Index;

	irp->IoStatus.Information = sizeof(struct usbip_header);

	if (!in) {
		if (get_read_payload_length(irp) >= urb_vc->TransferBufferLength) {
			RtlCopyMemory(hdr + 1, urb_vc->TransferBuffer, urb_vc->TransferBufferLength);
			irp->IoStatus.Information += urb_vc->TransferBufferLength;
		}
		else {
			urbr->vpdo->len_sent_partial = sizeof(struct usbip_header);
		}
	}
	return  STATUS_SUCCESS;
}

static NTSTATUS
store_urb_select_config(PIRP irp, PURB urb, struct urb_req *urbr)
{
	struct _URB_SELECT_CONFIGURATION	*urb_sc = &urb->UrbSelectConfiguration;
	struct usbip_header	*hdr;
	usb_cspkt_t	*csp;

	hdr = get_usbip_hdr_from_read_irp(irp);
	if (hdr == NULL) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	csp = (usb_cspkt_t *)hdr->u.cmd_submit.setup;

	set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, 0, 0, 0, 0);
	build_setup_packet(csp, 0, BMREQUEST_STANDARD, BMREQUEST_TO_DEVICE, USB_REQUEST_SET_CONFIGURATION);
	csp->wLength = 0;
	csp->wValue.W = urb_sc->ConfigurationDescriptor->bConfigurationValue;
	csp->wIndex.W = 0;

	irp->IoStatus.Information = sizeof(struct usbip_header);
	return STATUS_SUCCESS;
}

static NTSTATUS
store_urb_select_interface(PIRP irp, PURB urb, struct urb_req *urbr)
{
	struct _URB_SELECT_INTERFACE	*urb_si = &urb->UrbSelectInterface;
	struct usbip_header	*hdr;
	usb_cspkt_t	*csp;

	hdr = get_usbip_hdr_from_read_irp(irp);
	if (hdr == NULL) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	csp = (usb_cspkt_t *)hdr->u.cmd_submit.setup;

	set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, 0, 0, 0, 0);
	build_setup_packet(csp, 0, BMREQUEST_STANDARD, BMREQUEST_TO_INTERFACE, USB_REQUEST_SET_INTERFACE);
	csp->wLength = 0;
	csp->wValue.W = urb_si->Interface.AlternateSetting;
	csp->wIndex.W = urb_si->Interface.InterfaceNumber;

	irp->IoStatus.Information = sizeof(struct usbip_header);
	return  STATUS_SUCCESS;
}

static NTSTATUS
store_urb_bulk_partial(pusbip_vpdo_dev_t vpdo, PIRP irp, PURB urb)
{
	struct _URB_BULK_OR_INTERRUPT_TRANSFER	*urb_bi = &urb->UrbBulkOrInterruptTransfer;
	PVOID	dst, src;

	dst = get_read_irp_data(irp, urb_bi->TransferBufferLength);
	if (dst == NULL)
		return STATUS_BUFFER_TOO_SMALL;

	src = get_buf(urb_bi->TransferBuffer, urb_bi->TransferBufferMDL);
	if (src == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;
	RtlCopyMemory(dst, src, urb_bi->TransferBufferLength);
	irp->IoStatus.Information = urb_bi->TransferBufferLength;
	vpdo->len_sent_partial = 0;

	return STATUS_SUCCESS;
}

static NTSTATUS
store_urb_bulk(PIRP irp, PURB urb, struct urb_req *urbr)
{
	struct _URB_BULK_OR_INTERRUPT_TRANSFER	*urb_bi = &urb->UrbBulkOrInterruptTransfer;
	struct usbip_header	*hdr;
	int	in, type;

	urbr->pipe_handle = urb_bi->PipeHandle;

	hdr = get_usbip_hdr_from_read_irp(irp);
	if (hdr == NULL) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	/* Sometimes, direction in TransferFlags of _URB_BULK_OR_INTERRUPT_TRANSFER is not consistent with PipeHandle.
	 * Use a direction flag in pipe handle.
	 */
	in = PIPE2DIRECT(urb_bi->PipeHandle);
	type = PIPE2TYPE(urb_bi->PipeHandle);
	if (type != USB_ENDPOINT_TYPE_BULK && type != USB_ENDPOINT_TYPE_INTERRUPT) {
		DBGE(DBG_READ, "Error, not a bulk pipe\n");
		return STATUS_INVALID_PARAMETER;
	}

	set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, in, urb_bi->PipeHandle,
				    urb_bi->TransferFlags, urb_bi->TransferBufferLength);
	RtlZeroMemory(hdr->u.cmd_submit.setup, 8);

	irp->IoStatus.Information = sizeof(struct usbip_header);

	if (!in) {
		if (get_read_payload_length(irp) >= urb_bi->TransferBufferLength) {
			PVOID	buf = get_buf(urb_bi->TransferBuffer, urb_bi->TransferBufferMDL);
			if (buf == NULL) {
				DBGE(DBG_READ, "Error, STATUS_INSUFFICIENT_RESOURCES\n");
				return STATUS_INSUFFICIENT_RESOURCES;
			}
			RtlCopyMemory(hdr + 1, buf, urb_bi->TransferBufferLength);
		}
		else {
			urbr->vpdo->len_sent_partial = sizeof(struct usbip_header);
		}
	}
	return STATUS_SUCCESS;
}

static NTSTATUS
copy_iso_data(PVOID dst, struct _URB_ISOCH_TRANSFER *urb_iso)
{
	struct usbip_iso_packet_descriptor	*iso_desc;
	char	*buf;
	ULONG	i, offset;

	buf = get_buf(urb_iso->TransferBuffer, urb_iso->TransferBufferMDL);
	if (buf == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;

	if (PIPE2DIRECT(urb_iso->PipeHandle)) {
		iso_desc = (struct usbip_iso_packet_descriptor *)dst;
	}
	else {
		RtlCopyMemory(dst, buf, urb_iso->TransferBufferLength);
		iso_desc = (struct usbip_iso_packet_descriptor *)((char *)dst + urb_iso->TransferBufferLength);
	}

	offset = 0;
	for (i = 0; i < urb_iso->NumberOfPackets; i++) {
		if (urb_iso->IsoPacket[i].Offset < offset) {
			DBGW(DBG_READ, "strange iso packet offset:%d %d", offset, urb_iso->IsoPacket[i].Offset);
			return STATUS_INVALID_PARAMETER;
		}
		iso_desc->offset = urb_iso->IsoPacket[i].Offset;
		if (i > 0)
			(iso_desc - 1)->length = urb_iso->IsoPacket[i].Offset - offset;
		offset = urb_iso->IsoPacket[i].Offset;
		iso_desc->actual_length = 0;
		iso_desc->status = 0;
		iso_desc++;
	}
	(iso_desc - 1)->length = urb_iso->TransferBufferLength - offset;

	return STATUS_SUCCESS;
}

static ULONG
get_iso_payload_len(struct _URB_ISOCH_TRANSFER *urb_iso)
{
	ULONG	len_iso;

	len_iso = urb_iso->NumberOfPackets * sizeof(struct usbip_iso_packet_descriptor);
	if (!PIPE2DIRECT(urb_iso->PipeHandle)) {
		len_iso += urb_iso->TransferBufferLength;
	}
	return len_iso;
}

static NTSTATUS
store_urb_iso_partial(pusbip_vpdo_dev_t vpdo, PIRP irp, PURB urb)
{
	struct _URB_ISOCH_TRANSFER	*urb_iso = &urb->UrbIsochronousTransfer;
	ULONG	len_iso;
	PVOID	dst;

	len_iso = get_iso_payload_len(urb_iso);

	dst = get_read_irp_data(irp, len_iso);
	if (dst == NULL)
		return STATUS_BUFFER_TOO_SMALL;

	copy_iso_data(dst, urb_iso);
	vpdo->len_sent_partial = 0;
	irp->IoStatus.Information = len_iso;

	return STATUS_SUCCESS;
}

static NTSTATUS
store_urb_iso(PIRP irp, PURB urb, struct urb_req *urbr)
{
	struct _URB_ISOCH_TRANSFER	*urb_iso = &urb->UrbIsochronousTransfer;
	struct usbip_header	*hdr;
	int	in, type;

	in = PIPE2DIRECT(urb_iso->PipeHandle);
	type = PIPE2TYPE(urb_iso->PipeHandle);
	if (type != USB_ENDPOINT_TYPE_ISOCHRONOUS) {
		DBGE(DBG_READ, "Error, not a iso pipe\n");
		return STATUS_INVALID_PARAMETER;
	}

	urbr->pipe_handle = urb_iso->PipeHandle;

	hdr = get_usbip_hdr_from_read_irp(irp);
	if (hdr == NULL) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid,
				    in, urb_iso->PipeHandle, urb_iso->TransferFlags | USBD_SHORT_TRANSFER_OK,
				    urb_iso->TransferBufferLength);
	hdr->u.cmd_submit.start_frame = urb_iso->StartFrame;
	hdr->u.cmd_submit.number_of_packets = urb_iso->NumberOfPackets;

	irp->IoStatus.Information = sizeof(struct usbip_header);

	if (get_read_payload_length(irp) >= get_iso_payload_len(urb_iso)) {
		copy_iso_data(hdr + 1, urb_iso);
		irp->IoStatus.Information += get_iso_payload_len(urb_iso);
	}
	else {
		urbr->vpdo->len_sent_partial = sizeof(struct usbip_header);
	}

	return STATUS_SUCCESS;
}


static NTSTATUS
store_urb_control_transfer_partial(pusbip_vpdo_dev_t vpdo, PIRP irp, PURB urb)
{
	struct _URB_CONTROL_TRANSFER_EX* urb_control_ex = &urb->UrbControlTransferEx;
	PVOID	dst;
	char* buf;

	dst = get_read_irp_data(irp, urb_control_ex->TransferBufferLength);
	if (dst == NULL)
		return STATUS_BUFFER_TOO_SMALL;

	/*
	 * reading from TransferBuffer or TransferBufferMDL,
	 * whichever of them is not null
	 */
	buf = get_buf(urb_control_ex->TransferBuffer, urb_control_ex->TransferBufferMDL);
	if (buf == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;
	RtlCopyMemory(dst, buf, urb_control_ex->TransferBufferLength);
	irp->IoStatus.Information = urb_control_ex->TransferBufferLength;
	vpdo->len_sent_partial = 0;

	return STATUS_SUCCESS;
}

static NTSTATUS
store_urb_control_transfer(PIRP irp, PURB urb, struct urb_req* urbr)
{
	DBGI(DBG_READ, "ControlEx timeout: %i", urb->UrbControlTransferEx.Timeout);
	struct _URB_CONTROL_TRANSFER_EX* urb_control_ex = &urb->UrbControlTransferEx;
	struct usbip_header* hdr;
	int	in = urb_control_ex->TransferFlags & USBD_TRANSFER_DIRECTION_IN ? 1 : 0;

	hdr = get_usbip_hdr_from_read_irp(irp);
	if (hdr == NULL) {
		DBGE(DBG_READ, "Cannot get usbip header\n");
		return STATUS_BUFFER_TOO_SMALL;
	}

	urbr->pipe_handle = urb_control_ex->PipeHandle;

/*	if (urb_control_ex->PipeHandle) {
		in = PIPE2DIRECT(urb_control_ex->PipeHandle);
		type = PIPE2TYPE(urb_control_ex->PipeHandle);
	}
	else {
		DBGI(DBG_READ, "Setting explicitly in and type\n");
		in = USBIP_DIR_IN;
		type = USB_ENDPOINT_TYPE_CONTROL;
	}

	if (type != USB_ENDPOINT_TYPE_CONTROL) {
		DBGE(DBG_READ, "Not a transfer pipe\n");
		return STATUS_INVALID_PARAMETER;
	}*/
	if (!urb_control_ex->PipeHandle) {
		DBGI(DBG_READ, "Pipe handle empty\n");
	}
	if (urb_control_ex->TransferFlags & USBD_DEFAULT_PIPE_TRANSFER) {
		DBGI(DBG_READ, "Use default pipe\n");
	}
	if (urb_control_ex->TransferFlags & USBD_TRANSFER_DIRECTION_IN) {
		DBGI(DBG_READ, "Control ex: USBD_TRANSFER_DIRECTION_IN\n");
	}
	else {
		DBGI(DBG_READ, "Control ex: USBD_TRANSFER_DIRECTION_OUT\n");
	}
	if (urb_control_ex->TransferFlags & USBD_SHORT_TRANSFER_OK) {
		DBGI(DBG_READ, "Control ex: Short transfer OK\n");
	}

	set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, in, 0,
		urb_control_ex->TransferFlags | USBD_SHORT_TRANSFER_OK, urb_control_ex->TransferBufferLength);
	RtlCopyMemory(hdr->u.cmd_submit.setup, urb_control_ex->SetupPacket, 8);

	DBGI(DBG_READ, "  setup: %02hhx_%02hhx_%02hhx_%02hhx_%02hhx_%02hhx_%02hhx_%02hhx\n",
		hdr->u.cmd_submit.setup[0], hdr->u.cmd_submit.setup[1], hdr->u.cmd_submit.setup[2],
		hdr->u.cmd_submit.setup[3], hdr->u.cmd_submit.setup[4], hdr->u.cmd_submit.setup[5],
		hdr->u.cmd_submit.setup[6], hdr->u.cmd_submit.setup[7]);
	DBGI(DBG_READ, "  flags:%x,len:%x,sf:%x,#p:%x,intv:%x\n",
		hdr->u.cmd_submit.transfer_flags,
		hdr->u.cmd_submit.transfer_buffer_length,
		hdr->u.cmd_submit.start_frame,
		hdr->u.cmd_submit.number_of_packets,
		hdr->u.cmd_submit.interval);

	irp->IoStatus.Information = sizeof(struct usbip_header);

	DBGI(DBG_READ, "Buffer length: %i\n", urb_control_ex->TransferBufferLength);

	if (!in) {
		if (get_read_payload_length(irp) >= urb_control_ex->TransferBufferLength) {
			PVOID buf = get_buf(urb_control_ex->TransferBuffer, urb_control_ex->TransferBufferMDL);
			if (buf == NULL)
				return STATUS_INSUFFICIENT_RESOURCES;
			RtlCopyMemory(hdr + 1, buf, urb_control_ex->TransferBufferLength);
		}
		else {
			DBGI(DBG_READ, "TransferEx - Partial!\n");
			urbr->vpdo->len_sent_partial = sizeof(struct usbip_header);
		}
	}

	return STATUS_SUCCESS;
}

static NTSTATUS
store_urbr_submit(PIRP irp, struct urb_req *urbr)
{
	PURB	urb;
	PIO_STACK_LOCATION	irpstack;
	USHORT		code_func;
	NTSTATUS	status;
	KIRQL		oldirql;
	PLIST_ENTRY	le;
	struct urb_req* urbr_local;
	usb_cspkt_t *csp;
	//PUSBD_PIPE_INFORMATION pipe_inf;
	//PUSBD_INTERFACE_INFORMATION int_inf;
	USBD_PIPE_HANDLE pipe_handle;
	//unsigned int i, k, counter;
	//NTSTATUS abort_stat;
	//BOOLEAN pipe_found = FALSE;
	struct usbip_header* hdr;

	DBGI(DBG_READ, "store_urbr_submit: urbr: %s\n", dbg_urbr(urbr));

	irpstack = IoGetCurrentIrpStackLocation(urbr->irp);
	urb = irpstack->Parameters.Others.Argument1;
	if (urb == NULL) {
		DBGE(DBG_READ, "store_urbr_submit: null urb\n");

		irp->IoStatus.Information = 0;
		return STATUS_INVALID_DEVICE_REQUEST;
	}

	code_func = urb->UrbHeader.Function;
	DBGI(DBG_READ, "store_urbr_submit: urbr: %s, func:%s\n", dbg_urbr(urbr), dbg_urbfunc(code_func));

	switch (code_func) {
	case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
		status = store_urb_bulk(irp, urb, urbr);
		break;
	case URB_FUNCTION_ISOCH_TRANSFER:
		status = store_urb_iso(irp, urb, urbr);
		break;
	case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
		status = store_urb_get_dev_desc(irp, urb, urbr);
		break;
	case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
		status = store_urb_get_intf_desc(irp, urb, urbr);
		break;
	case URB_FUNCTION_CLASS_DEVICE:
	case URB_FUNCTION_CLASS_INTERFACE:
	case URB_FUNCTION_CLASS_ENDPOINT:
	case URB_FUNCTION_CLASS_OTHER:
	case URB_FUNCTION_VENDOR_DEVICE:
	case URB_FUNCTION_VENDOR_INTERFACE:
	case URB_FUNCTION_VENDOR_ENDPOINT:
		status = store_urb_class_vendor(irp, urb, urbr);
		break;
	case URB_FUNCTION_SELECT_CONFIGURATION:
		status = store_urb_select_config(irp, urb, urbr);
		break;
	case URB_FUNCTION_SELECT_INTERFACE:
		status = store_urb_select_interface(irp, urb, urbr);
		break;
	case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
		status = store_urb_reset_pipe(irp, urb, urbr);
		break;
	case URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR:
		status = STATUS_NOT_SUPPORTED;
		break;
	case URB_FUNCTION_CONTROL_TRANSFER_EX:
		status = store_urb_control_transfer(irp, urb, urbr);
		break;
	case URB_FUNCTION_ABORT_PIPE:
	//	status = STATUS_SUCCESS;
	//	irp->IoStatus.Information = 0;
	//	break;
		hdr = get_usbip_hdr_from_read_irp(irp);
		RtlZeroMemory(hdr->u.cmd_submit.setup, 8);
		csp = (usb_cspkt_t*)hdr->u.cmd_submit.setup;
		set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, 0, urb->UrbPipeRequest.PipeHandle, 0, 0);
		build_setup_packet(csp, 0, BMREQUEST_STANDARD, BMREQUEST_TO_ENDPOINT, USB_REQUEST_CLEAR_FEATURE);
		irp->IoStatus.Information = sizeof(struct usbip_header);
		status = STATUS_SUCCESS;
		if (urb->UrbPipeRequest.PipeHandle) {
			pipe_handle = urb->UrbPipeRequest.PipeHandle;
			KeAcquireSpinLock(&urbr->vpdo->lock_urbr, &oldirql);
			DBGI(DBG_READ, "Pipe abort before pending\n");
			for (le = urbr->vpdo->head_urbr.Flink; le != &urbr->vpdo->head_urbr;) {
				urbr_local = CONTAINING_RECORD(le, struct urb_req, list_all);
				le = le->Flink;
				if (urbr_local->seq_num == urbr->seq_num)
					continue;
				DBGI(DBG_READ, "Creawl thru all pipes: ADDR: %02x, TYPE: %d\n", PIPE2ADDR(pipe_handle), PIPE2TYPE(pipe_handle));
				DBGI(DBG_READ, "Creawl thru all pipes: ADDR: %02x, TYPE: %d\n", PIPE2ADDR(urbr_local->pipe_handle), PIPE2TYPE(urbr_local->pipe_handle));
				if (urbr_local->irp) {
					if (PIPE2ADDR(pipe_handle) == PIPE2ADDR(urbr_local->pipe_handle) &&
							PIPE2TYPE(pipe_handle) == PIPE2TYPE(urbr_local->pipe_handle)) {
						KeReleaseSpinLock(&urbr->vpdo->lock_urbr, oldirql);
						urbr_local->irp->IoStatus.Status = STATUS_CANCELLED;
						urbr_local->irp->IoStatus.Information = 0;
						IoCompleteRequest(urbr_local->irp, IO_NO_INCREMENT);
						KeAcquireSpinLock(&urbr->vpdo->lock_urbr, &oldirql);
						free_urbr(urbr_local);
					}
				}
				else {
					if (PIPE2ADDR(pipe_handle) == PIPE2ADDR(urbr_local->pipe_handle) &&
							PIPE2TYPE(pipe_handle) == PIPE2TYPE(urbr_local->pipe_handle)) {
						DBGI(DBG_READ, "Pipe abort one\n");
						RemoveEntryListInit(&urbr_local->list_state);
						RemoveEntryListInit(&urbr_local->list_all);
						free_urbr(urbr_local);
					}
				}
			}
			KeReleaseSpinLock(&urbr->vpdo->lock_urbr, oldirql);
		}
		break;

			//	if (!(IsListEmpty(&urbr_local->list_all) && IsListEmpty(&urbr_local->list_state))) {
			//		le = le->Flink;
			//		continue;
			//	}
			//	urbr_local->irp->IoStatus.Status = STATUS_CANCELLED;
			//	set_pipe(pipe_inf, urbr_local->vpdo->dsc_conf, urbr_local->vpdo->speed);
			//	if (pipe_inf->PipeHandle != urb->UrbPipeRequest.PipeHandle)
			//		continue;
			/*	int_inf = urbr_local->vpdo->int_inf;
				pipe_found = FALSE;
				DBGI(DBG_READ, "Creawl thru all interfaces\n");
				for (i = 0; i < urbr_local->vpdo->int_inf_num; i++) {
					DBGI(DBG_READ, "Creawl thre interfaces - setup inf start\n");
					abort_stat = setup_intf(int_inf, urbr_local->vpdo->dsc_conf, urbr_local->vpdo->speed);
					DBGI(DBG_READ, "Creawl thre interfaces - setup inf end\n");
					if (abort_stat != STATUS_SUCCESS) {
						DBGI(DBG_READ, "Cannot setup interface\n");
						status = STATUS_UNSUCCESSFUL;
						break;
					}
					DBGI(DBG_READ, "Creawl thru all pipes\n");
					for (k = 0; k < int_inf->NumberOfPipes; k++) {
						DBGI(DBG_READ, "Creawl thru all pipes: %i, ADDR: %02x, TYPE: %d\n", k, PIPE2ADDR(pipe_handle), PIPE2TYPE(pipe_handle));
						DBGI(DBG_READ, "Creawl thru all pipes: %i, ADDR: %02x, TYPE: %d\n", k, PIPE2ADDR(int_inf->Pipes[k].PipeHandle), PIPE2TYPE(int_inf->Pipes[k].PipeHandle));
						if (PIPE2ADDR(pipe_handle) == PIPE2ADDR(int_inf->Pipes[k].PipeHandle) &&
								PIPE2TYPE(pipe_handle) == PIPE2TYPE(int_inf->Pipes[k].PipeHandle)) {
							pipe_found = TRUE;
							counter++;
							DBGI(DBG_READ, "Pipe found\n");
							break;
						}
					}
					if (pipe_found)
						break;
					int_inf = NEXT_USBD_INTERFACE_INFO(int_inf);
				}
				if (!pipe_found) {
					le = le->Flink;
					continue;
				}
				le = le->Flink;
				RemoveEntryListInit(&urbr_local->list_state);
				RemoveEntryListInit(&urbr_local->list_all);
				KeReleaseSpinLock(&urbr->vpdo->lock_urbr, oldirql);

				if (urbr_local->irp != NULL) {
					urbr_local->irp->IoStatus.Status = STATUS_CANCELLED;
					urbr_local->irp->IoStatus.Information = 0;
					//	urbr_local->irp->IoStatus.Information = 0;
						//KeReleaseSpinLock(&urbr->vpdo->lock_urbr, oldirql);
					IoCompleteRequest(urbr_local->irp, IO_NO_INCREMENT);
					free_urbr(urbr_local);
				}
				DBGI(DBG_READ, "Processing pipe inoamation processed\n");
				//IoCancelIrp(urbr_local->irp);
				//KeAcquireSpinLock(&urbr->vpdo->lock_urbr, &oldirql);	

				//le = le_tmp;
				KeAcquireSpinLock(&urbr->vpdo->lock_urbr, &oldirql);
				//break;
			}
			KeReleaseSpinLock(&urbr->vpdo->lock_urbr, oldirql);
			//if (!counter)
			//	status = STATUS_INVALID_PARAMETER;
		}
		if (urb->UrbPipeRequest.PipeHandle == NULL)
			status = STATUS_SUCCESS;
		break;
		*/
	default:
		irp->IoStatus.Information = 0;
		DBGE(DBG_READ, "unhandled urb function: %s\n", dbg_urbfunc(code_func));
		status = STATUS_INVALID_PARAMETER;
		break;
	}

	return status;
}

static NTSTATUS
store_urbr_partial(PIRP irp, struct urb_req *urbr)
{
	PURB	urb;
	PIO_STACK_LOCATION	irpstack;
	USHORT		code_func;
	NTSTATUS	status;

	DBGI(DBG_READ, "store_urbr_partial: urbr: %s\n", dbg_urbr(urbr));

	irpstack = IoGetCurrentIrpStackLocation(urbr->irp);
	urb = irpstack->Parameters.Others.Argument1;
	code_func = urb->UrbHeader.Function;

	switch (code_func) {
	case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
		status = store_urb_bulk_partial(urbr->vpdo, irp, urb);
		break;
	case URB_FUNCTION_ISOCH_TRANSFER:
		status = store_urb_iso_partial(urbr->vpdo, irp, urb);
		break;
	case URB_FUNCTION_CLASS_DEVICE:
	case URB_FUNCTION_CLASS_INTERFACE:
	case URB_FUNCTION_CLASS_ENDPOINT:
	case URB_FUNCTION_CLASS_OTHER:
	case URB_FUNCTION_VENDOR_DEVICE:
	case URB_FUNCTION_VENDOR_INTERFACE:
	case URB_FUNCTION_VENDOR_ENDPOINT:
		status = store_urb_class_vendor_partial(urbr->vpdo, irp, urb);
		break;
	case URB_FUNCTION_CONTROL_TRANSFER_EX:
		status = store_urb_control_transfer_partial(urbr->vpdo, irp, urb);
		break;
	default:
		irp->IoStatus.Information = 0;
		DBGE(DBG_READ, "store_urbr_partial: unexpected partial urbr: %s\n", dbg_urbfunc(code_func));
		status = STATUS_INVALID_PARAMETER;
		break;
	}
	DBGI(DBG_READ, "store_urbr_partial: status: %i\n", status);

	return status;
}

static NTSTATUS
store_cancelled_urbr(PIRP irp, struct urb_req *urbr)
{
	struct usbip_header	*hdr;

	DBGI(DBG_READ, "store_cancelled_urbr\n");

	hdr = get_usbip_hdr_from_read_irp(irp);
	if (hdr == NULL)
		return STATUS_INVALID_PARAMETER;

	set_cmd_unlink_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, urbr->seq_num_unlink);

	irp->IoStatus.Information = sizeof(struct usbip_header);
	return STATUS_SUCCESS;
}

NTSTATUS
store_urbr(PIRP irp, struct urb_req *urbr)
{
	PIO_STACK_LOCATION	irpstack;
	ULONG		ioctl_code;
	NTSTATUS	status;

	DBGI(DBG_READ, "store_urbr: urbr: %s\n", dbg_urbr(urbr));

	if (urbr->irp == NULL) {
		return store_cancelled_urbr(irp, urbr);
	}

	irpstack = IoGetCurrentIrpStackLocation(urbr->irp);
	ioctl_code = irpstack->Parameters.DeviceIoControl.IoControlCode;
	switch (ioctl_code) {
	case IOCTL_INTERNAL_USB_SUBMIT_URB:
		status = store_urbr_submit(irp, urbr);
		break;
	case IOCTL_INTERNAL_USB_RESET_PORT:
		status = store_urb_reset_dev(irp, urbr);
		break;
	default:
		DBGW(DBG_READ, "unhandled ioctl: %s\n", dbg_vhci_ioctl_code(ioctl_code));
		irp->IoStatus.Information = 0;
		status = STATUS_INVALID_PARAMETER;
		break;
	}

	return status;
}

static NTSTATUS
process_read_irp(pusbip_vpdo_dev_t vpdo, PIRP read_irp)
{
	struct urb_req	*urbr;
	KIRQL	oldirql;
	NTSTATUS status;

	DBGI(DBG_GENERAL | DBG_READ, "process_read_irp: Enter\n");

	KeAcquireSpinLock(&vpdo->lock_urbr, &oldirql);
	if (vpdo->pending_read_irp) {
		KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);
		return STATUS_INVALID_DEVICE_REQUEST;
	}
	if (vpdo->urbr_sent_partial != NULL) {
		urbr = vpdo->urbr_sent_partial;
		KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);

		status = store_urbr_partial(read_irp, urbr);

		KeAcquireSpinLock(&vpdo->lock_urbr, &oldirql);
		vpdo->len_sent_partial = 0;
	}
	else {
		urbr = find_pending_urbr(vpdo);
		if (urbr == NULL) {
			IoMarkIrpPending(read_irp);
			vpdo->pending_read_irp = read_irp;
			KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);
			return STATUS_PENDING;
		}
		vpdo->urbr_sent_partial = urbr;
		KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);

		status = store_urbr(read_irp, urbr);

		KeAcquireSpinLock(&vpdo->lock_urbr, &oldirql);
	}

	if (status != STATUS_SUCCESS) {
		RemoveEntryListInit(&urbr->list_all);
		KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);

		if (urbr->irp != NULL) {
			IoSetCancelRoutine(urbr->irp, NULL);
			urbr->irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
			IoCompleteRequest(urbr->irp, IO_NO_INCREMENT);
		}
		free_urbr(urbr);
	}
	else {
		if (vpdo->len_sent_partial == 0) {
			InsertTailList(&vpdo->head_urbr_sent, &urbr->list_state);
			vpdo->urbr_sent_partial = NULL;
		}
		KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);
	}
	return status;
}

PAGEABLE NTSTATUS
vhci_read(__in PDEVICE_OBJECT devobj, __in PIRP irp)
{
	pusbip_vhub_dev_t	vhub;
	pusbip_vpdo_dev_t	vpdo;
	pdev_common_t		devcom;
	PIO_STACK_LOCATION	irpstack;
	NTSTATUS		status;

	PAGED_CODE();

	devcom = (pdev_common_t)devobj->DeviceExtension;

	DBGI(DBG_GENERAL | DBG_READ, "vhci_read: Enter\n");

	if (!devcom->is_vhub) {
		DBGE(DBG_READ, "read for vhub is not allowed\n");

		irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return STATUS_INVALID_DEVICE_REQUEST;
	}

	vhub = (pusbip_vhub_dev_t)devobj->DeviceExtension;

	inc_io_vhub(vhub);

	// Check to see whether the bus is removed
	if (vhub->common.DevicePnPState == Deleted) {
		status = STATUS_NO_SUCH_DEVICE;
		goto END;
	}
	irpstack = IoGetCurrentIrpStackLocation(irp);
	vpdo = irpstack->FileObject->FsContext;
	if (vpdo == NULL || !vpdo->Present)
		status = STATUS_INVALID_DEVICE_REQUEST;
	else
		status = process_read_irp(vpdo, irp);

END:
	DBGI(DBG_GENERAL | DBG_READ, "vhci_read: Leave: %s\n", dbg_ntstatus(status));
	if (status != STATUS_PENDING) {
		irp->IoStatus.Status = status;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
	}
	dec_io_vhub(vhub);
	return status;
}