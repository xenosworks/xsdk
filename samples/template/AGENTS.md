# This title

An Xbox 360 title built with xsdk, which makes `.xex` files for the retail kernel and dashboard of an RGH or JTAG
console. `<xsdk>` below is xsdk's checkout, built with `make` as its docs/GETTING_STARTED.md says. Its docs/GUIDE.md
has the calls by task, docs/PITFALLS.md what breaks a title on the console and not on a PC, and samples/ titles that
work.

## Building

In WSL Debian, from this folder (from PowerShell, `wsl.exe -d Debian --exec bash -c 'cd <folder> && ...'`, as Git
Bash rewrites absolute paths):

```
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=<xsdk>/cmake/xsdk.cmake -DXSDK_TOOLCHAIN=<prefix> \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

`<prefix>` is where xsdk's toolchain is installed, the `PREFIX` its `tools/toolchain/build.sh` took; left out, it's
`~/xsdk-gcc`, that script's default. A build folder keeps the compiler it was first configured with: to change
toolchains, configure again with `cmake --fresh`.

That makes `build/title.xex`, named after the CMake target. Code builds with `-Wall -Wextra -Werror`; keep it free of
warnings. GLSL and shader model 3 HLSL shaders go through `xsdk_add_shaders(title a.vert a.frag b_vs.hlsl b_ps.hlsl)`,
which writes `title_shaders.h` (<xsdk>/docs/GUIDE.md, "Shaders and their constants"); HLSL needs glslang-xenon
(<xsdk>/docs/GETTING_STARTED.md, section 4).

## Running and checking

You can't run the title: the user copies the `.xex` to the console over FTP, launches it from the dashboard, and sends
back its log, `<title>.log`, which xsdk writes beside the `.xex`. Ask for a run when a change needs the console to
settle it, and say what the user should see and do.

The log is the only way to see what the title did:
- each check writes `PASS: <what>` or `FAIL: <what>`, then `checks: <n> passed, <m> failed`;
- every call that can fail logs its name and status as `0x%08X` when it does;
- `exit: status <n>` is the title's last line; a log that stops before it ended in a hang or a crash.

A change is done when the console's log shows its checks passing. Make each check able to fail: build it broken on
purpose once and see it fail. Xenia Canary is a crash and smoke check only: it has no display controller, so
`xsdk_present_open` fails there, and <xsdk>/docs/GETTING_STARTED.md, "Xenia", lists what else it gets wrong.

## Rules the console enforces without a word

Each breaks the title on the console and works on a PC or in Xenia (<xsdk>/docs/PITFALLS.md has the symptoms and the
evidence):
- **The CPU is big-endian.** Data made on a PC, in files or arrays, needs its byte order swapped.
- **GPU memory takes volatile 32-bit stores.** Memory from `xsdk_gpu_alloc` is write-combined: a 64-bit store there
  that isn't 8-byte aligned faults.
- **After the CPU writes vertices, indices or texels, `xsdk_gpu_invalidate` them,** or the GPU draws what its caches
  held before.
- **A resolve undoes the target, the depth test, the shaders and texture slot 0:** set them again after it, each
  frame, and a shader's literals after each `xsdk_gpu_set_shaders`.
- **Shaders are GLSL or HLSL in xost's subset:** no bitwise operations, no `switch` and, in HLSL, no early `return`;
  `sin` and `cos` near -pi to pi only (<xsdk>/xenos-toolchain/docs/xsc.md, "HLSL"). The build says what it refuses.
- **Each `xsdk_gpu_alloc` takes at least 64 KiB;** allocate once and split the memory, not per object, starting each
  vertex stream and index list on 4 bytes and each texture on 4 KB.
- **A sleep takes up to a millisecond more than asked,** 2 ms for 1 ms; pace frames with `xsdk_present_begin`, not
  sleeps.
- **Files open sharing nothing:** a second open of a file already open fails.
- **A mutex locked after its memory is freed corrupts the heap.**

## Code

- C17 (or C++23), xsdk's headers in `<xsdk>/include/xsdk/`; the title starts at `main`, after xsdk has opened the log.
- Check every call that can fail, and log it with its status as `0x%08X`.
- Facts about the kernel, the GPU or the `.xex` come from xsdk's docs, Xenia or libxenon, never from Microsoft's SDK or
  leaked material.
