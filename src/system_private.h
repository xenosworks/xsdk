#ifndef XSDK_SYSTEM_PRIVATE_H
#define XSDK_SYSTEM_PRIVATE_H

// Makes the console's time zone newlib's, as TZ; without one it can read, the time stays UTC.
void xsdk_system_set_time_zone(void);

#endif
