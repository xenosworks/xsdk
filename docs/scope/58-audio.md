# Milestone 58: audio

Agreed on 2026-10-21, with every recommendation taken (Decided, below).

The entry agreed with the user: a mixer on the render driver (port FAudio), then XMA decoding in hardware.
docs/PARITY.md has playing PCM as Partial, and XMA, the speaker configuration and the dashboard's volumes as No.
Ahead of all of it comes a problem you hear: Dusklight's audio crackles, even at a steady 30 fps.

## Why

What a title has today:
- **PCM through the kernel's render driver, raw.** `XAudioRegisterRenderDriverClient`,
  `XAudioUnregisterRenderDriverClient` and `XAudioSubmitRenderDriverFrame` (0x1F3 to 0x1F5, imports/imports.txt;
  include/xsdk/kernel.h:182-186, 429-431). The driver calls back once a frame, 256 samples of 6 channels of big-endian
  float at 48 kHz, 376 times in 2000 ms (docs/HARDWARE.md, "Runtime, milestone 2"). Every user writes its own client:
  tests/test_audio.c, tests/audio_check.c, samples/sound and SDL-xenon each keep their own frames, driver word and
  silence.
- **SDL3's audio, through SDL-xenon's xsdk driver** (SDL-xenon:src/audio/xsdk/SDL_xsdkaudio.c):
  - a ring of 4 frames, at most 3 queued ahead of the one playing, 768 samples or 16 ms (lines 41-42, 111-119);
  - SDL's device thread refills it, polling with `SDL_Delay(1)` while it's full (111-119), and a 1 ms sleep takes
    about 2 (docs/HARDWARE.md, "Sleeps, write speeds, and a set-up that wouldn't run from a cached stream");
  - the kernel's callback takes the oldest frame, or a silent one when SDL is behind (57-73). Nothing counts those;
  - since milestone 55 the thread asks for NORMAL, as polling at SDL's 16 cost Dusklight 2.5% (158-165;
    docs/HARDWARE.md, "M55 acceptance").
- **No mixer in xsdk.** SDL's streams mix, convert and resample. Code written against XAudio2, which retail titles used
  (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_audio_xma.cc:57-59, xenia:src/xenia/apu/audio_system.cc:25-29), has
  nothing to call.
- **No XMA, no speaker configuration, no dashboard volumes** (docs/PARITY.md, "Audio").

**Dusklight crackles at a steady 30 fps.** Nobody has measured why. What its code shows:
- It renders its audio inside SDL's get-callback (dusklight-xenon:src/dusk/audio/DuskAudioSystem.cpp:84-85, 153-164),
  which SDL calls on its device thread when the stream runs short (SDL-xenon:src/audio/SDL_audiocvt.c:1393;
  src/audio/SDL_audio.c:1175-1290). The game's DSP emulation runs on the thread that must refill the ring.
- Each call renders 10 subframes of 80 samples: 800 samples, 16.7 ms of sound (DuskAudioSystem.cpp:166-181;
  libs/JSystem/src/JAudio2/JASAiCtrl.cpp:104-114; libs/JSystem/include/JSystem/JAudio2/JASDSPInterface.h:20). One
  burst is longer than the ring's 768 samples. It renders under a recursive mutex the game's threads take too
  (src/dusk/audio/JASCriticalSection.cpp:7-15).
- SDL names its device thread `SDLAudioP` and a number (SDL_audio.c:1781-1783), which isn't on Aurora's list
  (dusklight-xenon:extern/aurora/lib/thread.cpp:295-300). So it runs where its creator does (docs/PARITY.md, "Placing
  threads"), likely beside the game's main thread on hardware thread 0
  (results/2026-10-21-milestone-55-acceptance/dusklight/sdl.log). Two busy threads on one hardware thread take turns,
  neither getting more than 60% of its work done (SCOPE.md, milestone 26).

That gives three likely causes, none measured: the device thread waking late beside a busy thread; a ring shorter than a
burst plus the time to render it; or Dusklight's render, or its mutex, taking too long. Step 1 measures which, with
nobody listening.

What Xenia shows, little of it the console's (under xenia:src/xenia/; ordinals from
kernel/xboxkrnl/xboxkrnl_table.inc):

