#ifndef XSDK_TERMINATE_PRIVATE_H
#define XSDK_TERMINATE_PRIVATE_H

// Takes xsdk_on_terminate's registration out, if it's in, before the title exits by itself.
void xsdk_terminate_close(void);

#endif
