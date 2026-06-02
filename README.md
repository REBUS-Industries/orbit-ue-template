# orbit-ue-template

Unreal Engine **5.7** project for the PRISM Visualiser inside
[REBUS-ORBIT/prism](https://github.com/REBUS-ORBIT/prism). The PRISM
orchestrator launches this project in `-game` + Pixel Streaming, the ORBIT
connector imports the selected ORBIT model into the live world, and the
`RebusVisualiser` plugin drives lighting fixtures and reports state back to the
portal.

## Layout

The project lives in **[`REBUS_Visualiser/`](REBUS_Visualiser/)**:

```
REBUS_Visualiser/
├── REBUS_Visualiser.uproject        # UE 5.7; enables the plugins below
├── Source/                          # minimal C++ game module + Game/Editor targets
├── Config/                          # DefaultEngine/Game/... (PixelStreaming2, Python)
├── Content/Python/                  # build_rebus_base_level.py (portal-controllable env)
└── Plugins/
    ├── RebusVisualiser/             # vendored — fixture control, PS2 data channel, REST scene intake
    ├── glTFRuntime/                 # vendored (MIT) — runtime .glb loader (receive)
    └── OrbitConnector/              # NOT vendored — installed from orbit-connectors releases
```

## Plugins

| Plugin | Vendored here? | Why |
| --- | --- | --- |
| `RebusVisualiser` | yes | UE side of the PRISM Visualiser: moving-head fixtures, IES/photometrics, motion-rig parity, PixelStreaming2 `UIInteraction` data channel, REST `/api/ue/*` scene intake. |
| `glTFRuntime` | yes (MIT) | Runtime `.glb` loading for the connector's receive path. |
| `OrbitConnector` | **no** | Canonical property of [`orbit-connectors`](https://github.com/REBUS-ORBIT/orbit-connectors); imports the full ORBIT geometry. Install it (plus the bundled `orbit-cli.exe`) from an orbit-connectors release — see below. |
| `PixelStreaming2` | engine | Streams the viewport to the ORBIT web client. |
| `ModelingToolsEditorMode`, `PythonScriptPlugin`, `EditorScriptingUtilities` | engine | Editor-only tooling (Python base-level builder, asset scripting). |

## Installing OrbitConnector

`OrbitConnector` is deliberately not committed here. Install it into the
project's `Plugins/` from an [orbit-connectors release](https://github.com/REBUS-ORBIT/orbit-connectors/releases):

```powershell
# from a checkout of orbit-connectors:
installers\ue5\Update-OrbitConnector.ps1 -ProjectPath "<path>\REBUS_Visualiser"
```

This places `Plugins/OrbitConnector/` and the bundled
`Plugins/OrbitConnector/ThirdParty/Cli/win-x64/orbit-cli.exe`.

## Building

This is a C++ project (`RebusVisualiser` + the project game module), so it
**must be compiled** before it can run:

1. Install **Unreal Engine 5.7.x** and a C++ toolchain (Visual Studio 2022 with
   the *Game development with C++* workload).
2. Install `OrbitConnector` (above).
3. Open `REBUS_Visualiser/REBUS_Visualiser.uproject` — UE will prompt to build
   `RebusVisualiser` / `REBUS_VisualiserEditor`; accept. (Or build the
   `REBUS_VisualiserEditor` target via UBT / RunUAT.)

## How PRISM uses it

The PRISM orchestrator launches the **fixed** project in `-game` +
PixelStreaming and passes the selected model as `-Orbit*` command-line tokens;
the connector auto-imports it inside the streamed instance. See
[`PRISM/docs/VISUALISER_CONNECTOR_IMPORT.md`](https://github.com/REBUS-ORBIT/prism/blob/main/docs/VISUALISER_CONNECTOR_IMPORT.md)
and the connector's `PRISM-INTEGRATION.md` in `orbit-connectors` for the full
contract.

## Releases

Every tag matching `v*` triggers [`.github/workflows/release.yml`](.github/workflows/release.yml),
which zips the repo (excluding `.git` / `.github` / `dist`) and attaches
`orbit-ue-template-<tag>.zip` to the GitHub Release. Tag with the engine
major.minor as a suffix (e.g. `v1.0.0-ue5.7`).

## Licence

[MIT](LICENCE.txt) — matches `REBUS-ORBIT/orbit-connectors`.
