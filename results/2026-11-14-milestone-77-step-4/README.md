# Milestone 77, step 4: the kernel loads the module from the NAND

2026-11-14. The patch of docs/scope/77-exfat.md, step 4, built and checked against the 17559 kernel image; the NAND
build and the flash are the user's (xeBuild in J-Runner 3.3.0r4). Nothing here has booted yet.

## What was built

- `plugins/exfat/kernel_patch.S`: the stub, 13 instructions and the path string. ExpSystemInitThread's
  `bl sub_800C8260` at 0x800613DC becomes `bl stub` (until run 29 it was the `bl ExpCheckSystemUpdate` at
  0x80061388, before xam and launch.xex; the runs below say why it moved); the stub calls
  `XexLoadImage("\Device\Flash\exfat.xex", 8, 0, NULL)`, the arguments step 3's loader title proved (the loader's entry
  requires the DLL bit 8 and tests no other; the version argument applies only to modules whose header flags ask for
  it, which ours don't), drops the status, so a flash without the file boots as before, and goes on to
  sub_800C8260 with the original return address.
- `tools/exfat_patch.py`: assembles the stub at 0x80163000 with the toolchain's as and ld, checks the kernel image
  (the bl at the boot point, a bl XexLoadImage at 0x800613b0, zeros where the stub goes), and writes xeBuild's patch
  records: address without its top bit, word count, words (xeBuild/about_patches.S). `make exfat_patch` builds
  `build/exfat.bin`, 96 bytes, copied here with the disassembly `kernel_patch.lst`.
- The place in the padding (0x801616c8-0x8016ffff, zeros): xeBuild's XeLL add-ons xl_both, xl_hdd and xl_usb take it
  up to 0x801621e8, so the stub sits at 0x80163000-0x8016304c, clear of them. No other 17559 patch set of xeBuild's
  touches the padding; the kernel records of its sets stop at 0x801621e8.

The records:

```
00163000 00000013 7d8802a6 9181fff8 9421ffe0 3c608016 38633034 38800008 38a00000 38c00000 4bf1a7a1 81810018
                  38210020 7d8803a6 4bf04478 5c446576 6963655c 466c6173 685c6578 6661742e 78657800
00061388 00000001 48101c79
```

## Installing (the user)

1. `build/exfat.bin` → `xeBuild\17559\bin\exfat.bin`.
2. `build/exfat.xex` → `xeBuild\17559\exfat.xex`, and `exfat.xex,` added to the `[flashfs]` list of the ini the build
   uses (as `..\launch.xex,` is listed; a path works too).
3. xeBuild with `-a exfat` next to the console's usual add-ons (about_patches.S: each `-a name` appends bin/name.bin
   to the kernel patches; up to 16). The user's zero-fuse console needs `-a hvFixKeys` too: the first flash of
   2026-11-14 went without it and sat at the Xbox 360 logo. Flash.

Done on 2026-11-14 in the user's J-Runner-with-Extras xeBuild: `17559\exfat.xex` and `17559\bin\exfat.bin` match
the builds (md5 804a7c6a... and 36a91a76...), and `exfat.xex,` closes the `[flashfs]` list of `_devgl.ini`, the ini of
the user's zero-fuse console. `bin\addon.idx`, J-Runner's add-on index, lists eight add-ons and not exfat, so the
`-a exfat` goes to xeBuild by hand.

## The check, per the scope

A boot with the exFAT stick in, no title run, no DashLaunch plugin loaded:

- hdd:\XSDK\exfat\exfat.log gains "exfat: loaded ..." from the boot and "\Device\Mass0: mounted exFAT ..." without
  exfat_load.xex having run (the module's own log; lines before the HDD is up are lost, so the first may be the mount).
- Aurora sees the stick and its files.
- exfat_check.xex passes on that boot (the 157 checks), exfat_load.xex not run.
- A second NAND without `exfat.xex` in the flash file system boots, and the stick is the kernel's raw volume again
  (unrecognized).

## The boots (2026-11-14, the user flashing)

| NAND | stick | result |
|---|---|---|
| `-a exfat`, no hvFixKeys | in | the Xbox 360 logo animates forever (the zero-fuse console needs hvFixKeys) |
| `-a hvFixKeys -a exfat` | in | hangs |
| the same | out | hangs: the mount is not needed for it |
| `poweroff/`: the stub sends the SMC shutdown after XexLoadImage returns | out | the console switches off: the load returns |
| the module's thread sleeps 30 s before its first kernel call | out | Aurora |
| the thread logs "loaded" at once, then sleeps 30 s before its poll loop | out | hangs: the first HDD access is enough |
| the thread waits for a volume on `\Device\Harddisk0\Partition1` before logging | in | hangs |
| the same | out | hangs with the boot animation frozen mid-swirl: a halt, seconds after our access, while our thread sleeps |
| the thread waits for `XexExecutableModuleHandle`, set when xam has loaded an executable (launch.xex, from the init thread) | in | hangs at the Xbox 360 logo |
| `recorder/`: logs at once, then samples the boot (the first recorder, module names from `XexGetModuleHandle`) | out | hangs; its log: launch.xex loaded 1.3 s after the first line, then 77 s of no change: the executable handle 0, the started flag 0, both file caches idle |
| `recorder/exfat2.xex`, the second recorder, left on four minutes | out | the boot animation freezes mid-swirl; its log holds its first line only: the thread wrote "recorder 2 loaded" and never its first sample, so this time our thread died or blocked too, while the first recorder logged for 77 s |
| `gated/exfat.xex`: the thread's whole start (log folder, log, first line, poll) waits for `XexExecutableModuleHandle` | not noted | hangs at the Xbox 360 logo with the animation inside the X running: the kernel is alive, the dash never comes |

So the stub, the load and the image are fine; the module's thread, which the DLL entry starts while
ExpSystemInitThread is still inside XexLoadImage, opens the HDD's content partition before xam has, and the boot's own
path never recovers from that. The module now waits until the kernel has a volume on `\Device\Harddisk0\Partition1`
(a lookup of the device object, which mounts nothing) before its first log line and its first poll. `boot/exfat.log` (the module's log, fetched after the 30 s boot) carries "loaded" and "mounted" lines from the
hung boots and a "dismounted (media gone)" from the stick being pulled while one of them sat hung: the thread and the
HDD kept working through the hang, so what we hold or do blocks xam or the init thread, not us.

Ruled out offline before that: no stock xeBuild patch set touches the stub or the hook (hvFixKeys is two hypervisor
records); no xex in the flash file system overlaps 0x91580000-0x9167B000 (ximecore.xex starts at 0x91680000); xeBuild's
spin until launch.xex is loaded sits in XexLoadExecutable (ordinal 0x198, 0x8007d7e0), not in XexLoadImage (ordinal
0x199, 0x8007d7c0), which both the kernel's xam load and the stub call.

### The recorder (`recorder/`, built after the executable gate hung)

Read offline in the 17559 image meanwhile: the kernel's file cache, one for title threads at 0x801A5620 and one for
system threads at 0x801A56C0, 20-byte elements whose low byte at +8 counts references, waits with no timeout for a
referenced element to be released, both in `FscSetCacheElementCount` (0x8006E920; the shrink at 0x8006E778 waits at
0x8006E844) and in the element allocator (0x8006EC58, waiting at 0x8006ECA8). `XexStartExecutable` (0x8007B0C8) sets
the title cache to the XEX header's 0x20301 value, or 16 elements, as it starts the title's thread, and xam reaches
both functions through the exports. A boot path stuck in one of those waits, our thread running on, would look like
these hangs; a system thread's I/O, ours, uses the system cache.

`recorder/exfat.xex` (md5 1505bc5d..., its start in `plugin_main_recorder.c`) logs at once, as the hung variants did,
then for ten minutes samples every 250 ms the executable and its name, the started flag 0x801A5DD0, the state of
UsbdBootEnumerationDoneEvent (0x8017217C), both caches' element count, pinned count and waiters, and which of xam,
bootanim, launch, hud, ximecore, signin, dash and Aurora are loaded, writing a line when any of it changes and every
5 s regardless. Flashed with the stick out, so the HDD log is the only storage touched; the log is read on the next
NAND that boots.

What the first recorder's boot showed (`boot/exfat.log`, the lines after "recorder loaded"): our thread is a system
thread (process type 2); at its first sample xam.xex is loaded and UsbdBootEnumerationDoneEvent set; 1.3 s later
launch.xex is loaded; then nothing changes for the 77 s the console was left on: XexExecutableModuleHandle stays 0, the
started flag 0x801A5DD0 stays 0, both file caches have no pinned element and no waiter, and bootanim.xex never shows.
So no title was ever loaded, and the file cache is not what waits.

Where that leaves the boot path, read in xeBuild's own 17559 patches (bin/patches_g2corona.bin, the kernel section;
record addresses are the address without its top bit): at 0x800613CC, where the init thread would branch past the
bugcheck for a failed xam load, it calls xeBuild's stub at 0x8010BF40, which loads `\Device\Flash\launch.xex` with
`XexLoadImage(name, 8, 0, NULL)`, as a DLL and as our stub loads the module a second earlier, then clears the word at
0x8010BFEC and goes on to 0x800613DC, after which the init thread only raises its priority and ends. XexLoadExecutable
is patched at 0x8007D7F8 to spin until that word is 0, so no title can load before launch.xex's DllMain, DashLaunch's
start-up, has returned on the init thread. The hang is therefore one of two: DashLaunch's DllMain never returns, or it
does and xam never gets to load the dash. `recorder/plugin_main_recorder2.c` (the second recorder, exfat.xex md5
below) tells them apart: it logs the word at 0x8010BFEC and the names of every module in the loader's list
(0x80197700), so the plugins DashLaunch loads and the dash show up by whatever name.

`recorder/exfat2.xex` is that second recorder, md5 95ead725... Also read meanwhile, from the console on the NAND
without the module: `boot/launch.ini`, the console's DashLaunch configuration. DashLaunch loads three plugins from the
HDD at start, `Hdd:\Plugins\Xbdm.xex`, `Hdd:\Engine.xex` and `Hdd:\Plugins\devagent.xex`; `debugout`, `exchandler`,
`liveblock`, `livestrong`, `nxemini`, `noupdater` and `xhttp` are on, `fatalfreeze` and `fatalreboot` off. Its
`hddtimer = 210` was first read here as a 210 s wait for the HDD, and the second recorder's boot was left on four
minutes for it; DashLaunch's source (below) says it is the interval of the HDD keep-alive task (dll/opts.c,
dll/tasks/hdalive.c), so those minutes proved nothing.