| What | Exports | Xenia |
|---|---|---|
| The render driver | 0x1F3 to 0x1F5 | apu/audio_system.cc:88-147, 171-227: up to 8 clients (audio_system.h:78), called back on one worker thread; registering runs 64 callbacks at once (audio_system.cc:179, audio_system.h:68), then one a frame played; a frame is copied as it's submitted (apu/xaudio2/xaudio2_audio_driver.cc:192-232) |
| Speaker configuration | `XAudioGetSpeakerConfig` 0x1FF | kernel/xboxkrnl/xboxkrnl_audio.cc:22-26, always 0x00010001 |
| The dashboard's volumes | `XAudioGetVoiceCategoryVolume` 0x1F8, `XAudioGetVoiceCategoryVolumeChangeMask` 0x1F7 | xboxkrnl_audio.cc:28-50, stubs giving 1.0 and no change; the first argument unnamed |
| Underruns and timing | `XAudioGetUnderrunCount` 0x35A, `XAudioGetRenderDriverTic` 0x34C, `XAudioQueryDriverPerformance` 0x1FD | no code; nothing public gives their arguments |
| XMA | `XMACreateContext` 0x224 to `XMAGetInputBufferReadOffset` 0x238, 21 exports | kernel/xboxkrnl/xboxkrnl_audio_xma.cc; contexts in a kernel array the XMA block at 0x7FEA0000 reads (apu/xma_decoder.cc:112-128), decoded in software through FFmpeg (apu/xma_context.h:33-37) |
| Waking a thread from a callback | `KeSetEvent` 0x9D | kernel/xboxkrnl/xboxkrnl_threading.cc:406-420 |

Xenia runs each title thread on a host thread of its own (CLAUDE.md), so it can't show a feeder starved beside a busy
thread either.

What FAudio is (github.com/FNA-XNA/FAudio, zlib licence, 26.10 on its master branch):
- XAudio2, X3DAudio, XAPO and XACT3 reimplemented in C, targeting XAudio 2.8 (include/FAudio.h,
  `FAUDIO_TARGET_VERSION`). It depends on SDL 3.2.0 or later alone (its README); SDL-xenon is SDL 3.4.10.
- Its SDL3 platform layer (src/FAudio_platform_sdl3.c) opens an SDL audio stream in float at the mastering voice's
  channels and rate, and mixes 10 ms a call (`spec.freq / 100`) in SDL's get-callback, on SDL's device thread. The
  rest of the layer is threads, mutexes, sleeps, a clock and file reads through SDL. Off Windows, src/FAudio_internal.h
  maps memory, maths and byte swaps to SDL's too.
- Its voices take PCM of 8 to 32 bits, float and MS-ADPCM (FAudio_internal.h); XNA's songs play Ogg Vorbis through
  stb_vorbis (src/XNA_Song.c); WMA only on Windows (src/FAudio_platform_win32_wmadec.c); no XMA.
- It mixes with SSE2 or NEON where it finds them and in scalar code otherwise (FAudio_internal.h,
  `FAudio_INTERNAL_InitSIMDFunctions`), so scalar code on Xenon.
- 16-bit samples go straight to float (FAudio_internal.c, `FAudio_INTERNAL_DecodePCM16`), so as far as its code shows
  a voice takes them in the host's byte order, big-endian on the console.
- FACT detects a bank's byte order and converts it for a little-endian host (src/FACT_internal.c, its `se` flags).
- A title can wrap the engine's processing with `FAudio_SetEngineProcedureEXT` (FAudio.h), and see each 10 ms of the
  mix before it goes out.

## What it takes

Steps 1 and 2 measure on the console before anything is built for audio; the rest follows from what they find.

