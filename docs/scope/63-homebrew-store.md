# Milestone 63: a homebrew store

**Agreed** on 2026-10-24 (Decided, below). The user, on 2026-10-24: write up a scope for the store, and answer two questions, whether
we should figure out compressed `.xex` files, and how version tracking works. The look is agreed: the "Stage" mockup
with a category list that slides into an app list, and an app's own page with its screenshot
(memory: homebrew-store-design).

## What there is

Milestone 56 laid the store's networking (docs/scope/56-store-networking.md):
- **curl-xenon** for HTTP and HTTPS on mbedtls-xenon, without zlib; a title passes `cacert.pem` as its CA bundle.
- **cJSON** for a catalogue.
- **`xsdk_launch(path, data, size)`** to start an app, and `xsdk_launch_data` for an app to find its way back.
- **`xsdk_net_link` and `xsdk_net_address`** to say there's no cable, and show the console's address.
- **A safe download:** `<name>.part`, `fsync`, `rename` over the final name, a `Range` request to resume.
- **Measured rates** (docs/HARDWARE.md, "Milestone 56 on the console"): HTTPS is bound by its cipher, AES-256-GCM at
  about 1.9 MB/s, ChaCha20-Poly1305 at 5 to 8 MB/s with `SO_RCVBUF` raised to 255 KB. Written straight onto `hdd:` as it
  arrived, HTTPS fell to about 1 MB/s.

Milestone 57 brought Dear ImGui (imgui-xenon), which SwanStation's picker already draws with; its jewel-case screens
are the store's model for drawing by hand on a draw list.

## Compressed `.xex` files

**Yes, the cheap kind, in xsdk's packer, for every title.** tools/elf2xex.py writes the whole image raw: its file format
header is basic compression with one block of `len(image)` bytes and no zero run, so bss and a JIT's `.jit` pool go into
the file as zeros. Of SwanStation's 30.5 MB `.xex`, 25.8 MB is whole 4 KB pages of zeros.

The XEX format already covers this. Basic compression is a list of `{data_size, zero_size}` blocks, the zeros left out of
the file and filled in by the loader (xenia:src/xenia/kernel/util/xex2_info.h:345-348; read in
xenia:src/xenia/cpu/xex_module.cc:564-648). Writing a block for each run of data and each run of zero pages would take
SwanStation to about 4.7 MB, and xsdk_test from 10.4 MB to about 3.7 MB, 6.7 MB of it zero pages. It changes the packer only: nothing
new runs on the console. Whether the console's loader takes many blocks in an unsigned `.xex` is the console's to say,
and the first step asks it.

**Not LZX.** The format's other kind, normal compression, is LZX in hashed blocks (xex_module.cc:654-757). Xenia and
libmspack decompress it; no maintained open compressor writes XEX's framing that I know of, so it would be ours to
write. It would only shrink the `.xex` on the drive after installing, as a zip of an LZX `.xex` downloads no smaller,
and it compresses the `.xex` alone, where a package also holds an app's data files. After zero runs, deflate on the
download (below) gets SwanStation to 2.2 MB, the measure that matters for a download. LZX can be a packer feature later
if drive space ever counts, on small USB sticks say.

**Deflate for downloads.** A package is a zip, deflated, and the store unpacks it with miniz (MIT, one `.c` file, as
upstream ships it) as it downloads. Deflate unpacks far faster than the 1 to 8 MB/s HTTPS gives, so a download takes
about half as long as the files' size says.

## Version tracking

Three parts, each doing one job.

1. **The `.xex` says what it is.** The execution info header carries a version and the title ID
   (xenia:src/xenia/kernel/util/xex2_info.h:479-493); elf2xex.py writes the title ID and a version of 0. A `VERSION`
   argument to `xsdk_add_xex` (`--version 0.5.0.0` to elf2xex.py) writes the app's version there, packed as major,
   minor, build and fix in 4, 4, 16 and 8 bits, as elf2xex.py's `version()` packs import versions. The store reads the
   header of each `.xex` it finds, so it knows an app's version even when it was copied over FTP, not installed.
2. **The catalogue says what's current.** One JSON file lists each app: an id, its title ID, name, category, sentence,
   the version as a packed number to compare and a string to show, size, the package's URL and SHA-256, what it needs,
   its changes and its screenshot. An update is a catalogue version greater than the installed `.xex`'s, for the same
   title ID. The catalogue carries a serial number too, and the store refuses one older than the last it accepted, so an
   old catalogue can't roll apps back.
3. **The store remembers what it installed.** Each app folder holds `app.json` from the store: the id, version, the
   catalogue it came from, and every file it installed with its size and SHA-256. An update replaces and a removal
   deletes those files only, so what the app made itself, saves, settings or SwanStation's `games\`, stays.

