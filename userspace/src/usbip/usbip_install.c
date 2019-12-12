
#include "usbip.h"



static const char usbip_install_usage_string[] =
"usbip install <args>\n"
"    -i, --install	install or reinstall usbip VHCI driver\n";

void usbip_install_usage(void)
{
	printf("usage: %s", usbip_install_usage_string);
}




static int usbip_install_remove_device()
{
}


int usbip_install(int argc, char *argv[])
{
	return 0;
}