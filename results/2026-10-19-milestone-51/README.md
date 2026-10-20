# Milestone 51: saves, profiles and XAM's asynchronous calls (2026-10-19)

docs/scope/51-saves-and-profiles.md. Step 1's probe ran on the console; steps 2 to 4 are built and wait for their
console runs.

- `probe_xam.log`: `tests/probe_xam.c`'s unattended cases, no profile signed in, auto sign-in off, a USB stick in,
  title ID 0x5853444B.
- `probe_xam_t2.log`: the same ELF under title ID 0x5853444C, run after it in the same folder.

## Proposed for docs/HARDWARE.md

### XAM's saves, profiles and asynchronous calls, milestone 51's probe (2026-10-19)

`tests/probe_xam.c` ran its unattended cases with nobody signed in, presenting the bar from a thread of its own
throughout (logs: `results/2026-10-19-milestone-51/`, `probe_xam.log` and, under a second title ID,
`probe_xam_t2.log`).

- **An overlapped call** returns 0x3E5, `ERROR_IO_PENDING`, with the overlapped's result already 0x3E5 and its context
  0x000106A0. When done, XAM writes the result, length and extended error, sets the event if there is one, and puts
  the context back to 0. Without an event the call completes just the same.
- **`XamGetOverlappedResult` (XAM 0x1FB)** with its third argument 0, straight after the call: 0x3E4,
  `ERROR_IO_INCOMPLETE`, in under a microsecond, the length left alone. Once the call is done, 0 and 1 both give its
  result and length, with an event or without one; 1 straight after the call, on a thread of its own, returned once
  the call was done.
- **Errors come in the overlapped:** `XamContentClose` of a root not mounted completed with 0x65B and extended error
  0x80070002, `XamContentGetDeviceState` of device 0x7F with 0x65B and 0x8007048F. With no overlapped,
  `XamContentGetDeviceState` returned 0 at once.
- **With nobody signed in,** `XamUserGetSigninState` gives 0 for users 0 to 3; `XamUserGetXUID` for user 0 gives
  0x80070525 with type masks 1, 2, 4 and 7, the XUID set to 0; `XamUserGetName` gives 0x525, writing only a zero
  first byte; `XamUserGetSigninInfo` gives 0x80070525 with flags 0 to 2, the 40 bytes zero but byte 0x17, 0xFE.
- **The device enumerator** for saved games, 8 devices, asked for 0x280 bytes and gave the hard disk, device 1, type
  1, "Hard Drive", 154,888,372,224 bytes, and the USB stick, device 3, type 0x10, "USB Storage Device", in one pass;
  the next pass gave 0x103. `XamContentGetDeviceData`, `XamContentGetDeviceName` and `XamContentGetDeviceState` agreed.
- **Saves need no profile for users 0xFE and 0xFF.** `XamContentCreateEx` on device 1, open always, made each package
  (disposition 1) in 32 to 98 ms; `fopen("save:\probe.dat")` wrote 26 bytes, `XamContentFlush` took 49 to 93 ms,
  `XamContentClose` 0 to 2; open existing found it again (disposition 2) at once, and it read back as written. For
  user 0 with nobody signed in it completed with 0x65B, extended error 0x80070525, and its content enumerator
  returned 0x525. The enumerators for 0xFE and 0xFF each list both packages, with the display names given.
- **Each title ID has its own saves:** the packages are files of 61,440 bytes at
  `\Device\Harddisk0\Partition1\Content\0000000000000000\<title ID>\00000001\<file name>`. Under 0x5853444C the same
  names made new packages beside 0x5853444B's, and its enumerators listed only its own.
- **A package closed with a file open in it:** the close took 82 ms, 49 under the second title ID, and succeeded;
  the file's handle then failed writes and reads with 0xC000026E and closed fine, and the 7 bytes written before the
  close, with no flush, were there when the package opened again.

## What xsdk_test checks, with nobody signed in

`tests/test_xam.c`, each check under a second:

| Check | On the console | Under Xenia |
|---|---|---|
| `xam: nobody is signed in` | pass | fails: user 0 always signed in (xenia:src/xenia/kernel/xam/user_profile.h:229) |
| `xam: with nobody signed in, user 0's XUID and name fail as on the console` | pass | fails: both succeed (xam_user.cc:29-57, 105-120) |
| `xam: the device enumerator finds the hard disk` | pass | pass: a dummy hard disk, device 1 (xam_content_device.cc:35-40) |
| `xam: an overlapped call completes through its event` | pass | pass, even broken (below) |
| `xam: a save no profile owns mounts, takes a file and unmounts` | pass | pass: packages are host folders (content_manager.cc:129-154) |
| `xam: a save is flushed before it's unmounted` | pass | pass: the flush is a stub (xam_content.cc:267-277) |
| `xam: a save mounted again opens the package and reads back what was written` | pass | pass |
| `xam: the save is deleted` | pass | pass (content_manager.cc:231-245) |

They need nobody signed in: with auto sign-in on, the first two fail.

**Breaks** (`sh tests/breaks/build.sh` in WSL, `build/breaks/xsdk_test_<break>.xex`):
- `xsdk_test_noflush.xex`, unmount with no flush: fails `xam: a save is flushed before it's unmounted` and nothing
  else, as a close alone keeps what was written.
- `xsdk_test_nowait.xex`, a wait that ignores the event: fails `xam: an overlapped call completes through its event`
  and nothing else on the console. Xenia completes `XamContentGetDeviceState` before it returns
  (xam_content_device.cc:91-94), so it passes there.

## save_check, with a person and a profile

`build/save_check.xex` (title ID 0x5853444D, "xsdk save_check") and `build/save_check_t2.xex` (0x5853444E,
"xsdk save_check 2"), the same ELF. Each keeps a marker beside its log, `save_check.saved` or `save_check_t2.saved`,
which says which launch is next. The bar moves throughout; the title ends only on Back with XAM's UI closed.

1. Sign a profile in. Launch `save_check.xex`. It logs the gamertag and saves. When the keyboard shows, type a word
   and accept it; when the message box shows, press either button. The log says when it's done: press Back.
2. Launch `save_check_t2.xex` and do the same. Its `save_check: the save was made new` passing says it didn't find the
   first title's save.
3. Look at the hard disk's `Content` folder over FTP: a `save_check` file in a `5853444D\00000001` folder and another in
   `5853444E\00000001`, under the profile's folder or `0000000000000000`. The dashboard's storage view shows whether
   it names them "xsdk save_check" and "xsdk save_check 2".
4. Reboot, sign the profile in, launch each again: it reads its save back and deletes it and its marker; press Back.

Its checks: `save_check: GPU, scene and present open`, `save_check: a profile is signed in and its gamertag read`;
on the first launch `save_check: the save was made new`, `save_check: the save written`, `save_check: the save flushed
and unmounted`, `save_check: the marker written for the next launch`, `save_check: the keyboard closed and its text was
read`, `save_check: XAM's UI showed open while the keyboard was`, `save_check: the message box closed on a button`,
`save_check: XAM's UI showed open while the message box was`; on the second `save_check: the last launch's save is
there`, `save_check: it reads back as the last launch wrote it`, `save_check: the save flushed and unmounted`,
`save_check: the save and the marker deleted`; then `save_check: every frame queued in time` and
`save_check: GPU close`. The keyboard's line logs its result, extended error, length and text as XAM gave them.
