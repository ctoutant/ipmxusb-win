#pragma once

#ifndef IoCompleteRequest
#error dont include this on its own, it needs wdm.h already included
#endif

#define DBGI_FUN(part, fmt, ...) \
    DBGI(part, __FUNCTION__ "@" __LINE__ fmt, ## __VA_ARGS__)

VOID IoCompleteRequest_debug(
    _In_ PIRP Irp,
    _In_ CCHAR PriorityBoost
)
{
    DBGI(DBG_GENERAL, "IoCompleteRequest");
    IofCompleteRequest(Irp, PriorityBoost);
}


#undef IoCompleteRequest
#define IoCompleteRequest(a,b) \
    IoCompleteRequest_debug(a, b)
        
