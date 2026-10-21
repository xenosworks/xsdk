# Milestone 54: what porters hit first, files and the system

docs/scope/54-porters-files-and-system.md; docs/HARDWARE.md, "Drives, time zones and Xbox Home, milestone 54", and
the letterboxing section after it. Run on the console through the console agent on 2026-10-20; the 4:3 picture looked
at by the user through OBS.

## What was built

- **File calls:** `pwrite`, `ftruncate`, `fsync`, `fdatasync`, `statvfs` and `fstatvfs` (src/file.c, src/path.c,
  include/sys/statvfs.h). The hard disk's file system brings a file's old bytes back when its end moves out
  (`step2_xsdk_test.log`), and into the gap a write past the end leaves (`final_xsdk_test.log`), so `truncate`,
  `ftruncate` and a write past the end write the zeros themselves.
- **Drive names:** `hdd:`, `usb:`/`usb0:` to `usb2:`, `mu:`/`mu0:`, `mu1:` and `dvd:` (src/path.c), from the device
  paths `probe_drives.log` found, each root opened with its trailing backslash.
- **The console's settings:** the time zone from XAM's `GetTimeZoneInformation` as newlib's TZ, so `localtime` is the
  console's local time; `xsdk_system_language` and `xsdk_system_country` (include/xsdk/system.h).
- **Xbox Home:** `xsdk_on_terminate` (include/xsdk/terminate.h), its routine run on a system thread with the title
  frozen, the registration always taken out before it returns.
- **Letterboxing on 4:3:** 1280x960 front buffers with the frame at rows 128 to 847 between black bars, the scaler
  planned for the whole buffer; `xsdk_display_force_letterbox` forces it for tests.

Two choices changed on the way, both the user's (the scope's Decided section): Xbox Home is the safe hook alone, as
the title's handles are already closed when its routine runs; and letterboxing draws its bars, as radeonhd's overscan
registers read back 0 on Xenos (`xsdk_test_overscan.log`) and the kernel's own scaler builder ended the title
(`probe_letterbox_kernel.log`).

## On the console

- **The final set** (`final/`), built clean at d373636's code: xsdk_test 549 of 550 (`final/xsdk_test.log`), the
  one failure the gap check that found the gap; every break failed the lines local/m54/final/BREAKS.txt names for it,
  besides that check (`final/x_*.log`); terminate_check passed its two lines and the console went on to the dashboard,
  its break failing both (`final/terminate_check*.log`); cmake_check 6 of 6; vulkan4xenon_test 517 of 517 twice;
  vkbench 17 of 17.
- **The gap fix** (b899d25): xsdk_test passes "pwrite: past the end, the gap before it reads zeros"
  (`final2_xsdk_test.log`, 548 of 550, the controller's two with the pad idle), and the `write_gap` break fails it
  alone (`final2_xsdk_test_write_gap.log`).
- **The time zone** with the console set to US Eastern (`step3_xsdk_test_eastern.log`): bias 300, EST and EDT,
  daylight from the second Sunday of March and standard from the first of November, TZ
  `<EST>5:00<EDT>4:00,M3.2.0/2:00:00,M11.1.0/2:00:00`, 4 h behind UTC; the sign break failing that check
  (`step3_xsdk_test_tz_sign_eastern.log`). With no zone set, both the console settings and XAM gave zeros
  (`step2_xsdk_test.log`).
- **Xbox Home's probe:** the routine on a system thread, every title thread frozen, the title's handles closed and
  `game:` gone, 2 s given (`probe_terminate_registered_late.txt`); a registration left in place froze the console as
  the next title started, one taken out let it go on (`probe_terminate_unregistered_late.txt`).
- **Letterboxing:** xsdk_test's three letterbox lines (`step5_xsdk_test.log`); on a 4:3 480p output the probe's fill
  showed 1280x720 as an 800x450 picture between bars of about 82 and 68 lines in an 800x600 capture, the circle
  round and the squares square (`letterbox_4x3_480p.png`). Holding the display on one fixed surface without VdSwap
  hung the console (`probe_letterbox_fill.log`, `fill_16x9_1080p_fixed_surface.png`).

## Not done

- Asynchronous and unbuffered reads, `mmap` (milestone 55), the device selector, 720p on interlaced outputs, and a
  pillarbox.
- A write after the file was shortened by path or through another handle can still leave a gap: only `pwrite` checks
  the size every time (src/file.c).
- The splash shows its 1280x720 logo stretched for its 2 s on a 4:3 screen, as it bypasses the present.
