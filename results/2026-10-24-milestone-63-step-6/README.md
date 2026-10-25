# Milestone 63, step 6, on the console (2026-10-24)

The Homebrew App Store (xenosworks/homebrew-app-store, local) and store_check, runs by the console agent against
tools/net_http.py `--site` on Windows' own Python, serving the store's tests/make_site.py site with xsdk's
`build/launch_target.xex`, store_check 0.0.0.2 and the store 0.2.0.0 in it, signed with a test key.
[SHA256SUMS](SHA256SUMS) has the logs' hashes. docs/HARDWARE.md, "Launching, and the store updating itself, milestone
63", says what they found.

- [store_check](store_check/store_check.log): 23 of 23 over three starts. The first runs step 4's and 5's 21, installs
  launch_target and launches it with its own path as launch data; launch_target launches it back with the data
  reversed, and the second start finds its own path there, then installs store_check 0.0.0.2 over the .xex it runs
  from and starts it; the third, 0.0.0.2, finds itself so. A state file carries the checks from start to start; the log
  is the third's.
- The seventeen cases of the store's tests/breaks.py, [one folder each](breaks/), each run to the end of its starts
  before the next: each fails the checks its case names. `launch_data_dropped` launches launch_target with no data, so
  launch_target can't come back ([its log](breaks/launch_data_dropped/launch_target.log)) and store_check's log ends at
  the launch, with no tests line. [store_check after them](store_check_after_breaks/store_check.log): 23 of 23, the
  state that case left ignored, as a start saying launched with no launch data is a run that never came back.
- The store, launching from its screens with `shots=1`: [its log](store_launch/homebrew_app_store.log) to launching
  launch_target, [launch_target's](store_launch/launch_target.log), given the store's path, and
  [the store's](store_launch/homebrew_app_store_back.log) when launch_target started it again.
- The store updating itself from 0.1.0.0 to 0.2.0.0: [the log](store_update/homebrew_app_store.log) of 0.2.0.0,
  started again by 0.1.0.0 after installing it, and [its page](store_update/shot_installed.png), Running.

First runs, not kept, showed faults in the checks, the store doing right: store_check 0.0.0.2, the update, is built as
it should be, so checks run after the update tested none of a break, and the store's shots ran into an empty category
in the launch site. Everything but the update's own check now runs before the launch, and the site's real apps sit in
Tools alone.
