/*
 * Bring-up trace output of the v9 Job Manager port ([PANVK_DEBUG_*] lines:
 * descriptor dumps, job status, kbase allocations). Off by default; set
 * PANVK_V9_TRACE=1 to get it on stderr.
 */
#ifndef PAN_V9_TRACE_H
#define PAN_V9_TRACE_H

#include <stdbool.h>
#include <stdio.h>
#include "util/u_debug.h"

static inline bool
pan_v9_trace_enabled(void)
{
   static int on = -1;
   if (on < 0)
      on = debug_get_bool_option("PANVK_V9_TRACE", false);
   return on;
}

#define PAN_V9_TRACE(...)                                                      \
   do {                                                                        \
      if (pan_v9_trace_enabled())                                              \
         fprintf(stderr, __VA_ARGS__);                                         \
   } while (0)

#endif
