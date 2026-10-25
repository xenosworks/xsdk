# Milestone 63, step 4, on the console (2026-10-24)

The Homebrew App Store (xenosworks/homebrew-app-store, local) and store_check, built against xsdk with this commit's
tools, runs by the console agent against tools/net_http.py `--site` on Windows' own Python, serving the test site of the
store's tests/make_site.py, signed with a test key. [SHA256SUMS](SHA256SUMS) has the logs' hashes. docs/HARDWARE.md,
"The store's catalogue and screenshots, milestone 63", says what they found.

- [store_check](store_check/store_check.log): 12 of 12. The catalogue: a signed one taken and kept; one with a byte
  changed, one signed with another key and one older than the kept one each refused, the kept one shown; with nothing
  at its address the kept one shown, and none when none is kept; a signed new address followed. Screenshots: fetched,
  scaled to 768x432, and kept, the kept one coming back with the site out of reach; one not the signed one refused.
  Installed states from an app's `.xex`: current, an update ready, and another title ID not the app.
- The seven cases of the store's tests/breaks.py, [one folder each](breaks/): each fails the checks its case names,
  among them two that follow from the break: `serial_unchecked` keeps the older catalogue, so the offline check finds
  serial 4, and `kept_cut_short` keeps nothing, so no new address is followed.
  [store_check after them](store_check_after_breaks/store_check.log): 12 of 12.
- The store with `shots=1`: [its log](store/homebrew_app_store.log) and its three screens, the
  [categories](store/shot_categories.png), a category's [apps](store/shot_apps.png) and an
  [app's page](store/shot_app.png); with nothing at the catalogue's address, the kept catalogue and screenshot
  [offline](store_offline/shot_apps.png).

A first store_check run of the breaks, not kept, showed a fault in the check: a run with screenshot hashes unchecked
kept beta's wrong screenshot under the signed hash, and the next run took it, as store_check cleared alpha's alone. It
now clears both, and every run above is from after that.
