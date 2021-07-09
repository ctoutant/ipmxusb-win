#include "vhci.h"
#include "vhci_pnp.h"
#include "usbip_vhci_api.h"
#include "vhci_irp.h"

#include <stdbool.h>

#define DEVID_VHCI	HWID_VHCI
#define DEVID_VHUB	HWID_VHUB

/*
 * The first hardware ID in the list should be the device ID, and
 * the remaining IDs should be listed in order of decreasing suitability.
 */
#define HWIDS_VHCI	DEVID_VHCI L"\0"

#define HWIDS_VHUB \
	DEVID_VHUB L"\0" \
	VHUB_PREFIX L"&VID_" VHUB_VID L"&PID_" VHUB_PID L"\0"

// vdev_type_t is an index
static const LPCWSTR vdev_devids[] = {
	NULL, DEVID_VHCI,
	NULL, DEVID_VHUB,
	NULL, L"USB\\VID_%04hx&PID_%04hx" // 21 chars after formatting
};

static const size_t vdev_devid_size[] = {
	0, sizeof(DEVID_VHCI),
	0, sizeof(DEVID_VHUB),
	0, (21+1) * sizeof(wchar_t)
};

static const LPCWSTR vdev_hwids[] = {
	NULL, HWIDS_VHCI,
	NULL, HWIDS_VHUB,
	NULL, L"USB\\VID_%04hx&PID_%04hx&REV_%04hx;" // 31 chars after formatting
	      L"USB\\VID_%04hx&PID_%04hx;" // 22 chars after formatting
};

static const size_t vdev_hwids_size[] = {
	0, sizeof(HWIDS_VHCI),
	0, sizeof(HWIDS_VHUB),
	0, (31+22+1) * sizeof(wchar_t)
};

void multi_z_replace_char(wchar_t *s, wchar_t ch, wchar_t rep)
{
	for ( ; *s; ++s) {
		if (*s == ch) {
			*s = rep;
		}
	}
}

/*
  Enumeration of USB Composite Devices.

  The bus driver also reports a compatible identifier (ID) of USB\COMPOSITE,
  if the device meets the following requirements:
  * The device class field of the device descriptor (bDeviceClass) must contain a value of zero,
    or the class (bDeviceClass), subclass (bDeviceSubClass), and protocol (bDeviceProtocol) fields
    of the device descriptor must have the values 0xEF, 0x02 and 0x01 respectively, as explained
    in USB Interface Association Descriptor.
  * The device must have multiple interfaces.
  * The device must have a single configuration.

  The bus driver also checks the device class (bDeviceClass), subclass (bDeviceSubClass),
  and protocol (bDeviceProtocol) fields of the device descriptor. If these fields are zero,
  the device is a composite device, and the bus driver reports an extra compatible
  identifier (ID) of USB\COMPOSITE for the PDO.
*/
static bool is_composite(pvpdo_dev_t vpdo)
{
	USB_DEVICE_DESCRIPTOR *dd = vpdo->dsc_dev;

	BOOLEAN ok = !vpdo->usbclass || // generic composite device
	             (vpdo->usbclass == 0xEF &&
	              vpdo->subclass == 0x02 &&
	              vpdo->protocol == 0x01); // IAD composite device

	if (ok && vpdo->dsc_conf->bNumInterfaces > 1) { // && dd && dd->bNumConfigurations == 1) {
		return true;
	}

	return dd && !(dd->bDeviceClass || dd->bDeviceSubClass || dd->bDeviceProtocol);
}

/*
 * For all USB devices, the USB bus driver reports a device ID with the following format:
 * USB\VID_xxxx&PID_yyyy
 */
