# REBUS_Visualiser — connector-import UE project

This is the **connector-import PRISM Visualiser** Unreal Engine **5.7** project,
captured from the working build on the PRISM workstation (`PC01`). It is the
buildable, plugin-wired evolution of the root scaffold in this repo and is
checked in here so other agents can work on it under version control.

> **Relationship to the root scaffold (`/REBUSVis.uproject`)** — This project is
> placed in its own top-level directory rather than replacing the root scaffold.
> The root scaffold is what the PRISM orchestrator currently fetches as a tagged
> **release zip** (`v0.1.0-ue5.7-scaffold`), so it is deliberately left untouched.
> A maintainer should decide whether to promote this project to the repo root and
> cut a new `v*-ue5.7` release. See the PR description for the full rationale.

## What's here

| Path | Notes |
| --- | --- |
| `REBUS_Visualiser.uproject` | UE 5.7 project. Plugins enabled: `PixelStreaming2`, `glTFRuntime`, `OrbitConnector`, `ModelingToolsEditorMode`. Declares a minimal `REBUS_Visualiser` Runtime game module. |
| `Source/` | Minimal C++ game module + `Game`/`Editor` build targets. They exist only so the project (and its in-tree plugins) compile in-context — no game logic lives here. |
| `Config/` | `DefaultEngine.ini` (Lumen / VSM / Substrate / RayTracing / DX12-SM6), `DefaultEditor.ini`, `DefaultGame.ini`, `DefaultInput.ini`. |
| `Content/` | Empty on capture (no committed artist assets yet); created on first editor open. Per-user `Content/Developers/` scratch is gitignored. |
| `Plugins/glTFRuntime/` | **Vendored** third-party plugin (see below). |

## Plugins

### OrbitConnector — NOT vendored (intentional)

`OrbitConnector` is the **canonical property of `REBUS-ORBIT/orbit-connectors`**
and ships as a release zip / via the connector updater. To avoid creating a
competing source of truth, its source is **not** committed here, and its
`Binaries/`, `Intermediate/`, and `ThirdParty/` (including the `orbit-cli.exe`
release artifact) are excluded entirely.

The `.uproject` still lists `OrbitConnector` as an enabled plugin. To build/open
this project, install the plugin into `Plugins/OrbitConnector/` from the
`orbit-connectors` release (or let the PRISM Agent/updater drop it in).

### glTFRuntime — vendored (MIT)

`glTFRuntime` (by Roberto De Ioris, MIT) is vendored so the template can build
standalone. Only the redistributable parts are committed — `Source/`, `Content/`
(material library), `Config/`, `Resources/`, descriptors, and the upstream
`LICENSE`. Its `Binaries/` and `Intermediate/` are excluded.

## Excluded from version control

`Binaries/`, `Intermediate/`, `Saved/`, `DerivedDataCache/`, `.vs/`, `*.sln`,
`*.VC.db`, every plugin's `Binaries/`/`Intermediate/`, the `OrbitConnector`
plugin tree (and `orbit-cli.exe`), `*.uproject.bak*` / `*.orig-backup`
descriptor backups, and build logs. See `.gitignore`.

## Building / opening

1. Install **Unreal Engine 5.7.x**.
2. Install the `OrbitConnector` plugin into `Plugins/OrbitConnector/` (from the
   `orbit-connectors` release).
3. Right-click `REBUS_Visualiser.uproject` → *Generate Visual Studio project
   files*, then build the `REBUS_VisualiserEditor` target (or just open the
   `.uproject` and let the editor compile).
