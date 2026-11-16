#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"

// Milestone 78 step 1 (docs/scope/78-exfat-hdd.md): what xam holds about the storage devices, and the kernel's record
// for the content partition. xam 2.0.17559.0 keeps twenty device entries of 160 bytes at 0x81a9cb30: the device name
// at 0, the device id at 120, the type at 124 (1 the hard drive, 16 a stick, 2 a memory unit) and the state at 128
// (3 listed, 4 unformatted, 0x8167e5b8). The kernel's BuiltInMuMmc driver announces its storage to xam with the record
// at 0x801a608c (sub_8008F3F8 calls 0x8012bca8 with it; a record is flink, blink, the name, the action, the type;
// empty on a console without that storage), and the exFAT module announces the partition again after its mount
// (plugins/exfat/exfat_module.c, announce_partition). Nothing is written. For results/2026-11-15-milestone-78-step-1,
// runs 11 and 12 and the sign-in checks.

enum { SPLASH_MS = 2000, ENTRIES = 20, ENTRY_BYTES = 160, NAME_BYTES = 64, ID = 120, TYPE = 124, STATE = 128 };
enum { RECORD_WORDS = 6, BLOCK_BYTES = 0x120 };

static const uint8_t *const XAM_DEVICES = (const uint8_t *)(uintptr_t)0x81A9CB30;
static const uint32_t *const KERNEL_RECORD = (const uint32_t *)(uintptr_t)0x801A608C;
static const uint8_t *const KERNEL_HDD_BLOCK = (const uint8_t *)(uintptr_t)0x801A5FA0;
static const uint32_t *const KERNEL_LIST_HEAD = (const uint32_t *)(uintptr_t)0x80172A20;

static uint32_t be32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}

// A kernel pointer that may be read as a string.
static const char *kernel_string(uint32_t pointer)
{
	return pointer >= 0x80000000u && pointer < 0x90000000u ? (const char *)(uintptr_t)pointer : "";
}

static void dump(const char *what, const uint8_t *bytes, unsigned count)
{
	for (unsigned start = 0; start < count; start += 16) {
		char line[16 * 3 + 1] = "";
		for (unsigned i = start; i < start + 16; i++)
			snprintf(line + strlen(line), sizeof(line) - strlen(line), "%02X ", bytes[i]);
		xsdk_log_write("%s +%03X: %s", what, start, line);
	}
}

int main(void)
{
	xsdk_log_write("probe_storage_state: kernel %u.%u.%u.%u", XboxKrnlVersion->major, XboxKrnlVersion->minor,
		XboxKrnlVersion->build, XboxKrnlVersion->qfe);
	(void)xsdk_splash_show(SPLASH_MS);

	xsdk_log_write("RESULT kernel record 0x801A608C: %08X %08X %08X %08X %08X %08X, name \"%s\"",
		(unsigned)KERNEL_RECORD[0], (unsigned)KERNEL_RECORD[1], (unsigned)KERNEL_RECORD[2], (unsigned)KERNEL_RECORD[3],
		(unsigned)KERNEL_RECORD[4], (unsigned)KERNEL_RECORD[5], kernel_string(KERNEL_RECORD[3]));
	xsdk_log_write("RESULT kernel notification list head: %08X %08X", (unsigned)KERNEL_LIST_HEAD[0],
		(unsigned)KERNEL_LIST_HEAD[1]);
	dump("hdd block", KERNEL_HDD_BLOCK, BLOCK_BYTES);

	for (unsigned i = 0; i < ENTRIES; i++) {
		const uint8_t *const entry = XAM_DEVICES + i * ENTRY_BYTES;
		const uint32_t type = be32(entry + TYPE);
		const uint32_t state = be32(entry + STATE);
		if (!entry[0] && !type && !state)
			continue;
		char name[NAME_BYTES + 1];
		memcpy(name, entry, NAME_BYTES);
		name[NAME_BYTES] = 0;
		xsdk_log_write("RESULT xam entry %u: \"%s\" id %u type %u state %u", i, name, (unsigned)be32(entry + ID),
			(unsigned)type, (unsigned)state);
	}
	return 0;
}
