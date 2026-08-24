# Dialectic Text Input

The chatbox uses the native plugin for hotkey routing and target selection, with
a narrow JIP UI adapter for `ShowTextInputMenu`.

1. `ln_DialecticBootstrap.txt` registers the configured MCM hotkeys and starts
   `TextInputMenuTick.txt` once when a save is loaded.
2. `GameLoop` requests the text-input menu through the native command path.
3. `OpenTextInputMenu.txt` opens the JIP text input and passes submissions to the
   parser-safe `TextInputSubmit.txt` callback. The title shows the target and the
   current chat mode (`Talking to Veronica [CHEAT]`), Narrator mode always targets
   The Narrator, and typed text wraps onto a second line instead of running off the
   right edge.
4. `ApplyTextInputMenuLayout.txt` tunes the open menu through runtime trait writes and
   injects `menus/prefabs/Dialectic/TextInputCloseButton.xml` next to JIP's OK button.
   JIP's own `texteditmenu.xml` is never overridden.
5. `TextInputCloseClick.txt` handles that Close button, registered once from
   `ln_DialecticBootstrap.txt`. It defers teardown by one frame to
   `TextInputCloseDeferred.txt`, which marks the bridge closed without `submitted=1`,
   clears the open signal, and closes the menu without sending a chat request. Clicking
   OK with an empty box cancels the same way.
6. `TextInputMenuTick.txt` returns submitted text to the plugin.
7. `TargetManager` resolves the current crosshair actor or nearest valid actor.
8. The plugin sends the structured JSON request to DialecticServer.

No chatbox or microphone key is hardcoded. Both are configured through MCM and
remain inactive while unbound.
