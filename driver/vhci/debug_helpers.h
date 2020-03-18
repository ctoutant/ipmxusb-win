#pragma once

#ifndef IoCompleteRequest
#error dont include this on its own, it needs wdm.h already included
#endif

#define TO_STR(s) _TO_STR(s)
#define _TO_STR(s) #s

#define DBGI_FUN(part, fmt, ...) \
    DBGI(part, __FILE__ "@" TO_STR(__LINE__) "" fmt, ## __VA_ARGS__)

#undef IoCompleteRequest
#define IoCompleteRequest(a,b) \
    DBGI_FUN(DBG_GENERAL, "IoCompleteRequest %p boost? %u", a, b); \
    IofCompleteRequest(a, b)
     
