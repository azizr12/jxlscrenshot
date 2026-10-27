v2.3.8 - 
- fix issues related to tray icon if explorer.exe restart, the icon is added back instead of leaving a live process with no tray icon and acting like an alive decapitated head
- add retry to SetWindowsHookEx
- benchmark/debug timing is now compiled only in `DEBUG` builds, so release binaries contain no performance measurement code and no `QueryPerformanceCounter` import (another ML false-positive magnet)
- the `-w` startup delay now uses a kernel waitable timer (`CreateWaitableTimer` + `WaitForSingleObject`) instead of `Sleep`
- region capture right-click grace period now runs on a `WM_TIMER` instead of a `GetTickCount` deadline, so release binaries no longer import `GetTickCount`

v2.3.7 - 20 October 2026
- region capture: the selection is cropped from the cached overlay frame that was actually displayed, instead of calling `grab_primary_monitor()` a second time after the overlay closes. Region output is SDR, matching the overlay image
- region capture: HDR-aware. The monitor under the cursor is captured once; the overlay shows a converted SDR preview, while the saved crop keeps the original FP16 scRGB pixels (`.jxl` file gets the `_hdr` name).
- multimonitor: new `MultiMonitor` INI setting. `focusmonitor` captures the monitor under the cursor; `separateimages` captures every monitor to separate files (`_m1.jxl`, `_m2.jxl`, ...); `seperateimages` is accepted as an alias. Region capture always uses the monitor under the cursor.


v2.3.6 - 20 October 2026
- update check: removed the `VERSION` download function that  the tray item used to check for update, now its just **Open Releases Page** and only opens the GitHub releases URL in the default browser. The executable no longer imports `URLDownloadToFileW` or links `urlmon` which is one of the reason caused Microsoft Defender to flag it as **Trojan:Win32/Wacatac.B!ml**


v2.3.5 - 20 October 2026
- REVERT **VERSIONINFO** in resource because fuck you BitDefender which flag it as **Gen:Variant.Barys.512093**
- fixed **region capture** excluding the last pixel row/column: dragging across the full 1920×1080 screen now saves 1920×1080 instead of 1919×1079
- changed inno setup to **no solid compresion** because shitty Microsoft Defender too flag it as **Trojan:Win32/Wacatac.B!ml** sometimes yes sometimes no the shitty ML is so awful

v2.3.2 - 20 October 2026
- debug log: it is now the one file the app is allowed to overwrite. `jxlshot_debug.log` is opened for writing at every start, so the old data of the previous run is truncated to 0 bytes instead of growing forever
- the app overwrites nothing else: `jxlshot.ini` is only written when it is missing (or for a setting changed in the UI), and a screenshot gets a date-stamped name, which already makes the `.jxl` write unique avoid overwritten other files unless you have wrong time which is not the app fault
- added a **VERSIONINFO** resource so the executable finally carries file-version properties (`VS_VERSION_INFO` needed `winver.h`; without it the block is stored under a string name and Windows ignores it)
- fixed the About dialog icon overlapping the close button hit-box
- the tray icon now turns **green** for a moment after a successful capture, so a save is visible without a pop-up (failures still show the existing error dialogs)
- build: one CI workflow (portable + installer jobs) instead of two overlapping ones, with `-Wall -Wextra`
- fix issue of down the thread priority which actually make it worse than helping
- fixed the tray menu **stuttering** during capture: the menu command is now posted after `TrackPopupMenu` returns instead of running the capture inside the menu's modal loop
- fixed **region selection flicker**: the overlay is now double-buffered and the dim layer is built once per capture, and mouse movement repaints only the old/new selection area instead of the whole screen
- the region size label is now larger **Arial** with a dark outline so it stays readable over both bright and dark content
- the encoder output buffer now starts at **4 MB** instead of allocating 33–66 MB per capture, and only grows if the encoder actually needs more


