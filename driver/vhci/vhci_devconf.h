#pragma once

#include "devconf.h"

#include <usbdi.h>

#define NEXT_USBD_INTERFACE_INFO(info_intf)	(USBD_INTERFACE_INFORMATION *)((PUINT8)(info_intf + 1) - \
	(1 * sizeof(USBD_PIPE_INFORMATION)) + (info_intf->NumberOfPipes * sizeof(USBD_PIPE_INFORMATION)));

extern NTSTATUS
select_config(struct _URB_SELECT_CONFIGURATION *urb_selc, UCHAR speed);

extern NTSTATUS
select_interface(struct _URB_SELECT_INTERFACE *urb_seli, PUSB_CONFIGURATION_DESCRIPTOR dsc_conf, UCHAR speed);


NTSTATUS
setup_intf(USBD_INTERFACE_INFORMATION* intf, PUSB_CONFIGURATION_DESCRIPTOR dsc_conf, UCHAR speed);