The second recorder's boot left one line, "recorder 2 loaded", and no sample: the walk of the loader's list, or the
log write after it, never came back, while the first recorder had written for 77 s. (Its names would have come out
wrong anyway: `base_dll_name` is a UNICODE_STRING, as XexpSetExecutable's halfword copy at 0x8007AFA0 shows, not the
ANSI one the recorder assumed.) So the hang takes our thread with it on some boots and not others, which reads as a
race between our first HDD access and the boot path, not a fixed sequence.

### DashLaunch's start-up, from its source

Read in the public source (github.com/XeFreedom/DashLaunch at f57eca9, cloned to Z:\dashlaunch-src: dll/launch.c,
dll/hooks/XexHook.c, dll/hooks/except.c, dll/utility.c, dll/opts.c, dll/tasks/hdalive.c), as launch.xex's DllMain is
what runs on the init thread between xeBuild's stub and the spin that holds every title load:

- DllMain (DLL_PROCESS_ATTACH) checks that lhelper.xex is in the flash (a `tmt:` link to `\Device\Flash\`, then
  deleted) and calls `launchStartup`, which only installs hooks and patches: `hookFunctionStart` on the kernel's
  XexpVerifyXexHeaders and XexpLoadImage (found through the branches behind ordinals 420 and 409), on xam's loader
  prepare, XNotifyBroadcast, the launch path, XNetDnsLookup, the privilege, licence and security checks, KeBugCheckEx,
  HalReturnToFirmware and the exception path, plus a few word patches (revoke check, NXE install). `hookFunctionStart`
  writes a four-word jump at the function's start and relinks the displaced words in an eight-word stub; `doSync` is
  `dcbst; sync; isync`, no icbi and no stopping of the other hardware threads. No HDD access and no wait in DllMain, so
  it returns, and xeBuild's spin opens.
- The HDD work comes later, on xam's thread, inside the loader: the XexpLoadImage hook calls `firstRunTasks` when
  xam loads the dash and `g_FirstRun` is set. It waits on UsbdBootEnumerationDoneEvent, sleeps 1 s, makes the
  `dlaunch:` symbolic link (under the system or the user object directory by KeGetCurrentProcessType), reads
  launch.ini from usb, hdd or flash, writes the ini's kernel patches, then `loadPlugins`: each plugin is an
  `XexLoadImage(path, 8, 0, NULL)` on that thread, the plugins' DllMains nested in the loader, the console agent's
  among them, whose thread then writes the HDD. It returns lhelper.xex as the executable to load first, which launches
  the default dash. So DashLaunch's own first HDD access, and the agent's, come after xam's start-up, with the dash
  load already under way.
- `keBugCheckExHook` prints and, with `fatalfreeze` and `fatalreboot` both off as here, calls
  HalReturnToFirmware(HalPowerDownRoutine): a kernel bugcheck after the hook is up switches the console off. The hung
  boots stayed on. One before the hook, during XexLoadImage of our module or of launch.xex, halts the kernel with the
  ring of light pattern, which this console's RGB LEDs don't show. The exception hook writes its dump to the ini's
  Dumpfile, Hdd:\crashlog.txt; the HDD has none, so no title exception was caught in the hangs.
- `dlaunchStartSysModule` creates its thread with EX_CREATE_FLAG_SYSTEM and XSetThreadProcessor(4); the agent's thread
  takes the system flag alone. Ours is the only one pinned at creation (bit 2 in the creation flags' top byte).

Also read in the kernel image: ExpCheckSystemUpdate (0x800674A8), the function our stub goes on to, opens
`\SystemRoot\systemupdate2pre.xex` and `\SystemRoot\oddupd1.xex` to `oddupd3.xex` only (0x800672A0, NtOpenFile on each)
and loads one if present: flash, not the HDD. So between our module's load and firstRunTasks' HDD reads, the boot's own
HDD work is xam's start-up, and that is what our early access collides with.

### xam's storage start-up, from its image

The user's labelled xam database needs an IDA newer than the 9.1 here, so xam 17559 was read from the flash's xam.xex
(xextool basefile at 0x815F0000, its kernel imports named from the kernel's export table; `local/ida/export_labels.py`
and `import_labels.py` carry the user's labels over when they have run the first in their IDA). What xam does with
storage between its load and the dash:

- xam's DLL entry (0x816968A8, on the init thread inside the kernel's XexLoadImage, a second after our module's) runs
  its init chain; 0x8167ED90 starts the content storage thread 0x8167EB60 and registers the kernel's
  DrvSetContentStorageCallback, then xam's init thread 0x816964A8 starts, which waits on UsbdBootEnumerationDoneEvent
  and then, in 0x8167B718, waits with no timeout on the event 0x81A81358.
- The storage thread sets that event only when DrvGetContentStorageNotification has nothing more to give, that is
  once every storage device present at boot has been processed. For each arrival of an HDD-class device (type 2 or
  16) it: reads `<device>\secdata.bin` (0x816BAC18, an NtOpenFile, NtReadFile, NtClose on the device's file system),
  calls **IoDismountVolumeByName(device)**, which in the kernel (0x8006B1D0) runs FatDismountVolume (0x800A2650:
  clears the device's mounted volume under the device object lock, marks every open file dismounted, discards the
  volume's cache elements, IoDeleteDevice), then registers the device (0x8167E918, 0x8167E5B8): 0x8167B610 queries
  the root (`%s\` through NtQueryFullAttributesFile in 0x816E0540, which mounts the file system again) and dismounts
  once more if that fails, 0x8167E400 opens the device for IOCTLs (0x8167DED0) and makes the content device. The
  same sequence runs for a USB stick's arrival, at boot or at a plug.
- 0x816EB760, the system update search over the content devices that precedes the dash load, waits on 0x81A9E0F4,
  which xam's init thread sets after the storage wait. So no executable loads before the storage thread has finished
  its first pass, and a storage thread that never finishes is a boot that never shows the dash.
- The kernel's open path (IopParseDevice 0x8006C0F0) mounts in a loop: while the device has no volume, clear the
  device's 0x20 flag, IopMountDevice (0x8006BFA8: wait on the device's mount lock, FatMountVolume, HfsMountVolume or
  RawMountVolume, set the lock), re-check. An open that finds the 0x20 flag set fails with 0xC004FFF4.

So our thread's early HDD access runs concurrently with xam's dismount-and-remount of the content partition, and the
mount and the poll of a stick with xam's identical pass over `\Device\Mass0`; the boot waits for that pass to end. Which
wait never returns is not pinned down in the code (FatDismountVolume takes the volume's lock exclusively and the file
cache's discard, IopMountDevice the mount lock; our opens hold the first briefly and the last while mounting), but the
sequence is the collision, and it ends when the storage thread's pass does. The console agent's write works every
boot because it comes after it. The kernel-side sign that xam's pass is over: the executable handle,
XexExecutableModuleHandle (ordinal 0x193, 0x801A5DD4), set when xam loads lhelper.xex or the dash through
XexLoadExecutable, which the update search holds back until the storage event. The exec-gate boot above gated only
the log on it, with the stick in and the poll and mount running from the start; the next build gates the thread's
whole start on it.

A comparison that works every boot: the console agent plugin (local/console-agent/agent/agent.c), loaded by DashLaunch,
starts a thread with the same ExCreateThread system flag, and that thread's first acts are a symbolic link to
`\Device\Harddisk0\Partition1` and the opening, renaming and writing of its log there. Two differences to ours: the
plugin runtime pins our thread to hardware thread 2 (src/plugin.c, `PLUGIN_HARDWARE_THREAD`), the agent's takes the
default, and the agent's write comes whenever DashLaunch loads its plugins, which the dead recorder was to time.

`quiet/exfat.xex` is the next run: the production module with `QUIET` set in plugins/exfat/exfat_module.c, so it
makes no HDD access at all (no log folder, no log, no unload or trace polls) and only watches for and mounts sticks;
the runtime's `xsdk_plugin_log` returns at once while no log is open (src/plugin.c), so the loaded and mounted lines
are dropped too. At boot its thread does ObReferenceObjectByName on `\Device\Mass0` to `Mass2` every 500 ms and, on a
stick, the two geometry IOCTLs, sector reads through IoSynchronousFsdRequest, the device's mount lock, a dismount of the
kernel's raw volume and IoCreateDevice. Booted with the stick in, it answers whether that alone passes the boot that the
log line hangs; Aurora seeing the stick's files is the check, as the module writes nothing. Read against xam's
storage start-up (below), both answers fit the same collision, so the build after it is the same either way: the
production module with its thread's whole start, log and poll, held until XexExecutableModuleHandle is set (`gated/`).
The quiet result still says whether the mount alone, racing xam's pass over the stick, passes the boot.

`gated/exfat.xex` (md5 12a8d3ad..., `gate.diff`) is that build: the production module, its thread sleeping in 500 ms
steps until XexExecutableModuleHandle is set, then the log folder, the log, "loaded" and the poll as before. Nothing
else changed; the recorder's KeGetCurrentProcessType import is gone again. Flashed (the 22:32 image) and booted: the
Xbox 360 logo stays, the animation inside the X running, so the kernel is alive, and the dash never comes. Holding the
module's whole start until xam has an executable did not pass the boot, so the collision with xam's storage pass is
not the whole cause. What still differs from the console agent, whose early HDD write passes every boot: our thread's
pin to hardware thread 2, and its creation by the init thread inside XexLoadImage, before xam exists. The next
variants boot from RAM (below), one difference at a time.

## The shadowboot loop (set up after the gated hang)

Every variant so far cost a flash and, hanging, a second one to get the console back. From here the NAND keeps a
plain devgl image (`-a hvFixKeys`, no `-a exfat`) and the variants boot from RAM through hax360's ExShadowBoot
(codeberg.org/hax360/ExShadowBoot, cloned to Z:\exshadowboot-src): a retail title that parks the other five hardware
threads over the kernel's IPI and, through the freeboot backdoor that xeBuild's hypervisor patches carry (syscall 0,
key 0x72627472; the patch at hypervisor offset 0xb510 in the image's patch set), runs a payload in the hypervisor. The
payload decrypts a NAND image's SB with the 1BL key, signature unchecked, sets the shadowboot media flag in its
header, clears memory and enters it from secure SRAM; the SB then takes SC, CD, CE, CF, CG and the patch set from the
image in RAM. The image carries no file system, so xam, the dash and `\Device\Flash\exfat.xex` stay the NAND's.

- `local/shadowboot/` (git-ignored, XDK-built as the console agent is): `build.ps1` makes `out\ExShadowBoot.xex`
  (26624 bytes, retail-converted with xextool) and `out\HvxShadowBoot.bin` (1648 bytes; our toolchain's as and ld,
  `-a32 -many`, the flat link ExShadowBoot's Makefile does with xenon-ld).
- `make_rom.py <updflash.bin> <rom> [--addon <patch>.bin]`: the image's first 2 MB with the spare bytes stripped
  (header, SMC, keyvault, SB 14352, SC 16547, CD 12905, CE 1888 to 0x6d150, XeLL at 0x70000, CF and CG 17559 from
  0xb0000, the patch set at 0xc0000), the exFAT records cut from the patch set and the add-on's put in their place.
  The set has a 0x60-byte header, fuse rows and the CPU key, then records to an 0xFFFFFFFF end and no checksum.
- `kernel_patch_hdd.S`, built with tools/exfat_patch.py into `out\exfat_hdd.bin`: the production stub with the module
  at `\Device\Harddisk0\Partition1\XSDK\exfat\exfat.xex`, so the module iterates by FTP. The production stub keeps the
  flash path; the flash load is checked last, once, from a ROM made of the final image, then flashed.
- A run: `out\<variant>\` (ExShadowBoot.xex, HvxShadowBoot.bin, xboxromw2d.bin) to a folder on the HDD, the module to
  hdd:\XSDK\exfat\exfat.xex, ExShadowBoot.xex launched from Aurora; the console reboots into the ROM. A hang costs a
  power cycle, as the NAND's plain image boots again.
- With the plain NAND, a "loaded" line in exfat.log can only come from a shadow ROM's stub, so it proves the ROM
  booted and that the CD patches took the patch set from RAM.

Both ROMs come from the user's 22:32 image: `out\plain\xboxromw2d.bin`, its exFAT records cut, and
`out\hdd\xboxromw2d.bin`, with the HDD stub. Open until the first run: whether the CD patches take the patch set
from the image in RAM or from the NAND, and whether SB 14352 boots here without the glitch (ExShadowBoot's users
shadowboot devkit SBs on RGH consoles; in the devgl image SC and CD are signed with the SB private key xeBuild
holds, the SB is Microsoft's). `out\plain\` is the first run, Aurora as usual; `out\hdd\` the second.

Run 1 (plain, launched from Aurora by the user): black screen, nothing boots. The console agent and the debug
monitor still answered: kernel uptime kept counting from the boot, ExShadowBoot.xex was not among the loaded modules,
Aurora.xex was loaded again and drew nothing. So the title ran and returned, which its code does only on a failure
before the shadowboot or when the backdoor syscall comes back (its `HvxFreebootExecute` never returns when the
payload runs). The built title was checked in IDA: the eight arguments reach the `sc` in r3-r10 untouched, the DPC
dance is the source's. The title was given a phase log (`GAME:\exshadow.log`, written through) and a probe of
syscall 0 before it touches USB: a wrong key must return the hypervisor version, the key with action 9 returns 2 from
the backdoor (its "unknown action" exit) and nothing else.

Run 2 (plain with the probing title, rebooted and launched over the agent): the console stopped answering at once,
agent and debug monitor both, and never came back; the user power-cycled it into the NAND's Aurora. Its log
(`exshadow_plain_run2.log`): payload at physical 0x1fc40000, ROM at 0x1601000 (2 MB), syscall 0 with a wrong key
0x44970000 (the hypervisor version, 17559), with the key and action 9 → 2: the backdoor is live; "USB power down",
"Executing shadowboot", nothing after. So the title and the ROM pipeline are fine and the payload was entered; the
loss is in the payload or in SB 14352. Run 1's return (title back, Aurora black) is unexplained; likely its 2 MB
allocation in the 16-24 MB window failed with Aurora's memory in use, as the launch from the agent right after a boot
got the memory. The payload's UART prints, disabled by hax360, became POST codes (`payload/post_codes.py`): D1 entered,
D2 HvpSaveThread done, D3 ROM checks passed, D4 SB decrypted, D5 SB in SRAM with the media flags, D6 memory cleared
and entering the SB, DD a check failed; after D6 the SB posts its own codes.

Run 3 (plain, the band channel): the user has no POST display, so the title puts a blue 1280x720 frame up through
D3D (Clear and Present, 2 s on screen) and stores the display controller's surface address
(D1GRPH_PRIMARY_SURFACE_ADDRESS, register 0x6110, read at 0x7FC86110; libxenon xenos.h:36, xe.c:962) in a slot the
payload declares as its fourth symbol-table word. The payload's PrintChar then zeroes one 32-line tile row of the
frame per code (0x28000 bytes of the tiled 32-bit surface, tile row 2 × (code & 0xf): D1 at line 64, D2 128, D3 192,
D4 256, D5 320, D6 384, and the dead loop's code moved from DD to D8 so its band at 512 stays inside the frame). The
display controller scans the frame on after the kernel is gone, so the lowest band tells where the payload stopped.
Log (`exshadow_plain_run3.log`): surface 0x1f698000, pitch 1280, D1GRPH_CONTROL 2, the rest as run 2. Screen (the
user's photo): blue with six black bands at lines 64 to 384, none at 512, holding until the power cycle. So the
payload ran through D6: ROM checks, SB decryption, the copy to secure SRAM, the media flags, the memory clear all
done, and the loss is in its last 40 instructions (HID4, LPCR, SLB, TLB, the `rfid`) or in SB 14352 itself.

Run 4 (plain, band D7 added right before the `rfid`; the fill now `std` + `dcbf` per 8 bytes, so it also works once
real mode is cache-inhibited): seven bands, the seventh at line 448. The payload is clean through its HID4, LPCR, SLB
and TLB setup; the loss is the `rfid` into the SB in secure SRAM or SB 14352 itself, and the frame held, so DRAM
kept its contents (the SB did not retrain memory, or died first).

What the SB and the CD do, read from the ROM's bootloaders decrypted with the keys they derive themselves (SB key =
HMAC-SHA1(1BL key, SB nonce), SC key = HMAC(SB data key at SB+0x378, SC nonce), CD key = HMAC(SC key, CD nonce);
`local/shadowboot/shadow_patch.py` has the chain). The SB 14352, a devkit-type 2BL (magic "SB", entry 0x3d0): POST
0x20, SoC setup, 0x21 fuse checks (row 0's top bits; console type in row 1, or all zero when row 0's low bits are
clear; LDV; a 53-bit CPU key only when row 0 marks a retail; a zero pairing block skips the SMC HMAC), 0x22 SECENG keys
from the fuses and the time base, 0x2F relocation into the encrypted window at 0x03000000 and an `rfid` into it, then
0x23-0x2D load and verify the SC after itself in SRAM, a RAM-size register read (0x80000200e1040000, 512 MB = 0x2000,
else panic 0xAF), then 0x30-0x3A load and verify the CD into the window at 0x04000000 and jump to it with the next
stage in r31. Its header flag 0x200 (hax360's "media flags", BLDR_SHADOW_BOOT in DevShadowBoot.cpp) is the shadowboot
switch: with it the SB takes the value it was given as a RAM pointer for the SC and the CD and does not run the SC,
which is the hardware-init script (POST 0x2E, a bytecode interpreter at 0x20018f0: DRAM, PCI; so shadowboot never
retrains memory, which the held frame showed). Without it the value is a NAND offset added to the NAND base word at
SB+0x258 (0x80000200c8000000).

The CD 12905 (retail, xeBuild's) tests no such flag anywhere: its main takes the low 32 bits of what the SB passed as a
NAND offset (POST 0x41 offset check at 0x4000bec, then the CE header through its TLB window on the NAND at that
offset, panic 0xB2 on a bad header). In shadowboot the SB passes the CE's RAM address, 0x80000000016170e0, whose low
word is 22 MB into a 16 MB NAND: the CD halts at 0xB2, or on the aperture read, with the frame on the screen. That is
the hang: ExShadowBoot works with devkit ROMs, whose SD knows the flag, and a devgl chain's retail CD does not.

The fix, `shadow_patch.py` (12 bytes of the ROM): the title passes flags 0, so the SB runs its NAND path, with the
NAND base word pointed at the ROM in RAM (0x8000000001601000, the address the title got in every run; it now refuses
any other). The SB then reads the SC and the CD from the ROM at their NAND offsets and hands the CD the CE's NAND
offset 0x170e0, so the CD loads CE, CF and CG from the real NAND. The SB's call of the SC script (0x2002b38) and its
CD signature check (0x2002d70) are forced past, because the CD is patched too: xeBuild's patch-set loader in it
(0x4005370, which reads the records at NAND 0xc0060) is pointed at the ROM, so the kernel patch records come from RAM
and an add-on variant no longer needs a flash. The unpatched ROMs stay beside the patched ones as
`xboxromw2d.unpatched.bin`. Open risks: the CD's xeBuild preamble talks to the SMC FIFO and writes 0xea00101c, cold-boot
code never run warm before, and the HV and kernel start on live hardware, as on a devkit shadowboot.

Run 5 (plain, patched ROM, 2026-11-15): the chain booted the kernel. The title logged the ROM at 0x1601000, the frame
at 0x1f698000 and "Executing shadowboot" (`exshadow_plain_run5.log`); the agent was silent for the first 80 s, then
answered from Aurora with an uptime of 220 s, so the kernel restarted within seconds of the launch (the user did not
touch the console; a power cycle would have put the restart minutes later). Nobody watched the screen. The loop works:
a 2 MB ROM on the HDD and a title launch replace a flash.

Run 6 (`out\hdd\` ROM, HDD-path stub records, gated module at `hdd:\XSDK\exfat\exfat.xex`): the screen held the
bands and blue (the user's report); `exfat.log` unchanged after the power cycle (12119 bytes, the recorder's line
last), title log identical to run 5's.

Run 7 (plain again, polled every 10 s): silent for 150 s, while the console answers 17 s after a power cycle, so the
plain ROM does not boot the kernel either. Run 5's 220 s uptime was therefore not the shadowboot: the agent was silent
for its first 80 s too, and the context reset between the polls left room for a power cycle. Runs 5 to 7 all end the
same way: the payload's seven bands, then nothing. The chain dies in the SB or later, where no band is painted yet.

Run 8 plan: the SB gets the band channel. Its POST helper (0x2002e20: `sldi r4,r4,56; std r4,0(r3); blr`) is the one
path of every progress code 0x21 to 0x3A and every panic 0x9B to 0xB0 (0x20 and 0x2F are inline); its `blr` becomes
a branch to a painter placed in the SB's RSA signature (SB+0x40, 0x100 bytes nobody verifies in a shadowboot), which
paints a white band at tile row code-0x20 for progress and a red one at row code-0x9B for a panic (rows 0 to 21 of
the 720-line frame, 32 lines each), then returns to the helper's caller. The frame's real address comes from the
payload, which stores it at SB+0x138 in SRAM before the jump. Callers keep nothing in r3 to r7 or CTR across the
helper (compiler code: r11/r10/r9 recomputed, the POST address in r20/r31, the SB base in r28/r29).

Run 8 (plain, SB bands; the user's screenshot `4.png`): white rows 0 to 3 (codes 0x21 to 0x24), the payload's black
rows 4 to 14, and a **red row 10: panic 0xA5**. That is the SC loader's offset check (0x2002904 → sub_2002dc8: 16-byte
aligned, 0x80 to 0x07FFFFFF as a 64-bit value) on the argument the SB got in r31, which in the NAND path it adds to
its NAND base. The payload still passed the SC's RAM pointer (0x800000000160d0e0, the SB end pointer of its own ROM
checks), as the flag-0x200 path wanted. Fix in the payload: after the SB size check, r31 = SB offset + SB size, the
SC's offset in the ROM (0xc0e0), so the patched SB reads the SC at ROM + 0xc0e0 and the CD after it. The SB band
channel itself is proven by this run: every painted row landed where the table says.

Run 9 (plain, r31 = offset; the user's screenshot `5.png`): the SB ran through. White rows 2 to 12 and 15 to 21 are
0x23 to 0x2D and 0x30 to 0x36; row 13 stayed blue (0x2E is the SC script we skip), row 14 is the payload's D7 (0x2F
is inline); green rows 0, 1 and 3 are 0x37, 0x38 and 0x3A (0x39 is not on this path). No red. The SB handed over to
the CD, and the console died there or later without a word: the CD's codes 0x40 to 0x53 and xeBuild's appended code
were not painted.

Run 10 plan: the CD gets the channel too. Its POST helper (0x4001020) is the SB's twin, called 25 times with r3 built
fresh each time, and the CD reaches the SoC through the same real-mode addresses, so `payload/cd_painter.S` reads the
same SRAM frame word; it sits in the CD header's zero bytes at 0x120 to 0x240 (after the signature the SB clears with
its memset at 0x2002d90, before the CD's data at 0x240), reached from the helper's `blr`. Yellow at row code-0x40 for
0x41 to 0x55, magenta at row code-0xB1 for a panic, both visible over the SB's white. 0x40 is inline at the entry.
The CD turns data relocation on at 0x4000450 (`mfmsr; ori 0x10; mtmsrd`) and xeBuild's patch applier at 0x400535c
clears it (`mfmsr; li 0x10; andc; mtmsrd; isync`) to read the NAND window and restores it to write the kernel, so the
painter clears it the same way around its SRAM read and frame stores.

Run 10 (plain, SB and CD bands; the user's screenshot `6.png`): yellow rows 1 and 3 to 9 (CD codes 0x41, 0x43 to
0x49), **magenta row 2: panic 0xB3**, painted over 0x42's yellow after 0x49. The CD's sequence (0x4000b80): 0x41
offset check on the SB's value, 0x42 CE header from the NAND window into 0x280000, 0x43 header check, 0x44 the rest
of the CE, 0x45 CE key = HMAC-SHA1(CD header+0x10, CE nonce) (0x4001bf8), 0x46 RC4 key setup, 0x47 RC4 of the CE
body, 0x48 SHA-1 of the CE header and plaintext body (0x4001d90), 0x49 memcmp with the hash the CD carries at
CD+0x24c (0x4001350) → 0xB3 on a mismatch. So the NAND's CE did not decrypt to what our CD expects: the ROM's
bootloaders come from the exFAT build (updflash.bin of 2026-11-14 22:32, SB nonce b8ebcb61…, CD nonce 28deffcf…)
and the NAND holds whatever image the user flashed afterwards; a different CD key chain (SB data key → SC nonce →
CD nonce) encrypts the CE differently, and this CD's hash is of its own build's CE plaintext. Fix: build the ROMs
from the image that is in the NAND (make_rom.py on that file), nothing else changes. The user confirmed the NAND is
their own dump from before the step-4 flashes, `C:\Users\Mena\Downloads\flashdmp.bin` (2026-11-14 18:13, 16 MB with
spare bytes): same builds (SB 14352, SC 16547, CD 12905, CE 1888), other nonces (SB f578ab2f…, CD b0d20795…), a
76-record patch set. Both ROMs were remade from it (`make_rom.py`, the HDD one with `--addon out/exfat_hdd.bin`,
78 records) and patched as before; shadow_patch.py decrypted its SB and CD to the known entry code.

Run 11 (plain from the dump, SB and CD bands; the user's screenshot `7.png`): yellow rows 1 to 9 and 11 (CD codes
0x41 to 0x49 and 0x4B), no magenta: the CE now decrypts and verifies. Rows 13 (0x4D), 14 (0x4E), 15 (0x4F) and 19
(0x53) stayed as they were, so the CD died silently inside what follows 0x4B: `sub_4001188(CE at 0x280000,
0x370000, 1)`, the CE decompressor (LZX setup through 0x4002438, then per block the destination page mapped by
0x4000490 and decompressed by 0x4002510, then 0x4001310 over the whole range); a 0 result would have been panic 0xB4
(magenta row 3), and 0x4000960 begins with 0x4D. The destination pages are mapped through the encrypted alias
(0x4000490 sets bit 40, the 0x0100 of the HV's 0x8000010600000000 addresses, and replicates the 64 KB page index at
bit 33).

Run 12 plan: `payload/cd_probe.S`, two trampolines in the CD's signature area (the SB's memset of it at 0x2002d90 is
nopped) around those two calls, painting rows 10/12 before/after the page mapper and 20/21 before/after the block
decompressor. No rows: LZX setup; 10 only: the first page map; 10, 12, 20: the first block; all four: a later block,
map or the final pass.

Run 12 (the user's screenshot `8.png`): the same frame as run 11, none of the four probe rows. Either the CD dies
before its first page map, in the LZX setup `sub_4002438` (bump-allocates 0x3018 bytes of state at 0x370000, then
`sub_40029a8` builds the position-slot table, allocates the 0x20105-byte window after the state and resets the
decoder, all plain loads and stores to 0x370000-0x3a0000 and the stack at 0x3bff00, nothing a warm start should
change), or the trampolines themselves never ran (the SB's memset skip or the signature area not holding them). Run
12 cannot tell the two apart.

Run 13 plan: the same two trampolines, retargeted (`PROBE_SITES` in shadow_patch.py, `--defsym target_a/target_b` in
build.ps1): trampoline A around the LZX setup call at 0x40011d4, the first call after 0x4B, so row 10 proves the
trampolines run at all; trampoline B around the decoder init call at 0x40024e0 inside the setup. No rows: the
trampolines are dead; 10 only: the setup's state allocation and stores; 10, 20: the decoder init; 10, 20, 21: the
return path of the setup; all four: the block table or the first page map, between 0x40011d8 and 0x4001250.

Run 13 (the user's screenshot `9.png`): the same frame again, no probe rows, so the trampolines are dead. Cause, in
the SB's CD load: its RSA verify (0x20044e8 -> 0x2004688) computes the signature's modular exponentiation into the
signature buffer itself (the last multiply at 0x2004714 writes r31 = CD+0x20, and 0x2004650 compares that buffer
with the expected encoding built at 0x2004750), so with the memset skipped CD+0x20..0x120 holds that result, not
the trampolines, and the first redirected `bl` runs garbage. Runs 12 and 13 therefore died at their first probe
site, not in the LZX setup: the run 12 reading above is void. What stands is run 11: the CD dies after 0x4B and
before 0x4D without any probe.

Run 14 plan: with `--cd-probe` the SB's verify call (0x2002d68) is a nop too (its result was already ignored at
0x2002d70). Three trampolines: A at CD+0x30 (rows 10/12) around the page mapper 0x4000490 at 0x4001250, B at
CD+0xA0 (rows 20/21) around the block decompressor 0x4002510 at 0x400126c, C in the 124 free bytes after the CD
painter, CD+0x1c4 (rows 16/18, codes 0x50/0x52) around the LZX setup at 0x40011d4. No rows: the trampolines are
still dead; 16 only: the LZX setup; 16, 18: the block table before the first page map; 10 added: the first page
map; 12, 20 added: the first block's decompression; all: a later block, the final pass 0x4001310 or the entry of
0x4000960.

Run 14 (the user's screenshot `10.png`): all seven probe rows yellow (10, 12, 16, 18, 20, 21 besides the CD's own):
the LZX setup, the first page map and the first block's decompression all return. The CE (build 1888 at ROM 0x170e0,
0x5606a bytes) decompresses 0x120000 bytes to physical 0 (CE+0x20 destination, CE+0x28 total): 36 inline blocks of
32 KB (a 4-byte header of compressed and uncompressed halfwords before each block's data, so 18 pages of 64 KB. The
page mapper 0x4000490 writes one TLB entry per page itself (SPR 947 index from the page number, 949 RPN = the page
through the encrypted alias with flags 0x196, 948 VPN, then dcbzl + icbi over the page), the final pass 0x4001310 is
dcbst + icbi over the whole 0x120000. On a warm start the TLB still holds the old kernel's entries, which is the
one thing a cold boot never has beside these writes.

Run 15 plan: the trampolines paint magenta on the way in and yellow on the way out, so a row's final colour says
whether the last call through it returned: A (page mapper) row 21 plus, after each map, a magenta row 4 + page index
capped at row 18 (pages 14 to 17 share it); B (block decompressor) row 20; C now wraps the final pass 0x4001310 at
0x40012ac, row 19. Rows 0 to 3 stay free for the panics. The last magenta page row is the last page mapped; magenta
on 21 = died in a map, on 20 = in a block, on 19 = in the final pass; all yellow = after the decompressor returned.

Run 15 (the user's screenshot `11.png`): magenta rows 4 to 13 (pages 0 to 9 mapped), rows 20 and 21 yellow (the
last block decompression and the last page map both returned), row 14 black and 19 white (the page 10 map was never
entered, the final pass never ran), row 3 yellow (no panic 0xB4, so no fail path of the loop either). The CE's blocks
are 0x8000 bytes each, inline in the stream (4-byte header, then the data): block 19 (destination 0x98000) has its
header at EA 0x2b3382, block 20 (0xa0000) at 0x2b6db8. So the CD died in the loop's own code between the return
from block 19 and the `bl 4000490` for page 10 (0x4001278-0x4001298, then 0x400120c-0x400124c): two halfword loads
from the stream, one stack store, register arithmetic, through trampoline B's return. Plain code dies like that only
when its memory changes under it, and a warm start has a mechanism a cold boot never has: the CD maps everything it
writes through the encrypted alias (bit 40, the 64 KB page number again at bit 33: its scratch and stack 0x280000-
0x3cffff at entry, 0x4000364-0x4000444, and each CE destination page in 0x4000490), while the kernel and title that
ran a moment ago left dirty L2 lines for the same DRAM under the plain real addresses (the PPE's L1 is write-through,
so the shared 1 MB L2 holds them). The two tags differ, so both lines live in the L2 until the old one is evicted
and written back over the CD's data, and the eviction comes after about a megabyte of new traffic: 20 blocks of
output and their stream reads are about that. The same holds for the CD's code at 0x04000000 (the SB writes it
through the 0x8000017c alias, 0x2002008) and for the new HV and kernel at 0x0-0x11ffff, which the old kernel's data
lines would corrupt after the boot. The SB only writes back its own lines (dcbst over the CD), nobody flushes the
rest.

Run 16 plan: the payload flushes the whole 512 MB with dcbf through the HRMOR-bypass real addresses right before it
jumps to the SB (4M lines, a few tens of ms), then sync. Same ROMs as run 15, so the probe rows show how far the CD
gets if that is not the whole story.

Run 16 (the user's screenshot, payload md5 3c4ef7959a9e1d1f1b7d4b19b611bbc1 with the 512 MB flush): the same frame
as run 15 to the row. The flush ran (the payload's D6 and D7 bands come after it), so the dirty-cache explanation is
wrong, and the death is deterministic: it does not move with tens of milliseconds more before the SB. Left for it:
the twenty or so plain instructions between the return from a block of page 9 (18 or 19, run 15 cannot tell them
apart) and the next call, or something that stops the page mapper's trampoline before its first paint.

Run 17 plan: the loop itself is instrumented. Trampoline A paints magenta on row 4 + page at its entry and yellow
there on return (so row 14 black = the page 10 map never entered, magenta = entered and died, yellow = returned);
B keeps row 20 (magenta in, yellow out); stub D replaces the tail's `cmplwi cr6,r11,0` at 0x4001294 and paints row
19 by the block just finished (yellow after an odd block, magenta after an even one), then redoes the compare; stub
E replaces the head's `cmplw cr6,r29,r25` at 0x4001248, right before the map decision, and paints row 21 by the
block about to run (yellow odd, magenta even). Readings: 19 yellow and 21 magenta with 14 black = the head ran for
block 20 and the `bl` to A did not paint; 19 yellow and 21 yellow = died in the head for block 20 (its two loads
from the stream at 0x2b6db8 or the stack store); 19 magenta and 21 yellow with 20 yellow = died in the tail after
block 19, before D (the stream re-read at 0x2b3382); 20 magenta = died inside block 19.

Run 17 (the user's screenshot `12.png`, ROM md5 95ce00a6…): rows 1 to 14 yellow, 15 to 18 white, 19 to 21 yellow.
Row 14 yellow = the page 10 map entered and returned. Stub E's colours are the reverse of what cd_probe.S's comment
says (rlwinm takes bit 15 of r30, set for an odd block, and 0x55 + 0x71 = 0xC6 is magenta), so row 21 yellow = an
even block was about to run, row 19 yellow = an odd block had just finished, row 20 yellow = the last block call
returned. Read together: block 21 finished, block 22's head ran through E, and trampoline A's entry paint for page
11 (row 15) never came. Two blocks further than runs 15 and 16, with the probe's paints the only change (C removed, D
and E added): the point moves with the probe's traffic, not with time, since the slower run got further.

Run 18 plan: something writing memory on its own clock would move like that. ExShadowBoot skips the kernel's device
shutdown, so the USB host controllers (OHCI writes its HCCA every millisecond), the NIC and SATA keep their DMA
going while the CD works in memory the old kernel owned. The payload now stops them before zeroing memory, through
the real-mode SoC addresses, registers as libxenon drives them (ohci.c HcCommandStatus HCR at 0xea002008 and
0xea004008; tinyehci.c USBCMD RUN off at 0xea003000 and 0xea005000 plus CAPLENGTH; enet.c reset 0x01805508 held at
0xea001428; ata.c bus master command byte 0 at 0xea001320 and 0xea001220). Payload 1984 bytes, md5 27e9dd2a….

Run 18 (the user's screenshot `13.png`, same ROM): identical to run 17 to the row. No device's DMA is what stops the
CD, and the death is deterministic per build. The payload keeps the device stop (the new kernel resets them again).

Run 19 plan: the CD painter reads the frame address from the SB header word in SRAM (0x8000020000011138) on every
call and paints nothing when that word reads zero, so a frame can also mean "blind", not "dead": a dirty L2 line
over that header, written through a cacheable mapping and evicted after about a megabyte of traffic, would silence
every later paint while the CD goes on, and the eviction point moves with the paints' own traffic. That fits both
observations better than a death in plain code does. The CD painter now has the frame address baked in
(0x800000001f698000, the same in every run; cd_painter.S, 148 bytes), the SB painter still reads the word, and the
probe is unchanged. ROMs: plain md5 c64128db…, hdd 5cbbdac1…. If the rows go further, the frame was lying from the
old point on; if the frame is identical, the SRAM word was not it and the death is real.

Run 19 (2026-11-15 03:11, plain ROM md5 c64128db…, payload md5 27e9dd2a…): **the console booted to Aurora from the
shadow ROM.** The user watched it come up ("It's already working"); the agent answered with kernel uptime 52 s at
03:12, launch at 03:11, so the chain (title, payload with its 512 MB flush, SB, CD, CE, CF, CG, HV, kernel, dash)
took about 25 s. The same arithmetic on the earlier agent answers says runs 17 and 18 booted too: run 17 was
launched about 02:38 and the agent later reported a kernel started at 02:38:30, run 18 was launched about 02:59 and
the kernel reported started at 03:00:33, both while the band frame was still on the screen for the screenshot. So
the "held frames" of runs 17 and 18 were the frame as it looked while the chain still ran, and the one change that
separated them from runs 15 and 16 (the user power-cycled after 16, so whether those booted is unknown) was never
the cause of a death: from run 17 on the CD finished its work and the frame only stopped updating because the
painter's SRAM read of the frame word (0x8000020000011138) came back zero, or the CD's later codes paint rows the
probe had already painted. Run 19's baked-in address removes the first; the painter keeps it.

What the loop is now: `hdd:\XSDK\shadow\plain\ExShadowBoot.xex` boots the NAND's kernel and patch set from RAM in
about 25 s, no flash, a hang costs a power cycle. The payload stops the devices' DMA before the SB (harmless, kept),
the SB and CD paint their codes (kept as the debug channel), and the decompressor probe is still in both ROMs (to be
taken out once the exFAT iteration no longer needs an identical chain; it adds about a hundred paints to the boot).

Run 20 plan: `hdd:\XSDK\shadow\hdd\ExShadowBoot.xex`, the ROM whose patch set carries the HDD-path stub
(out/exfat_hdd.bin: the production hook at 0x80061388 loading `hdd:\XSDK\exfat\exfat.xex`), with the module (61440
bytes) and its log (12119 bytes before the run) in place. A new "loaded" line in `hdd:\XSDK\exfat\exfat.log` is
also the proof that the running kernel took its patch set from the shadow ROM, not the NAND (the NAND's plain image
has no exFAT hook). The flashed variants all hung the dash at the module's start; here a hang costs a power cycle
and the log says how far the module got.

Run 20 (2026-11-15 03:14, hdd ROM md5 5cbbdac1…, module md5 12a8d3ad… = `gated/exfat.xex`): the agent did not
answer at 85 s or 100 s after the launch, while run 19's plain ROM had answered within a minute. The user
power-cycled. `exfat.log` stayed at 12119 bytes, which is what the gated module does while the executable handle
stays clear, so the log says nothing about the module and the boot hung as the flashed gated boot did (dash never
comes). The one difference between the two ROMs is the patch set's exFAT records, so the kernel took its patch set
from the shadow ROM: the loop reproduces the step-4 hang without a flash, one launch and one power cycle a variant.

Run 21 plan: the first difference from the console agent, whose early HDD write passes every boot, is the thread's
pin to hardware thread 2 (src/plugin.c). The module's thread is now created with the agent's flags exactly
(`CREATE_SYSTEM_THREAD`, no hardware-thread bits; local/console-agent/agent/agent.c line 311), nothing else changed,
clean full build, the module put to `hdd:\XSDK\exfat\exfat.xex`, same hdd ROM. Readings: Aurora and a new "loaded"
line in the log = the pin was the cause and an unpinned system thread runs; Aurora without the line = the pin was
the cause but the thread never ran (xcn's note), so the next build pins to hardware thread 0; a hang = the pin is
not it, and the next difference is the thread's creation by the init thread inside XexLoadImage.

Run 21 (2026-11-15 03:27, hdd ROM, module md5 9699b4d2…: the gated module with its thread unpinned, src/plugin.c
flags `CREATE_SYSTEM_THREAD` alone as the console agent's): the user watched the boot animation freeze; the agent
never answered; `exfat.log` unchanged. So the pin to hardware thread 2 is not the cause, and the hang has two shapes
by where our thread runs: pinned to hardware thread 2, the kernel stays alive (the animation inside the X runs, the
dash never comes; the flashed gated boot and, by its silence, run 20); unpinned, so on the init thread's own
hardware thread, a halt (the frozen swirl, as the volume-wait boot and recorder 2 had). src/plugin.c is back to the
pin. The module's log from the console (scratchpad exfat_console.log) has six bare "loaded" lines without a mount
from the flashed boots whose module logged, and recorder 1's 16 samples; nothing dates them, so whether the
30 s-sleep boot's thread ever woke after Aurora is still unknown.

Run 22 plan: read the stuck point instead of guessing it. Recorder 1 proved a thread pinned to hardware thread 2
runs and writes the HDD every 5 s through the alive-kernel hang, so a recorder can watch the boot path from inside
it. Recorder 2 was to log the spin word and the module list and died at its first sample; its list walk read
`base_dll_name` as an ANSI string (it is a UNICODE_STRING) and copied from whatever pointer an entry held, a
half-built entry included, so that death is most likely its own. Recorder 3 (`recorder/plugin_main_recorder3.c`,
`recorder/exfat3.xex`, swapped into plugins/exfat/exfat_module.c for the build and swapped out again) reads the
names as UTF-16, skips an entry whose name is unset or outside 0x80000000-0xA0000000, drops the file-cache
sampling, and logs its own stack pointer. Lines every 5 s and on every change: the spin word 0x8010BFEC (nonzero for
good = DashLaunch's DllMain never returned on the init thread), the started flag 0x801A5DD0, the executable handle,
and every module's name (DashLaunch's plugins after launch.xex = firstRunTasks ran; lhelper.xex = the executable
loaded). Pinned to hardware thread 2 as recorder 1 was, so the kernel stays alive to be read; the user power-cycles
after about 90 s and the log is fetched.

Run 22 (2026-11-15 03:40, hdd ROM, recorder 3 md5 c2c2c174…, 40960 bytes): **booted to Aurora**, the agent up,
the recorder still sampling two minutes later. Its log (`boot/exfat_run22.log`), the timeline of a good boot with a
module loaded at our hook: launch word 0x8010BFEC already 0 at +2.2 s (DashLaunch's DllMain returned), the
executable handle set at +3.0 s (lhelper.xex) and the started flag at +3.3 s, both cleared at +12.8 s and +13.0 s
(lhelper done), the handle set again at +13.5 s (Aurora) and started at +13.5 s, unchanged from then on. The module
list came out empty every sample, so 0x80197700 is not the head's forward link as assumed (left as is). Two facts
the line "recorder 3 loaded" adds: its image is 0x91580000-0x9158A000, 40 KB, where every variant that hung was
0x91580000-0x9167B000, 1 MB (the three 269 KB Volume structs in BSS; the recorder's `(void)volumes` reference let
the compiler drop them), and its thread's stack is at 0x7A03FC20, in title memory, where it survived both title
starts. So an HDD write before xam exists does not hang the boot by itself (recorder 3 wrote at +0 ms and every 5 s
through xam's whole start-up), and the hung builds share one thing the passing one lacks: the 1 MB image. Checked
offline against the HDD plugins' XEX headers (results `boot/Engine.xex`, scratchpad Xbdm.xex and devagent.xex):
Xbdm.xex loads at 0x91000000 (0x1E000), Engine.xex at 0x90CD0000 (0x18000), devagent.xex at 0x91D00000 (0x2C000),
none inside 0x91580000-0x9167B000, so no fixed-base plugin collides with the image; the flash's xexes were ruled out
before (ximecore.xex from 0x91680000). What a 1 MB kernel-resident image costs the boot is then its physical memory
or its range in the system region, not an image overlap.

Run 23 plan: the production gated module with the volumes out of the image: `volumes` becomes a pointer, allocated
from the kernel pool and zeroed after the gate releases, nothing else changed, so the image shrinks to the code and
the small statics. Aurora with "loaded" and (stick in) "mounted" lines = the image size was the cause and the fix is
the production one; a hang = the behaviour after the gate (the poll, the stick mount) is next.

Run 23 (2026-11-15 03:46, hdd ROM, the gated module with the volumes allocated after the gate, image
0x91580000-0x915B1000, 196 KB, md5 5736a967...): stuck at the boot animation, looping, the kernel alive, as run 20; the
user power-cycled. `exfat.log`, fetched after the reboot, has no line from it: the only new bytes are recorder 3's
samples from run 22, which ran on until the power cycle (its exe word changes at +488 s, when ExShadowBoot.xex was
launched for run 23). So the gated module never gets past its gate: the executable is never set while it is loaded,
which puts the hang before lhelper.xex loads, not in anything the module does after the gate. Recorder 1's samples
(1 MB image, flashed) say the same: xam.xex and launch.xex loaded by +1.3 s and nothing after for 77 s, the executable
never set, where recorder 3 (40 KB) saw the spin word clear at +2.2 s and the executable at +3.0 s. The image size is
not it either, or not at 196 KB: a 1 MB recorder hangs, a 40 KB recorder boots, the 1 MB module hangs, the 196 KB
module hangs. Checked offline: launch.xex links at 0x91F00000 (dashlaunch-src dll/xex.xml) and lhelper.xex at
0x92000000 (lhelper/lhelper.vcxproj), neither inside the image; neither the module nor the recorder imports from
xam.xex.

Run 24 plan: the production module with recorder 3's sampling while it waits at the gate: the log opened at once, a
line every 5 s and on change with the executable, the started flag and the spin word 0x8010BFEC, then the production
path once the executable is set. Against recorder 3 the differences are the production image (its size, imports and
statics) and the behaviour after the gate; against run 23 the early log and the sampling. A hang with the spin word
never clearing = DashLaunch's DllMain does not return with this image loaded; cleared but no executable = xam stops
before the dash launch; a boot = the early HDD write changes it.

Run 24 (2026-11-15 04:00, hdd ROM, the production module sampling at its gate, md5 c33c4952..., 57344 bytes, image
0x91580000-0x915B1000): stuck at the boot animation, the kernel alive; the user power-cycled after about a minute.
`boot/exfat_run24.log` has ten samples, +0 to +45.5 s, every one `exe 00000000 started 0 launch 12345678`: xeBuild's
spin word never clears while this image is loaded, where recorder 3 saw it clear at +2.2 s. The word is cleared by the
stub that loads launch.xex, so launch.xex's load never completes with the production image loaded, and recorder 1's
list (launch.xex present at +1.3 s, 1 MB image) puts the stop inside that load, after the loader has listed the module:
DashLaunch's DllMain, or what the stub does after it. Before the gate the module and recorder 3 behave the same (a log
line at once, a sample every 500 or 250 ms), so it is the image that matters, not the thread: its 196 KB against 40 KB,
its imports, or its statics.

Run 25 (2026-11-15 04:11, hdd ROM, recorder 4: `recorder/plugin_main_recorder4.c`, `recorder/exfat4.xex`, md5
c32840e0..., 45056 bytes, image 0x91580000-0x915B2000): recorder 3 plus 160 KB of read-only data the image carries,
so the production module's image size with the recorder's behaviour. Stuck at the boot animation, looping, the kernel
alive; the user power-cycled after about two minutes. Its log (`boot/exfat_run25.log`): 29 samples to +144 s, every
one `exe 00000000 started 0 launch 12345678`. So the image's size alone does it: the same code boots behind 40 KB
(run 22) and hangs behind 200 KB, with xeBuild's spin word never cleared, and what the module does is not involved.

The stub that clears the word (xeBuild's, kernel 0x8010BF40, from the patch record at offset 0x10BF40 of the hdd ROM):
it runs from `bge cr6` at 0x800613CC in ExpSystemInitThread, where cr6 holds the result of xam.xex's load, calls
XexLoadImage("\Device\Flash\launch.xex", 8, 0, 0), stores 0 to 0x8010BFEC with no test of the result, and jumps
back to 0x800613DC. Our own hook at 0x80061388 replaces the `bl ExpCheckSystemUpdate` just before xam's load: the stub
at 0x80163000 loads the module and tail-jumps to ExpCheckSystemUpdate. So the init thread's path is our module, then
ExpCheckSystemUpdate, xam.xex, sub_8006A1A8 on some hardware, then launch.xex; the word never cleared with launch.xex
in the module list (recorder 1) means XexLoadImage of launch.xex never returns while a 200 KB image is loaded.

Run 26 plan: the production module with run 24's sampling, plus at +10 s a dump of the init thread read from inside
the hang: the plugin runtime keeps the loading thread's object from the processor block (src/plugin.c, run 26 lines),
the module logs the object's first 0x180 bytes, the top 16 KB of its stack by the bounds at +0x5C and +0x60, and
launch.xex's loader entry. The stack's return addresses say where XexLoadImage sits.

Run 26 (2026-11-15 04:22, hdd ROM, the production module with the dump, md5 31dc5df9...): hung as before, 27
samples to +131 s with the spin word never cleared (`boot/exfat_run26.log`). The dump gave one line, `loader thread
3A06B020`, and stopped there: the thread object is in the kernel's pool, below 0x80000000, and the dump's range check
accepted only 0x80000000 and up. Run 27 is the same with that check gone, and with the stack bounds' offsets checked
on the module's own thread object first (the two adjacent words that bracket its stack pointer) before they are
applied to the init thread's.

Run 27 (2026-11-15 04:28, hdd ROM, the production module with the dump fixed, md5 bb690b73...): hung as before; the
dump landed (`boot/exfat_run27.log`). The init thread's object is at 0x3A06B020 in the kernel's pool: stack base
0x3A075000 at +0x5C, limit 0x3A06D000 at +0x60 (the offsets checked against the module's own thread, whose stack
pointer 0x7A03FD80 its +0x5C/+0x60 bracket), start address 0x800610E8 = ExpSystemInitThread at +0x150, last processor
0 at +0xBF. The stack below 0x3A0743E0 is untouched zeros, so the words at +0x64 (0x3A0740F0) and +0xA8 (0x3A074240)
are not a saved stack pointer. The frame chain, read by the ABI's rule that a frame's owner saved its return address
8 bytes below its caller's frame (ExpSystemInitThread's 0xA0 frame at 0x3A074EA0, with xeBuild's stub's return address
0x8010BF64 at 0x3A074E98, anchors it):

| frame | size | owner returns to | owner |
|---|---|---|---|
| 0x3A074E20 | 0x80 | 0x8010BF64 (the stub, after `bl XexLoadImage`) | XexLoadImage's body, sub_8007D6E8 |
| 0x3A074D80 | 0xA0 | 0x8007D790 | sub_8007D528, which calls the entry point |
| 0x3A074D20 | 0x60 | 0x8007D6BC | sub_8007AE40, the entry caller |
| 0x3A074C90 | 0x90 | 0x8007AEF0 | launch.xex's entry (0x91F0EAD8) |
| 0x3A074C20 | 0x70 | 0x91F0EA18 | the function the entry calls at 0x91F0EA14: DllMain |
| 0x3A074B80 | 0xA0 | 0x91F0E840 | the function DllMain calls at 0x91F0E83C |

No frame links below 0x3A074B80, and the thread's wait list entries at +0x74 and +0x7C are empty (point to
themselves), so the init thread is running, not waiting: inside that last function of launch.xex or a leaf it
calls. launch.xex's loader entry at that moment: image 0x91F00000-0x91F1D000, entry 0x91F0EAD8, flags 0x00100200,
load count 65535. Below the live frames, stale: MountPath's "\System??\tmt:" link name, several STATUS_NOT_FOUND
(C0000225), returns into XexGetProcedureAddress (DashLaunch's resolveFunct, from hookImpStub), NtCreateFile,
IopParseDevice and sub_80079768, so launchStartup's hooks have run at least in part. Next: the loaded launch.xex
image, copied to the HDD by `tests/probe_dump_launch.c` from the dash, names the function at 0x91F0E83C and what it
loops on.

Read against that image (objdump of `launch_image.bin`; DashLaunch's `dll/launch.c` names the functions): the frame at
0x3A074B80 is launchStartup's (0x91F0A420, frame 0xC0; the 0x70 frame above it is DllMain's prologue size 0xD0 read
wrong, so DllMain's frame is 0x3A074BC0). The return address 0x91F0E840 lies in its XexHook stretch: after the second
hookFunctionStart call (0x91F0A5B8, which writes the hook into XexLoadImage's body sub_8007D6E8 via ordinal 409) and
before 0x91F0A5E8, with genericRevokeCheckPatch never called. hookFunctionStart (0x91F0D0F0) is a frameless leaf that
writes the relocated instructions and the jump into kernel code, with dcbst/hwsync/isync and bounded loops, so the init
thread never came back from one of those kernel-code stores or their flushes, and the stack says no more. Why that
stretch stalls with ~200 KB more system-image memory mapped is not established.

Run 28 (2026-11-15 04:55, hdd ROM unchanged, a 16 KB loader module `boot/exfat_boot_run28.xex` at the stub's path
`hdd:\XSDK\exfat\exfat.xex`, md5 da9ef595..., and the production module unchanged as `hdd:\XSDK\exfat\exfat_fs.xex`,
md5 5736a967...): BOOTED to Aurora (uptime 85 s at the first query after the 80 s wait; `boot/exfat_run28.log`). The
loader (`plugins/exfat/exfat_boot.c`, `make build/exfat_boot.xex`, base 0x915C0000) waits until xeBuild's spin word
0x8010BFEC clears, which is launch.xex loaded and its DllMain returned, then XexLoadImage's the production module:
status 0, handle 0x3A129DE8, and the module logged itself loaded at 0x91580000-0x915B1000. So the hang needs both the
~200 KB image and its load before launch.xex's: the same image loaded after it boots. Not checked: a volume mount (no
stick known to be plugged in), and run 27's open question stays open. For production the choice is to keep the loader
(a second module file, and a wait on xeBuild's word, which only a DashLaunch patch set has) or to move the kernel
hook past xeBuild's launch stub, to the instruction it returns to at 0x800613DC, so the stub loads the module itself
after launch.xex: one file, one more ROM run.

Run 29 (2026-11-15 12:52, the hook moved; the user chose it over the loader): the stub's `bl` now replaces the
`bl sub_800C8260` at 0x800613DC, the first call after xam's load and the address xeBuild's launch stub jumps back to
(`b 0x800613dc` at 0x8010BF78), so the module loads after launch.xex with no loader and no wait on xeBuild's word;
sub_800C8260 takes no arguments (it writes one hardware register for two processor revisions, from its IDA
decompile) and the stub ends in `b sub_800C8260` with the original return address. `tools/exfat_patch.py`,
`plugins/exfat/kernel_patch.S`, `local/shadowboot/kernel_patch_hdd.S` and `make_rom.py` (which now cuts both hook
addresses) carry the change; the kernel check passed (0x800613DC holds 48066e85). The ROM is run 28's with the two
exFAT records swapped, 5 bytes differ (`boot/exfat_hdd_run29.bin`; md5 d562b81a...); the production module alone at
`hdd:\XSDK\exfat\exfat.xex` (md5 5736a967...), the loader and `exfat_fs.xex` removed. BOOTED to Aurora (uptime 89 s
at the first query), and with the user's exFAT stick left in, the module mounted it at boot (`boot/exfat_run29.log`):

```
exfat: loaded, kernel 2.0.17559.0, image 0x91580000-0x915B1000, 269 KB a volume, 3 volumes
\Device\Mass0: mounted exFAT "XSDK" serial 643FC9ED: 120172544 sectors of 512 from sector 2048, 128 KB clusters, 390172 of 469400 free, volume device 3A157D18
```

On that boot, step 3's check title (`build/exfat_check.xex`, run from the dash through the agent with the stick in):
157 passed, 0 failed in 381 s (`boot/exfat_check_run29.log`), so the scope's step 3 checks hold on the module the
kernel loaded. So step 4's boot path works from the shadow ROM with the hook at 0x800613DC. The loader (`boot/exfat_boot_run28.c`,
`boot/exfat_boot_run28.xex`) is kept here as run 28's record and taken out of the tree. Left: the flash path itself
(`\Device\Flash\exfat.xex` from a flashed image, the production `build/exfat.bin`), and why a ~200 KB image loaded
before launch.xex stops DashLaunch's XexHook.

## The flash (2026-11-15, the user flashing)

The user rebuilt the NAND in J-Runner's xeBuild with the fresh `17559\bin\exfat.bin` (96 bytes, md5 03412b49...) and
`17559\exfat.xex` (md5 5736a967...), the same add-ons as the 22:32 build of 2026-11-14 (`-a hvFixKeys -a exfat`) and
their NAND dump as `data\nanddump.bin`. `verify_image.py` (here) read the image before the flash: 78 patch records,
our two byte-identical to `build/exfat.bin` (the stub at 0x163000, 19 words; the hook word 48101c25 at 0x613dc), no
record at 0x61388 any more, xeBuild's launch stub records at 0x10bf40 and 0x613cc as before; `exfat.xex`'s four 16 KB
blocks contiguous in the flash file system (at 0xe2c000 in the spare-stripped image; xeBuild's log says raw offset
0xe28800, its own accounting); against the 22:32 image the differing blocks are the bootloaders and CF/CG (new nonces
each build), the patch set, the system update container, the module's blocks with the files behind them, and fsroot.
The build's log differs from the 22:32 one in the module's size, the dump's mobile files, and nothing else. (The
build's log holds the CPU key and stays out of this folder.)

Flashed and booted (the user: "it worked"). On that boot, with the stick in and no title run (`boot/exfat_flash.log`,
the log's last two lines; the first two are run 30's):

```
exfat: loaded, kernel 2.0.17559.0, image 0x91580000-0x915B1000, 269 KB a volume, 3 volumes
\Device\Mass0: mounted exFAT "XSDK" serial 643FC9ED: 120172544 sectors of 512 from sector 2048, 128 KB clusters, 390172 of 469400 free, volume device 3A14DD58
```

Then `exfat_check.xex` from the dash through the agent: 157 passed, 0 failed in 384 s (`boot/exfat_check_flash.log`).
So the kernel loads the module from the NAND at boot and the module works there. Of the scope's step 4 checks, left:
Aurora's view of the stick and the FTP speed (the user's), and the NAND without `exfat.xex` in its flash file system
booting with the stick raw (another flash).

## Known limits

- `boot/Engine.xex` (Aurora's plugin, not ours to distribute) and `boot/launch.ini` (the console's DashLaunch
  configuration) stay out of the repository; the readings above were taken from the local copies.
- Kernel 2.0.17559.0 only: the addresses are that kernel's, and the tool refuses an image whose words differ.
- The module's poll stays the mount trigger (the development seam of plugins/exfat/kernel_seam.md, section 6); the
  production hook at 0x8006c070 is not part of this step.
