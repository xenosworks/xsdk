#ifndef XSDK_TERMINATE_H
#define XSDK_TERMINATE_H

#ifdef __cplusplus
extern "C" {
#endif

// Has routine run when Xbox Home ends the title, on a thread of the console's with the title frozen; a later call
// replaces it. What the routine gets and may do: docs/GUIDE.md, "A title's life".
void xsdk_on_terminate(void (*routine)(void));

#ifdef __cplusplus
}
#endif

#endif
