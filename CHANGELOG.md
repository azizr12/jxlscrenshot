v2.2.2 - 26 September 2026
- fix thread may **terminated** if write didnt finish or app exit
- fix **picture folder** name may be localized which will cause path to fail
- add notification pop up if something goes wrong with path or writing!
- fixed about **fail** to drag on text and icon
- removed cursors **dead code** and other redundant code
- add **libjxl version** (0.12.0) info  and **app version** and **cpu instruction** info to about window
- Fixed the Confusing **ini config**


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

- Hdr notone by @azizr12 in #4
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
- first realease
