#pragma once

#include <ntddk.h>
#include <ntstrsafe.h>
#include <initguid.h> // required for GUID definitions

#include "basetype.h"
#include "vhci_dbg.h"

#define ENABLE_DEBUG_HELPERS 1
#if ENABLE_DEBUG_HELPERS
#include "debug_helpers.h"
#endif

#define USBIP_VHCI_POOL_TAG (ULONG) 'VhcI'

extern NPAGED_LOOKASIDE_LIST g_lookaside;
