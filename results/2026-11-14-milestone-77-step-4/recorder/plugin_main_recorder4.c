void plugin_main(HANDLE module)
{
	// Boot recorder 4 (results/2026-11-14-milestone-77-step-4, run 25): recorder 3 plus 160 KB of read-only data the
	// image must carry, so the image is the production module's size with the recorder's behaviour. Recorder 3: recorder 2 with the module names read as the
	// UNICODE_STRINGs they are and skipped while unset (recorder 2's own first walk is its likely death), without the
	// file caches (idle for 77 s in recorder 1), plus this thread's stack pointer. Logs at once, as the hung variants
	// did, then samples every 250 ms for ten minutes, a line on change and every 5 s regardless. 0x8010BFEC is the
	// word xeBuild's stub clears once it has loaded launch.xex, on which its XexLoadExecutable patch spins (so a
	// nonzero word for good = DashLaunch's DllMain never returned); 0x801A5DD0 is set when the title's thread starts;
	// 0x80197700 heads the loader's module list, so the modules after launch.xex say whether DashLaunch's
	// firstRunTasks ran (its plugins) and whether the executable (lhelper.xex, the dash) ever loaded.
	// the production main's helpers, unused here, kept from -Werror
	(void)device_names, (void)file_exists, (void)poll, (void)remove_file, (void)TRACE_PATH, (void)UNLOAD_PATH, (void)volumes;
	static const volatile uint8_t pad[160 * 1024] = { 1 };
	make_log_folder();
	xsdk_plugin_log_open(LOG_PATH);
	RtlInitializeCriticalSection(&lock);
	const LDR_DATA_TABLE_ENTRY *const entry = (const LDR_DATA_TABLE_ENTRY *)(uintptr_t)module;
	uint32_t stack;
	__asm__ volatile("mr %0,1" : "=r"(stack));
	xsdk_plugin_log("exfat: recorder 4 loaded, kernel %u.%u.%u.%u, image 0x%08X-0x%08X, stack 0x%08X, pad %u",
		(unsigned)XboxKrnlVersion->major, (unsigned)XboxKrnlVersion->minor, (unsigned)XboxKrnlVersion->build,
		(unsigned)XboxKrnlVersion->qfe, (unsigned)entry->image_base, (unsigned)(entry->image_base + entry->image_size),
		(unsigned)stack, (unsigned)(pad[0] + pad[sizeof pad - 1]));
	struct {
		uint32_t exe, started, launch;
		char modules[240];
	} now, last;
	memset(&last, 0xFF, sizeof last);
	uint64_t start;
	KeQuerySystemTime(&start);
	for (unsigned sample = 0; sample < 10 * 60 * 4; sample++) {
		memset(&now, 0, sizeof now);
		size_t used = 0;
		unsigned walked = 0;
		for (uint32_t node = *(volatile uint32_t *)(uintptr_t)0x80197700;
			 node != 0x80197700 && node >= 0x80000000 && node < 0xA0000000 && walked < 64;
			 node = *(volatile uint32_t *)(uintptr_t)node, walked++) {
			const LDR_DATA_TABLE_ENTRY *const m = (const LDR_DATA_TABLE_ENTRY *)(uintptr_t)node;
			const uint32_t name = m->base_dll_name[1];
			const size_t length = (m->base_dll_name[0] >> 16) / 2;
			if (used + 2 + length >= sizeof now.modules)
				break;
			now.modules[used++] = ' ';
			if (name < 0x80000000 || name >= 0xA0000000 || length == 0 || length > 64) {
				now.modules[used++] = '?';
				continue;
			}
			const uint16_t *const units = (const uint16_t *)(uintptr_t)name;
			for (size_t i = 0; i < length; i++)
				now.modules[used++] = (char)(units[i] >= 0x20 && units[i] < 0x7F ? units[i] : '?');
		}
		now.exe = (uint32_t)(uintptr_t)*XexExecutableModuleHandle;
		now.started = *(volatile uint32_t *)(uintptr_t)0x801A5DD0;
		now.launch = *(volatile uint32_t *)(uintptr_t)0x8010BFEC;
		if (memcmp(&now, &last, sizeof now) != 0 || sample % 20 == 0) {
			uint64_t time;
			KeQuerySystemTime(&time);
			xsdk_plugin_log("exfat: +%u ms exe %08X started %u launch %08X; modules:%s", (unsigned)((time - start) / 10000),
				(unsigned)now.exe, (unsigned)now.started, (unsigned)now.launch, now.modules);
			last = now;
		}
		sleep_ms(250);
	}
}
