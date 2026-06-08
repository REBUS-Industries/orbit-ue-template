# `Content/REBUS/IES/Source/` — committed source `.ies` files for the v1.0.121 IES pre-bake

This folder is walked by `build_rebus_base_level.ensure_ies_profiles()` (Python builder,
`REBUS_Visualiser/Content/Python/build_rebus_base_level.py`). Every `<name>.ies` here is
converted into a `UTextureLightProfile` `.uasset` at `/Game/REBUS/IES/<name>` and tagged
with the v1.0.121 `IesProfileRevision` metadata so the runtime cache
(`URebusVisualiserSubsystem::GetCachedIesProfile(FName Name)`) can `LoadObject` it without
ever calling the editor-only `IESConverter.h` at runtime.

## Why this exists

`IESConverter.h` (the engine's runtime IES → `UTextureLightProfile` builder) is editor-only.
In a packaged / `-game` launch the v1.0.120 runtime fallback path through
`RebusIes::BuildLightProfile` logs
```
IESConverter.h not available in this engine build; cannot load IES at runtime.
Falling back to the synthesized cone.
```
and the fixture renders with a smooth synthetic cone instead of the photometrically-correct
IES distribution. v1.0.121 pre-bakes IES profiles into `.uasset` form so the runtime just
loads them as cooked content — no editor APIs touched.

## How to add a new profile

1. Drop the `.ies` file here, named **`<profile id>.ies`** where `<profile id>` is the same
   id the portal sends via `RegisterFixtureIes` (typically a UUID like
   `96d62ffd-faf6-4bf5-a551-c4c774aa066c`). The bake sanitizes filenames to UE-safe
   asset names (`[A-Za-z0-9_\-]` only); profile ids that are already UUIDs pass through
   unchanged.
2. Run the bake commandlet from the workspace root:
   ```
   "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
       REBUS_Visualiser\REBUS_Visualiser.uproject ^
       -run=PythonScript ^
       -Script="import build_rebus_base_level as b; b.ensure_ies_profiles(force=True)" ^
       -unattended -nop4 -nosplash -stdout -FullStdOutLogOutput
   ```
3. Commit the produced `REBUS_Visualiser/Content/REBUS/IES/<name>.uasset` (and `.uexp`)
   alongside the source `.ies`.

## Alternative path: capture-from-runtime

If you don't have the source `.ies` files but the portal pushes them inline at runtime via
`RegisterFixtureIes`, the v1.0.121 C++ handler will capture the bytes to
`<ProjectSaved>/REBUS/IES_Inbox/<sanitized id>.ies` the first time it sees them in an
editor / commandlet session (the capture path is gated by the same `GIsEditor &&
!IsRunningGame()` check the bake regen uses). Then re-run the bake commandlet — the
inbox is walked alongside this folder and the captured `.ies` files are baked too.
Optionally `git mv` the captured files from the Saved inbox into this folder so they're
committed and the bake doesn't depend on the Saved inbox surviving a CI scrub.

## Revision

`REBUS_IES_PROFILE_REVISION = 121` (defined in
`build_rebus_base_level.py`, kept in lockstep with `REBUS_BEAM_MATERIAL_REVISION`).
A future bump invalidates every baked profile so the next bake re-stamps them.