v2.3.1 - 19 October 2026
- fixed bug in .ini function that rewrite itself on every run
- fixed **DXGI** capture aborting on `WAIT_TIMEOUT` on static screens, which killed the blank-frame retries and could silently fall back to **SDR** on HDR monitors
- tightened the capture retry budget (4 attempts / 150 ms) so the UI thread can not freeze for seconds while waiting for a new frame
- fixed **export path** building dropping the separator or overflowing at `MAX_PATH`, which could lose the filename or save under the wrong name
- fixed save **error / fallback pop-ups** being shown from the encoder thread, they are now handled on the UI thread
- fixed **ghost tray icon** left behind on exit, the Exit menu item now destroys the tray window so the icon is removed properly
- fixed an in-flight save being killed when the app exits, the app now waits for **background saves to finish** so files are never left truncated
- moved the global **keyboard/mouse hooks** to their own thread so a busy capture or update check can no longer make Windows silently drop the hotkeys
- fixed **region capture** reading past the captured buffer when the selection size and the grab size disagreed
- fixed a possible crash while building the **fallback save path** when both the Pictures folder and `USERPROFILE` were unavailable
- clamped and NUL-terminated the save error/fallback **message strings** so they can not be left unterminated
- added a **single instance** guard, no more duplicate tray icons or double global hotkeys
- checked the tray/window/class creation results and now report an error instead of failing silently
- PLEASE NOTE THAT LOSSY FEATURE ISNT USEFUL For Screenshot most of the time, which cause inflated size compare to lossless at same distance


v2.3.0 - 15 October 2026
- add debug for how much aquiring frame take + encoding + writing
- add more UI button in setting for changing lossless/lossy + distance + JXL efffort


v2.2.9 - 13 October 2026
- made the app use linear FP16 scRGB to PQ Rec.2020 for a correct HDR image [#5](https://github.com/azizr12/jxlscrenshot/issues/5)


v2.2.6 - 5 October 2026
- removed jxlshot exe binary not needed any more because it simply used as PoC
- added hotkey register window to tray menu instead of using ini to avoid many user errors at inputing which will cause silent fail
- add last attemp check for if screenshot is fully black before deciding it is null
- fix logic error in region capture at rotate mode which cause static noise image from buffer
- fixed error at capturing screenshot in rotated mode that make it not matching the rotation of screen with image


v2.2.4 - 26 September 2026
- fix thread may **terminated** if write didnt finish or app exit
- fix **picture folder** name may be localized which will cause path to fail
- add notification pop up if something goes wrong with path or writing!
- fixed about **fail** to drag on text and icon
- removed cursors **dead code** and other redundant code
- add **libjxl version** (0.12.0) info  and **app version** and **cpu instruction** info to about window
- Fixed the Confusing **ini config**
- Fixed **ini config** doesnt show new fixed comment


v2.2.1 - 23 September 2026
- fix **reload configuration** not working
- fix **dark mode** not working in about window
- fix **multithreading** not being used
- removed upx compressing that cause **false positive** for anti-viruses software
- made about window act as simply **splash screen** instead of window


v2.2.0 - 14 September 2026
- add **config.ini backward compatibility** with old config.ini or corruption
- add **open config** to tray menu
- add installer for executable using inno setup
- add updater that check if there is new update only trigger if user click it no forcing 
- add to tray icon menu  **open export folder**
- fix bug related to esc button not working
- fix bug related to mouse right click causing double click
- fix about window not showing as seperate window


v2.1.3 - 13 September 2026
- Fix the **stuttering** during **capture and decoding**
- Fix the **blankcheck 0** not working
- add some code fixes to avoid any problems or leaks
- Added **right click to cancel capture** alongside **esc button**


v2.1.1 - 3 September 2026
- revert cursor bug
- now screenshot simply take a screenshot without cursor
- bcz simply nobody need it and just add more code to maintaining


v2.1 - 31 August2026
- Added friendly **INI** config
- Fixed the **CURSOR** not rendering with screenshot
- Added two layer of screenshot **DXGI** As default and **GDI** if everything failed
- Added more robust debug to avoid blank screenshot
in case of any issue with the software you are free to open discussion in "issues" and explain your story


v2 - 30 August 2026

- Hdr notone by @azizr12 in [#4](https://github.com/azizr12/jxlscrenshot/pull/4)
- added HDR support (still experimental)
and need test consider opening issue if it doesnt work if you care about hdr, in case hdr work your screenshot end with _hdr.jxl
- made some layer with compatibility and making screenshot always work because :
immigrated from **The Legacy Graphics Device Interface (GDI)** to **DXGI Desktop Duplication API**
- the cursor isnt showing for some reason maybe because dxgi who still need fix
other than that this is even more robust release


v1.2 - 22 August 2026
- add dpi aware
- add path bug fix
- tested for fuzz
- add dark mode


v1.1 - 22 August 2026
- add dpi aware
- add path bug fix
- tested for fuzz

v1 - 18 August 2026
- first release
