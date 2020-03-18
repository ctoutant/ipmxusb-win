#pragma once

#ifndef IoCompleteRequest
#error dont include this on its own, it needs wdm.h already included
#endif

#define DBGI_FUN(part, fmt, ...) \
    DBGI(part, __FUNCTION__ "@" __LINE__ fmt, ## __VA_ARGS__)

#undef IoCompleteRequest
#define IoCompleteRequest(a,b) \
    DBGI_FUN(DBG_GENERAL, "IoCompleteRequest %p boost? %u", a, b); \
    IofCompleteRequest(a, b)
     
