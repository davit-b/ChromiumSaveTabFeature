# ![Logo](chrome/app/theme/chromium/product_logo_64.png) Chromium

Feature Description:
Project to add a feature to Chromium that saves and restores a list of open tabs without bookmarking all open tabs.
This branch has a working button in the main toolbar menu ui that when pressed will saved all active tabs using the Chromium class GURL.

![Alt text](https://user-images.githubusercontent.com/12516060/33656593-7e278270-da44-11e7-9647-50f207302ac1.png "Snapshot of feature in UI")

Ongoing work to add feature to push saved collection of GURL objects to the tab_restore feature in chromium api, to build a new window with all the tabs from the saved state.




Setting Specific notes:
Recommended compile command for logging output of GURL instance variables
because all debug logging is done with v=0, and v=1 is full of clutter
/path/to/out/buildDirectory/chrome --enable-logging --v=0


Default chrome_debug.log file is hard to find. Located in
/chromium/src/chrome_debug.log


gn generates .ninja files for the Ninja builder. Have active args to speed up build time. Expect 40 min on cloud computer.
gn args that are active for my builds:
use_jumbo_build=true
enable_nacl=false
symbol_level=1
remove_webcore_debug_symbols=true






CHROMIUM SPECIFIC README.md:
The project's web site is https://www.chromium.org.
Documentation in the source is rooted in [docs/README.md](docs/README.md).

Learn how to [Get Around the Chromium Source Code Directory Structure
](https://www.chromium.org/developers/how-tos/getting-around-the-chrome-source-code).
