# Dialectic Text Input

The chatbox uses the native plugin for hotkey routing and target selection, with
a narrow JIP UI adapter for `ShowTextInputMenu`.

1. `ln_DialecticBootstrap.txt` registers the configured MCM hotkeys and starts
   `TextInputMenuTick.txt` once when a save is loaded.
2. `GameLoop` requests the text-input menu through the native command path.
3. `OpenTextInputMenu.txt` opens the one-line JIP text input.
4. `TextInputMenuTick.txt` returns submitted text to the plugin.
5. `TargetManager` resolves the current crosshair actor or nearest valid actor.
6. The plugin sends the structured JSON request to DialecticServer.

No chatbox or microphone key is hardcoded. Both are configured through MCM and
remain inactive while unbound.
