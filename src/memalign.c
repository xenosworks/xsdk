#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <stdlib.h>

// On newlib's memalign, whose memory free takes back.
int posix_memalign(void **memory, size_t alignment, size_t size_bytes)
{
	// POSIX asks for a power of two that's a multiple of a pointer's size.
	if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
		return EINVAL;
	void *const allocated = memalign(alignment, size_bytes);
	if (!allocated)
		return ENOMEM;
	*memory = allocated;
	return 0;
}