**Trust:** the catalogue is signed, the package hashes are in it, so one signature covers every download. ECDSA P-256
with SHA-256, which Mbed TLS 3.6 verifies; its classic API has no Ed25519. The private key stays with you, off line; the
store has the public key built in. HTTPS keeps the download private, the signature keeps it honest even if the host is
not.

**Other people's homebrew** (Decided, 5). Anyone submits an app by pull request to the catalogue repository: its source,
its license, its release's package and a screenshot. You review it, merge it, and sign the catalogue on your own machine.
The signature then says the list is the one you published, with each package's exact hash, so a file can't be swapped
after review or by a host that's been broken into, for anyone's app. It doesn't say who wrote an app:
- each entry names its author; xenosworks's own apps say so, as do others';
- before an app not made by xenosworks installs for the first time, or after its author changes, the store says so and
  waits for A: "<App> is made by <author>, not xenosworks. xenosworks lists it but hasn't checked what it does, and
  isn't responsible for any damage to your console or your files." B goes back.

## Steps, proposed

Each done on a clean console run, as xsdk's milestones are.

1. **Probes on the console:**
   - a `.xex` packed with many basic-compression blocks loads and runs (xsdk_test, and SwanStation with its game list);
   - a running title can replace its own `.xex`, as the store must to update itself; if not, how it hands over;
   - a folder renames, for an app's folder replaced whole;
   - the rate from GitHub's release hosting with ChaCha20-Poly1305 offered first, and which cipher it picks.
2. **elf2xex.py:** zero runs as basic-compression blocks, and `VERSION`. Done when every title in the tree packs, the
   `.xex` sizes drop by their zeros, xsdk_test passes on the console, and Xenia still loads them.
3. **The catalogue and its tool:** tools/store_catalogue.py checks a catalogue against its schema, packs an app into a
   zip, hashes it, and signs the catalogue. The catalogue lives in a repository of its own (Decided, 2), served as a file;
   packages are each app's GitHub release assets, which redirect, as curl follows.
4. **The store title,** its own repository (Decided, 1), on xsdk, imgui-xenon, curl-xenon, mbedtls-xenon, cJSON, miniz
   and stb_image for screenshots:
   - the three levels of the mockup, drawn on ImGui's draw lists like SwanStation's picker;
   - the catalogue fetched, its signature checked, kept on `hdd:` for when the network is down;
   - screenshots fetched as the app is opened, scaled and kept.
5. **Install, update and remove:**
   - download into memory in large pieces and write them from another thread, as writing as it arrives halved the rate;
   - the zip unpacked into `<file>.part` files, each checked against the catalogue's hash, then renamed into place,
     `app.json` written last, so a cut install starts again cleanly;
   - removal deletes what `app.json` lists, and offers to delete the rest.
6. **Launching and coming back,** through `xsdk_launch`, the store's path in the launch data; and the store updating
   itself, as step 1 shows it can.

**Checks,** each broken on purpose before it counts:
- a catalogue with one byte changed is refused, and so is one older than the last;
- a package whose hash doesn't match is refused and leaves the installed version as it was;
- install, update and remove of a test app, served by tools/net_http.py from the PC, leave exactly the files `app.json`
  lists, plus the app's own;
- a download cut half way resumes, and an install cut half way finishes or rolls back on the next start;
- with no cable, the store shows what's installed from its kept catalogue.

## Decided

On 2026-10-24, all seven:
1. **The name is Homebrew App Store,** in a repository of its own under xenosworks, as SDL-xenon and curl-xenon are,
   that you create (homebrew-app-store, say).
2. **Hosting on GitHub, no domain of our own:** packages as each app's GitHub release assets, which GitHub gives "no
   limit on the total size of a release, nor bandwidth usage" (docs.github.com, "About releases"); the catalogue and
   screenshots on GitHub Pages at xenosworks's own address, well inside its 1 GB and soft 100 GB a month. Throttling for
   "significantly excessive" bandwidth (its Acceptable Use Policies, section 9) is unlikely for a small community's
   apps of a few MB (you, 2026-10-24). If the catalogue ever has to move, a signed field in it names the new address,
   which the store takes from then on.
3. **The key:** ECDSA P-256, held by you, its public half in the store.
4. **Packages:** zip with deflate through miniz, holding `.xex` files packed with zero runs (step 2).
5. **Whose apps:** anyone's, by pull request, reviewed by you; the catalogue signed whole; apps not by xenosworks
   marked as such, with the warning above before they first install.
6. **Where apps go:** `hdd:\apps\<id>\`, with USB drives later.
7. **LZX:** left out, as above.
