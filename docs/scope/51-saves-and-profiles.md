# Milestone 51: saves, profiles and XAM's asynchronous calls

Agreed on 2026-10-19, to run beside milestone 48 and the others agreed that day.

## Why

A port needs to keep its saves where the player expects them, know who's playing, and ask for a name. docs/PARITY.md
marks all of these No:
- save games in content packages;
- choosing a device;
- sign-in state, the profile and its settings;
- the on-screen keyboard and message boxes;
- XAM's asynchronous calls, which every one of them takes.

Today a title can only write files beside its `.xex` (`game:`). Every xsdk title also has one title ID, 0x5853444B,
tools/elf2xex.py's default, which `xsdk_add_xex` never changes, so if XAM keys content by title, all of them would
share one place.

What Xenia shows, from its sources:
- **The asynchronous model** (xenia:src/xenia/kernel/kernel_state.cc:696-783, xam/xam_msg.cc:113-136):
  - An `XOVERLAPPED` is 0x1C bytes. A call returns `ERROR_IO_PENDING`, then writes its result and length and sets the
    overlapped's event.
  - `XamGetOverlappedResult` (XAM 0x1FB) reads the result.
  - Completion routines arrive as APCs, which need alertable waits xsdk doesn't have, so xsdk uses events.
- **Content** (xam/xam_content.cc, content_manager.cc):
  - `XamContentCreateEx` (0x259) mounts a package under a root name such as `save:`.
  - xsdk's paths already send any drive name through the kernel's DOS devices (src/path.c), so `open("save:/...")`
    should need nothing new.
  - Enumeration, delete, flush, device data and the device selector are 0x25A to 0x267, 0x250 and 0x2CB.
- **Profiles** (xam/xam_user.cc): sign-in state, XUID, gamertag, sign-in info and profile settings, 0x20A to 0x227, and
  the sign-in UI, 0x2BC.
- **Dialogs** (xam/xam_ui.cc): the keyboard (0x2C1) and the message box (0x2CA).
  - Each sends XN_SYS_UI as it opens and closes.
  - The Guide's dialogs show only while the title keeps calling `VdSwap` (docs/HARDWARE.md, "The Guide shows through
    VdSwap"), so a helper that blocks the presenting thread would deadlock.

Xenia fakes most of this. It reports user 0 signed in always, has a fixed fake device, and stores content by title
ID alone. So the design rests on a console probe first.

## What it takes

1. **A probe of XAM on the console,** hang-safe, its cases in a start file. It settles:
   - the third argument of `XamGetOverlappedResult`;
   - whether content can be created with no profile signed in (users 0xFE, 0xFF and 0) for 0x5853444B and for
     another title ID, and where it lands on the hard disk;
   - what the dashboard shows of it;
   - the device enumerator's IDs for the hard disk and a USB drive;
   - closing a package with a file still open;
   - whether the UI calls take a NULL overlapped, and whether they're refused while the Guide is open;
   - the keyboard's cancel result;
   - `XamUserGetXUID`'s type mask.

   The parts that need a profile run once with the user signed in, and then from a profile set to sign in
   automatically, so later runs need nobody.
2. **The imports and an `xsdk/xam.h` of a few helpers:**
   - XAM's exports above, with `XOVERLAPPED`, `XCONTENT_DATA` and `X_USER_SIGNIN_INFO` in `kernel.h`;
   - `xsdk_xam_wait`: start-then-poll or block on an overlapped's event;
   - `xsdk_save_mount` and `xsdk_save_unmount`: a content package as a drive name, opened or created;
   - the signed-in user, with their gamertag;
   - `xsdk_system_ui_open`: whether the Guide or a dialog is up, from the XN_SYS_UI notifications, so a title knows
     when to pause.

   Raw imports cover the rest: enumeration, profile settings, the device selector.
3. **A title's own title ID and name in its `.xex`.** `xsdk_add_xex(... TITLE_ID ... NAME ...)` passes them to
   tools/elf2xex.py, which takes `--title-id` already, and a name the dashboard can show. This step is needed only if
   the probe shows content kept by title ID, so titles stop sharing saves.
4. **The keyboard and the message box,** each started on one call and polled on the frame loop, never blocking it.

Left out:
- achievements, Live, avatars and System Link (Out or low in docs/PARITY.md);
- the XMsg request calls, which only achievements need;
- a profile-settings wrapper past the raw call.

## Verification

- **xsdk_test, unattended, no profile signed in:**
  - the sign-in state reads none, and the XUID and name calls fail as the probe found;
  - the device enumerator finds the hard disk;
  - an overlapped call completes through its event;
  - a save, if the probe shows one can be made with no profile, is written, closed, mounted again and read back.
- **A `save_check` title,** like `guide_check`, run with a profile signed in. It reads the gamertag and saves to a
  package, then reads the package back after a reboot. It opens the keyboard and a message box, and the user types
  and presses, once.
- **Breaks:** a save that never flushes before unmounting, and a wait that ignores the event, each failing the
  checks they name.

## Done when

- A title saves into a content package on the hard disk and reads it back after a reboot, with a profile signed in,
  and with none if the probe allows it.
- It knows the signed-in user's gamertag, takes typed text from the keyboard and a button from a message box, and
  knows when the Guide or a dialog is open.
- Titles with their own title IDs keep their saves apart, if step 3 was needed.
- xsdk_test passes on the console, and docs/PARITY.md's rows say Yes or Partial as they stand.
