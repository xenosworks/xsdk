#ifndef XSDK_SEMAPHORE_H
#define XSDK_SEMAPHORE_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEM_VALUE_MAX INT32_MAX

typedef struct {
	uint32_t handle;
	int32_t value;
} sem_t;

// Unnamed, process-local semaphores; destroy only after all users have stopped.
int sem_init(sem_t *semaphore, int shared, unsigned value);
int sem_destroy(sem_t *semaphore);
int sem_wait(sem_t *semaphore);
int sem_trywait(sem_t *semaphore);
int sem_timedwait(sem_t *semaphore, const struct timespec *deadline);
int sem_clockwait(sem_t *semaphore, clockid_t clock, const struct timespec *deadline);
int sem_post(sem_t *semaphore);
int sem_getvalue(sem_t *semaphore, int *value);

#ifdef __cplusplus
}
#endif
#endif
