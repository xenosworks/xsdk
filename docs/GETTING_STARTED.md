# Getting started

From a Windows PC and an RGH or JTAG console to a title of your own running on the console. Take the steps in order:
each gives its commands, and what they print, shortened. Times are from the PC xsdk is built on, which has 32 hardware
threads. docs/GUIDE.md then has the calls by task, and docs/PITFALLS.md what breaks a title on the console and not on
a PC.

## What you need

- **Windows 10 or 11** with WSL 2, and about 10 GB free for WSL: the toolchain takes 1.5 GB installed and 7 GB while it
  builds, which can be deleted after.
- **An RGH or JTAG console** whose dashboard launches unsigned `.xex` files and serves FTP, such as Aurora, on the same
  network as the PC. A retail console won't load xsdk's titles: they aren't signed (docs/PARITY.md, "Building and
  packaging").
- **A controller** for the console: every sample ends on Back.

## 1. WSL Debian

In PowerShell:

```
wsl --install -d Debian
```

It asks for a user name and a password for Debian. Then `wsl -l -v` lists it at version 2:

```
  NAME      STATE           VERSION
  Debian    Stopped         2
```

The rest runs inside Debian: open it from the Start menu, or run one command from PowerShell with
`wsl.exe -d Debian --exec bash -c '<command>'`. Git Bash rewrites absolute paths given to `wsl.exe`, so call it from
PowerShell, not Git Bash. A command run that way doesn't read `~/.bashrc`, so these steps give full paths rather than
changing `PATH`.

In Debian, install what the toolchain and the builds need:

```
sudo apt update
sudo apt install build-essential bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev curl xz-utils patch python3 \
  cmake ninja-build git
```

apt lists what it installs. To see they're all there:

```
dpkg-query -W build-essential bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev curl xz-utils patch python3 cmake \
  ninja-build git
```

```
bison	2:3.8.2+dfsg-1+b2
build-essential	12.12
cmake	3.31.6-2
...
libgmp-dev:amd64	2:6.3.0+dfsg-3
...
python3	3.13.5-1
texinfo	7.1.1-1
xz-utils	5.8.1-1
```

A package that isn't there is named in a `no packages found matching` line instead.

That's Debian 13. CMake must be 3.25 or later, and Python 3.11 or later.

## 2. xsdk

Clone it into Debian's own file system, where builds run several times faster than under `/mnt/c`:

```
git clone https://github.com/xenosworks/xsdk.git ~/xsdk
```

```
Cloning into '/home/<you>/xsdk'...
```

xost, the shader compiler, is in it, in `xenos-toolchain/`.

## 3. The toolchain

xsdk builds with its own GCC 15.3, binutils 2.47 and newlib 4.6 for the target `powerpc-xsdk-elf`, patched for the
console's calling convention and what Xenia gets wrong (`tools/toolchain/build.sh`, its specs and its patches say what
differs from a stock build). The script downloads the three sources, 136 MB, checks their SHA-256, and builds them:

```
cd ~/xsdk && PREFIX=$HOME/xsdk-gcc WORK=$HOME/xsdk-gcc-work JOBS=$(nproc) tools/toolchain/build.sh
```

It installs into `PREFIX` and builds in `WORK`, which can be deleted once it's done. Both are read from the
environment, and the values above are what it takes when they aren't set. To put the toolchain somewhere else, change
them. **Every `~/xsdk-gcc` or `$HOME/xsdk-gcc` from here on means your `PREFIX`.**

It prints a line as each stage starts, and the stages' own output goes to logs in `$WORK/logs`:

```
== binutils (02:42:46)
== gcc-stage1 (02:44:11)
== newlib (02:46:58)
== gcc (02:47:12)
== check: the linker refuses objects of another calling convention (02:51:30)
== check: no float register load at memory not known to be aligned (02:51:31)
== done: /home/<you>/xsdk-gcc/bin/powerpc-xsdk-elf-gcc (02:51:31)
```

That's 9 minutes with 32 threads, once the sources are downloaded. A stage that fails stops the script; its log says
why. Run again, it skips the stages it has built. Then, to see the compiler runs:

```
~/xsdk-gcc/bin/powerpc-xsdk-elf-gcc --version
```

```
powerpc-xsdk-elf-gcc (GCC) 15.3.0
```

With a `PREFIX` other than the default, give make `TOOLCHAIN=<PREFIX>/bin/powerpc-xsdk-elf-` on every run, as in
`make -j$(nproc) TOOLCHAIN=$HOME/tc/bin/powerpc-xsdk-elf-`. `make samples`, `make sample_breaks` and
`make cmake_check` hand it on to CMake. A title's own CMake build takes `-DXSDK_TOOLCHAIN=<PREFIX>` (section 8).

## 4. glslang

