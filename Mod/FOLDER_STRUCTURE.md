# Dialectic Mod Folder Structure

`Mod/Data` is the complete deployable mod package.

```text
Mod/
  Data/
    Dialectic.esp
    Dialectic/
      fallout_builtin_voices.csv
    MCM/
      Dialectic.json
    menus/prefabs/Dialectic/
      PassiveSubtitle.xml
    NVSE/
      Plugins/
        dialectic.ini
        dialectic_custom.ini.example
        scripts/
          ln_DialecticBootstrap.txt
      user_defined_functions/Dialectic/
```

The release build adds `NVSE/Plugins/dialectic.dll`. `Dialectic.esp` contains
only the quest, topic, package, faction, and form-list records required by the
runtime; obsolete compiled script records have been removed. User overrides belong in
the separate `Dialectic_custom` MO2 mod as `dialectic_custom.ini` so updates do
not replace them.

`fallout_builtin_voices.csv` maps Fallout voice types to files in the user's
installed `Data/Sound/Voice` tree. Extracted game voice samples are not part of
the Dialectic package or source repository.
