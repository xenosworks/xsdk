#ifndef XSDK_TIME_PRIVATE_H
#define XSDK_TIME_PRIVATE_H

#include <stdint.h>
#include <time.h>

// NT time counts 100 ns intervals from 1601; POSIX time counts from 1970.
struct timespec xsdk_time_from_nt(uint64_t nt_time);
// 0 for a time NT can't hold, or a tv_nsec out of range.
uint64_t xsdk_time_to_nt(const struct timespec *time);

#endif
