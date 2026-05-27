# orbit-ue-template

Unreal Engine **5.7** template project for the PRISM Visualiser inside
[REBUS-ORBIT/prism](https://github.com/REBUS-ORBIT/prism).

This repo ships **only the project shell** — `.uproject`, `Config/*.ini`, an
empty `Content/REBUS/{Maps,BP,Materials}` tree, and the release CI. The actual
scene, importer Blueprint and master material are populated by artists and
checked in via Git LFS as `.umap` / `.uasset` binaries.

The PRISM Agent fetches a tagged release of this repo, unpacks it under
`%LOCALAPPDATA%\PRISM.Agent\ue-template\<tag>\`, copies the tree into a per-job
working directory, drops the converted glTF in, and launches Unreal Editor
in `-game` mode with Pixel Streaming.

---

## How to open this project

1. Install **Unreal Engine 5.7.4** (or later 5.7.x) via the Epic Games Launcher.
2. Install **Git LFS** (`git lfs install --system`) before cloning, otherwise the
   placeholder `.uasset` / `.umap` files come down as 130-byte pointer files.
3. Clone this repo (or unzip a release): `git clone https://github.com/REBUS-ORBIT/orbit-ue-template.git`.
4. Double-click `REBUSVis.uproject`. The launcher will associate it with your
   5.7 install, prompt to download any missing plugins, and open the editor.

No C++ compilation step. No Visual Studio. No UBT. If the editor asks to
generate Visual Studio project files, you can decline.

---

## Required plugins

These are already declared as `"Enabled": true` in `REBUSVis.uproject` — UE
will surface a warning on first open if any are missing from your install:

| Plugin | Why we need it |
| --- | --- |
| `PixelStreaming2` | The orchestrator launches `-game` with `-PixelStreamingURL` to stream the viewport to the ORBIT web client. |
| `Interchange` / `InterchangeAssets` / `InterchangeEditor` | The Python importer (in `BP_OrbitImporter`) wraps Interchange to bring the converted glTF in as a `World` + meshes + materials. |
| `PythonScriptPlugin` | Lets the orchestrator drive the editor from a `-ExecutePythonScript=` command line. |
| `EditorScriptingUtilities` | Required by the Python importer for level/asset manipulation (`unreal.EditorAssetLibrary`, `LevelEditorSubsystem`, etc.). |
| `MovieRenderPipeline` | Used by the optional "render still / turntable" code path. |
| `DMXEngine` / `DMXProtocol` / `DMXFixtures` | Reserved for lighting-design playback driven from ORBIT. Safe to leave on even when unused. |

---

## Placeholders artists need to fill

The orchestrator's Python importer assumes three assets exist at known paths.
On a fresh clone they're empty `.gitkeep` placeholders. See
[`Content/REBUS/README.md`](Content/REBUS/README.md) for the full table; the
short version is:

- `Content/REBUS/Maps/BaseLevel.umap` — empty level with `SkyAtmosphere`,
  `VolumetricCloud`, `DirectionalLight` (sun), `SkyLight`,
  `ExponentialHeightFog`, `PostProcessVolume`, and a 1×1 km neutral landscape.
- `Content/REBUS/BP/BP_OrbitImporter.uasset` — Blueprint exposing a
  Python-callable function `ImportGltf(gltfPath, targetFolder, levelName)` that
  drives the Interchange import pipeline.
- `Content/REBUS/Materials/M_DefaultLit.uasset` — master material applied to
  any imported mesh whose `RenderMaterial` didn't survive the glTF round-trip.

When all three exist, cut a release tagged `v1.0.0-ue5.7` (see Versioning
below). The orchestrator will start fetching that tag.

---

## How the orchestrator fetches this repo

The PRISM Agent caches releases on disk so it doesn't re-download per job:

```
%LOCALAPPDATA%\PRISM.Agent\ue-template\
├── v1.0.0-ue5.7\          ← unpacked release zip
├── v1.0.1-ue5.7\
└── ...
```

Resolution flow:

1. Agent reads its configured `ueTemplateTag` (defaults to the latest `v*-ue5.7`).
2. If the cache for that tag is missing, it calls
   `GET https://api.github.com/repos/REBUS-ORBIT/orbit-ue-template/releases/tags/<tag>`,
   downloads the attached `orbit-ue-template-<tag>.zip` asset, and unpacks it.
3. Per-job, the agent copies (`robocopy /MIR`) the unpacked tree into the
   job's scratch directory, drops the converted glTF under
   `<job>\Content\REBUS\Imports\<jobId>\`, rewrites
   `Config/DefaultEngine.ini`'s `GameDefaultMap` / `EditorStartupMap` to the
   per-job level, and launches `UnrealEditor.exe`.

The orchestrator's fetch / launch code lives in **PRISM Phase E**, not in this
repo.

---

## Versioning

We tag with the Unreal engine major.minor as a suffix so the agent can pick
the right template for the editor it's about to launch.

| Tag pattern | Meaning |
| --- | --- |
| `v0.1.0-ue5.7-scaffold` | Scaffold-only — no usable scene yet. |
| `v1.0.0-ue5.7` | First artist-populated release. |
| `vX.Y.Z-ue5.7` | Subsequent releases on UE 5.7. |
| `vX.Y.Z-ue5.8` | If/when we move to UE 5.8, run the `v1.0.0-ue5.8` line in parallel for a release window. |

Every tag matching `v*` triggers `.github/workflows/release.yml`, which zips
the project (excluding `.git` / `.github` / `dist`) and attaches
`orbit-ue-template-<tag>.zip` to the matching GitHub Release.

---

## Licence

[MIT](LICENCE.txt) — matches `REBUS-ORBIT/orbit-connectors`.
