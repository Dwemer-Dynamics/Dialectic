Dialectic - AI NPCs for Fallout New Vegas and TTW
=================================================

Version: 0.5.0
Author: Dwemer Dynamics

REQUIREMENTS
------------
- Fallout: New Vegas with xNVSE 6.3.3 or newer
- JIP LN NVSE 57 or newer
- JohnnyGuitar NVSE 5.17 or newer
- Mod Configuration Menu and MCM Extender
- DialecticServer

INSTALLATION
------------
Install the complete contents of this Data folder with a mod manager and enable
Dialectic.esp. The release package must include NVSE/Plugins/dialectic.dll.

Keep user settings in the separate Dialectic_custom mod at:

    NVSE/Plugins/dialectic_custom.ini

Only values in dialectic_custom.ini override the shipped defaults in
dialectic.ini, allowing normal updates without replacing user settings.

HOTKEYS
-------
All hotkeys are unbound by default. Configure Chatbox, microphone, halt actions,
mode selection, model selection, and profile selection through MCM.

TROUBLESHOOTING
---------------
Check NVSE/Plugins/dialectic.log and jip_ln_nvse.log for plugin or script-runner
errors. Verify DialecticServer discovery or the custom server override if the
plugin loads but requests fail.

LICENSE
-------
MIT License - See LICENSE.txt