1. **Measure the crackling,** on the console, with nobody listening. Diagnostic builds stay in local/m58, uncommitted,
   as milestone 55's Dusklight runs did:
   - **SDL-xenon's driver** counts from the first frame SDL queues:
     - silent frames handed to the kernel;
     - the fewest frames queued when the kernel took one;
     - how late each refill came, from the callback that freed a slot to the frame queued in it, by the time base;
     - how long each `SDL_Delay(1)` took;
     - samples past ±1.0 in what it queued;
     - the hardware thread its device thread ran on.
   - **Dusklight** times each `GetNewAudio`, its 10 subframes, and its waits for the JAS mutex.
   - Dusklight's title scene runs as in milestone 55, 32 windows of 10 s, and each window's line adds these numbers to
     its frame rate (results/2026-10-21-milestone-55-acceptance/README.md, "Dusklight and sdl_check").

   The numbers say which cause it is:
   - refills late while renders are short: the device thread is scheduled late;
   - renders about as long as the ring: the ring is too short, or the render too slow;
   - long mutex waits: Dusklight's own threads hold it up;
   - no silent frames but clipped samples: the crackle is in the samples, not their delivery.

2. **A probe of the render driver and of waking a feeder,** `tests/probe_audio.c`, run in the same session as step 1.
   It settles:
   - **where the callback runs:** from inside it, the hardware thread, the current thread and the DPC flag from the
     processor control region (offsets 0x10C, 0x100 and 0x150, xenia:src/xenia/kernel/xthread.h:73-86), only read;
   - **how far ahead the kernel takes frames:** callbacks in the first 50 ms after registering, where Xenia runs 64
     at once, and the shortest and longest gap between callbacks over 1 s;
   - **a second client:** whether the kernel registers another while one is registered, and calls both;
   - **the kernel's time slice:** two busy threads at one priority on hardware thread 1 for 1 s, and the longest each
     goes without running;
   - **waking a feeder beside a busy thread:** a busy thread on hardware thread 1 for 1 s, and beside it a feeder that
     wakes each frame, by a 1 ms sleep and then by milestone 55's periodic timer at 5 ms. Each runs at the default
     priority and at +1, +2 and 16 (the levels milestone 55 found, docs/GUIDE.md, "Threads and where they run"). The
     probe logs how late the feeder woke at most, and what the busy thread lost;
   - **waking it from the callback,** last, logged before it runs, with `net_log.txt` (docs/HARDWARE.md, "The log over
     the network, milestone 53"). The callback sets an event the feeder waits on:
     - by `KeSetEvent` on the event's object (0x9D, the object from `ObReferenceObjectByHandle`) if the DPC flag was
       set;
     - by `NtSetEvent` if not.

     Then the probe measures the same wake-ups. If the kernel won't take a signal from there, the console may stop; you
     restart it, and the log is on the PC already. **This case runs only with your go-ahead.** If it fails, the feeder
     waits on the periodic timer instead.
   - **the speaker configuration:** `XAudioGetSpeakerConfig`'s word, logged (Decided, 5).

3. **An xsdk audio API,** include/xsdk/audio.h and src/audio.c, over the render driver, one client a title:
   - `xsdk_audio_open(frames_ahead)` registers the client. It allocates its frames once, in physical memory: the ring,
     the frame the kernel holds, and silence. They're never freed, as a callback can still be running when
     unregistering returns (SDL_xsdkaudio.c:29-31; xenia:src/xenia/apu/audio_system.cc:210-227).
   - The callback does no floating point (docs/GUIDE.md, "Input and audio through the kernel"). It hands the kernel
     the oldest queued frame or silence, counts, and wakes the feeder the way step 2 found works.
   - `xsdk_audio_next_frame(timeout_ms)` blocks until a slot is free, and gives the frame to fill: 6 channels of
     big-endian floats, channel after channel. `xsdk_audio_queue_frame()` hands it over. The feeder sleeps until the
     kernel has taken a frame; nothing polls.
   - `xsdk_audio_stats(&stats)` gives the frames played, silent frames since the first one queued, the fewest frames
     queued at a callback, and the longest a freed slot waited. A title's log shows underruns without anyone listening.
   - `xsdk_audio_close()` unregisters, and a feeder waiting in `xsdk_audio_next_frame` returns.
   - The feeder is the title's own thread, and xsdk starts none. Its priority and hardware thread are the title's, with
     the defaults (Decided, 6).
   - tests/test_audio.c and samples/sound move onto it. tests/audio_check.c, the frame layout checked by ear, stays raw.

4. **SDL-xenon on the API:** `WaitDevice` blocks in `xsdk_audio_next_frame`, and `PlayDevice` lays SDL's mix out in
   the frame and queues it. The driver's own ring, silence and polling go. Its `ThreadInit` (SDL_xsdkaudio.c:158-165)
   sets what step 2 found keeps the device thread on time:
   - a raised priority, if that's enough;
   - otherwise another hardware thread, named by an SDL hint the title sets, as SDL's device thread is SDL's to start.

5. **The fix for Dusklight,** where step 1 points:
   - late wake-ups: steps 3 and 4, nothing in Dusklight;
   - a ring shorter than a burst: the default `frames_ahead` (Decided, 6);
   - a slow render or a held mutex: Dusklight renders ahead on a thread of its own and puts its audio into the stream
     there, under a target `#if` with the original kept (CODE_GUIDELINES.md, section 14).

   The scene must then hand the kernel no silent frame in its 32 windows, at a frame rate no lower than milestone 55's
   (144.9 and 145.5 over its heaviest five windows, results/2026-10-21-milestone-55-acceptance/README.md).

6. **FAudio,** in a new FAudio-xenon fork (Decided, 2): its latest release on a `xenon` branch, built through
   cmake/xsdk.cmake on SDL-xenon, on FAudio's own SDL3 platform layer (Decided, 3). It mixes on SDL's device thread, so
   it takes step 4's feeding as it is.
   - The fork changes only what the toolchain and a big-endian host need, each change marked, as SDL-xenon's are.
   - `faudio_check`, a check title in the fork, reads the mix through `FAudio_SetEngineProcedureEXT` and judges it by
     numbers: level per channel, frequency by zero crossings, and time spent mixing.
   - Effects (reverb, EQ, echo, limiter, volume meter), 3D (F3DAudio) and XACT build with it. XACT's banks go unchecked
     (Left out).

7. **Speaker configuration:** `XAudioGetSpeakerConfig` imported, and `xsdk_audio_speakers()` saying stereo or 5.1 from
   the bits step 2 read (Decided, 5). The driver still takes 6 channels, and on a stereo set the centre and back
   channels sound as a downmix allows (docs/HARDWARE.md, "Runtime, milestone 2"). A title that offers a stereo or a
   surround mix picks its default from it, as Dusklight's settings could (DuskAudioSystem.cpp:48-59).

8. **Docs:**
   - docs/GUIDE.md's audio section: the API, the feeder's priority and place, what the stats mean, and the byte order
     FAudio's voices take.
   - docs/PARITY.md:
     - "Playing PCM" to Yes, through `xsdk_audio_*`, which counts its missed frames;
     - a new row for mixing (voices, resampling, volumes, submixes, effects, 3D), Yes through FAudio-xenon;
     - the speaker configuration Yes, the dashboard's volumes No, as two rows;
     - XMA stays No, its own milestone named in Missing;
     - FAudio-xenon added to "Libraries ported to xsdk";
     - item 9 of the order: the mixer and speaker configuration done; XMA, the volumes and recording open.
   - docs/HARDWARE.md: step 1's cause and step 2's probe.
   - CLAUDE.md's Xenia note: its driver runs 64 callbacks as a client registers (audio_system.cc:179), and its threads
     each have a host thread, so the frame-count and busy-thread checks fail there.

## Left out

- **XMA decoding** (Decided, 4).
- **The dashboard's voice-category volumes and the ducker:** Xenia stubs them with an argument it can't name
  (xboxkrnl_audio.cc:28-53), and a guessed argument could fault in the kernel (Decided, 5).
- **Recording,** the microphone and headset: Xenia has stubs only (docs/PARITY.md, "Input").
- **Several clients at once,** whatever step 2 finds: one feeder a title, which SDL and FAudio share through SDL.
- **XACT banks checked:** making a bank takes Microsoft's XACT tool, so none is checked until a port brings its own.
- **VMX in FAudio's mixer,** unless `faudio_check`'s mixing cost goes over its limit; then it comes back to you.
- **The kernel's own underrun count and timings** (0x35A, 0x34C, 0x1FD): no code in Xenia and no public signature.
  xsdk counts its own.
- **Digital bypass, the echo-cancellation client and the frame-process callback** (0x1FA to 0x1FC, 0x320 to 0x322,
  0x36F): no code in Xenia.

## Verification

On the console, each check its own line, each 0.5 to 1 s, and each broken on purpose once: xsdk's through tests/breaks/,
the forks' through their own, as milestone 55's were.
- **xsdk_test** (tests/test_audio.c):
  - **Frames keep coming:** a feeder queues a tone for 500 ms. The kernel plays 94 frames ±2, none silent after the
    first. Broken: the callback hands over silence every tenth time.
  - **Underruns are counted:** the feeder stops for 53 ms, and the stats count 10 silent frames ±1. Broken: the count
    never rises.
  - **Beside a busy thread:** the feeder and a busy thread share a hardware thread for 1 s, at the feeder's default.
    No frame is silent, and the busy thread keeps the share of its work step 2 measured with a blocked feeder. Broken:
    the feeder polls with a 1 ms sleep, at the priority step 2 found late or costly.
  - **The ring's depth:** with `frames_ahead` 8, eight frames queue before `xsdk_audio_next_frame(0)` times out.
    Broken: one slot fewer.
  - **Closing:** `xsdk_audio_close` returns a waiting feeder at once, and no callback counts after 20 ms. Broken: the
    close doesn't wake the feeder, so its wait times out instead.
  - **Speakers:** `xsdk_audio_speakers` gives the setting step 2 mapped. Broken: the bits read from the word's other
    half.
- **sdl_check** (SDL-xenon): its tone (test/xsdk/sdl_check.c:202-225) plays with no silent frame by xsdk's stats, and
  its audio checks pass as before. Broken: the driver queues every other mix.
- **faudio_check** (FAudio-xenon):
  - **A voice:** a 375 Hz float sine at 48 kHz reaches front left and right at its level ±1%, the other four channels
    silent. Broken: the voice's volume ignored.
  - **Resampling and pitch:** a 44.1 kHz 16-bit voice comes out at 375 Hz ±1%, and at a frequency ratio of 2, 750 Hz.
    Broken: the source rate taken as 48 kHz.
  - **Byte order:** the same samples big-endian and little-endian; only the order the guide names plays at full
    level. Broken: the samples swapped.
  - **A submix with reverb:** the output stays finite, and its tail outlasts the dry sound. Broken: the effect chain
    dropped.
  - **3D:** an emitter on the listener's left puts more in front left than front right. Broken: the emitter's x
    negated.
  - **Its cost:** 32 voices resampled into 5.1 take under a quarter of each 10 ms on their hardware thread, logged, and
    the run hands the kernel no silent frame. Broken: the mix run four times over.
- **Dusklight's title scene,** once after the fix: no silent frame in its 32 windows, its frame rate no lower than
  milestone 55's.

xsdk_test passes as before otherwise. vulkan4xenon_test and vkbench aren't rerun, as nothing on the GPU's path changes.
Xenia is a smoke check only.

## Done when

- Dusklight's title scene hands the kernel no silent frame in a full run, at a frame rate no lower than milestone 55's.
- xsdk_test's audio checks, sdl_check and faudio_check pass on the console, each break failing.
- docs/HARDWARE.md records what caused the crackling and what the probe found; docs/GUIDE.md and docs/PARITY.md say
  what's there.

## Decided

On 2026-10-21:
1. **The render driver is fed** through an xsdk API whose feeder is the title's own thread, blocking until the kernel
   takes a frame and counting the silent ones (step 3), with SDL-xenon and samples/sound on it. Every user of the
   driver gets the same wake-up and the same counts, and the title keeps control of its threads, as milestone 26
   decided for placement. xsdk starting a hidden thread of its own that calls the title back, as SDL does, and no xsdk
   API, SDL-xenon's driver fixed alone, were the others.
2. **The mixer: FAudio, ported,** in a repository of its own under xenosworks, as SDL-xenon and mbedtls-xenon are. You
   create xenosworks/FAudio-xenon; it's pushed to only when you ask, after the work has passed on the console. It
   brings XAudio2's API, which retail titles used, so XAudio2 code and FNA-based ports build against it, with its
   effects, 3D and XACT; the cost is a fork to carry, mixing in scalar code. A mixer of xsdk's own, with no XAudio2
   API, so each port rewrites its audio onto it, was the other way. A port that already mixes for itself, as
   Dusklight does, needs neither.
3. **FAudio's platform layer** is its own SDL3 layer on SDL-xenon, unchanged: there's no platform code to write, a
   title using FAudio links SDL3, and FAudio mixes on SDL's device thread, fed as step 4 has it. A layer of xsdk's own
   on its audio API and pthreads was the other way; off Windows, FAudio_internal.h maps memory, maths and byte swaps to
   SDL as well, so those would change too, and the fork would drift from upstream.
4. **XMA is a milestone of its own,** later, that starts with how to make XMA data without Microsoft's tools. A probe
   of the context calls with no data in this milestone was the other way; it would show the calls don't hang, not that
   anything decodes. What that milestone starts from:
   - **No public encoder.** FFmpeg decodes XMA1 and XMA2 (libavcodec/wmaprodec.c) but can't encode them, and Xenia
     decodes through FFmpeg (apu/xma_context.h:33-37). Microsoft's encoder is out (CLAUDE.md), and retail data can't
     go in the repository (CODE_GUIDELINES.md, section 2). So that milestone needs an encoder of our own for XMA's WMA
     Pro-based format (xma_context.h:22-25), or a stream built by hand from the packets Xenia parses, 2 KB each
     (xma_context.h:135-140).
   - **What its probe must settle:**
     - whether the kernel's calls match Xenia's: the 56-byte setup with its work buffer, which Xenia ignores
       (xboxkrnl_audio_xma.cc:104-118, 176), and `XMADisableContext`'s wait argument (367-378);
     - the 320 contexts (apu/xma_decoder.h:83);
     - the output's byte order and channel layout, 16-bit samples by Xenia (xma_context.h:139);
     - the four rates, 24, 32, 44.1 and 48 kHz (apu/xma_context.cc:213-226);
     - the interrupt when a context is done, which Xenia doesn't do (xma_context.cc:384).
   - **The risk:** the decoder reads and writes physical memory by the addresses a context holds
     (xboxkrnl_audio_xma.cc:162-189), so a wrong context could corrupt memory or hang the console. Each risky case
     would run only with your go-ahead.
   - **Who needs it:** homebrew and ports from systems up to the GameCube and Wii bring PCM, ADPCM, Vorbis or Opus,
     which the CPU decodes. XMA matters to ports of 360 titles, whose data is XMA already.
5. **Speaker configuration in:** `XAudioGetSpeakerConfig` imported. Step 2 reads it with the dashboard as it is; you
   set the dashboard's audio output the other way once (stereo or Dolby Digital 5.1) for a second read, a minute of
   yours, so its bits are mapped. The voice-category volumes stay out. The configuration read raw only, with no
   mapping and no run of yours, and the volumes probed with guessed arguments too, were the others.
6. **The defaults** come from steps 1 and 2: the smallest ring that holds twice the longest gap Dusklight showed, and
   no fewer than 8 frames (43 ms), as stability comes before latency; a title may ask for fewer. The feeder is raised
   only if step 2 shows a blocked feeder at NORMAL waking late beside a busy thread. A blocked feeder costs only the
   frames it fills, where milestone 55's poll at 16 cost its sleeps' wake-ups too. 3 frames ahead, 16 ms, as
   SDL-xenon has now, and no default, each title choosing, were the others.
7. **Nobody needs to listen** to close the milestone: silent frames and clipped samples are the measure. You'll hear
   Dusklight when you next play it; if it still crackles with both at zero, that's a new finding to chase, not this
   milestone's failure.

## Size

About milestone 54's.
- **xsdk:** src/audio.c and include/xsdk/audio.h, a few hundred lines; tests/probe_audio.c, and test_audio.c's six
  checks with their breaks; one import; docs.
- **SDL-xenon:** its driver gets shorter.
- **Dusklight:** diagnostics kept local; a change only if step 1 points there.
- **FAudio-xenon:** a new fork, its build, the big-endian fixes it turns out to need, and faudio_check.
- **Console runs:** three sessions, the breaks run unattended through the agent:
  1. step 1's Dusklight run and step 2's probe, its last case with your go-ahead;
  2. the speaker read with the dashboard set the other way (Decided, 5);
  3. acceptance: xsdk_test, sdl_check and faudio_check with their breaks, and Dusklight once.

XMA as a milestone of its own is bigger than this one: its encoder or hand-built stream alone is more work than all of
the above.
