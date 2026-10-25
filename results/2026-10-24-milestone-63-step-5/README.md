# Milestone 63, step 5, on the console (2026-10-24)

The Homebrew App Store (xenosworks/homebrew-app-store, local) and store_check, built against xsdk with this commit's
tools, runs by the console agent against tools/net_http.py `--site` on Windows' own Python, serving the store's
tests/make_site.py site, signed with a test key. [SHA256SUMS](SHA256SUMS) has the logs' hashes. docs/HARDWARE.md,
"Installing, updating and removing, milestone 63", says what they found.

- [store_check](store_check/store_check.log): 21 of 21, step 4's 12 and step 5's 9. Alpha installs with its files and
  app.json and nothing else; its update in a package changed after signing is refused, the version installed staying;
  the right update replaces its files, deletes the one it dropped and keeps a file the app made; removal deletes
  app.json's files and app.json and keeps that file, and with everything takes the folder. Beta from a catalogue
  signing a wrong hash for one file is refused, nothing of it left; its download cut at half by `/site-drop/` is taken
  up with a Range for the other 134 of 268 bytes; and of the two cut installs laid out by hand, the one before its files
  were checked is taken back and the one after is finished.
- The fourteen cases of the store's tests/breaks.py, [one folder each](breaks/): each fails the checks its case names,
  among them three that follow from the break (step 4's README names two; `dropped_file_kept` leaves the dropped file,
  so the removal after it leaves more than the app's own file). [store_check after them](store_check_after_breaks/store_check.log):
  21 of 21.
- The store with `shots=1` and `apps=apps`: [its log](store/homebrew_app_store.log), alpha's page
  [before](store/shot_page.png) and [once installed](store/shot_installed.png), its button saying Install and then
  Launch, and beta's with [the word](store/shot_warning.png) before an app not by xenosworks first installs.

Two first runs, not kept, showed faults in the check, the store doing right: the cut download was tried from a kept
catalogue whose serial was above drop's, so drop's was refused as older and the download wasn't cut; and badupdate's
package was refused for its size before its hash could be. The check now forgets what's kept first, and badupdate's
package has one byte changed at its size.
