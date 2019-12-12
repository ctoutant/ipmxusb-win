
#include "usbip.h"
#include <windows.h>
#include <setupapi.h>

#include <pathcch.h>

#include <string.h>

#define BUFFER_SIZE 1024

#define DEVICE_DRIVER_INF_FILENAME "usbip_vhci.inf"

#define DEVICE_INSTANCE_ID "ROOT\\USBIP_VHCI\\00000"




static const char usbip_install_usage_string[] =
"usbip install <args>\n"
"    -i, --install	install or reinstall usbip VHCI driver\n";

void usbip_install_usage(void)
{
	printf("usage: %s", usbip_install_usage_string);
}


static int usbip_install_get_inf_path(char* buffer, size_t buffer_size) {
}

static int usbip_install_get_class_id(char* buffer, size_t buffer_size) {
}

static int usbip_install_get_hardware_id(char* buffer, size_t buffer_size)
{
	assert(buffer != NULL);
	assert(buffer_size > 0);

	char inf_path[BUFFER_SIZE] = { 0 };
	HRESULT result = GetModuleFileName(NULL, inf_path, BUFFER_SIZE - 1);
	if (result)
		return result;
	result = PathCchRemoveFileSpec(inf_path, BUFFER_SIZE);
	if (result)
		return result;
	result = PathCchAppend(inf_path, BUFFER_SIZE, DEVICE_DRIVER_INF_FILENAME);
	if (result)
		return result;

	HINF inf_handle = SetupOpenInfFile(inf_path, NULL, INF_STYLE_WIN4, NULL);
	if (inf_handle == INVALID_HANDLE_VALUE) {
		return 1;
	}

	char device_info[BUFFER_SIZE] = { 0 };
	BOOL ok = SetupGetLineText(NULL, inf_handle, "Standard.NTamd64", "USB/IP VHCI", device_info, BUFFER_SIZE, NULL);
	if (!ok) {
		return 1;
	}

	char* separator_pos = strchr(device_info, ',');
	if (separator_pos == NULL) {
		return 1;
	}
	// End of the string
	if (separator_pos - device_info >= BUFFER_SIZE) {
		return 1;
	}

	char* hw_id_ptr = separator_pos + 1;
	strncpy(buffer, hw_id_ptr, buffer_size);

	return 0;
}

static int usbip_install_remove_device(HDEVINFO devinfo_set, char* device_instance_id)
{
	assert(devinfo_set != NULL);
	assert(device_instance_id != NULL);

	SP_DEVINFO_DATA devinfo_data = { 0 };
	devinfo_data.cbSize = sizeof(SP_DEVINFO_DATA);
	const BOOL open_ok = SetupDiOpenDeviceInfo(devinfo_set,
		device_instance_id,
		NULL,
		DIOD_CANCEL_REMOVE,
		&devinfo_data);
	if (!open_ok) {
		return 1;
		//std::cerr << "Cannot open or create" << std::endl;
	}
	const BOOL uninstall_ok = DiUninstallDevice(0,
		devinfo_set,
		&devinfo_data,
		0, FALSE);
	if (!uninstall_ok) {
		return 1;
	}
	return 0;
}


int usbip_install(int argc, char *argv[])
{
	HDEVINFO devinfoset = SetupDiCreateDeviceInfoList(&class_guid, NULL);
	if (devinfoset == INVALID_HANDLE_VALUE) {
		return 1;
	}

	SP_DEVINFO_DATA devinfo_data = { 0 };


	const bool devinfo_ok = SetupDiCreateDeviceInfo(devinfoset,
		L"ROOT\\USBIP_VHCI\\00000",
		&class_guid,
		L"USB/IP VHCI Test",
		NULL,
		0,
		&DeviceInfoData);
	return 0;
}