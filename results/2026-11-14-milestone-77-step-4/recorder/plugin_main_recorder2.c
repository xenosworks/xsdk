void plugin_main(HANDLE module)
{
	// The boot recorder, a diagnostic for results/2026-11-14-milestone-77-step-4: logs at once, as the variants that
	// hung did, then samples what the boot has reached every 250 ms for ten minutes, writing a line when it changes
	// and every 5 s regardless, so a hung boot's log shows the state its own path stopped in. The addresses are the
	// 17559 kernel's: 0x80197700 heads the loader's module list (an entry's first link), 0x801A5DD0 is set when
	// XexStartExecutable starts the title's thread, 0x8010BFEC is the word xeBuild's stub clears once it has loaded
	// launch.xex, on which xeBuild's XexLoadExecutable spins (patches_g2corona.bin, the stub at 0x8010BF40), and the
	// file caches are at 0x801A5620 (title) and 0x801A56C0 (system): the element count at +0x14, the elements at
	// +0x18 (20 bytes each, the low byte of +8 a reference count) and at +0x58 the threads waiting for one.
	make_log_folder();
	xsdk_plugin_log_open(LOG_PATH);
	RtlInitializeCriticalSection(&lock);
	const LDR_DATA_TABLE_ENTRY *const entry = (const LDR_DATA_TABLE_ENTRY *)(uintptr_t)module;
	xsdk_plugin_log("exfat: recorder 2 loaded, kernel %u.%u.%u.%u, image 0x%08X-0x%08X, process type %u",
		(unsigned)XboxKrnlVersion->major, (unsigned)XboxKrnlVersion->minor, (unsigned)XboxKrnlVersion->build,
		(unsigned)XboxKrnlVersion->qfe, (unsigned)entry->image_base, (unsigned)(entry->image_base + entry->image_size),
		(unsigned)KeGetCurrentProcessType());
	struct {
		uint32_t exe, started, launch, count[2], pinned[2], waiters[2];
		char modules[200];
	} now, last;
	memset(&last, 0xFF, sizeof last);
	uint64_t start;
	KeQuerySystemTime(&start);
	for (unsigned sample = 0; sample < 10 * 60 * 4; sample++) {
		memset(&now, 0, sizeof now);
		size_t used = 0;
		for (uint32_t node = *(volatile uint32_t *)(uintptr_t)0x80197700; node != 0x80197700;
			 node = *(volatile uint32_t *)(uintptr_t)node) {
			const LDR_DATA_TABLE_ENTRY *const m = (const LDR_DATA_TABLE_ENTRY *)(uintptr_t)node;
			const char *const name = (const char *)(uintptr_t)m->base_dll_name[1];
			const size_t length = m->base_dll_name[0] >> 16;
			if (used + 1 + length >= sizeof now.modules)
				break;
			now.modules[used++] = ' ';
			memcpy(now.modules + used, name, length);
			used += length;
		}
		now.exe = (uint32_t)(uintptr_t)*XexExecutableModuleHandle;
		now.started = *(volatile uint32_t *)(uintptr_t)0x801A5DD0;
		now.launch = *(volatile uint32_t *)(uintptr_t)0x8010BFEC;
		for (unsigned c = 0; c < 2; c++) {
			const uintptr_t cache = c ? 0x801A56C0 : 0x801A5620;
			now.count[c] = *(volatile uint32_t *)(cache + 0x14);
			now.waiters[c] = *(volatile uint32_t *)(cache + 0x58);
			const uintptr_t elements = *(volatile uint32_t *)(cache + 0x18);
			for (uint32_t i = 0; i < now.count[c]; i++)
				if (*(volatile uint32_t *)(elements + i * 20 + 8) & 0xFF)
					now.pinned[c]++;
		}
		if (memcmp(&now, &last, sizeof now) != 0 || sample % 20 == 0) {
			uint64_t time;
			KeQuerySystemTime(&time);
			const LDR_DATA_TABLE_ENTRY *const exe = (const LDR_DATA_TABLE_ENTRY *)(uintptr_t)now.exe;
			xsdk_plugin_log("exfat: +%u ms exe %08X \"%s\" started %u launch %08X; cache title %u/%u %u waiting, "
							"system %u/%u %u waiting; modules:%s",
				(unsigned)((time - start) / 10000), (unsigned)now.exe,
				exe ? (const char *)(uintptr_t)exe->base_dll_name[1] : "-", (unsigned)now.started,
				(unsigned)now.launch, (unsigned)now.pinned[0], (unsigned)now.count[0], (unsigned)now.waiters[0],
				(unsigned)now.pinned[1], (unsigned)now.count[1], (unsigned)now.waiters[1], now.modules);
			last = now;
		}
		sleep_ms(250);
	}