Shaders are GLSL or shader model 3 HLSL, which glslang turns into SPIR-V for xost. xsdk uses glslang-xenon, Khronos's
glslang 16.3.0 with two changes for HLSL: its `fmod` truncates, as HLSL's does, and it keeps a bool's `register(bN)`
and an int's `register(iN)`, which xost puts where D3D9 does (xenos-toolchain/docs/xsc.md, "HLSL"). Its GLSL comes
out as Khronos's 16.3.0's, the version xost is checked with: the same SPIR-V, byte for byte, for 596 GLSL programs of
xost's tests, vulkan4xenon and Dusklight, and the 27 others refused alike (results/2026-10-20-milestone-50/README.md,
"Step 7" and "Step 8"). glslang runs SPIR-V's optimiser on all HLSL, so glslang-xenon is built with it, from
SPIRV-Tools and SPIRV-Headers at the revisions its `known_good.json` names, which `update_glslang_sources.py` fetches.
In about a minute and a quarter:

```
git clone --depth 1 --branch xenon https://github.com/xenosworks/glslang-xenon.git ~/glslang-xenon
cd ~/glslang-xenon && python3 update_glslang_sources.py
cmake -S ~/glslang-xenon -B ~/glslang-xenon/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_OPT=ON \
  -DSPIRV_SKIP_EXECUTABLES=ON -DGLSLANG_TESTS=OFF
cmake --build ~/glslang-xenon/build
```

The second fetches SPIRV-Tools and SPIRV-Headers into `External/`, and googletest, which the build doesn't use;
`SPIRV_SKIP_EXECUTABLES` leaves out SPIRV-Tools' own tools. The four print, at their ends:

```
Cloning into '/home/<you>/glslang-xenon'...
...
In External/spirv-tools/external/spirv-headers: ['git', 'checkout', '1a22b167081842915a1c78a0b5b5a353a23284aa']
b''
...
CMake Deprecation Warning at CMakeLists.txt:111 (message):
  ENABLE_HLSL is deprecated.  The HLSL front-end will be removed in a future
...
-- Build files have been written to: /home/<you>/glslang-xenon/build
...
[393/394] Building CXX object StandAlone/CMakeFiles/glslang-standalone.dir/StandAlone.cpp.o
[394/394] Linking CXX executable StandAlone/glslang
```

The warning is Khronos's: its HLSL front end takes fixes, no new work (KhronosGroup/glslang#4210). Put glslang beside
the toolchain, where xsdk's CMake looks for it (`XSDK_GLSLANG` in `cmake/xsdk.cmake`):

```
cp ~/glslang-xenon/build/StandAlone/glslang ~/xsdk-gcc/bin/
~/xsdk-gcc/bin/glslang --version
```

```
Glslang Version: 11:16.3.0
ESSL Version: OpenGL ES GLSL 3.20 glslang Khronos. 16.3.0
GLSL Version: 4.60 glslang Khronos. 16.3.0
```

That's Khronos's version, which glslang-xenon keeps, so nothing tells the two apart: HLSL through Khronos's glslang
builds too, with `fmod` floored and every bool and int at the lowest constant left. `tools/build_shaders.py`, run by
hand, takes `--glslang ~/xsdk-gcc/bin/glslang`; without it, it uses the `glslang` on `PATH`, then, for GLSL alone, a
Windows build in `xenos-toolchain/tools/glslang/bin/glslang.exe`, if one is there. That one is Khronos's, so for HLSL
it stops instead and names this section.

## 5. Building xsdk and the samples

```
cd ~/xsdk && make -j$(nproc)
```

With another `PREFIX`, add section 3's `TOOLCHAIN=` to this and to each `make` below. That builds `build/libxsdk.a`,
which titles link, xost's compiler for the PC (`build/host/libxsc.so`), the test title `build/xsdk_test.xex` and the
probes, in about 15 seconds. Its last lines:

```
xenia_scan: _dtoa_r: 822f70b0 rldic r10,r9,32,1
xenia_scan: 43 instructions Xenia skips; Xenia results through these functions are unreliable
python3 tools/elf2xex.py build/xsdk_test.elf build/xsdk_test.xex
```

Each link lists the instructions Xenia skips by function ("Xenia" below); on the console they run.

The samples are CMake projects built as a title's are:

```
make samples
```

Each prints CMake's configure, then its build, about 35 seconds in all:

```
-- Build files have been written to: /home/<you>/xsdk/build/samples/hello
[1/2] Building C object CMakeFiles/hello.dir/main.c.obj
[2/2] Linking C executable hello
...
-- Build files have been written to: /home/<you>/xsdk/build/samples/triangle
[1/3] Generating triangle_shaders.h
...
-- sdl3: not built, as SDL3 wasn't found; set SDL3_DIR to SDL-xenon's build
...
[2/2] Linking C executable title
```

