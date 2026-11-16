#include <stdint.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"

// Milestone 78 step 1 (docs/scope/78-exfat-hdd.md), the sign-in that spins after a profile's creation: the exFAT
// module reads xam's export table where the kernel's loader entry points (the XEX header at the entry's +0x58, the
// security info at the header's +0x10, the table at the security info's +0x160, as sub_8007A790 resolves an import)
// and finds no "H\0\0\0\0HVE" there; this reads the same words from a title, resolves four ordinals through the kernel
// and scans xam's image for the magic. Nothing is written.

enum { SPLASH_MS = 1500, WORDS = 16 };
static const uint32_t ORDINALS[] = { 523, 548, 560, 564 };

int main(void)
{
	xsdk_log_write("probe_xam_exports: kernel %u.%u.%u.%u", XboxKrnlVersion->major, XboxKrnlVersion->minor,
		XboxKrnlVersion->build, XboxKrnlVersion->qfe);
	(void)xsdk_splash_show(SPLASH_MS);

	HANDLE handle;
	if (XexGetModuleHandle("xam.xex", &handle) != 0) {
		xsdk_log_write("RESULT no xam handle");
		return 0;
	}
	const LDR_DATA_TABLE_ENTRY *const xam = (const LDR_DATA_TABLE_ENTRY *)(uintptr_t)handle;
	const uint32_t header = *(const uint32_t *)((const uint8_t *)xam + 0x58);
	const uint32_t security = *(const uint32_t *)(uintptr_t)(header + 0x10);
	const uint32_t table = *(const uint32_t *)(uintptr_t)(security + 0x160);
	xsdk_log_write("RESULT xam %08X-%08X, header %08X, security %08X, export table %08X", (unsigned)xam->image_base,
		(unsigned)(xam->image_base + xam->image_size), (unsigned)header, (unsigned)security, (unsigned)table);
	const uint32_t *const words = (const uint32_t *)(uintptr_t)table;
	for (unsigned i = 0; i < WORDS; i += 4)
		xsdk_log_write("RESULT table +%02X: %08X %08X %08X %08X", i * 4, (unsigned)words[i], (unsigned)words[i + 1],
			(unsigned)words[i + 2], (unsigned)words[i + 3]);
	for (unsigned i = 0; i < sizeof ORDINALS / sizeof ORDINALS[0]; i++) {
		void *address = NULL;
		const NTSTATUS status = XexGetProcedureAddress(handle, ORDINALS[i], &address);
		xsdk_log_write("RESULT ordinal %u: %08X, status %08X", (unsigned)ORDINALS[i], (unsigned)(uintptr_t)address,
			(unsigned)status);
	}
	unsigned found = 0;
	for (uint32_t a = xam->image_base; a + 12 <= xam->image_base + xam->image_size; a += 4) {
		if ((a & 0xFFFF) == 0 && !MmIsAddressValid((const void *)(uintptr_t)a)) {
			a += 0xFFFC;
			continue;
		}
		const uint32_t *const w = (const uint32_t *)(uintptr_t)a;
		if (w[0] == 0x48000000 && w[1] == 0x00485645 && w[2] == 0x48000000) {
			xsdk_log_write("RESULT magic at %08X", (unsigned)a);
			found++;
		}
	}
	xsdk_log_write("RESULT %u export table magics in xam's image", found);
	return 0;
}
