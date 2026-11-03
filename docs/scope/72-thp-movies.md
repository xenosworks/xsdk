# Milestone 72: GameCube THP movies decoded natively

Proposed on 2026-11-03, at the user's request, beside milestone 71's goal of full speed.

## Why

Milestone 71's console run plays Colosseum's opening movie at 18-22 VI fields a second, a third of full speed, with
85% of the CPU thread in the game's own code: the movie decoder's (results from 2026-11-02, local/m71/run2). The FIFO
thread (71's step 3a) takes nothing off that, and cross-block optimisation (step 3) won't make it three times faster.
Run natively, a 640x480 frame is a few milliseconds of the console's time, against the 33 ms a frame the movie has.

## What there is

- **The decoder** is the SDK's THP library, linked into the game: Colosseum's DOL names it at 0x803751F0, "Dolphin
  SDK - THP release build: Aug 27 2002". Its frame decode starts at 0x801E5548 and calls the decompression at
  0x801E6578. That function sets GQR5 and GQR6 for its paired-single work in the locked cache, then picks a row routine
  by size: 512x448 (0x801E6684), 640x480 (0x801E810C) or any size (0x801E9B98). The Huffman decoding is at
  0x801EB644 and 0x801EBCC0 (Colosseum's map, local/m71/GC6E01.map.txt).
- **The movies:** Colosseum's disc has five, all THP 1.1 at 640x480 and 29.97 fps:
  - gs_logo (130 frames)
  - tpc (75 frames, with audio)
  - openingdemo (1,926 frames, with audio)
  - autodemo01 (2,700 frames)
  - staffroll (5,250 frames)

  Only movies use it; the game's own rendering doesn't.
- **The format** is public: the container is in ffmpeg's libavformat/thp.c, and each frame is a baseline JPEG that
  ffmpeg decodes as MJPEG (libavcodec/mjpegdec.c, `AV_CODEC_ID_THP`).
- **Taken from Colosseum's own code, not from any SDK material:**
  - the decoder's arguments and result;
  - the layout of its Y, U and V output in RAM (tiled as GX textures or not);
  - the work area it uses;
  - its arithmetic.

## The plan

1. **Match the decoder on the PC.** Work out from the code what the frame decode takes and leaves. Write a native
   decoder for it in C++, and a host harness that runs the game's own decoder on M68's interpreter over the same
   frames. Done when every frame of the five movies decodes to the same Y, U and V bytes both ways. If the game's float
   IDCT can't be matched exactly, the measured difference and why go here first.
2. **Hook it.** At the frame decode's entry, the interpreter, the JIT and the static image call the native decoder
   instead, with the same result in r3 and the guest's time charged as a short call.
   - gcmap finds the entry by its code's bytes, which every game built with this THP release shares, and the per-game
     hints file can name it instead.
   - Done when Colosseum's movies play on the console through it, the log counts its decodes, and a console check
     comparing it with the game's decoder over a movie's frames passes, while a deliberate break fails it.
3. **Make it fast.** Use VMX where it wins (the IDCT, colour and tile stores), measured on the console. Done when the
   opening movie plays at 60 fields a second, or the limit left is named with the console's numbers.

## Checks

- The PC harness: the native decoder against the game's own, on every frame of the player's movies, with a deliberate
  break failing.
- The console check from step 2, and the opening movie's fields a second in the log.

## Not in it

THP audio, which is ADPCM and cheap (measured first), other movie formats, other THP releases until a game needs one,
and the Wii.

## Size

Medium: step 1 is most of it.