They land in `build/samples/<sample>/<sample>.xex`:

| Sample | What it shows | Ends on |
|---|---|---|
| `hello` | the log, the display and a controller: the screen's colour follows the sticks and triggers, and A rumbles | Back |
| `triangle` | GLSL shaders, vertices in GPU memory, a draw, a resolve and a present: a triangle whose colours move round once a second | Back |
| `cube` | an indexed, textured cube with depth, its matrix in shader constants, turned by the left stick | Back |
| `sound` | a tone fed through `xsdk_audio_*` from a thread of its own, its pitch on the triggers, the logo left up | Back |
| `sdl3` | SDL3's gamepad and audio beside xsdk's drawing; its gamepad check fails unless a controller is connected as it starts | Back |
| `template` | the start of a title of your own: it clears the screen; built as `title.xex` | Back |

`sdl3` needs SDL3 built for xsdk: SDL-xenon, built as its `docs/README-xsdk.md` says, then
`make samples SDL3_DIR=<SDL-xenon>/build-xsdk`. `make sample_breaks` builds each sample but the template broken on
purpose, into `build/samples/<sample>_brk/`, so its check fails (each sample's `SAMPLE_BREAK` says how).

## 6. Running a title on the console

Copy the `.xex` to the console over FTP, into a folder of its own: the title's log goes beside it. Debian's files are
at `\\wsl.localhost\Debian\home\<you>` from Windows. Any FTP client does; Windows has `curl.exe`. In PowerShell, with
the console's address, the user name and password its dashboard's FTP server takes, and the drive as that server names
it, `Hdd1` here for the hard disk:

```
curl.exe -T \\wsl.localhost\Debian\home\<you>\xsdk\build\samples\hello\hello.xex --ftp-create-dirs `
  ftp://<console>/Hdd1/xsdk/hello/ --user <user>:<password>
```

curl prints its transfer table, then nothing more when the copy worked. Then launch `Hdd1/xsdk/hello/hello.xex` from
the dashboard's file browser. Every sample shows the vulkan4xenon logo first, then does what the table above says,
until Back. Back ends it, and the console goes back to the dashboard. Leaving through the Guide's Xbox Home ends it
too, but before it writes its last lines; and a title that ends by itself while the Guide is open hangs (TODO.md, "The
Guide, beyond milestone 37").

## 7. Reading the log

Each title writes its log beside its `.xex`, named after it: `hello.xex` writes `hello.log` (docs/HARDWARE.md, "Each
title's log is named after its .xex"). It can be read while the title runs (docs/HARDWARE.md, "The log, read while
it's open, milestone 27"):

```
curl.exe ftp://<console>/Hdd1/xsdk/hello/hello.log --user <user>:<password>
```

xsdk writes the first lines and the last; between them is the title's own. hello writes:

```
log: game:\hello.log
log: named after \Device\Harddisk0\Partition1\xsdk\hello\hello.xex
...
hello: move the sticks and triggers to change the colour, hold A to rumble, press Back to end
hello: the display is 1280x720
hello: the first frame cleared to FF808000
PASS: hello: the first resolved frame holds the colour it was cleared to
hello: Back pressed, done
checks: 1 passed, 0 failed
exit: status 0
```

- **`PASS:` and `FAIL:`** lines are the title's checks of itself, and `checks:` counts them. A sample checks once what
  it can, such as a few pixels of its first frame, read back. That's how an assistant, which can't see the screen,
  knows a title works.
- **A call that fails** logs its name and its status, as `0x%08X`: `xsdk_present_open failed: 0xC00000BB` is
  `STATUS_NOT_SUPPORTED`, the statuses being NT's (`include/xsdk/kernel.h`).
- **`exit: status <n>`** is the last line of a title that ended; a log that stops before it hung or crashed there.
  `stdout` and `stderr` go to the log too.

The broken builds of section 5 each log a `FAIL:` line where the working one logs `PASS:`.

**Over the network, as the title writes it.** Put a `net_log.txt` beside the `.xex` holding your PC's address and a
port, such as `192.168.0.125 7371`, and run `tools/net_log.py` on the PC:

```
python tools/net_log.py --out net_log
```

Every line of the log also comes to the PC as it's written, from the first, and the script prints it and writes each
run to a file of its own in `net_log/`, byte for byte the log on the console. It goes over UDP, so the title never
waits on the PC: with nothing listening the title runs as it would without the file, and a line lost on the way is
reported as `lines <n> to <m> never came`. Allow UDP on that port through the PC's firewall. A line costs about 0.3 ms
to the file and up to 0.13 ms more over the network.

**DbgPrint** gets the log too only where a `dbgprint.txt` is beside the `.xex`, or the title calls
`xsdk_log_set_dbgprint(true)`: it reaches a kernel debugger or Xenia's `xenia.log`, and on the console it cost 3 to
7 ms a line.

## 8. A title of your own

A title links `~/xsdk/build/libxsdk.a`, so build xsdk first, as section 5 does, with the same toolchain. Then copy the
template out of xsdk and build it there:

```
cp -r ~/xsdk/samples/template ~/mytitle
cd ~/mytitle
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$HOME/xsdk/cmake/xsdk.cmake -DXSDK_TOOLCHAIN=$HOME/xsdk-gcc \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

`XSDK_TOOLCHAIN` is your `PREFIX`; left out, it's build.sh's default one. A build folder keeps the compiler it was first
configured with, whatever `XSDK_TOOLCHAIN` says after; to change toolchains, configure again with `cmake --fresh`.

```
-- Build files have been written to: /home/<you>/mytitle/build
[1/2] Building C object CMakeFiles/title.dir/main.c.obj
[2/2] Linking C executable title
xenia_scan: _dtoa_r: 8204c2f8 rldic r10,r9,32,1
xenia_scan: 3 instructions Xenia skips; Xenia results through these functions are unreliable
```

That's `build/title.xex`, which runs as section 6 says and logs as section 7 does, to `title.log`. Rename the target
in `CMakeLists.txt`, and the `.xex` and the log take its name. `AGENTS.md`, which `CLAUDE.md` imports, tells an
assistant working in the folder how to build the title, check it, and stay clear of docs/PITFALLS.md. GLSL and HLSL
shaders join the build through `xsdk_add_shaders`, as `samples/triangle/CMakeLists.txt` adds its GLSL and
`tests/cmake/CMakeLists.txt` its HLSL; docs/GUIDE.md goes through the rest, a task at a time.

## 9. Xenia, as a smoke check

Xenia Canary runs a `.xex` on the PC, which catches a crash at start-up or a call that never returns before a trip to
the console. It isn't the console: its kernel is written at a high level, it skips the `.xex`'s checks, and it gets
several things wrong. The console decides (CODE_GUIDELINES.md, section 12). Copy the `.xex`, and anything it
reads beside it, to a Windows folder, then from PowerShell:

```
xenia_canary.exe C:\xsdk\hello\hello.xex
```

Xenia stays open after the title exits; close it. The title's log is in Xenia's `cache` folder, named after the
`.xex` (`cache\hello.log`), and `DbgPrint`'s output in Xenia's own log, `xenia.log`. For
titles that read back what they draw, add `--readback_resolve=full`, so the CPU sees what resolves write, and
`--async_shader_compilation=false`, or Xenia skips draws while it compiles a shader.

Where Xenia differs from the console, so a check passes or fails in Xenia alone (xenia is xenia-project/xenia
`95a5c3e`, in `xenos-toolchain/reference/xenia`):
- **No display controller,** so `xsdk_splash_show` and `xsdk_present_open` fail with `0xC00000BB`. The samples that
  draw log that and end; sound plays on without its logo.
- **Instructions it doesn't implement are skipped,** silently (xenia:src/xenia/cpu/ppc/ppc_emit_*.cc,
  `XEINSTRNOTIMPLEMENTED`); the build's `xenia_scan` lines name the functions holding them, and anything through those
  functions is unreliable in Xenia.