static NTSTATUS
setup_device_id(PWCHAR *result, pvdev_t vdev, PIRP irp)
{
	UNREFERENCED_PARAMETER(irp);

	NTSTATUS status = STATUS_SUCCESS;
	PWCHAR id_dev = NULL;

	size_t str_sz = vdev_devid_size[vdev->type];
	LPCWSTR str = vdev_devids[vdev->type];

	if (!str) {
		DBGI(DBG_PNP, "%s: query device id: NOT SUPPORTED\n", dbg_vdev_type(vdev->type));
		return STATUS_NOT_SUPPORTED;
	}

	id_dev = ExAllocatePoolWithTag(PagedPool, str_sz, USBIP_VHCI_POOL_TAG);
	if (!id_dev) {
		DBGE(DBG_PNP, "%s: query device id: out of memory\n", dbg_vdev_type(vdev->type));
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	if (vdev->type == VDEV_VPDO) {
		pvpdo_dev_t vpdo = (pvpdo_dev_t)vdev;
		status = RtlStringCbPrintfW(id_dev, str_sz, str, vpdo->vendor, vpdo->product);
	} else {
		RtlCopyMemory(id_dev, str, str_sz);
	}

	if (status == STATUS_SUCCESS) {
		*result = id_dev;
		DBGI(DBG_PNP, "%s: device id: %S\n", dbg_vdev_type(vdev->type), id_dev);
	} else {
		ExFreePoolWithTag(id_dev, USBIP_VHCI_POOL_TAG);
		DBGE(DBG_PNP, "%s: query device id failure\n", dbg_vdev_type(vdev->type));
	}

	return status;
}

static NTSTATUS
setup_hw_ids(PWCHAR *result, pvdev_t vdev, PIRP irp)
{
	UNREFERENCED_PARAMETER(irp);

	NTSTATUS status = STATUS_SUCCESS;
	bool multi_z = vdev->type == VDEV_VPDO;
	PWCHAR ids_hw = NULL;

	size_t str_sz = vdev_hwids_size[vdev->type];
	LPCWSTR str = vdev_hwids[vdev->type];

	if (!str) {
		DBGI(DBG_PNP, "%s: query hw ids: NOT SUPPORTED%s\n", dbg_vdev_type(vdev->type));
		return STATUS_NOT_SUPPORTED;
	}

	ids_hw = ExAllocatePoolWithTag(PagedPool, str_sz, USBIP_VHCI_POOL_TAG);
	if (!ids_hw) {
		DBGE(DBG_PNP, "%s: query hw ids: out of memory\n", dbg_vdev_type(vdev->type));
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	if (multi_z) {
		pvpdo_dev_t vpdo = (pvpdo_dev_t)vdev;
		status = RtlStringCbPrintfW(ids_hw, str_sz, str,
					    vpdo->vendor, vpdo->product, vpdo->revision,
					    vpdo->vendor, vpdo->product);
	} else {
		RtlCopyMemory(ids_hw, str, str_sz);
	}

	if (status == STATUS_SUCCESS) {
		*result = ids_hw;
		DBGI(DBG_PNP, "%s: hw id: %S\n", dbg_vdev_type(vdev->type), ids_hw);
		if (multi_z) {
			multi_z_replace_char(ids_hw, L';', L'\0');
		}
	} else {
		ExFreePoolWithTag(ids_hw, USBIP_VHCI_POOL_TAG);
		DBGE(DBG_PNP, "%s: query hw id failure\n", dbg_vdev_type(vdev->type));
	}

	return status;
}

/*
Instance ID

An instance ID is a device identification string that distinguishes a device
from other devices of the same type on a computer. An instance ID contains
serial number information, if supported by the underlying bus, or some kind
of location information. The string cannot contain any "\" characters;
otherwise, the generic format of the string is bus-specific.

The number of characters of an instance ID, excluding a NULL-terminator, must be
less than MAX_DEVICE_ID_LEN. In addition, when an instance ID is concatenated
to a device ID to create a device instance ID, the lengths of the device ID
and the instance ID are further constrained by the maximum possible length
of a device instance ID.

The UniqueID member of the DEVICE_CAPABILITIES structure for a device indicates
if a bus-supplied instance ID is unique across the system, as follows:
 * If UniqueID is FALSE, the bus-supplied instance ID for a device is unique
   only to the device's bus. The Plug and Play (PnP) manager modifies
   the bus-supplied instance ID, and combines it with the corresponding device ID,
   to create a device instance ID that is unique in the system.
 * If UniqueID is TRUE, the device instance ID, formed from the bus-supplied
   device ID and instance ID, uniquely identifies a device in the system.

An instance ID is persistent across system restarts.
*/
static NTSTATUS
setup_inst_id_or_serial(PWCHAR *result, pvdev_t vdev, PIRP irp, const char *query, bool serial)
{
	UNREFERENCED_PARAMETER(irp);

	NTSTATUS status = STATUS_SUCCESS;
	pvpdo_dev_t vpdo = (pvpdo_dev_t)vdev;
	PWCHAR id_inst = NULL;

	size_t max_wchars = MAX_VHCI_SERIAL_ID + 1;
//	static_assert(MAX_VHCI_SERIAL_ID <= MAX_DEVICE_ID_LEN, "assert");

	if (vdev->type != VDEV_VPDO) {
		DBGI(DBG_PNP, "%s: query %s: NOT SUPPORTED\n", dbg_vdev_type(vdev->type), query);
		return STATUS_NOT_SUPPORTED;
	}

	id_inst = ExAllocatePoolWithTag(PagedPool, max_wchars * sizeof(wchar_t), USBIP_VHCI_POOL_TAG);
	if (!id_inst) {
		DBGE(DBG_PNP, "vpdo: query %s: out of memory\n", query);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	status = vpdo->winstid ? RtlStringCchCopyW(id_inst, max_wchars, vpdo->winstid) : // is a serial
		 serial ? RtlStringCchPrintfW(id_inst, max_wchars, L"") : // has no serial
		 RtlStringCchPrintfW(id_inst, max_wchars, L"%04hx", vpdo->port);

	if (status == STATUS_SUCCESS) {
		*result = id_inst;
		DBGI(DBG_PNP, "vpdo: query %s: %S\n", query, id_inst);
	} else {
		ExFreePoolWithTag(id_inst, USBIP_VHCI_POOL_TAG);
		DBGE(DBG_PNP, "vpdo: query %s failure\n", query);
	}

	return status;
}

static NTSTATUS
setup_compat_ids(PWCHAR *result, pvdev_t vdev, PIRP irp)
{
	UNREFERENCED_PARAMETER(irp);

	pvpdo_dev_t vpdo = (pvpdo_dev_t)vdev;
	NTSTATUS status = STATUS_SUCCESS;
	PWCHAR ids_compat = NULL;

	NTSTRSAFE_PWSTR dest_end = NULL;
	size_t remaining = 0;

	const wchar_t comp[] = L"USB\\COMPOSITE\0";

	const wchar_t fmt[] =
	L"USB\\Class_%02hhx&SubClass_%02hhx&Prot_%02hhx;" // 33 chars after formatting
	L"USB\\Class_%02hhx&SubClass_%02hhx;" // 25 chars after formatting
	L"USB\\Class_%02hhx;"; // 13 chars after formatting

	const size_t max_wchars = 33 + 25 + 13 + sizeof(comp)/sizeof(*comp);

	if (vdev->type != VDEV_VPDO) {
		DBGI(DBG_PNP, "%s: query compatible id: NOT SUPPORTED\n", dbg_vdev_type(vdev->type));
		return STATUS_NOT_SUPPORTED;
	}

	ids_compat = ExAllocatePoolWithTag(PagedPool, max_wchars * sizeof(wchar_t), USBIP_VHCI_POOL_TAG);
	if (!ids_compat) {
		DBGE(DBG_PNP, "vpdo: query compatible id: out of memory\n");
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	status = RtlStringCchPrintfExW(ids_compat, max_wchars, &dest_end, &remaining, 0, fmt,
				     vpdo->usbclass, vpdo->subclass, vpdo->protocol,
				     vpdo->usbclass, vpdo->subclass,
				     vpdo->usbclass);

	if (status == STATUS_SUCCESS && is_composite(vpdo)) {
		NT_ASSERT(sizeof(comp) == remaining);
		RtlCopyMemory(dest_end, comp, sizeof(comp));
	}

	if (status == STATUS_SUCCESS) {
		*result = ids_compat;
		DBGI(DBG_PNP, "%s: query compatible id: %S\n", dbg_vdev_type(vdev->type), ids_compat);
		multi_z_replace_char(ids_compat, L';', L'\0');
	} else {
		ExFreePoolWithTag(ids_compat, USBIP_VHCI_POOL_TAG);
		DBGE(DBG_PNP, "vpdo: query compatible id failure\n");
	}

	return status;
}

/*
 * On success, a driver sets Irp->IoStatus.Information to a WCHAR pointer
 * that points to the requested information. On error, a driver sets
 * Irp->IoStatus.Information to zero.
 */
PAGEABLE NTSTATUS
pnp_query_id(pvdev_t vdev, PIRP irp, PIO_STACK_LOCATION irpstack)
{
	NTSTATUS status = STATUS_NOT_SUPPORTED;
	PWCHAR result = NULL;

	DBGI(DBG_PNP, "%s: query id: %s\n", dbg_vdev_type(vdev->type), dbg_bus_query_id_type(irpstack->Parameters.QueryId.IdType));

	PAGED_CODE();

	switch (irpstack->Parameters.QueryId.IdType) {
	case BusQueryDeviceID:
		status = setup_device_id(&result, vdev, irp);
		break;
	case BusQueryInstanceID:
		status = setup_inst_id_or_serial(&result, vdev, irp, "instance id", false);
		break;
	case BusQueryHardwareIDs:
		status = setup_hw_ids(&result, vdev, irp);
		break;
	case BusQueryCompatibleIDs:
		status = setup_compat_ids(&result, vdev, irp);
		break;
	case BusQueryDeviceSerialNumber:
		status = setup_inst_id_or_serial(&result, vdev, irp, "device serial number", true);
		break;
	case BusQueryContainerID:
	default:
		DBGW(DBG_PNP, "%s: unhandled query id: %s\n",
			dbg_vdev_type(vdev->type),
			dbg_bus_query_id_type(irpstack->Parameters.QueryId.IdType));
	}

	irp->IoStatus.Information = (ULONG_PTR)result;
	return irp_done(irp, status);
}
