# Milestone 54: what porters hit first, files and the system

Agreed on 2026-10-20, with drive names in src/path.c, flushing and closing on Xbox Home, and letterboxing in this
milestone.

## Why

docs/PARITY.md ranks these after the log over the network (milestone 53): calls a ported game makes in its first
minutes, each a kernel or XAM export Xenia implements, so their behaviour is documented.
- **It doesn't link:** `pwrite`, `ftruncate`, `fsync`, `fdatasync`, `statvfs` and `fstatvfs` are missing from xsdk
  (src/file.c has `pread` alone), so code using any of them stops at the link.
- **Only `game:` has a name.** A path to the hard disk, a USB drive or the DVD is a device path written out
  (`\Device\Harddisk0\Partition1\...`); `hdd:`, `usb:` and `dvd:` don't resolve (src/path.c).
- **`localtime` is UTC,** whatever time zone the console is set to, and nothing tells a title the console's language.
- **Xbox Home ends a title without `exit`:** no `atexit` handler runs, stdio's buffers are lost, and files are left as
  the kernel finds them.
- **A 4:3 output stretches 1280x720** across the whole picture (src/display.c, `plan_scaler`).

## What it takes

Each step probes on the console first where Xenia can't answer.

1. **The file calls,** in src/file.c and src/path.c:
   - `pwrite`, as `pread` is, through `NtWriteFile` at an offset;
   - `ftruncate`, as `truncate` is, through `NtSetInformationFile`'s end-of-file class;
   - `fsync` and `fdatasync` through `NtFlushBuffersFile` (0xDB), which Xenia takes and does nothing with
     (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:591-600);
   - `statvfs` and `fstatvfs` through `NtQueryVolumeInformationFile` (0xEF), its size class (xboxkrnl_io_info.cc:280-345).
2. **Drive names:** `hdd:`, `usb:` (and `usb1:`, `usb2:` where there are more), `mu:` for a memory unit and `dvd:`,
   resolved by src/path.c to the device paths a console probe confirms. Xenia mounts `\Device\Harddisk0\Partition1`
   and `\Device\Cdrom0` (xenia:src/xenia/emulator.cc:298-353); xsdk's log already falls back to `\Device\Mass0`
   (src/log.c). The probe lists the root of each candidate path, with and without a USB stick and a disc.
3. **The console's settings:**
   - the time zone from `ExGetXConfigSetting` (0x10), its bias and daylight-saving dates (xboxkrnl_xconfig.cc:69-75),
     set at start-up as newlib's time zone, so `localtime` and `mktime` give the console's local time;
   - the language through `XGetLanguage` (XAM 0x3CD) and the country through `XamGetLocaleEx` (XAM 0x5D7), as
     `xsdk_system_language` and `xsdk_system_country`.
4. **Xbox Home:** xsdk registers a title-terminate notification with `ExRegisterTitleTerminateNotification` (0x15),
   which Xenia runs on the terminating thread (xenia:src/xenia/kernel/kernel_state.cc:495-504). The probe settles which
   thread runs it on the console, how long it has, and whether file writes from it complete. Then, on Xbox Home, xsdk
   flushes stdio's streams, closes the log and calls a routine the title registers (`xsdk_on_terminate`).
5. **Letterboxing on 4:3:** where `VdQueryVideoMode` says the output isn't widescreen, the display controller shows
   1280x720 at the output's width with bars above and below, in place of stretching. The probe settles which of the
   scaler's registers place the picture.

Left out: asynchronous and unbuffered reads, `mmap`, the device selector (milestone 51 left it to XAM's own UI),
720p on interlaced outputs (TODO.md), and running `atexit` handlers on Xbox Home, unless step 4's probe shows the
notification has the time and the threads' locks to run them safely.

## Verification

On the console, each check its own line in xsdk_test, each broken on purpose once:
- **Files:** `pwrite` at an offset reads back with `pread`, the file's size unchanged; `ftruncate` shortens and lengthens
  a file; `fsync` returns 0 on a file and fails on a socket; `statvfs` of `game:` gives a block size and free blocks
  that go down by a written file's size.
- **Drives:** `hdd:\` lists the folder xsdk_test runs from under it; `usb:` and `dvd:` list their roots where a stick
  or disc is in, and fail with `ENOENT` where not.
- **Settings:** `localtime` minus `gmtime` equals the console's bias; the language and country are logged and match
  what the dashboard shows.
- **Xbox Home:** a probe title writes through stdio without flushing, then the console agent sends the console to the
  dashboard; after it, the file holds every line and the title's routine ran.
- **Letterboxing:** with the console set to 4:3, the display controller's registers hold the planned bars, and you
  see 1280x720 unstretched once.

vulkan4xenon_test twice and vkbench, unchanged.

## Done when

Each item above works on the console and its check passes, its break failing; docs/GUIDE.md and docs/PARITY.md say
what's there.

## Decided

1. **Drive names in src/path.c:** xsdk maps `hdd:` and the rest itself, with nothing left behind in the kernel, rather
   than `ObCreateSymbolicLink`'s names the whole console sees.
2. **Xbox Home:** stdio flushed and the log closed, and the title's routine called; `atexit` handlers only if the probe
   shows the notification has the time and the threads' locks to run them.
3. **Letterboxing** is in this milestone; you set the console to 4:3 once and look.
