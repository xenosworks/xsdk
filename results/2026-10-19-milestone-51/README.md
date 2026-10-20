# Milestone 51: saves, profiles and XAM's asynchronous calls (2026-10-19)

docs/scope/51-saves-and-profiles.md. Step 1's probe ran on the console unattended, and steps 2 to 4's unattended
checks pass there; save_check and the probe's person cases wait for the user.

- `probe_xam.log`: `tests/probe_xam.c`'s unattended cases, no profile signed in, auto sign-in off, a USB stick in,
  title ID 0x5853444B.
- `probe_xam_t2.log`: the same ELF under title ID 0x5853444C, run after it in the same folder.

## On the console

docs/HARDWARE.md, "XAM's saves, profiles and asynchronous calls, milestone 51", says what the probe found. Steps 2
to 4, built at `ba49e3d` with the console's facts alone, ran on 2026-10-19 with nobody signed in:
- **xsdk_test** (`xsdk_test.log`): 476 passed, 2 failed, the controller's checks with no pad connected; all eight of
  its `xam:` checks passed.
- **Its breaks** (`xsdk_test_noflush.log`, `xsdk_test_nowait.log`): each failed only the check it names, below,
  and the controller's two.
- **cmake_check** (`cmake_check.log`), built through `xsdk_add_xex(cmake_check TITLE_ID 0x5853444F NAME "xsdk cmake
  check")`: 5 of 5, `cmake: the .xex holds the title name xsdk_add_xex gave it` among them. Packed again with no name
  (`cmake_check_noname.log`), that check failed.
- **The name and saves:** `probe_xam.elf` packed under title ID 0x58534450 with a name (`probe_xam_name.log`) made
  its saves under its own folder, their headers' title name empty, then deleted them (`probe_xam_name_delete.log`).

With the user at the console on 2026-10-20, a profile signed in:
- **The probe's person cases** (`probe_xam_person.log`): sign-in, the profile, its save and its settings, and the
  first keyboard; its second keyboard and message box came straight after the first and were refused with 5. Run
  again waiting for XAM's UI to close first (`probe_xam_dialogs.log`), the first keyboard stayed open about 6 minutes
  and the rest were refused as XAM's UI was still up, a message box under the Guide among them; the keyboard's cancel
  result is still unknown. `probe_xam_signed_in.log` is the unattended cases with the profile signed in.
- **save_check:** 12 of 12 for each title ID, then 8 of 8 each after a reboot (`save_check_*.log`). The dashboard
  lists both as unknown games.

## What xsdk_test checks

`tests/test_xam.c`, each check under a second:

| Check | On the console | Under Xenia |
|---|---|---|
| `xam: nobody is signed in` | pass | not run: user 0 is always signed in (xenia:src/xenia/kernel/xam/user_profile.h:229), so it takes the gamertag check |
| `xam: with nobody signed in, user 0's XUID and name fail as on the console` | pass | not run, as above |
| `xam: the signed-in user's gamertag reads` | with a profile signed in, in place of the two above | pass (xam_user.cc:105-120) |
| `xam: the device enumerator finds the hard disk` | pass | pass: a dummy hard disk, device 1 (xam_content_device.cc:35-40) |
| `xam: an overlapped call completes through its event` | pass | pass, even broken (below) |
| `xam: a save no profile owns mounts, takes a file and unmounts` | pass | pass: packages are host folders (content_manager.cc:129-154) |
| `xam: a save is flushed before it's unmounted` | pass | pass: the flush is a stub (xam_content.cc:267-277) |
| `xam: a save mounted again opens the package and reads back what was written` | pass | pass |
| `xam: the save is deleted` | pass | pass (content_manager.cc:231-245) |

With a profile signed in, as after the person run's auto sign-in, the first two give way to the gamertag check.

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