- **A function's end is found by scanning,** and a forward branch makes Xenia take everything up to its target as one
  function (xenia:src/xenia/cpu/ppc/ppc_scanner.cc:204). xsdk's toolchain turns off tail calls and hot and cold
  splitting for it, and `src/xsdk.ld` puts the import stubs first; code built another way can run the wrong code
  there.
- **The time base runs at 50 MHz,** the rate the kernel reports, not the console's 49.875 MHz (docs/HARDWARE.md, "The
  time base runs at 49.875 MHz").
- **Each thread runs on a host thread of its own** (xenia:src/xenia/kernel/xthread.cc:378), so two busy threads placed
  on one hardware thread don't share it.
- **Files ignore share access** (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:102-145), and renaming renames
  nothing (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io_info.cc:202-251).
- **`MmQueryStatistics` guesses** reserved bytes and counts free pages in its physical heaps only
  (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:500-525).
- **GPU registers hold what was last written** (xenia:src/xenia/gpu/graphics_system.cc:181-205), so the GPU's clock and
  its performance counters don't count.
- **DNS lookups are a stub** (xenia:src/xenia/kernel/xam/xam_net.cc:542-560), so `getaddrinfo` finds no name.
- **A shader's loops run pixel by pixel** (xenia:src/xenia/gpu/spirv_shader_translator.cc:567-614, 986-1002), so
  derivatives and the GPU's level of detail inside a loop whose pixels leave it at different passes differ from the
  console's.
- **Memory alignment isn't checked,** so a float load at an unaligned address, which faults on the console, runs
  (docs/HARDWARE.md, "Dusklight runs the title scene").
