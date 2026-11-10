# Planned Paper 4IN0 Factory

The `paper-4in0-v2` layout reserves a 3 MiB `factory` partition at `0x10000`.
The desktop is installed in `ota_0` at `0x310000` and the USB boot-selection
record selects it explicitly. No Factory launcher image is implemented or
registered yet; an empty reserved partition is not a recovery application.

Future work must supply this board's Factory source and schema-2 manifest, bind
it in the board build definition, and enable the managed image workflow with
the same versioned geometry. The existing AMOLED Factory is a workflow reference,
not a compatible binary. Shared Paper resources can be reused by the launcher.
App descriptors, app installation/boot management, covers and hardware acceptance
belong to that later implementation.

Until then, the desktop profile remains a single application build. Its full
USB package initializes the new layout, including the reserved Factory area;
it must not be used to preserve an independently installed future launcher.
Once a launcher exists, use its verified combined package and app-only updates.
