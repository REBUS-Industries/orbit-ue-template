# RebusVisualiser (UE 5.7 plugin)

The Unreal-engine side of the REBUS/ORBIT **PRISM Visualiser**, implemented against the
portal's live contracts as described in `ue-plugin-build-guide.md`. It consumes the
`/api/ue/*` REST contract and the bidirectional Pixel Streaming 2 data-channel descriptors,
drives moving-head fixtures with Speckle/Orbit viewer **motion parity**, and reports state
back to the portal.

This plugin is **self-sufficient**: it fetches the scene, builds fixture geometry proxies
from `/meshes`, places + drives the lights, and talks the data channel — it does not require
`OrbitConnector` to be present (in a packaged PRISM build, OrbitConnector still imports the
full ORBIT geometry; this plugin owns the *fixture* lifecycle and control regardless).

## Where it lives in the project

`REBUS_Visualiser/Plugins/RebusVisualiser/` and is enabled in `REBUS_Visualiser.uproject`.
Build the `REBUS_VisualiserEditor` (or `REBUS_Visualiser` Game) target as usual — it compiles
in-context with the project's other plugins.

## Source map

| File | Responsibility | Guide § |
| --- | --- | --- |
| `RebusCoordinates.*` | RH Z-up (placement) and RH Y-up (mesh/rig/beam) → Unreal conversions | §4.1, §7.2/§7.3 |
| `RebusSceneTypes.h` | Parsed `/scene`, `/fixtures/{id}`, `/meshes` data model | §3, §4 |
| `RebusJson.*` | Tolerant JSON parsers for every payload | §4 |
| `RebusRestClient.*` | Async HTTP w/ `x-api-key`, redirect-following byte fetch | §4.0–§4.6 |
| `RebusMotionSolver.*` | Pan/tilt parity: parent-first compose, tilt-under-pan +90°, pivotOffset | §7 |
| `RebusFixtureActor.*` | Per-fixture actor: geometry proxies, SpotLight, cone/IES/source, fades | §7, §8, §11 |
| `RebusCineCameraPawn.*` | v1.0.79 cinematic camera pawn (manual exposure, portal-driven focal/aperture/focus/EV/sensor/transform) | §6 |
| `RebusIes.*` | Runtime IESNA → `UTextureLightProfile` (brightness authority stays portal-side) | §8.2 |
| `RebusFixtureControlSubsystem.*` | `FixtureId → actor` registry + all `SetFixture*` + `SelectFixtures` | §3, §5.2/§5.3 |
| `RebusSceneSettingsSubsystem.*` | `SetSceneProperty`/`SetSceneProperties` catalogue + `SceneState` | §5.4, §9, §6.3 |
| `RebusDataChannel.*` | PS2 receive (register `UIInteraction` handler) + read-back send | §5, §6, §10 |
| `RebusVisualiserSubsystem.*` | Session orchestrator: launch tokens → fetch → spawn → `Ready` | §2, §10 |

## Identity (no id translation)

The **Speckle node id** (`/scene → fixtures[].id`) is the single control key. Spawned actors
are registered under it (`RegisterFixture(NodeId, Actor)`), and it is exactly what every
inbound descriptor and outbound `FixtureRegistered`/`FixtureState` carries. The **library id**
(`fixtures[].fixtureId`) is used only to fetch the profile (`/api/ue/fixtures/{libraryId}`).

## Base level

The project runs from `/Game/REBUS/Maps/BaseLevel` (wired in `Config/DefaultEngine.ini`).
It's a blank level whose default, portal-controllable environment is an **ExponentialHeightFog**
(volumetric, full extent) + an **unbound PostProcessVolume** + sun/sky. Generate the `.umap`
once with `Content/Python/build_rebus_base_level.py` (Tools > Execute Python Script, or headless
`-run=pythonscript`). As a safety net, the session subsystem also find-or-spawns the fog +
post-process volume at launch (`EnsureSceneEnvironment`), so the stream renders even on a level
that was authored without them. The fog (`FogDensity`, `bVolumetricFog`, ...) and studio lights
are then driven from the portal via `SetSceneProperty` (§9).

## Configuration

`PortalUrl` + `x-api-key` come from the launch command line (preferred for the secret) or
`[RebusVisualiser]` in `DefaultGame.ini`:

```
-PortalUrl="https://app.rebus.industries" -RebusApiKey=<plugin key>
```

PRISM launch tokens consumed (§10.2):
`-PixelStreamingID=orbit_<shortRunId>` (the streamer is registered/looked up under this id —
never hardcoded), `-OrbitProject` (portal doc id), `-OrbitModel`, `-OrbitVersion`,
`-OrbitServer`, `-OrbitTarget`.

> Quote the URL in `.ini` — UE strips `//` from unquoted values (§4.0).

### Pixel Streaming console commands (v1.0.54 → multi-layer fix v1.0.55)

The Pixel Streaming 2 streamer silently DROPS portal-sent `Command` / `ConsoleCommand` messages
unless a CVar gate is set to 1 (defaults off for security). Epic's built-in `PixelStreamingInput`
component routes the command; the CVar is what gates it. With it enabled the portal's console
pane can drive:

- Any built-in UE console command, e.g. `stat fps`, `stat unit`, `r.ScreenPercentage 75`.
- Every `Rebus.*` console command this plugin registers: `Rebus.DumpFixtureLights`,
  `Rebus.MeshBeams [0|1]`, `Rebus.DriveOrbitModels [0|1]`, and tunables like
  `Rebus.HeroShadowScatter <float>`.
- Any other CVar relevant to runtime tuning (e.g. `r.MegaLights.Allow`, `r.SkyLight.RealTimeReflectionCapture`).

> **v1.0.55 — wrong CVar in v1.0.54.** v1.0.54 set `PixelStreaming.AllowPixelStreamingCommands=1`,
> which is the **legacy PS1** CVar. This project ships the **PS2** plugin
> (`REBUS_Visualiser.uproject` → `"PixelStreaming2": enabled`), which gates on a **completely
> different CVar**: `PixelStreaming2.AllowPixelStreamingCommands`. Engine source (UE 5.7):
>
> - PS1 gate: `Engine/Plugins/Media/PixelStreaming/Source/PixelStreamingInput/Private/Settings.cpp:8`
>   `CVarPixelStreamingInputAllowConsoleCommands("PixelStreaming.AllowPixelStreamingCommands")`.
> - PS2 gate: `Engine/Plugins/Media/PixelStreaming2/Source/PixelStreaming2Settings/Private/PixelStreaming2PluginSettings.cpp:621`
>   `CVarInputAllowConsoleCommands("PixelStreaming2.AllowPixelStreamingCommands")`.
> - PS2 Command handler: `PixelStreaming2/Source/PixelStreaming2RTC/Private/RTCInputHandler.cpp:627`
>   reads the PS2 CVar on every incoming message (so a late `Set` works, BUT — see next bullet).
> - **Frontend gate sticks early.** The PS2 streamer (`EpicRtcStreamer.cpp:923-926`) bakes the
>   CVar value into the `InitialSettings` JSON it sends to the browser **once** when the data
>   channel opens; the frontend (`PixelStreaming.ts:568`) reads
>   `PixelStreamingSettings.AllowPixelStreamingCommands` from that payload and **refuses to send
>   commands at all** if it was `false`. So the gate must be 1 *before* the first data-channel
>   handshake, not just before each Command arrives.
>
> v1.0.55 therefore layers **three** defences so the gate is true no matter which load order /
> config phase runs first:
>
> 1. **Config (defence in depth).** `REBUS_Visualiser/Config/DefaultEngine.ini` carries the line
>    in BOTH `[SystemSettings]` AND `[ConsoleVariables]` (the latter is applied very early in
>    engine boot, before plugin StartupModule). Both PS1 and PS2 CVar names are set. The PS2
>    UDeveloperSettings UPROPERTY backing is also set in `DefaultGame.ini` under
>    `[/Script/PixelStreaming2Settings.PixelStreaming2PluginSettings]` →
>    `InputAllowConsoleCommands=True`.
> 2. **Force at module startup.** `RebusVisualiser.cpp::StartupModule()` looks up both PS1 and
>    PS2 CVars and calls `Set(1, ECVF_SetByProjectSetting)` immediately. If the relevant PS
>    module hasn't loaded yet (CVar not registered), it logs a benign "NOT REGISTERED" line and
>    retries from `FCoreDelegates::OnPostEngineInit` (all plugins are up by then), which always
>    succeeds.
> 3. **Diagnostic log line.** After the post-engine-init retry, the module logs a one-shot
>    status line:
>
>    ```
>    LogRebusVisualiser: PixelStreaming console gate status: PixelStreaming.AllowPixelStreamingCommands=<v> PixelStreaming2.AllowPixelStreamingCommands=<v> plugin=<PS2|PS1|both|none> ...
>    ```
>
>    Grep `LogRebusVisualiser` for `PixelStreaming console gate status:` after relaunching. For
>    this project, expect `PixelStreaming2.AllowPixelStreamingCommands=1 ... plugin=PS2`. If
>    instead you see `=0` for PS2 the gate never landed (file a bug with the surrounding
>    `Forced PixelStreaming2.* at phase=...` lines so we can see which phase actually ran).
>
> **Manual fallbacks** if all three layers somehow miss:
> `-execcmds="PixelStreaming2.AllowPixelStreamingCommands 1"` on the editor / cooked-game
> command line (highest CVar priority short of console). For PS1 builds the legacy launch arg
> `-AllowPixelStreamingCommands` is also parsed by `PixelStreamingInput/Settings.cpp:79`.
>
> **If the gate is 1 but the portal console still does nothing**, the diagnosis moves to the
> streamer/frontend protocol side:
>
> - Frontend: confirm the browser's PS frontend actually emits a `ConsoleCommand` JSON on the
>   data channel (check the page DevTools console for the `Logger.Info` line "Sending UE
>   ConsoleCommand" or watch the WebRTC data channel). If the InitialSettings handshake handed
>   it `AllowPixelStreamingCommands=false` it'll silently no-op (`PixelStreaming.ts:568-573`).
> - Streamer: PS2 maps the `ConsoleCommand` toStreamer opcode in
>   `RTCInputHandler.cpp:626`; if the opcode isn't in the protocol map the message never
>   reaches the handler. Mismatched PS1/PS2 frontend ↔ streamer pairings can cause this.
> - Portal-side: confirm the portal is using its PS2-compatible frontend bundle (matching the
>   `EpicRtcStreamer` protocol version) — a PS1 frontend talking to a PS2 streamer will not
>   propagate commands.

The change takes effect at next process launch (no rebuild needed for config-only re-issues,
but v1.0.55 carries a C++ change so a rebuild is required for it). Quick verify: with the
fresh build running, send `stat fps` from the portal console pane — the FPS overlay appears in
the streamed view, and the gate-status log line confirms `=1` for the active plugin.

### ConsoleCommand via UIInteraction (v1.0.56) — recommended path

v1.0.55 verified the UE-side PS2 gate is correctly latched to `1`
(`PixelStreaming console gate status: ... PixelStreaming2.AllowPixelStreamingCommands=1
plugin=PS2`). Despite that, the user's logs show the protocol-level PS2 `Command` /
`ConsoleCommand` messages never reach the streamer — the portal frontend either doesn't emit
the opcode, emits the wrong one, or the path drops them silently. UIInteraction descriptors
(`SelectFixtures`, `RegisterFixture*`, `LoadScene`, etc.) demonstrably arrive on the same
session, so v1.0.56 piggy-backs console-command execution onto the UIInteraction descriptor
channel that already works. **This is the recommended path for any portal using a non-Epic-stock
PS2 frontend.**

**Portal-side descriptor** (send over the existing `UIInteraction` data channel — exactly the
same transport you use for `SelectFixtures`):

```json
{
  "type": "ConsoleCommand",
  "command": "stat fps",
  "silent": false
}
```

| Field     | Type   | Required | Default | Notes                                                                                          |
| --------- | ------ | -------- | ------- | ---------------------------------------------------------------------------------------------- |
| `type`    | string | yes      | —       | Must be the exact string `"ConsoleCommand"`.                                                   |
| `command` | string | yes      | —       | The full console line, e.g. `"stat fps"`, `"Rebus.DumpFixtureLights"`, `"r.ScreenPercentage 75"`. |
| `silent`  | bool   | no       | `false` | When `true`, suppresses the success log line. Use for high-frequency telemetry commands.       |

The handler lives in
`REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusDataChannel.cpp`
inside `FRebusDataChannel::HandleDescriptor` (added as a peer of the `Ping` branch). It runs on
the game thread (the input-handler callback already marshals onto `ENamedThreads::GameThread`
before dispatching), picks the first live Game/PIE world, and calls
`GEngine->Exec(World, *Cmd, *GLog)` — UE's canonical entry point for console commands. Empty /
whitespace `command` is rejected with a warning; otherwise the command string is **not**
filtered (matching Epic's PS2 design — the gate is the portal's own authentication, not a
server-side allowlist).

**Examples** that round-trip cleanly:

- `stat fps`, `stat unit`, `stat gpu` — built-in stat overlays.
- `Rebus.DumpFixtureLights` — per-fixture light/cookie diagnostic (v1.0.51).
- `Rebus.MeshBeams 0` / `Rebus.MeshBeams 1` — toggle Epic's beam canvas.
- `Rebus.DriveOrbitModels 1` — Phase-1 Orbit-model motion sync.
- `Rebus.HeroShadowScatter 4` — hero light shadow softness tunable.
- `r.VolumetricFog.GridPixelSize 1` — fog grid live-tweak.
- `r.MegaLights.Allow 0` — disable MegaLights at runtime.

**Verify after rebuild + relaunch.** Send `{"type":"ConsoleCommand","command":"stat fps"}` from
the portal; you should see exactly this in `LogRebusVisualiser`:

```
LogRebusVisualiser: Descriptor type 'ConsoleCommand'.
LogRebusVisualiser: Fixture-channel ConsoleCommand: 'stat fps' (success=1)
```

If `success=0` the command is unknown / refused by `GEngine->Exec`; if the log line never
appears at all the portal didn't actually emit the descriptor (open browser DevTools and watch
the data-channel send). The protocol-level PS2 Command path remains supported (the v1.0.55 gate
is still in place) — if the portal frontend ever does start sending native `ConsoleCommand`
opcodes, both paths will work simultaneously.

## Lifecycle

1. Subsystem reads config + tokens, configures the REST client, creates the data channel.
2. Once a game world is live and config is usable: `GET /api/ue/scene`, pre-fetch each unique
   profile + `/meshes`, then spawn one `ARebusFixtureActor` per fixture at its placement and
   register it.
3. The data channel binds to the PS2 streamer and registers the `UIInteraction` handler.
4. When the channel is open, the scene is loaded, **and** the scene environment has been
   ensured, emit `Ready` (with `loadedModel` counts + capability flags), one `FixtureRegistered`
   per fixture, and re-apply the live selection. The portal then sends `RequestSceneState` → we
   answer `SceneState`.
5. Periodic `FrameStats` while live.

> **First-load state sync (no recycle needed).** `Ready` is gated on the scene environment being
> ensured (`bEnvEnsured`) so the portal only starts pushing `SetSceneProperty`/`LoadScene` after
> the fog/post-process/floor/sun/sky actors those pushes target actually exist — early pushes can
> no longer hit missing actors and get dropped. The scene-settings subsystem keeps an
> authoritative map of every applied value and **re-applies all of them** (`ReapplyAll`) whenever
> the environment is ensured or fixtures (re)spawn, so portal state (and seeded defaults) sticks
> on the first connection instead of only after a recycle. The ordering is verifiable in the log:
> `Scene environment ensured.` → `Re-applied N scene property value(s)...` → `Handshake: Ready
> (channel open, scene loaded, environment ensured) -> broadcasting.`

## Scene push over the data channel (REST-free fixtures)

When the portal cannot serve `/api/ue/scene` to the running instance (e.g. the instance has no
route to the portal API, or the REST call 404s), the **portal can push the same data over the
already-open Pixel Streaming data channel** instead. The plugin parses the identical payload
shapes the REST endpoints return and spawns/drives fixtures exactly as if they had been fetched.

Send these as normal `UIInteraction` descriptors (same envelope as `SetFixturePanTilt`/`Ping`):

```jsonc
// One-shot push (preferred when the whole scene fits in one data-channel message):
{
  "type": "LoadScene",
  "scene":    { /* identical body to GET /api/ue/scene */ },
  "profiles": { "<libraryFixtureId>": { /* identical body to GET /api/ue/fixtures/{id} */ } },  // optional
  "meshes":   { "<libraryFixtureId>": { /* identical body to GET /api/ue/fixtures/{id}/meshes */ } } // optional
}

// Incremental (for larger scenes, to stay under the per-message size limit):
{ "type": "RegisterFixtureProfile", "libraryId": "<id>", "profile": { ... }, "meshes": { ... } } // repeat per profile
{ "type": "LoadScene", "scene": { ... } }   // spawns using whatever profiles were cached above

// Chunked mesh delivery (for a single fixture whose meshes exceed the ~60k-char message budget):
{ "type": "RegisterFixtureMeshes",
  "libraryId": "<libraryFixtureId>",
  "meshes": { "version": 1, "meshes": [ /* a SUBSET of the fixture's meshes[] */ ] },
  "chunkIndex": 0,        // optional, 0-based; omit / single message => chunkCount defaults to 1
  "chunkCount": 1 }       // optional; total chunks for this libraryId — repeat per chunk

// Inline raw .ies photometrics (REST-free; no iesUrl fetch). The portal pushes the literal
// IESNA LM-63 file *text* per libraryId, optionally per zoom step, keyed by zoomDmx:
{ "type": "RegisterFixtureIes",
  "libraryId": "<libraryFixtureId>",
  "profiles": [
    { "profileId": "<id or \"default\">", // groups fragments; "default" = the default profile
      "zoomDmx": 0,                        // optional, 0..255; primary zoom index key
      "zoomAngleDeg": 0,                   // optional metadata (index without re-parsing the .ies)
      "beamAngleDeg": 0,                   // optional metadata
      "fieldAngleDeg": 0,                  // optional metadata
      "iesText": "<literal IESNA LM-63 file text>", // NOT base64 — the raw .ies file verbatim
      "part": 0,                           // optional, 0-based fragment index of THIS profile's iesText
      "partCount": 1 }                     // optional, total fragments for THIS profileId
  ],
  "chunkIndex": 0,        // optional, message-level; same model as RegisterFixtureMeshes
  "chunkCount": 1 }       // optional, message-level; total messages for this libraryId

// Inline base64 gobo wheel images (REST-free; no imageUrl fetch), keyed (libraryId, wheelIndex, slot).
// Field aliases accepted: dataBase64|data, mime|contentType, imageUrl|url, wheelKind|kind|type.
{ "type": "RegisterFixtureGobos",
  "libraryId": "<libraryFixtureId>",
  "gobos": [
    { "wheelIndex": 0,                     // PRIMARY key: 0-based index into the full wheels[]
      "wheel": "<wheel id/name>",          // secondary metadata (groups slots into a wheel)
      "wheelKind": "gobo",                 // wheelKind|kind|type; identifies the gobo-kind wheel
      "slot": 0,                           // 0-based slot index; correlates to SetFixtureGobo.goboIndex
      "slotName": "...",                   // optional slot display name (metadata)
      "name": "...",
      "dataBase64": "<base64 image bytes>",// dataBase64|data; base64 of the png/jpeg
      "mime": "image/png",                 // mime|contentType (informational; decode auto-detects)
      "imageUrl": "<absolute signed GCS url>", // imageUrl|url; fallback when no inline bytes
      "part": 0,                           // optional, 0-based fragment index of THIS (wheelIndex,slot) image
      "partCount": 1 }                     // optional, total fragments for THIS (wheelIndex,slot)
  ],
  "chunkIndex": 0,        // optional, message-level; same model as RegisterFixtureMeshes/Ies
  "chunkCount": 1 }       // optional, message-level; total messages for this libraryId

// Remove all pushed fixtures (also clears any inline-IES + inline-gobo caches):
{ "type": "ClearScene" }
```

Behaviour:
- `LoadScene` clears any previously spawned fixtures, spawns one `ARebusFixtureActor` per
  `scene.fixtures[]` at its `matrixZUpMeters` placement, registers it under the **Speckle node
  id** (`fixtures[].id`) — the same key `SetFixture*` uses — and re-broadcasts `Ready` +
  `FixtureRegistered`.
- `profiles`/`meshes` are optional. Without a profile a fixture is **light-only**: it has no
  custom geometry and no GDTF motion rig, so it falls back to a **default moving-head beam**
  that rests pointing straight down and pans/tilts the spotlight from the inbound `pan`/`tilt`.
  For full parity — the physical head/yoke geometry moving with exact pivots, axes and limits —
  include the `profile` (with `motionRig` + `parts`/`<Beam>`); the GDTF solver then takes over.
- Profiles can be sent inline in `LoadScene` or ahead of time via `RegisterFixtureProfile`.
- `RegisterFixtureMeshes` delivers a fixture's meshes **additively and chunk-by-chunk** when one
  fixture's full bundle is too large for a single message. Each message carries a *subset* of
  `meshes[]`; the plugin **merges** the chunks per `libraryId` (appending, not overwriting) and,
  once `chunkCount` messages have arrived (or `chunkCount <= 1` for a single message), commits the
  merged bundle to the mesh cache — replacing any prior partial. Chunks are counted, not indexed,
  so out-of-order/duplicate `chunkIndex` values are tolerated. On completion, if fixtures are
  already spawned, the affected scene is re-spawned (re-broadcasting the handshake) so light-only
  fixtures of that `libraryId` gain their geometry; otherwise a later `LoadScene` uses the now-
  cached meshes.
- `RegisterFixtureIes` delivers the fixture's **photometrics as raw IESNA LM-63 file text**
  inline (no URL fetch), keyed per `libraryId`. It has **two independent accumulation levels**:
  - **Message-level** (`chunkIndex`/`chunkCount`): `profiles[]` are appended per `libraryId`,
    order-independent (same model as `RegisterFixtureMeshes`), and finalized once `chunkCount`
    messages have arrived (or `chunkCount <= 1` for a single message).
  - **Per-profile fragmentation** (`part`/`partCount`): the accumulated entries are grouped by
    `profileId`; when any entry for a `profileId` has `partCount > 1`, its entries are sorted by
    `part` and their `iesText` is **concatenated** to rebuild the full file. Otherwise the lone
    entry's `iesText` is the whole file.
  - On finalize the plugin builds one `UTextureLightProfile` per `(libraryId, profileId)` from
    the reassembled text and **indexes by `zoomDmx`** (the default profile = `profileId
    == "default"`, else the entry nearest mid-zoom). `iesText` is fed verbatim to the **same**
    `RebusIes::BuildLightProfile` (engine `FIESConverter`) the URL fetch uses — no base64, no
    second parser. If fixtures are already spawned, the affected scene is re-applied so the
    fixtures gain their true IES immediately.
- **IES precedence** (`SelectIesForZoom`): an inline `iesText` profile for the requested zoom
  **wins**; if a needed profile isn't inline, the signed `iesUrl`/`iesProfileUrl` is fetched
  (existing REST path); if neither exists, the **synthetic cone** is kept. The inline push never
  regresses the URL path.
- `RegisterFixtureGobos` delivers gobo wheel images **inline as base64** (no URL fetch), keyed
  per `libraryId`. Same **two accumulation levels** as `RegisterFixtureIes`:
  - **Message-level** (`chunkIndex`/`chunkCount`): `gobos[]` are appended per `libraryId`,
    order-independent, finalized once `chunkCount` messages have arrived.
  - **Per-image fragmentation** (`part`/`partCount`): the accumulated entries are grouped by
    `(wheelIndex, slot)`; when any entry has `partCount > 1`, its entries are sorted by `part` and
    their `dataBase64` is **concatenated BEFORE a single `FBase64::Decode`** (so a base64 blob
    split mid-string reassembles correctly). Otherwise the lone entry's `dataBase64` is decoded.
    (Legacy entries without a `wheelIndex` fall back to grouping by wheel **name**.)
  - On finalize the plugin caches the decoded bytes per **`(libraryId, wheelIndex, slot)`** (the
    contract's primary key; `wheel` name is retained as secondary metadata). The `UTexture2D` is
    built lazily on selection via `FImageUtils::ImportBufferAsTexture2D` (engine image utils,
    auto-detects png/jpeg — no `ImageWrapper` dependency) and fed into the **same** light-function
    MID path the URL gobo fetch uses (texture swap only — no new render path). If the fixture is
    already spawned, the currently-selected gobo is re-applied so it appears without a reselect.
- **Gobo selection correlation** (`SetFixtureGobo(goboIndex, wheelIndex?)`): `goboIndex` (0-based)
  is the **slot**; `wheelIndex` is **0-based into the full `wheels[]`** (NOT just gobo-kind wheels).
  The cache is keyed by **`(wheelIndex, slot)`**, so selection is a direct lookup:
  1. With an explicit **`wheelIndex`**, the texture for `(wheelIndex, slot == goboIndex)` is used
     directly — a colour/effect wheel preceding the gobo wheel can no longer mis-resolve it.
  2. When **`wheelIndex` is absent**, fall back to the **first gobo-kind wheel** = the *smallest*
     `wheelIndex` among inline entries tagged `wheelKind/kind/type == "gobo"` (then the smallest
     `wheelIndex` of any entry). A legacy `wheel` **name** hint is still honoured as a secondary
     match for pushes that carry no `wheelIndex`.
  3. A null/empty/out-of-range `goboIndex` clears the gobo.

  Wheel resolution is centralized in `ARebusFixtureActor::ResolveGoboWheelIndex` (the one spot to
  tweak if the portal's delta differs).
- **Gobo precedence** (`AssignGobo`): an inline base64 image for the selected `(wheel, slot)`
  **wins**; else its (or the profile wheel's) signed `imageUrl` is fetched; else nothing.
- Gobo/IES URLs (when no inline data is present) are still fetched lazily; if the portal is
  unreachable they simply don't load, but dimmer/colour/pan-tilt/zoom still work. Treat each URL
  value as **opaque**: over the data channel the portal sends **absolute signed GCS URLs** (GET
  directly, no `x-api-key`, no redirect), while over REST they are relative **307 redirects**
  fetched with `x-api-key`.

This is purely additive: the REST path (§Lifecycle 2) still runs, and a `LoadScene` push
overrides it.

## Rendering: MegaLights + volumetric fog (§5.7)

The visualiser renders concert/beam-heavy moving heads with **MegaLights** (stochastic local
lighting) plus a **volumetric height fog** so beams read through haze. The baseline is enabled
at startup and re-tunable at runtime per quality tier.

### Baseline CVars (startup)

`Config/DefaultEngine.ini` `[SystemSettings]` sets the stable baseline (read at startup; applies
in `-game` / packaged):

```
r.MegaLights.Allow=1
r.MegaLights.NumSamplesPerPixel=4
r.MegaLights.DownsampleMode=1
r.MegaLights.Volume=1
r.MegaLights.Volume.GridPixelSize=2
r.MegaLights.Volume.GridSizeZ=128
r.VolumetricFog.HistoryWeight=0.9
r.VolumetricFog.GridPixelSize=2
r.VolumetricFog.GridSizeZ=128
```

`r.MegaLights.Allow=1`, `r.MegaLights.DownsampleMode=1`, and `r.MegaLights.Volume=1` are
**baselined** (the same for every tier — the runtime tiers always re-assert `Allow`/`Volume`).

The engine **VolumetricFog froxel-grid** CVars (`r.VolumetricFog.*`) are **fixed defaults** and
are **NOT** controlled by the `RenderQuality` tiers — they stay put regardless of tier:

| CVar | Default | Engine default |
| --- | --- | --- |
| `r.VolumetricFog.HistoryWeight` | `0.9` | 0.9 |
| `r.VolumetricFog.GridPixelSize` | `2` | 16 |
| `r.VolumetricFog.GridSizeZ` | `128` | 64 |

> Note: the engine `r.VolumetricFog.*` froxel grid is **separate** from the MegaLights lighting
> volume grid (`r.MegaLights.Volume.*`). Only the latter is tier-controlled.

> **Beam smoothness (v1.0.37).** The beam material `M_RebusBeam` has **no noise term** — its
> raymarch density is a smooth analytic radial-core × length-falloff profile. The "patchy" beam was
> the **coarse froxel grid** of the hero-shadow fog (`VolumetricScatteringIntensity` re-enabled on
> hero beams). Fixed by halving the froxel footprint (`r.VolumetricFog.GridPixelSize` 4 → **2**),
> raising temporal accumulation (`r.VolumetricFog.HistoryWeight` 0.75 → **0.9**), and lowering
> `RebusHeroShadowScatter` (1.5 → **0.8**) so the smooth mesh-cone raymarch dominates while the fog
> still carves the truss shadow gaps. To tune live without a rebuild: `r.VolumetricFog.GridPixelSize`
> (lower = finer/heavier), `r.VolumetricFog.HistoryWeight` (higher = smoother, slight latency).

> **Beam stays visible up close / inside the cone (v1.0.39).** The mesh-cone beam (`BeamCone`, a
> `UProceduralMeshComponent` + the additive two-sided `M_RebusBeam`) vanished when the camera moved
> **close to or inside** the cone (the lit floor pool stayed; only the volumetric shaft dropped out,
> on all beams). This was **not** culling — it was a mesh-bounded raymarch **entry** bug. The old
> Custom HLSL started the march at the rasterized fragment (`tFront`) and marched **forward
> downrange**; when the camera is inside, the only fragment is the far wall (a back face, drawn
> because the material is two-sided), and marching forward from it leaves the cone so every sample
> missed. Fixed by reworking the march interval analytically from the **view ray vs the cone**
> (re-baked `M_RebusBeam`, `0 error(s)`):
> - **ENTRY** = the camera itself when the camera is inside the cone (so the march always starts at
>   `t=0`), otherwise the analytic near (front-wall) intersection so distant beams stay tightly
>   sampled.
> - **EXIT** = this fragment's own surface distance, clamped by the opaque **scene depth** (occlusion
>   preserved). Because EXIT is the fragment's own depth, a front-face fragment marches a ~zero
>   interval and the far/back-face fragment carries the shaft — so two-sided drawing never
>   double-adds, with no per-face branching.
> - A short **near-camera soft fade** (last ~10 cm) avoids a hard wall in the lens when flying
>   through; it no longer blanks the whole beam.
>
> The v1.0.38 bounds work is kept but reduced to a **modest 1.5×** margin (`RebusBeamBoundsScale`,
> extent-only) since bounds were never the cause. Still to watch: looking straight **down the open
> axis** of the cone (toward the far opening) has no lateral back face along that ray, so the shaft
> can thin to nothing there — an acceptable edge case (the beam is a small bright dot from that view).

> **Beam brightness & source-to-tip falloff (v1.0.40).** The shaft was faint and *faded out near the
> fixture* — the opposite of a real light beam. Cause: the raymarch density had no width
> normalization, so a view ray crosses a **short** path through the narrow near-lens region (faint)
> and a **long** path through the wide far end (brighter), peaking mid-beam; and the old `lenA`
> length term pushed the far end toward zero. Reworked the per-sample density in `M_RebusBeam`
> (re-baked, `0 error(s)`):
> - **Width-bias normalization** — density scales as `REF_RADIUS_CM / radiusAt`, cancelling the
>   chord-length growth so the on-axis shaft reads **uniform along its length** before falloff
>   (near-lens no longer faint).
> - **Distance-from-source falloff** — a softened inverse square `1 / (1 + BeamFalloff·(axial/Length)²)`
>   makes the beam **brightest at the lens** and dim smoothly downrange (matches the inverse-square
>   intuition). `BeamFalloff` is now this falloff **strength** (0 = flat, higher = faster dimming),
>   not the old length-fade exponent.
> - **Lens-diameter start** — the cone base radius (`BeamBaseRadiusUnreal`, fed as `LensRadius`) is
>   the resolved lens radius, floored to `RebusBeamLensRadiusFloorCm` (**3 cm**) so the shaft begins
>   as a visible disc of the lens diameter, not a point; the mesh base ring and the material radial
>   profile agree on it at `axial=0`.
> - **Brighter defaults** — `RebusMeshBeamMaxIntensity` 3 → **4** and `RebusBeamDensity` 0.0025 →
>   **0.015**.
>
> **Tune live (MID scalar params, no re-bake):** `BeamDensity` (overall thickness/visibility, higher
> = denser), `BeamIntensity` (additive brightness; also driven by dimmer × `SetFixtureBeamVolumetrics`),
> `BeamFalloff` (source→tip dimming strength; lower = more even along length), `BeamSharpness` (radial
> edge softness).

> **Capped (closed) beam cone (v1.0.41).** `UpdateBeamConeGeometry` now adds a **base cap** (disc at
> the lens, `x=0`) and a **far cap** (disc at the throw, `x=L`) as triangle fans to axis-centre
> vertices, so the procedural cone is a fully closed volume. This resolves the v1.0.39 down-axis edge
> case: looking straight down the cone's axis previously hit no lateral wall → no fragment → the
> shaft thinned to nothing; the caps now provide a fragment along the axis, so the raymarch sees a
> surface and the full column inscatters (a bright disc/column instead of nothing). The raymarch is
> unchanged (C++ geometry only, **no re-bake**): a cap fragment behaves exactly like the side wall —
> EXIT = its own surface depth (clamped by scene depth), ENTRY = camera when inside else the analytic
> front intersection — so a near cap self-cancels and the far cap carries the column (**no
> double-add**). The material stays two-sided so caps render from both sides; cap normals are unused
> (unlit additive). The v1.0.40 distance falloff already dims the far end, so the far cap reads as the
> natural end of the column rather than a hard bright disc. Bounds are unchanged (cap centres lie on
> the axis within the existing ring extents).

> **DMX-fixtures-style beam + IES-driven light (v1.0.42).** Researched UE's native **DMX Fixtures**
> plugin (`Engine/Plugins/VirtualProduction/DMX/DMXFixtures`, `ADMXFixtureActor`): its visible beam
> is a **translucent cone static mesh raymarched** by `M_LightBeam` (params `DMX Quality Level` step
> size, `DMX Max Light Distance`, `DMX Max Light Intensity`, `DMX Lens Radius`), the lens is an
> **emissive disc**, and the cast light is a `USpotLightComponent` whose intensity is normalized by
> the cone solid angle and shaped by a **light-function cookie** — stock DMX does *not* read `.ies`.
> Our `ARebusFixtureActor` already matches this architecture (SpotLight + cone-mesh raymarch beam
> `M_RebusBeam` + emissive lens `M_RebusLensFlare`) **and exceeds it**: the SpotLight is driven by a
> true **runtime IES profile** built from the raw `.ies` text (`RegisterFixtureIes` →
> `RebusIes::BuildLightProfile` via the engine `IESFile`/`FIESConverter` module → `UTextureLightProfile`
> → `SpotLight->SetIESTexture`, `bUseIESBrightness=false` so the portal keeps brightness authority),
> with cone half-angle and candela intensity from the parsed photometrics. So no architectural
> "restart" was required; instead the beam **look** was matched to the DMX beam:
> - **Smooth Gaussian cross-section** (`core = exp(-rN²·BeamSharpness)`) replaces the old
>   `pow(1-rN)` hard rim — a soft glow with no crisp mesh edge.
> - **Soft depth fade** (`DEPTH_FADE_CM` = 50 cm) dissolves the shaft where it meets opaque geometry
>   (the DMX soft-particle look) instead of a hard scene-depth clip.
>
> Preserved: v1.0.34 direction, v1.0.39 camera-inside entry/exit + near fade, v1.0.41 end caps,
> pan/tilt/colour/dimmer, Orbit-model binding, no self-shadow. **IES path chosen: true runtime
> `UTextureLightProfile`** (already in place), not an angle approximation — the accurate option.

> **Epic's REAL DMX beam assets, byte-for-byte (v1.0.43).** Instead of *reproducing* the DMX look,
> the visible beam now uses Epic's **actual** DMX Fixtures content when it's installed: the official
> beam canvas mesh **`SM_Beam_RM`** + the official **`MI_Beam`** instance of **`M_Beam_Master`**
> (the world-space, object-transform-driven raymarch that uses `MF_WSIntersection`/`MF_BeamStepSize`/
> `MF_JitterOffset`). Verified on-disk object paths (UE 5.7, mount `/DMXFixtures`):
> - `/DMXFixtures/LightFixtures/DMX_Materials/MI_Beam.MI_Beam` (master `…/Masters/M_Beam_Master`)
> - `/DMXFixtures/LightFixtures/Meshes/SM_Beam_RM.SM_Beam_RM`
> - (lens content is also present: `…/DMX_Materials/MI_Lens`, lens meshes `…/Meshes/SM_*_Lens`)
>
> **How it's wired (`ARebusFixtureActor`).** `BuildBeamCone` still builds the procedural cone +
> `M_RebusBeam` as the **fallback canvas**, then `TryBuildEpicBeam()` attempts to load Epic's assets
> (cook-safe CDO `FObjectFinder`, else a config-overridable runtime `LoadObject`). On success it
> spawns an `EpicBeamCanvas` `UStaticMeshComponent` (`SM_Beam_RM` + a `MID` of `MI_Beam`), **hides**
> the procedural cone, and logs `using Epic M_LightBeam (MI_Beam + SM_Beam_RM)`; on failure it logs
> `Epic DMX content NOT found … using fallback beam (M_RebusBeam)`. **The active path is Epic's beam.**
>
> **Param mapping (our drives → `M_Beam_Master`, mirrors `ADMXFixtureActor::FeedFixtureData`):**
> - colour × dimmer × shutter-gate → **`DMX Color`** (vector) + **`DMX Max Light Intensity`** (scalar)
> - SpotLight throw (`AttenuationRadius`) → **`DMX Max Light Distance`**
> - lens radius (`ResolveLensDiameterMeters`, floored) → **`DMX Lens Radius`**
> - raymarch quality → **`DMX Quality Level`** = 1.0 (Epic "High")
> - pan/tilt/head (v1.0.34 ground truth) → the canvas **world transform**: `DriveEpicBeamFromSpotLight`
>   rides the live `USpotLightComponent` (origin = lens, mesh-local length axis → emission forward via
>   `FQuat::FindBetweenNormals`), and **scales `SM_Beam_RM` up** to enclose the cone (length × far
>   radius from the IES field angle). Because `M_Beam_Master` defines the cone in world space from its
>   params + the component transform, an over-sized canvas only adds coverage — it never distorts the
>   beam, so the shaft is robust at every camera angle including close/inside.
>
> **IES preserved:** the `USpotLightComponent` still uses our true runtime `UTextureLightProfile` —
> we're ahead of stock DMX (which uses light-function cookies). v1.0.36 no-self-shadow is applied to
> the Epic canvas too; v1.0.38 bounds/occluder flags match the procedural cone.
>
> **Install / cook.** The beam needs Epic's **DMX Fixtures** plugin content (the `DMXFixtures` plugin
> is enabled in `REBUS_Visualiser.uproject`). The content folder
> `Engine/Plugins/VirtualProduction/DMX/DMXFixtures/Content/LightFixtures` ships with the plugin; if a
> build is missing it, install/repair the **DMX Fixtures** plugin via the Epic Games Launcher (Unreal
> Engine → Installed → *Options* → enable the DMX/Virtual-Production components) or the **DMX** project
> template. `/DMXFixtures/LightFixtures` is added to `+DirectoriesToAlwaysCook` (`DefaultGame.ini`) so
> packaged builds include Epic's beam (it's only referenced at runtime). Optional path overrides:
> `[RebusVisualiser] EpicDmxBeamMaterial=…` / `EpicDmxBeamMesh=…`. If the content is ever absent the
> actor falls back cleanly to `M_RebusBeam` — no fabricated assets, fully reversible.

> **Epic beam alignment fix (v1.0.44).** v1.0.43 misaligned/reversed the Epic beam. Introspecting the
> installed content revealed how `M_Beam_Master` actually works: **`SM_Beam_RM` is a normalized unit
> tube** (local **length axis = −Z**, geometry spans Z `0..−1`, **pivot/apex at the Z=0 lens end**,
> bounds extended to ±10000 so it never culls), and the material builds the real cone with **World
> Position Offset** from its params — so the canvas component **must stay at scale (1,1,1)** (exactly
> why `ADMXFixtureActor::InitializeFixture` forces `WorldScale(1,1,1)`). The v1.0.43 bugs were: (1)
> **auto-scaling** the canvas (broke the WPO cone → misplaced), (2) **wrong local axis** (auto-picked
> +X instead of −Z → wrong way round), (3) **missing `DMX Zoom`/`DMX Dimmer` and wrong intensity
> scale**. `DriveEpicBeamFromSpotLight` now places the apex at the spotlight location, aims local −Z
> along the live spotlight forward (`FindBetweenNormals`), and sets `WorldScale(1,1,1)` — no scaling.
> Param mapping corrected to Epic's real vocabulary (verified defaults in `MI_Beam`):
> - cone **angle** → **`DMX Zoom`** = full beam angle in degrees (`2 × outer half`), `DMX Zoom Normalize=0`
> - **length** → **`DMX Max Light Distance`** (cm, capped to the ~10000 canvas length)
> - start radius → **`DMX Lens Radius`**; colour → **`DMX Color`**
> - brightness → **`DMX Max Light Intensity`** (Epic candela scale ~2000, not our `4`) × **`DMX Dimmer`** (0..1 = dimmer × shutter-gate)
>
> Verify with the log `Epic beam align: … dot=1.000 … |apex-lens|=0.00cm`. Live tuning knob:
> `RebusEpicBeamMaxIntensity` (beam brightness). The pan/tilt/head, IES spotlight, no-self-shadow and
> `M_RebusBeam` fallback are all preserved.

> **Epic beam pan direction + footprint size (v1.0.45).** Two follow-up fixes:
> - **Pan was mirrored.** v1.0.44 world-aimed the canvas every frame with `FindBetweenNormals(−Z,
>   spotFwd)`, whose roll varies with the aim; because `M_Beam_Master` derives the cone from the whole
>   object basis (not just the forward axis), that varying roll mirrored the yaw (and the v1.0.44
>   "dot≈+1" log was tautological — it compared the build quat to itself). Fix: the canvas is now
>   **parented to the `SpotLight`** with a **constant** relative transform (apex at the spotlight
>   origin, local −Z → spotlight local +X, scale 1) — exactly how `ADMXFixtureActor`'s beam rides its
>   `Head`. All pan/tilt now comes from the one basis that creates the footprint, so the beam tracks
>   **with** the pool. The alignment log now reads the canvas's **actual** world transform (real
>   proof): `dot≈+1` across the whole pan sweep, `|apex-lens|≈0`.
> - **Far end too wide.** `DMX Zoom` is now driven from the **SpotLight's live `OuterConeAngle`** (the
>   exact half-angle that defines the lit footprint — single source of truth) × `RebusEpicBeamZoomScale`
>   (default 1.0), instead of `2 × ResolveOuterHalfDeg()`. Empirically `M_Beam_Master` reads `DMX Zoom`
>   as ~the half-angle, so feeding the doubled value made the cone ~2× too wide; the beam edge now
>   meets the pool edge. Lower `RebusEpicBeamZoomScale` to hug the brighter IES core, raise it toward
>   the geometric field edge. Lens/start radius (`DMX Lens Radius`) unchanged.

> **Dynamic shadows in the Epic-beam footprint -- force-disable MegaLights on every Rebus SpotLight (v1.0.94).**
> User report (verbatim, against v1.0.93):
>
> > "with the Epic beam system we have, we are not seeing the shadow of an object in the
> > footprint can you fix this"
>
> "Epic beam system" = the v1.0.x DEFAULT mode (`bInternalBeam = false`), where the Epic
> DMX-Fixtures plugin's beam canvas + cone components are the visible shaft. The SpotLight
> still lights the floor footprint -- but objects placed between the fixture and the floor
> (a hanging truss / a person / a prop) lit up but cast NO silhouette into the lit pool.
> The operator expects to see a clear silhouette in the floor footprint of every fixture in
> every mode.
>
> ---
>
> **Investigation findings -- TWO independent gates were dropping dynamic-occluder shadows
> in the Epic-beam-mode footprint.**
>
> **(A) MegaLights routing dropped dynamic occluders below the shadow-fidelity floor.** UE
> 5.5+ MegaLights routes per-light shadow casting through a tile-clustered sampling pipeline
> (`MegaLightsRendering.cpp` -> `MegaLightsShadows.cpp`). On the user's hardware/quality tier
> the clustered shadow pass silently dropped dynamic occluders -- the lit pool rendered
> correctly but no silhouette appeared. v1.0.92 had already opted InternalBeam-mode SpotLights
> off MegaLights (for an unrelated volumetric LF/IES reason), but Epic-beam-mode fixtures
> stayed on `bAllowMegaLights = 1` (the v1.0.x BuildSpotLight default), inheriting the
> shadow-fidelity floor.
>
> **(B) `RefreshBeamShadowMode` cleared `SpotLight->CastShadows` on non-hero non-gobo
> fixtures.** The pre-v1.0.94 RefreshBeamShadowMode logic was
> `SpotLight->SetCastShadows(bShadowActive || bGoboActive)` -- a perf opt that disabled the
> per-light shadow map when neither the volumetric-shadow path (hero beams only, capped at
> 6 per spawn batch by `RebusMaxShadowFogBeams`) nor the cookie LF (a SpotLight LF only
> projects when the light is also casting shadows) needed it. A SpotLight with
> `CastShadows = false` produces NO shadows from any occluder, regardless of the MegaLights
> opt-out -- this gate alone would have produced the same symptom even if (A) had been
> fixed in isolation.
>
> **Both gates are now closed.** v1.0.94 forces `bAllowMegaLights = 0` on EVERY Rebus
> SpotLight at construction (gate A) AND keeps `CastShadows = true` always in BuildSpotLight
> + every RefreshBeamShadowMode call (gate B). Either one alone would not have fixed the
> reported symptom.
>
> Other root causes investigated and ruled out: D (volumetric vs solid shadow conflation --
> `bCastVolumetricShadow` is the FOG-volume-shadow flag, separate from `CastShadows` which
> controls solid shadow casting onto surfaces; both are now correctly scoped), E (cascaded
> shadow distance / `r.Shadow.*` -- the project ships defaults; nothing was clipping the
> ~10 m fixture-to-floor span), F (floor receives shadows -- the `RebusFloor`
> `StaticMeshActor` from `build_rebus_base_level.py` uses `/Engine/BasicShapes/Plane` which
> receives dynamic shadows by default; no override touches it).
>
> **The v1.0.87 shadow cache (`SetBodyMeshesCastShadow` / `OptPrimitiveOutOfInternalBeam
> Shadow`) was also audited.** The cache stores bit-packed `Entry.bCastShadow : 1` from
> `Comp->CastShadow ? 1 : 0`, restores via `Entry.bCastShadow != 0`, and the on/off paths
> are byte-exact symmetric. Weak-ptr staleness is handled by an `if (!Comp) continue;`
> guard. No restore-symmetry bug -- v1.0.94 leaves the v1.0.87 helper untouched.
>
> ---
>
> **New CVar -- `Rebus.AllowMegaLights [0|1]`** (default `0`).
>
> The HARD FLOOR for MegaLights routing on every Rebus SpotLight. Default `0` (every Rebus
> fixture forced off MegaLights, regardless of mode -- legacy clustered/deferred path always,
> dynamic-occluder shadows in the footprint always). Refresh sink walks every Rebus fixture
> in every loaded world and re-resolves `bAllowMegaLights` per the new value via
> `RefreshAllowMegaLightsFromCVar`, re-registering the SpotLight component when the value
> transitions. Live -- changing this re-pushes / restores immediately.
>
> ```
> Rebus.AllowMegaLights 0   # default (v1.0.94) -- legacy path always; dynamic shadows in every footprint; lose MegaLights' clustering perf
> Rebus.AllowMegaLights 1   # opt back in to MegaLights routing (per-fixture gobo / InternalBeam paths still force legacy when active)
> ```
>
> **Precedence vs `Rebus.InternalBeamForceLegacy` (v1.0.92).**
>
> | `Rebus.AllowMegaLights` | `Rebus.InternalBeamForceLegacy` | Effective per-fixture path                                              |
> |-------------------------|---------------------------------|-------------------------------------------------------------------------|
> | `0` (v1.0.94 default)   | any                             | EVERY Rebus SpotLight on legacy path. v1.0.92 CVar redundant but harmless. |
> | `1`                     | `1` (v1.0.92 default)           | Non-special fixtures: MegaLights on (perf). Gobo-active or InternalBeam fixtures: legacy. |
> | `1`                     | `0`                             | Non-special fixtures: MegaLights on (perf). Gobo-active fixtures: legacy. InternalBeam fixtures stay on MegaLights -- volumetric LF/IES on shaft NOT guaranteed (the v1.0.92 trade-off). |
>
> `Rebus.AllowMegaLights = 0` is the hard floor: ALL Rebus SpotLights run on the legacy path
> regardless of `Rebus.InternalBeamForceLegacy`. Both CVars live; new releases prefer
> `Rebus.AllowMegaLights` because it is the more fundamental gate.
>
> **Operator checklist after rebuilding to v1.0.94.**
>
> 1. Confirm `Rebus.AllowMegaLights` is at its default `0` (no operator action needed; the
>    CVar is registered in `RebusFixtureActor.cpp` with default `0`).
> 2. Spawn or re-spawn a fixture in the Epic-beam mode (`bInternalBeam = false`) -- the
>    BuildSpotLight log line should now read `allowMegaLights=0` (pre-v1.0.94 read
>    `allowMegaLights=1` in this mode).
> 3. Place a static occluder (a hanging truss / a `BP_StagePerformer` / a prop cube) BETWEEN
>    the fixture's lens plane and its lit floor pool. From a top-down camera the lit pool
>    should now show a CLEAR silhouette of the occluder -- the v1.0.94 fix.
> 4. Sweep pan/tilt: the silhouette should track the occluder's position relative to the
>    aim, dynamically (no shadow-map staleness on a Movable spotlight + Movable occluder).
> 5. Toggle `Rebus.AllowMegaLights 1` at runtime -- the silhouette should disappear (the
>    refresh sink switches every fixture back to MegaLights routing on this hardware/tier).
>    Toggle back to `0` -- the silhouette should reappear within one frame (the
>    ReregisterComponent transition rebuilds the FLightSceneInfo proxy with the new value).
> 6. (Optional) Run `Rebus.DumpFixtureLights` to confirm the per-fixture state matches:
>    `castShadows=1 bAllowMegaLights=0 LightFn=...`.
>
> **Perf trade-off.** Every Rebus SpotLight now loses MegaLights' clustering perf -- in a
> rig with hundreds of fixtures the per-frame lighting cost is higher than v1.0.93 by
> roughly the cost of one regular SpotLight per fixture (vs. the amortised clustered cost
> on the MegaLights path). In show-context rigs (tens of hero fixtures + a few dozen
> wash/PAR cans) this is the right default because shadow fidelity is non-negotiable for
> stage visualisation. Deployments hitting frame-budget limits with the new default can
> flip `Rebus.AllowMegaLights 1` and accept the loss of dynamic-occluder shadows in the
> footprint of non-special fixtures (gobo-active fixtures and `InternalBeam +
> InternalBeamForceLegacy = 1` fixtures still get the legacy path on a per-fixture basis).
>
> No subsystem-side `r.MegaLights.*` push was added -- the per-light `bAllowMegaLights = 0`
> already routes every Rebus fixture out of the MegaLights shadow path; touching the
> project-wide `r.MegaLights.Allow` would also affect any non-Rebus light spawned by the
> game (e.g. an Orbit-imported KHR_lights_punctual light), which is out of scope.
>
> **Files touched (v1.0.94).**
>
> - `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Public/RebusFixture
>   Actor.h` -- header-block legacy-path-policy comment + `RefreshAllowMegaLightsFromCVar`
>   declaration.
> - `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusFixture
>   Actor.cpp` -- `Rebus.AllowMegaLights` CVar + `ResolveAllowMegaLights` helper +
>   `RefreshAllowMegaLightsFromCVar` method; BuildSpotLight now resolves `bAllowMegaLights`
>   through the helper AND asserts `CastShadows = true`; RefreshBeamShadowMode now keeps
>   `CastShadows = true` always (was `bShadowActive || bGoboActive`); ApplyCurrentGobo
>   ToLightFn (clear branch) + PushInternalBeamMegaLightsOptOut + RestoreInternalBeamMega
>   Lights now route through the helper for consistency.
> - `README.md` / this file -- v1.0.94 release block (this one).

> **Cone-mesh InternalBeam shaft + Python-baked LF (`bUsedWithVolumetricFog=true`) + chrome lens (v1.0.93).**
> User report (verbatim, against v1.0.92):
>
> > "We are not seeing gobos in the volumetric beam of the spotlight. I think you need to
> > properly investigate how you do this in unreal. We should be adding it as a light function
> > material. Pelase make sure you are apply it to the internalbeam. we are also see the
> > volumetric beam inside the head, it should be hidden until it appears from out the lens.
> > can we also look at creating the lens material in the startup python script. It should have
> > metallic at 1 and roughness at 0. Base colour should be white."
>
> Three coupled fixes, one release.
>
> **Investigation finding (the smoking gun).** v1.0.92 forced `r.VolumetricFog.LightFunction=1`
> + `r.LightFunctionQuality=2` and pushed `bAllowMegaLights=0` on every InternalBeam
> SpotLight -- all necessary but **not sufficient**. The actual blocker is a per-`UMaterial`
> flag: **`bUsedWithVolumetricFog`**. The engine's `LightFunctionPixelShader` gates whether
> a light-function reaches the volumetric integrator on this flag on the bound `UMaterial`
> instance; Epic's stock `MI_Light` (parent `M_Light_Master`, the cookie material the v1.0.49
> path routes the gobo through) has `bUsedWithVolumetricFog = false`. With that flag off, no
> amount of CVar pushing makes the cookie appear on the volumetric shaft -- the material
> itself must opt in. v1.0.93 authors our own LF (`M_RebusGoboLightFunction`, Python-baked,
> `bUsedWithVolumetricFog = true`) and binds it to `SpotLight->LightFunctionMaterial` while
> InternalBeam mode is on. (The v1.0.92 CVar pushes stay -- they are still necessary; the new
> LF material now actually benefits from them.)
>
> **Three new Python-baked masters (`build_rebus_base_level.py`).** Bake on every startup
> via `ensure_base_level()`; force-regen on every full `build()`; self-heal probes detect a
> stale shape (missing parameter, missing flag) and force-regen on the next launch.
>
> | Path                                      | Used by                                    | Purpose |
> |-------------------------------------------|--------------------------------------------|---------|
> | `M_RebusFixtureLens`                      | Lens disc (GDTF `<Beam>`)                  | Chrome mirror: `Metallic=1`, `Roughness=0`, `BaseColor` white. Replaces the operator-authored asset; runtime fallback MID kept as a safety net. |
> | `M_RebusGoboLightFunction`                | `SpotLight->LightFunctionMaterial` in InternalBeam | LF with `bUsedWithVolumetricFog=true`; samples the per-fixture `GoboRT` so the cookie pattern reaches the volumetric integrator. |
> | `M_RebusInternalBeamShaft`                | `InternalBeamShaft` cone-mesh component    | Unlit additive cone-mesh; world-space cross-section UV + Gaussian falloff + SceneDepth soft-clip; samples `GoboRT` so the cookie pattern appears IN the shaft. |
>
> **Fix 1 -- gobo on the volumetric shaft (LF wiring).** v1.0.93 routes the cookie through
> the new `M_RebusGoboLightFunction` (LF MID, per fixture) instead of `MI_Light` while
> InternalBeam mode is on. The MID's `GoboRT` parameter is pushed alongside the existing
> v1.0.49 `DMX Gobo Disk Frosted` push (`ApplyCurrentGoboToLightFn` -> `PushGoboRTTo
> InternalBeamMaterials`), so a single gobo change updates both the v1.0.49 floor-footprint
> path AND the v1.0.93 LF that the engine's volumetric integrator samples. On the InternalBeam
> OFF edge the SpotLight's prior `LightFunctionMaterial` is restored byte-exact from a
> separate cache (`InternalBeamPriorLightFunction`) so the v1.0.49 cookie keeps owning the
> floor footprint outside of InternalBeam mode.
>
> **Fix 2 -- no more "shaft inside the head" (cone-mesh shaft).** v1.0.87's back-offset
> pushes the SpotLight INSIDE the head body; v1.0.92's per-light volumetric scattering then
> painted scattering EVERYWHERE in the cone, including between the SpotLight and the lens
> (the operator could see it from close camera angles). v1.0.93 decouples "lit footprint"
> from "visible shaft": the SpotLight now lights surfaces only (`VolumetricScattering
> Intensity = 0`), and a **translucent additive cone mesh** -- `InternalBeamShaft`,
> procedurally built off `BuildBeamCone`'s frustum recipe -- starts AT the lens plane
> (`SpotLoc - back-offset * LiveFwd`) and is the only visible shaft. Material is the new
> `M_RebusInternalBeamShaft` which:
>
> - Projects each rasterized pixel into the cone's cross-section (Custom HLSL), so the
>   `GoboRT` texture sample wraps the cone correctly regardless of how the procedural mesh is
>   UV-mapped;
> - Multiplies by per-fixture `Color` + `Intensity` (pushed live by `RefreshInternal
>   BeamShaftEmissive` from `RefreshIntensity`, same envelope as `M_RebusBeam`);
> - Soft-Gaussian-fades to the cone silhouette (no hard rim);
> - Length fade so the shaft dims downrange (`1 / (1 + 1.5 * aN^2)`);
> - SceneDepth soft-fade so the shaft soft-clips against opaque geometry (no hard
>   intersection line).
>
> The cone is `CastShadow=false`, tagged `RebusInternalBeamShaft`, and explicitly skipped by
> `SetBodyMeshesCastShadow` / `OptPrimitiveOutOfInternalBeamShadow` (would corrupt the
> cache otherwise).
>
> **Fix 3 -- Python-author the chrome lens.** `M_RebusFixtureLens` was previously operator-
> authored; v1.0.93 bakes it on every startup with `Metallic=1`, `Roughness=0`, `BaseColor`
> white (exact spec the user asked for). C++ `ConstructorHelpers::FObjectFinder<UMaterial
> Interface> UserLensMatFinder(...)` already auto-loads the asset at the expected path, so
> the change is invisible to anyone whose project already had the operator-authored variant
> -- the self-heal probe (`_fixture_lens_master_is_current`) checks for the three named
> parameters (`Color` / `Metallic` / `Roughness`) and force-regens an older shape on the
> next launch.
>
> **New CVar -- `Rebus.InternalBeamCookieCone [0|1]`** (default `1`). When `1` (default),
> every fixture in InternalBeam mode uses the v1.0.93 cone-mesh shaft. When `0`, the cone
> mesh is hidden and `RefreshBeamShadowMode` restores the v1.0.92 per-light scattering -- A/B
> at runtime without a `Rebus.InternalBeam 0/1` cycle. Refresh sink walks every fixture in
> InternalBeam mode and calls `ApplyInternalBeamShaft(true)` / `RestoreInternalBeamShaft()`
> in-place.
>
> **Operator checklist after rebuilding to v1.0.93.**
>
> 1. **Run the Python builder once** (auto-runs on startup via `ensure_base_level`, but a
>    manual run via *Tools > Execute Python Script > build_rebus_base_level.py* with the
>    `build` entry point force-regenerates the three new masters under
>    `/Game/REBUS/Materials/`):
>    - `M_RebusFixtureLens` (chrome lens)
>    - `M_RebusGoboLightFunction` (volumetric-aware LF)
>    - `M_RebusInternalBeamShaft` (cone-mesh shaft)
> 2. **Toggle InternalBeam ON** on a fixture and verify, from the editor viewport:
>    - The visible volumetric shaft starts AT the lens plane (not inside the head body).
>    - The shaft envelope tracks the live zoom (sweep the zoom channel; the cone-mesh
>      far-radius rebuilds on changes > 0.5 cm via the same rebuild gate as `BeamCone`).
>    - The lens disc reads as a **chrome mirror** (high specular, no roughness, white
>      base) -- confirms `M_RebusFixtureLens` baked correctly.
> 3. **Assign a gobo** (`/animations/{id}/gobo` or via the wire) and verify:
>    - The cookie pattern projects through the floor footprint (v1.0.49 path, unchanged).
>    - The cookie pattern **appears IN the volumetric beam shaft** (the v1.0.93 fix). Sweep
>      `gobo.angle` -- the pattern spins in plane on the shaft AND on the floor in
>      lock-step (single source of truth: `GoboRT`).
>    - The shaft is GOBO-shaped through volumetric fog (`r.VolumetricFog 1`) -- this is
>      the smoking-gun proof that the new LF's `bUsedWithVolumetricFog=true` flag is now
>      reaching the integrator.
> 4. **Toggle `Rebus.InternalBeamCookieCone 0`** -- the cone-mesh shaft disappears and the
>    v1.0.92 per-light scattering returns (the shaft is now the SpotLight's volumetric
>    scattering pass again, with the v1.0.87 back-offset; you should see the head-body
>    artefact return). Toggle back to `1` -- the artefact goes away. A/B confirmation that
>    the cone-mesh path is what fixed the "shaft inside the head" symptom.
> 5. **Toggle InternalBeam OFF** and verify the SpotLight's `LightFunctionMaterial`
>    returns to whatever it was before (typically the v1.0.49 `MI_Light` cookie MID, OR
>    null if no gobo was active), `VolumetricScatteringIntensity` returns to the v1.0.92
>    cached value, and the cone-mesh shaft is hidden (not destroyed -- the next ON toggle
>    is cheap).
>
> **Trade-offs.**
>
> - The cone-mesh shaft is a SEPARATE draw call per InternalBeam fixture (one translucent
>   additive primitive per fixture). Per-fixture cost is small (24-segment frustum, ~96
>   triangles, no shadow casting, no Lumen contribution); negligible in a typical concert
>   rig (~tens of InternalBeam fixtures). Heaviest cost is the per-pixel Custom HLSL
>   (world-space basis computed per-pixel; ~12 alu); pre-rasterised by the additive blend
>   so it doesn't pollute opaque depth.
> - The cone-mesh shaft does NOT raymarch -- a single per-pixel sample of the cross-section
>   gives a Gaussian-tube look, not a true integrated volume. The v1.0.92 per-light
>   scattering remains available behind `Rebus.InternalBeamCookieCone 0` if a follow-up
>   release needs true integration. A future v1.0.94+ could swap the Gaussian for a
>   `M_RebusBeam`-style raymarch (`_BEAM_RAYMARCH_HLSL` is the prior art) at noticeably
>   higher per-pixel cost.
> - The SpotLight's per-light volumetric pass is OFF while the cone-mesh shaft is on. The
>   SpotLight still casts shadows for surface lighting + IES / LF projection on opaque
>   geometry; only the volumetric integration is suppressed. If a future scene needs BOTH
>   the cone-mesh shaft AND per-light volumetric scattering on the same fixture (e.g. for
>   fog that the cone-mesh's SceneDepth fade can't see), `Rebus.InternalBeamCookieCone 0`
>   reverts to the v1.0.92 path; a future toggle could let them co-exist by restoring
>   `VolumetricScatteringIntensity` to the cached value while leaving the cone-mesh
>   visible -- not implemented now because it'd double-shade the shaft.
> - The new LF MID is one-per-fixture (each fixture's `GoboRT` is independent, so the LF
>   MID must be too). Memory cost is ~1 MID + a per-fixture reference to the shared master
>   -- negligible.
>
> **Files touched (v1.0.93).**
>
> - `REBUS_Visualiser/Content/Python/build_rebus_base_level.py` -- three new ensure_*
>   functions + self-heal probes + wired into `build()` and `ensure_base_level()`.
> - `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Public/RebusFixture
>   Actor.h` -- declared `ApplyInternalBeamShaft` / `RestoreInternalBeamShaft` +
>   `EnsureFixtureInternalBeamMIDs` / `UpdateInternalBeamShaftGeometry` / `RefreshInternal
>   BeamShaftEmissive` / `DriveInternalBeamShaftFromSpotLight` / `PushGoboRTToInternalBeam
>   Materials`; declared the new master / MID / component UPROPERTYs.
> - `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusFixture
>   Actor.cpp` -- new `Rebus.InternalBeamCookieCone` CVar + refresh sink; constructor
>   `FObjectFinder` for the new masters; full implementations + wiring into `ApplyInternal
>   BeamPose` / `RestoreInternalBeamPose` / `RefreshMotion` (Drive*) / `RefreshIntensity`
>   (Refresh*) / `RecomputeConeAngles` (Update*Geometry) / `ApplyCurrentGoboToLightFn`
>   (PushGoboRT*); `InternalBeamShaft` skip-add to `SetBodyMeshesCastShadow` + `Opt
>   PrimitiveOutOfInternalBeamShadow`.
> - `README.md` / this file -- v1.0.93 release block (this one).

> **Gobo + IES modulate the volumetric beam shaft -- force legacy path, push volumetric LF CVars (v1.0.92).**
> User report (verbatim, against v1.0.90):
> *"gobo or IES is not being applied to the volumetric beam of the spotlight"*.
>
> The operator is in InternalBeam mode (v1.0.87+) with a gobo loaded and a per-fixture IES
> profile bound (the v1.0.91 candela-max chain landed both on every fixture's SpotLight).
> Symptoms confirmed: the gobo IS visible in the lit floor footprint (the cookie projects
> through the SpotLight's `LightFunctionMaterial`), the IES IS shaping the floor footprint
> (the v1.0.91 candela-max -> Intensity wiring works), but NEITHER reaches the visible
> volumetric beam shaft -- the cone in fog reads as a uniformly-bright untextured cone
> regardless of the gobo cookie or the IES profile.
>
> The v1.0.89 `r.LightFunctionAtlas.Enabled = 1` push was a necessary first step (caveat
> documented at the time: "the atlas push has now proven insufficient -- pursue the
> documented fallback only if it is"). v1.0.92 finds and fixes the actual root cause.
>
> ---
>
> **Root cause -- TWO independent gates were silently dropping LF + IES on the volumetric
> shaft.**
>
> **(A) MegaLights bypasses BOTH `IESTexture` and `LightFunctionMaterial` on the
> volumetric integrator.** UE 5.5+ MegaLights routes per-light volumetric scattering through
> the many-lights-per-pixel sampling pipeline (`MegaLightsRendering.cpp`), which does NOT
> sample either the IES texture OR the light function when computing the per-froxel
> contribution -- the IES + LF terms are evaluated only on the per-pixel surface lighting
> pass. Net effect: a SpotLight with `bAllowMegaLights = 1` (the v1.0.x default) reads as
> a uniform untextured cone in fog regardless of what its IES profile or its gobo cookie say.
>
> Pre-v1.0.92 we ONLY opted out of MegaLights when a gobo was active (the v1.0.50
> `ApplyCurrentGoboToLightFn` opt-out, so the legacy deferred path could project the
> cookie at all). With NO gobo, the SpotLight stayed on the MegaLights path and the IES
> profile silently lost its volumetric effect. With a gobo, the cookie now reached the
> *floor* via the deferred path but the volumetric integrator still didn't sample it (a
> separate engine-side gate, see (B)).
>
> **(B) `r.VolumetricFog.LightFunction` AND `r.LightFunctionQuality` gate volumetric LF
> integration.** Even on the legacy deferred path, two engine CVars determine whether a
> SpotLight's `LightFunctionMaterial` is folded into the volumetric scattering integrator:
>
> * `r.VolumetricFog.LightFunction` (default 1 in UE 5.5+) -- the actual gate. 0 = LFs
>   never modulate volumetric fog regardless of any other setting. The v1.0.x DefaultEngine
>   .ini doesn't touch it, but a prior `Rebus.GoboAntiGhost`-style scalability pack (or a
>   future portal-side push) can drive it to 0 silently.
> * `r.LightFunctionQuality` -- 0 disables LFs entirely (no floor footprint, no shaft).
>   The v1.0.74 anti-ghost pack pushes this to 2 (high), but v1.0.83 made that pack
>   default-OFF, so a deployment that hasn't enabled it can be running at the engine
>   default (1 = default), at which the volumetric LF samples are noticeably softer than
>   the floor-footprint samples.
>
> ---
>
> **Fix (per-fixture).** `ApplyInternalBeamPose` now calls
> `PushInternalBeamMegaLightsOptOut()` on the InternalBeam ON edge -- caches
> `SpotLight->bAllowMegaLights` into `bInternalBeamAllowMegaLightsOrig` (latched via
> `bInternalBeamMegaLightsOrigCached`), forces the flag to 0, and triggers a
> `ReregisterComponent()` because `bAllowMegaLights` is read at proxy creation time
> (`FLightSceneInfo` -> `Proxy->AllowMegaLights()`, see the v1.0.51 comment on the same
> flag near `ApplyCurrentGoboToLightFn`). `RestoreInternalBeamPose` calls the symmetric
> `RestoreInternalBeamMegaLights()` which pushes the cached value back byte-exact.
>
> Idempotency: a re-entrant call (e.g. ApplyCurrentGoboToLightFn already opted out because
> a gobo went active before InternalBeam was enabled) re-uses the existing cache and just
> short-circuits the proxy rebuild -- the OFF restore still lands the construction-time
> value, NOT the value the gobo path installed.
>
> Gated by a new operator-flippable CVar:
>
> ```
> Rebus.InternalBeamForceLegacy 1   # default (v1.0.92) -- volumetric LF + IES on, MegaLights off for InternalBeam fixtures
> Rebus.InternalBeamForceLegacy 0   # leaves bAllowMegaLights at whatever BuildSpotLight + the gobo path set it to
> ```
>
> The CVar refresh sink walks every fixture currently in InternalBeam mode and toggles the
> opt-out in-place: ON re-applies the push (re-priming the cache), OFF restores the cached
> value (so a deployment can A/B at runtime without a full `Rebus.InternalBeam 0/1` cycle).
>
> **Fix (engine CVars).** `URebusSceneSettingsSubsystem::SetInternalBeamEnabled(true)` now
> ALSO calls `PushVolumetricFogLightFunctionForInternalBeam(true)` (paired with the v1.0.89
> `PushLightFunctionAtlasForInternalBeam`) so the engine-side gates are forced ON for the
> duration of InternalBeam mode:
>
> | CVar                                | v1.0.92 push value | Restore               |
> | ----------------------------------- | ------------------ | --------------------- |
> | `r.LightFunctionAtlas.Enabled`      | `1` (v1.0.89)      | cached prior int      |
> | `r.VolumetricFog.LightFunction`     | `1` (v1.0.92)      | cached prior int      |
> | `r.LightFunctionQuality`            | `2` (v1.0.92, high)| cached prior int      |
>
> Each CVar's prior value is cached at the OFF -> ON edge into a private member of the
> subsystem (`VolumetricFogLightFunctionPriorValue`, `LightFunctionQualityPriorValue`,
> latched by `bVolumetricFogLightFunctionPushActive`). The OFF transition restores the
> cached value byte-exact via `IConsoleVariable::Set(value, ECVF_SetByGameOverride)` --
> sentinel `-1` ("no snapshot") falls back to `1` (the engine default in UE 5.5+), safer
> than `0` because any other gobo wiring on a stage scene expects volumetric LF live.
>
> Defensive against the CVars being unregistered (renderer module not loaded yet, or the
> engine drop renamed them): the helper logs a warning per missing CVar and SKIPS the
> latch flip when BOTH are missing, so a later call after the renderer module loads can
> still install the push.
>
> ---
>
> **Operator-visible logs.** Both transitions emit one line each so the operator can grep
> for the live state in one place:
>
> ```
> v1.0.92 InternalBeam: r.VolumetricFog.LightFunction was=1 now=1, r.LightFunctionQuality was=1 now=2 (push so SpotLight LightFunctionMaterial + IESTexture modulate the volumetric beam shaft).
> Fixture <id> InternalBeam MegaLights opt-out: bAllowMegaLights 1 -> 0 (orig=1 cached). Volumetric scattering integrator now samples IES + LightFunction; perf trade-off: this fixture loses MegaLights' clustering.
> ...
> Fixture <id> InternalBeam MegaLights restore: bAllowMegaLights 0 -> 1 (cached orig).
> v1.0.92 InternalBeam: r.VolumetricFog.LightFunction was=1 restored=1, r.LightFunctionQuality was=2 restored=1 (InternalBeam OFF -> released the v1.0.92 force pushes).
> ```
>
> The `SetInternalBeamModeEnabled` per-fixture summary log gains three new fields so a
> single line proves the chain landed:
>
> ```
> Fixture <id> InternalBeam mode ENABLED (... forceLegacy=1 allowMegaLights=0 cachedOrig=1).
> ```
>
> `forceLegacy` is the live CVar; `allowMegaLights` is the post-push value on the
> SpotLight; `cachedOrig` is what the OFF transition will restore (or `-1` when the cache
> wasn't primed because the operator had `Rebus.InternalBeamForceLegacy 0`).
>
> ---
>
> **Operator checklist after rebuilding to v1.0.92.**
>
> 1. Toggle InternalBeam ON via the portal (`SetSceneProperty bInternalBeam true`) or
>    console (`Rebus.InternalBeam 1`).
> 2. Send a fixture a gobo: `SetFixtureGobo` with a non-zero `goboIndex`.
> 3. Verify the gobo pattern carves through the visible volumetric beam shaft (not just
>    the floor footprint). The cookie's bright/dark areas should be visible inside the cone
>    of light through fog.
> 4. Sweep the fixture pan/tilt and confirm the shaft KEEPS the cookie pattern -- the
>    pattern should track the head, not stay screen-locked.
> 5. Verify the IES profile shapes the volumetric shaft (the shaft has the IES profile's
>    directional intensity falloff, not a uniform cone).
> 6. Send `SetFixtureZoom` with a few different zoom values and confirm the IES re-binds
>    (the v1.0.91 zoom-keyed selection re-runs `SelectIesForZoom`, which re-applies the
>    new candela max + the new `IESTexture` -- if the shaft shape doesn't change with
>    zoom, the IES re-bind isn't running).
> 7. Toggle InternalBeam OFF and confirm the Epic beam returns intact AND the SpotLight's
>    `bAllowMegaLights` flag returns to its v1.0.92 cached value (1 by default; 0 if a gobo
>    is still active, in which case the gobo path will keep MegaLights off).
> 8. (Optional) Flip `Rebus.InternalBeamForceLegacy 0` while in InternalBeam mode and
>    verify the SpotLight goes back onto the MegaLights path (uniformly-bright cone in fog
>    again -- expected for that mode). Flip back to 1 to re-shape the shaft.
>
> Diagnostic console commands (already in the codebase):
>
> * `Rebus.DumpFixtureLights` -- prints `bAllowMegaLights`, `LightFunctionMaterial`,
>   `IESTexture` per fixture (so the operator can confirm the per-fixture flags landed).
> * `Rebus.DumpFixtureIes` (v1.0.91) -- prints the active IES profile + parsed candela max
>   + the live `SpotLight->Intensity` formula breakdown.
> * `Rebus.DumpGoboState` -- prints `bGoboActive`, the GoboRT pointer, and confirms the
>   cookie material is bound.
>
> ---
>
> **Perf trade-off.** Forcing `bAllowMegaLights = 0` removes this fixture from MegaLights'
> tile-based clustering for the duration of InternalBeam mode -- the floor footprint and
> the volumetric shaft both render via the legacy deferred / clustered path. For a stage
> show with 4-32 fixtures (a typical InternalBeam-mode session), the cost is acceptable
> (the InternalBeam mode is itself a high-fidelity hero-beam choice; operators ship it
> when "looks photographic" matters more than "lights N+1"). For deployments where
> MegaLights' clustering is what makes the scene tractable (hundreds of fixtures simultaneously
> in InternalBeam mode), flip `Rebus.InternalBeamForceLegacy 0` to keep MegaLights on -- the
> floor footprint still shows the gobo + IES correctly (those paths don't depend on the
> per-fixture MegaLights flag), only the volumetric shaft loses LF/IES shaping.
>
> ---
>
> **Documented escalation path (NOT implemented in v1.0.92, only documented).** If a future
> engine drop changes the volumetric LF/IES routing again and the v1.0.92 push proves
> insufficient on someone's deployment (the gobo + IES still don't reach the shaft after
> a clean rebuild + a verified `Rebus.DumpFixtureIes` + `Rebus.DumpFixtureLights` pass), the
> next step is the **translucent additive cone-mesh fallback**: author a translucent
> additive cone mesh wrapped around the SpotLight that samples the cookie texture (and the
> IES profile if practical) via a Custom HLSL node, multiplied by `BeamColor * Intensity`
> and a soft Gaussian falloff. The existing `M_RebusBeam` raymarch material in
> `Content/Python/build_rebus_base_level.py` (`_BEAM_RAYMARCH_HLSL`) is a complete reference
> for the cone-mesh structure -- a v1.0.93+ would gate the new path behind a new
> `Rebus.InternalBeamCookieCone [0|1]` CVar (default OFF) so the operator can A/B against
> the engine-native v1.0.92 path without disturbing it. **DO NOT implement this fallback
> without first confirming v1.0.92 is genuinely insufficient on the user's hardware** --
> the cone-mesh path adds a per-fixture translucent draw call and another texture sample
> chain (cookie + IES + Gaussian) that the engine-native v1.0.92 path doesn't pay for.
>
> ---
>
> **Files touched.**
>
> ```
> REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Public/RebusFixtureActor.h
> REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusFixtureActor.cpp
> REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Public/RebusSceneSettingsSubsystem.h
> REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusSceneSettingsSubsystem.cpp
> REBUS_Visualiser/Plugins/RebusVisualiser/README.md
> ```
>
> No changes to `RebusVisualiser.cpp` -- the `Rebus.InternalBeamForceLegacy` CVar is
> registered via `FAutoConsoleVariableRef` (mirroring the v1.0.87 `Rebus.InternalBeamScatter`
> + v1.0.89 `Rebus.InternalBeamOffsetSign` registrations in `RebusFixtureActor.cpp`), so
> it auto-registers at module load and auto-unregisters at shutdown without a separate
> `IConsoleManager::RegisterConsoleCommand` / `UnregisterConsoleObject` plumbing block.

> **IES profile + IES candela max drive the SpotLight (v1.0.91).**
> User: *"can we make sure we use the IES profile and IES intensity for the beam/spotlight"*.
>
> The user is asking us to verify (and fix where missing) that the per-fixture `.ies` file
> drives BOTH the spatial distribution AND the absolute brightness of the SpotLight that
> v1.0.87's InternalBeam mode now promotes to the visible volumetric beam. The mode-of-the-
> day for the operator is InternalBeam ON, so the IES chain must work in that path in
> particular.
>
> ---
>
> **Audit findings (what already worked, what was missing).**
>
> Pre-v1.0.91 the SpotLight's `IESTexture` wiring was already in place end-to-end:
>
> 1. **Fetch.** `URebusVisualiserSubsystem` accepts both inline IESNA LM-63 text via the data-
>    channel `RegisterFixtureIes` descriptor (one or more profiles per `libraryId`, indexed by
>    `zoomDmx`) and signed `iesUrl` / `iesProfileUrl` URLs in the REST `/api/ue/fixtures/{id}`
>    profile. The blobs reach `ARebusFixtureActor::Setup` as `FRebusInlineIes` /
>    `FRebusFixtureProfile.IesProfiles` -- ALREADY WORKING.
> 2. **Build.** `RebusIes::BuildLightProfile` runs the raw `.ies` text through the engine's
>    `FIESConverter` (same path the editor importer uses) and returns a runtime
>    `UTextureLightProfile` -- ALREADY WORKING.
> 3. **Apply (spatial).** `ARebusFixtureActor::SelectIesForZoom` calls
>    `SpotLight->SetIESTexture(Profile)` and sets `bUseIESBrightness=false` +
>    `IESBrightnessScale=1.0` so the texture only reshapes the spatial falloff -- ALREADY
>    WORKING.
> 4. **Zoom-keyed selection.** `SelectIesForZoom` maps the live zoom half-angle to a 0..255
>    `zoomDmx` key and picks the nearest inline profile (preferred) then the nearest URL
>    profile. Called from `Setup`, from `ApplyZoom` (snap path), from `Tick` (fade path via
>    `bConeAnim`), so a zoom step OR a fade re-selects the right `.ies` automatically --
>    ALREADY WORKING.
> 5. **IntensityUnits = Candelas.** `BuildSpotLight` calls
>    `SpotLight->SetIntensityUnits(ELightUnits::Candelas)` once at construction -- ALREADY
>    WORKING. UE's default is `Unitless`, which would make a candela-typed Intensity value
>    physically meaningless.
> 6. **InternalBeam interplay.** v1.0.87's `ApplyInternalBeamPose` / `RestoreInternalBeam
>    Pose` touch volumetrics + visibility + per-primitive shadow flags, NOT the IES texture
>    nor `IntensityUnits` nor `Intensity`. Verified by inspection: the IES chain survives
>    `Rebus.InternalBeam 1` / `0` cycles unchanged.
>
> **What was MISSING.** The `.ies` file's **peak candela** (what the photometric file is
> literally describing -- the brightest direction's luminous intensity, expressed in
> candelas) was NEVER folded into `SpotLight->Intensity`. The intensity formula was:
>
> ```
> SpotLight->Intensity = BaseCandela * Dimmer.Current * shutter-gate
> ```
>
> where `BaseCandela` came from the JSON profile's `photometrics.luminousFlux /
> photometrics.fieldAngle` via the spherical-cap solid-angle integral:
>
> ```
> BaseCandela = LuminousFlux / (2 * PI * (1 - cos(fieldAngle / 2)))
> ```
>
> That's a defensible flux-derived estimate (and remains the fallback when no `.ies` is
> available), but it ignores the IES file's directly-measured peak candela. So a fixture
> with a photometric profile claiming `60000 cd` peak would render at whatever cosmic
> brightness the flux integral computed -- usually different. With `bUseIESBrightness=false`,
> UE treats the IES texture as a peak-1 spatial distribution multiplied by
> `SpotLight->Intensity`, so the candela max was getting silently lost.
>
> ---
>
> **Fix -- `IesCandelaMax` is now the BASE for `SpotLight->Intensity`.**
>
> ```
> SpotLight->Intensity = ((IesCandelaMax >= 0) ? IesCandelaMax : BaseCandela)
>                        * FMath::Clamp(Dimmer.Current, 0, 1)
>                        * shutter-gate
> ```
>
> `IesCandelaMax` is captured directly from `FIESConverter::GetBrightness() *
> GetMultiplier()` -- exactly the value the editor importer would write into
> `UTextureLightProfile::Brightness` for a cooked asset, so the runtime path is numerically
> identical to the cooked / `WITH_EDITORONLY_DATA` path. The dimmer (operator + DMX
> intensity multipliers fold into `Dimmer.Current`) and the shutter-gate (open/closed/strobe)
> stay as linear multipliers on top, so the existing wire surface
> (`SetFixtureDimmer` / `SetFixtureShutter`) and the v1.0.60 single-source-of-truth contract
> for dimmer on the SpotLight Intensity are untouched. The flux-derived `BaseCandela` is
> kept as the FALLBACK for fixtures that arrive with no inline IES + no `iesUrl` (e.g.
> light-only data-channel pushes), so behaviour for profile-less fixtures is byte-exact to
> pre-v1.0.91.
>
> Why NOT `bUseIESBrightness = true` (the "automatic" UE path). Flipping that flag tells UE
> "use the IES brightness as the absolute candela for the light", ignoring
> `SpotLight->Intensity`. But it also drives the per-sample intensity of the IES texture
> directly off `Brightness * IESBrightnessScale`, which would mean folding the dimmer in via
> `IESBrightnessScale` -- a different code path and a different multiplier semantic. The
> explicit BASE-via-`SpotLight->Intensity` path keeps the existing dimmer chain authoritative
> (one source of truth, see RefreshIntensity's v1.0.60 comment), keeps gate behaviour
> consistent across the SpotLight + Epic beam + cone-mesh paths, and matches the
> portal-keeps-brightness-authority contract in `RebusIes.h` step 4.
>
> ---
>
> **Where the candela max is captured + refreshed.**
>
> `RebusIes::BuildLightProfile(Outer, Bytes, float* OutCandelaMax)` is the single capture
> point -- the existing `FIESConverter` instance already parses the brightness; v1.0.91
> just surfaces it through a new out-parameter (defaults to `nullptr` so any
> hypothetical other caller stays source-compatible). Both call sites in
> `ARebusFixtureActor` -- the inline `iesText` path AND the URL-fetch async lambda -- now
> read the parsed candela max, store it on the actor as `IesCandelaMax`, and call
> `RefreshIntensity()` so the new BASE takes effect immediately (no waiting on the next
> dimmer push). The clear path (no inline + no URL -> synthetic cone fallback) ALSO calls
> `RefreshIntensity` so the actor falls back to `BaseCandela` cleanly instead of leaving
> the SpotLight at the previous IES's peak.
>
> **Zoom-keyed refresh.** `SelectIesForZoom` is re-entered on every zoom step
> (`ApplyZoom`'s snap path + the `Tick` fade path) and re-captures the candela max from
> whichever `zoomDmx` profile is now nearest. So a zoom-keyed IES set (one `.ies` per
> zoom step) drives a per-zoom intensity envelope naturally -- the spatial distribution
> AND the peak both come from the right `.ies` file at every zoom point. No additional
> `RefreshIesForZoom()` API was needed.
>
> **Verbose log on every IES apply.** Both call sites emit:
>
> ```
> Fixture <id> IES applied: profile=<id> zoomDmx=<n> candelaMax=<cd> intensityUnits=Candelas finalIntensity=<cd> (source=inline|url)
> ```
>
> Grep `IES applied:` to confirm a fresh IES landed AND that the resulting
> `SpotLight->Intensity` is what you expect (= candela max * live dimmer * gate).
>
> ---
>
> **New diagnostic console command.**
>
> ```
> Rebus.DumpFixtureIes [fixtureId]
> ```
>
> Dumps THIS fixture's complete IES state in one line (LogRebusVisualiser Log):
>
> ```
> DumpFixtureIes '<id>' source=inline|url|none(synthetic-cone) profileId='<id>' zoomDmx=<n>
>     zoomHalfDeg=<deg> iesTexture=<UObject name> candelaMax=<cd> baseCandela(flux-derived)=<cd>
>     -> activeBase=<cd> SpotLight intensityUnits=<n> intensityLive=<cd>
>     expected(=base*dim*gate)=<cd> dimmer=<0..1> shutterMode=<0|1|2> gate=<0..1>
>     inlineCount=<n> urlCount=<n> bUseIESBrightness=<0|1> IESBrightnessScale=<f>
> ```
>
> With NO arg dumps every fixture in every Game/PIE/Editor world. With a `fixtureId` (the
> Speckle node id -- same key `SetFixture*` uses) dumps only that one and logs a Warning if
> not found. Use it to verify the chain landed at a glance: `source` tells you whether an
> IES file is loaded at all, `candelaMax` is the parsed peak, `intensityUnits=2` confirms
> Candelas, and `intensityLive vs expected` lets you spot a dimmer/gate that's secretly
> zeroing the beam.
>
> ---
>
> **Files touched.**
>
> ```
> REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Public/RebusIes.h
> REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusIes.cpp
> REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Public/RebusFixtureActor.h
> REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusFixtureActor.cpp
> REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusVisualiser.cpp
> REBUS_Visualiser/Plugins/RebusVisualiser/README.md
> ```
>
> No header changes to `RebusSceneSettingsSubsystem` (this is a per-fixture intensity-source
> change, not a scene property) and no new UPROPERTYs (`IesCandelaMax` + `ActiveIesProfileId`
> are plain `float` / `FString` members on the actor; the existing `ActiveIesProfile`
> `UTextureLightProfile` UPROPERTY GC-roots the texture).
>
> **No regressions for InternalBeam, no regressions for the classic Epic-beam path.** The
> SpotLight is the same component the v1.0.87 InternalBeam mode promotes to the visible
> volumetric beam (`ApplyInternalBeamPose` only pushes volumetric flags + a back-offset onto
> it, never touches `IESTexture` / `Intensity` / `IntensityUnits`), so the v1.0.91 candela-max
> wiring lights up identically in both modes. The Epic-beam path's `M_Beam_Master` brightness
> param is fed `RebusEpicBeamMaxIntensity * MeshBeamUserScale` (a separate constant for the
> beam canvas raymarch), which intentionally does NOT track the SpotLight's candela value --
> that path's brightness story is its own (v1.0.45 final), unchanged here.
>
> **Operator checklist.**
>
> ```text
> # 1. Spawn a scene whose fixtures carry IES profiles (REST iesUrl OR data-channel
> #    RegisterFixtureIes inline iesText). Confirm:
> Rebus.DumpFixtureIes
> #    Expected per fixture:
> #    DumpFixtureIes '<id>' source=inline|url profileId='<id>' zoomDmx=<n> zoomHalfDeg=<deg>
> #        iesTexture=TextureLightProfile_N candelaMax=<cd> baseCandela(flux-derived)=<cd>
> #        -> activeBase=<cd>     <-- activeBase MUST equal candelaMax (proves IES won the BASE)
> #        SpotLight intensityUnits=2  <-- 2 = Candelas (the only physically-meaningful value)
> #        intensityLive=<cd> expected(=base*dim*gate)=<cd>  <-- live should equal expected
> #        dimmer=<0..1> shutterMode=<0|1|2> gate=<0..1>
>
> # 2. Push a dimmer change and verify Intensity scales linearly with it.
> SetFixtureDimmer <id> 0.5
> Rebus.DumpFixtureIes <id>
> #    intensityLive should be roughly candelaMax * 0.5 * (1 if shutter open else 0).
>
> # 3. Toggle InternalBeam ON. Confirm the candela max is preserved (the SpotLight is the
> #    same component the v1.0.87 mode promotes to the visible shaft):
> Rebus.InternalBeam 1
> Rebus.DumpFixtureIes <id>
> #    candelaMax, iesTexture, intensityUnits should be IDENTICAL pre/post toggle. The
> #    intensityLive value should ALSO be identical (assuming dimmer/gate didn't change).
>
> # 4. Verify the IES SHAPE matches the .ies file. Visually -- the beam's
> #    centre-vs-edge intensity falloff in the lit pool should match what the .ies file
> #    plots (e.g. a tight ~5 deg IES centred on a 25 deg field shows a bright core with
> #    a dim wash; a broad ~40 deg IES on the same field shows even illumination).
> #    Grep `IES applied:` in the log to confirm the right profileId is loaded.
>
> # 5. Change zoom and re-dump. The selected zoomDmx should jump to the nearest profile
> #    in a per-zoom set, and candelaMax usually changes between zoom steps (zoom-keyed
> #    IES often have a different peak at narrow vs wide field).
> SetFixtureZoom <id> 15
> Rebus.DumpFixtureIes <id>
> SetFixtureZoom <id> 35
> Rebus.DumpFixtureIes <id>
>
> # 6. Light-only / profile-less fallback. A fixture pushed with no IES (no inline +
> #    no iesUrl) should report:
> #    source=none(synthetic-cone) candelaMax=-1.0 -> activeBase=<flux-derived BaseCandela>
> #    intensityLive=<base*dim*gate>
> #    -- proves the fallback path is intact (no regression vs pre-v1.0.91 behaviour).
> ```

> **Post-process Bloom / Lens Flare / Vignette exposed as portal scene properties (v1.0.90).**
> User: *"can we expose post processing - Lens Flare, Bloom and Vignette so we can control these via the portal."*
>
> v1.0.90 wires seven new portal-controllable post-process scalars onto the unbound
> `APostProcessVolume` that the v1.0.x BaseLevel ships (and the `EnsureSceneEnvironment`
> backstop spawns when one is missing). They follow the existing `URebusSceneSettingsSubsystem`
> catalogue pattern verbatim -- each name is seeded in `Initialize` so SceneState round-trips
> the control before the portal pushes anything, each is dispatched through
> `ApplySceneProperty` so `ReapplyAll` re-asserts the operator's live values on every
> environment / fixture (re)spawn, and each rides the same `SetSceneProperty` /
> `SetSceneProperties` JSON descriptor wire route.
>
> **Properties.**
>
> | Name (wire)          | Type   | Default                | What it does (operator-facing)                                                  |
> | -------------------- | ------ | ---------------------- | ------------------------------------------------------------------------------- |
> | `BloomIntensity`     | number | `0.675` (UE default)   | Overall bloom strength on bright pixels. `0` = bloom off; `>1` = blown-out halo |
> | `BloomThreshold`     | number | `-1.0` (UE default)    | Luminance cutoff before bloom starts. `-1` disables thresholding (UE convention) -- pushing `0..N` only blooms pixels brighter than that value |
> | `LensFlareIntensity` | number | `1.0` (UE default)     | Master scale on the flare network. `0` = flares off; `>1` = brighter flares      |
> | `LensFlareTint`      | color  | `FLinearColor::White`  | RGBA tint multiplied onto every flare. White (default) = source colour passes through; push a colour to gel the flares (warm/cool/etc.)         |
> | `LensFlareBokehSize` | number | `3.0` (UE default)     | Diameter of the bokeh shape behind each flare element (in normalised screen units). Larger = chunkier flare highlights                          |
> | `LensFlareThreshold` | number | `8.0` (UE default)     | Brightness above which a pixel produces a flare. Higher = only the brightest highlights flare; lower = more aggressive flaring                  |
> | `VignetteIntensity`  | number | `0.4` (UE default)     | Edge darkening strength. `0` = no vignette; higher = more pronounced corner falloff                                                              |
>
> **Each property auto-sets its `bOverride_<Field>` flag.** A UE `APostProcessVolume` only
> applies a setting whose paired `bOverride_<Field>` is `true`; with the flag `false` the
> volume ignores the value entirely no matter what you write into `Settings.<Field>`. The
> v1.0.90 handlers therefore set `PP->Settings.bOverride_<Field> = true` BEFORE writing the
> value, so the portal push is guaranteed to render without the operator needing to author a
> custom volume in the level. (Source: `Engine/Source/Runtime/Engine/Classes/Engine/Scene.h` --
> the `bOverride_*` bits gate every override-able field, no exceptions.)
>
> **Lens-flare tint default = white means flares carry the source colour by default.** The
> seed is `FLinearColor::White` (opaque white, alpha=1) so the v1.0.90 push is visually
> indistinguishable from the unset / no-tint baseline -- operators only see a difference once
> they push a coloured tint via the JSON `{r,g,b,a}` path (`FRebusPropertyValue::MakeColor`,
> the same wire used by `SkyLightColor` / `InscatteringColor`).
>
> **Worked examples.** Send each of these as a normal `UIInteraction` descriptor (the same
> envelope as `SetFixturePanTilt` / `Ping`):
>
> ```json
> { "type": "SetSceneProperty", "name": "BloomIntensity",     "value": 1.5 }
> { "type": "SetSceneProperty", "name": "BloomThreshold",     "value": 1.0 }
> { "type": "SetSceneProperty", "name": "LensFlareIntensity", "value": 2.0 }
> { "type": "SetSceneProperty", "name": "LensFlareTint",      "value": { "r": 1.0, "g": 0.7, "b": 0.4, "a": 1.0 } }
> { "type": "SetSceneProperty", "name": "LensFlareBokehSize", "value": 4.5 }
> { "type": "SetSceneProperty", "name": "LensFlareThreshold", "value": 4.0 }
> { "type": "SetSceneProperty", "name": "VignetteIntensity",  "value": 0.8 }
> ```
>
> Or in a single batched `SetSceneProperties` push:
>
> ```json
> {
>   "type": "SetSceneProperties",
>   "properties": [
>     { "name": "BloomIntensity",     "value": 1.5 },
>     { "name": "BloomThreshold",     "value": 1.0 },
>     { "name": "LensFlareIntensity", "value": 2.0 },
>     { "name": "LensFlareTint",      "value": { "r": 1.0, "g": 0.7, "b": 0.4, "a": 1.0 } },
>     { "name": "LensFlareBokehSize", "value": 4.5 },
>     { "name": "LensFlareThreshold", "value": 4.0 },
>     { "name": "VignetteIntensity",  "value": 0.8 }
>   ]
> }
> ```
>
> Resetting back to the UE defaults -- the v1.0.90 seed values listed above -- is just another
> push (e.g. `{"name": "VignetteIntensity", "value": 0.4}`), since the override flag stays
> latched once written.
>
> **Pre-existing PP volumes are untouched until first push.** The seven `bOverride_<Field>`
> flags are ONLY raised when the operator (or the SceneState round-trip) actually pushes the
> property -- a stock BaseLevel that hasn't received any v1.0.90 push behaves byte-exact to
> v1.0.89. Once any one is pushed, that field's override stays on for the rest of the session
> (no in-protocol way to clear it; the value can always be restored to the UE default
> verbatim).
>
> **Files touched.**
>
> ```
> REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusSceneSettingsSubsystem.cpp
> REBUS_Visualiser/Plugins/RebusVisualiser/README.md
> ```
>
> No header changes required: `URebusSceneSettingsSubsystem::GetPostProcess()` and the
> `CachedPostProcess` weak handle were already in place from the original v1.0.x scaffold,
> and the seven new branches reuse them verbatim. ReapplyAll, SceneState read-back and the
> JSON descriptor pipeline pick up the new properties automatically through the existing
> catalogue / `Values` map.

> **InternalBeam fixes -- offset direction, gobo on shaft, volumetric shadows, lens shadow opt-out (v1.0.89).**
> User report (verbatim, four issues in one message):
> *"the beam offset is in the wrong direction. Its infront of the lens, not behind it.*
> *No gobo is being passed to this spotlight beam even though its in the footprint.*
> *Can we make sure volumetric shadows are on.*
> *Can we make sure the fixture lens doenst shadow this light either."*
>
> All four are bugs in the v1.0.87 InternalBeam A/B mode (and the v1.0.88 isBeam lens path),
> resolved per-issue below. The v1.0.87 + v1.0.88 contracts are unchanged at the surface --
> `bInternalBeam` scene property + `Rebus.InternalBeam` console toggle still drive the same
> per-fixture state machine, and the v1.0.88 `Rebus.ForceSyntheticLensFallback` lens A/B is
> untouched.
>
> ---
>
> **(1) Back-offset sign was inverted on the user's GDTF profile.**
>
> Root cause. `BuildSpotLight` builds the SpotLight rest rotation with
> `FRotationMatrix::MakeFromXZ(Forward, Up)`, where `Forward` is the GDTF `<Beam>` node's `+Y`
> axis (or the explicit `beamDirectionWorld` when the portal sends one). After this rotation
> the SpotLight's local `+X` is intended to point OUT of the lens (the emission direction).
> v1.0.87 wrote `SpotLight->AddRelativeLocation(-offset * LiveFwd)` to push the spotlight
> UP-stream (back into the head body) so the cone exits the lens at the lens diameter at
> MaxZoom. That is the right math for a profile whose `+Y` matches the convention; the user
> reported the spotlight ending up IN FRONT of the lens, proving the resolved `+X` axis on
> their content is pointing INTO the head, not out of the lens. With the inverted axis,
> `-offset * LiveFwd` pushed the spot DOWN-stream past the lens.
>
> Fix. The sign is now an operator-flippable global, default flipped to the corrected
> direction:
>
> ```
> Rebus.InternalBeamOffsetSign +1   # default (v1.0.89 corrected) -- adds +offset * LiveFwd
> Rebus.InternalBeamOffsetSign -1   # legacy v1.0.87 behaviour, for diagnostics
> ```
>
> The CVar refresh sink walks every fixture currently in InternalBeam mode and re-runs
> `ARebusFixtureActor::RefreshInternalBeamOffset()` (a public thin-wrapper around
> `RefreshMotion`) so the new sign re-applies in-place without a respawn or a full pose
> rebuild. Both RefreshMotion branches (rig-attached `BeamRest * Head` and the
> synthetic-pan/tilt fallback) read `GRebusInternalBeamOffsetSign` at the moment of
> application, so the change lands in the same frame the CVar is set. A new Verbose log
> ```
> Fixture <id> InternalBeam offset: applied +<cm> along LiveFwd=(<x>,<y>,<z>) (sign=+1, restLoc=(...), spotLoc=(...))
> ```
> makes the resolved direction grep-visible the next time we revisit this. The per-toggle
> `SetInternalBeamModeEnabled` summary log also surfaces the live sign + signed offset.
>
> ---
>
> **(2) Gobo cookie modulated the floor pool but not the volumetric shaft.**
>
> Root cause. UE 5.7 routes a SpotLight's `LightFunctionMaterial` onto the volumetric
> scattering integrator ONLY when `r.LightFunctionAtlas.Enabled = 1`. With the atlas off,
> the cookie still projects onto lit surfaces (the user saw the gobo on the floor) but the
> volumetric shaft sees flat scattering (no cookie modulation). The v1.0.74 anti-ghost pack
> deliberately pushes `r.LightFunctionAtlas.Enabled = 0` (defensive against stale-atlas
> sample artefacts on a rotating cookie), and although v1.0.83 made that pack default-OFF,
> the engine baseline of `1` still loses to any later push that drives it to `0`.
>
> Fix. `URebusSceneSettingsSubsystem::SetInternalBeamEnabled(true)` now FORCES
> `r.LightFunctionAtlas.Enabled = 1` for the duration of the InternalBeam session. Cached
> prior value + `ECVF_SetByGameOverride` write + restore-on-disable byte-exact, mirroring
> the existing v1.0.74/78 CVar-pack pattern. The push is GLOBAL (one CVar, not per-fixture)
> so a session with N fixtures in InternalBeam mode pushes the CVar exactly once;
> idempotency latch prevents a double-true / double-false from flipping the CVar twice.
>
> Single helper: `URebusSceneSettingsSubsystem::PushLightFunctionAtlasForInternalBeam(bool)`
> -- called from `SetInternalBeamEnabled` BEFORE the per-fixture walk on the ON transition,
> AFTER the per-fixture walk on the OFF transition (so the atlas stays on for the entire
> restore phase). Restore is symmetric: prior value cached on the OFF -> ON edge, restored
> on the ON -> OFF edge; sentinel `-1` means "no snapshot" and the restore falls back to
> the engine default `1` (safer than `0` because any other gobo wiring expects the atlas
> path live). One-line log on each transition:
>
> ```
> v1.0.89 InternalBeam: r.LightFunctionAtlas.Enabled was=0 now=1 (push so SpotLight LightFunctionMaterial modulates the volumetric shaft, not just the lit floor).
> ...
> v1.0.89 InternalBeam: r.LightFunctionAtlas.Enabled was=1 restored=0 (InternalBeam OFF -> released the v1.0.89 force-1 push).
> ```
>
> Caveat (documented for the next round). Epic's `MI_Light` (`M_Light_Master`) is NOT
> currently `MaterialIsLightFunctionAtlasCompatible` and our `ApplyCurrentGoboToLightFn`
> sets `bAllowMegaLights = 0` on the SpotLight while a gobo is active (so the cookie
> projects via the legacy deferred light-function path, which DOES modulate the volumetric
> integrator when `r.LightFunctionAtlas.Enabled = 1`). If a future engine version changes
> the volumetric LF routing again and the cookie still doesn't reach the shaft after the
> v1.0.89 atlas push, the documented next step is a translucent additive cone mesh
> wrapped around the spotlight that samples the same cookie texture (visible-shaft cookie
> via geometry, not via light function). DO NOT pursue without first confirming the
> v1.0.89 atlas push is insufficient on the user's deployment.
>
> ---
>
> **(3) Volumetric-shadow lock-in.**
>
> The v1.0.87 `RefreshBeamShadowMode` already calls `SpotLight->SetCastShadows(true)` and
> `SpotLight->SetCastVolumetricShadow(true)` on the `bInternalBeamEnabled` branch (with
> `MarkRenderStateDirty()` to propagate to the render thread). v1.0.89 adds:
>
> * A one-shot `Log`-level diagnostic emitted from `SetInternalBeamModeEnabled(true)` so the
>   user can confirm the state landed without a debugger:
>
>   ```
>   Fixture <id> InternalBeam: scatter=1.00 castShadows=1 volumetric-shadow=1 allowMegaLights=0 light-function=GoboLightFnMID gobo-active=1
>   ```
>
>   `castShadows=0` here would catch a future regression where the SpotLight's CastShadows
>   was suppressed by some other code path; `volumetric-shadow=0` would catch a regression
>   on the VSM fog path; `light-function=<none>` while a gobo is selected is the diagnostic
>   for "gobo on floor, not on shaft".
>
> * **Volumetric-fog warning promoted Verbose -> Warning.** v1.0.87 logged a Verbose hint
>   when `bInternalBeam=1` was applied while the scene's `AExponentialHeightFog`
>   `bEnableVolumetricFog` was off (the shaft has no scattering medium to render against).
>   The user explicitly asked us to "make sure volumetric shadows are on" -- so the
>   prerequisite warning is now `Warning`-level and self-diagnosing:
>
>   ```
>   bInternalBeam=1 but bVolumetricFog=0 on the scene fog -- the SpotLight's volumetric shaft will be invisible until you enable volumetric fog (SetSceneProperty bVolumetricFog true, or set r.VolumetricFog 1 in console).
>   ```
>
>   A second Warning fires if the scene has no `AExponentialHeightFog` actor at all
>   (BaseLevel ships with one; if it's missing, something stripped the level). We
>   deliberately do NOT force `bVolumetricFog` on -- that's still a separate scene property
>   the operator owns -- but the operator now gets one obvious "do this to see the beam
>   carve" warning per InternalBeam toggle.
>
> ---
>
> **(4) Lens shouldn't shadow the InternalBeam spotlight.**
>
> Root cause. The v1.0.87 `SetBodyMeshesCastShadow` walker forces
> `CastShadow / bCastDynamicShadow / bCastHiddenShadow / bCastShadowAsTwoSided = false` on
> every body primitive when the mode is enabled, EXCLUDING the SpotLight, the Epic beam
> canvas, the procedural cone, and the synthetic LensDisc. v1.0.88 added a NEW class of
> primitive -- the real `<Beam>` lens meshes (tagged `RebusIsBeamLens`, tracked in
> `IsBeamLensComponents`) -- which the walker ALREADY includes today (they're not in the
> skip list). So the v1.0.89 fix is sequencing-driven: when `BuildMeshes` lands AFTER
> `SetInternalBeamModeEnabled(true)` has already walked the actor (e.g. a delayed
> `/meshes` push, or a re-build path that re-creates the procedural meshes), the new
> `RebusIsBeamLens` mesh keeps its default `CastShadow=true` and shadows the spotlight.
>
> Fix.
>
> 1. **Defensive single-primitive opt-out at BuildMeshes time.** New public method
>    `ARebusFixtureActor::OptPrimitiveOutOfInternalBeamShadow(UPrimitiveComponent*)`. Called
>    immediately after every freshly-created procedural mesh component in `BuildMeshes`.
>    No-op when not in InternalBeam mode (the typical first-spawn flow); when in
>    InternalBeam mode it caches the comp's original four shadow flags into the v1.0.87
>    `InternalBeamShadowCache`, opts the comp out (CastShadow + bCastDynamicShadow +
>    bCastHiddenShadow + bCastShadowAsTwoSided -> false), and `MarkRenderStateDirty()`s.
>    Mirrors the per-actor walker's filter rules (skips BeamCone / EpicBeamComp /
>    LensDisc / any ULightComponent) and re-entrancy-guards via a linear scan of the
>    cache so a comp can't be opted out twice.
>
> 2. **Walker still INCLUDES `RebusIsBeamLens`.** No skip-list addition for the new tag --
>    the body opt-out is for them too. Walker log message updated to make this explicit
>    (`v1.0.89 INCLUDES RebusIsBeamLens-tagged real <Beam> lens meshes`). The synthetic
>    LensDisc is still skipped because it's an emissive disc that was always non-shadowing;
>    the per-beam `RebusIsBeamFlare` discs are also already non-shadowing by construction
>    (`BuildIsBeamLensFlares` hard-sets CastShadow=false on every flare disc).
>
> 3. **Restore-on-disable still byte-exact.** `RestoreInternalBeamPose` -> `SetBodyMeshes
>    CastShadow(true)` walks the cache (NOT the live primitive list) and restores each
>    entry's original four flags. Because `OptPrimitiveOutOfInternalBeamShadow` adds to
>    the SAME cache, late-arrival primitives are restored alongside walker-touched ones --
>    no separate code path. After the cache replay the cache is `Reset()`'d so a
>    subsequent ON transition rebuilds it from scratch (preventing dangling weak refs
>    from a previous session).
>
> ---
>
> **C++ surface additions.**
>
> * `void ARebusFixtureActor::RefreshInternalBeamOffset()` (public). Re-runs `RefreshMotion`
>   so the offset re-applies; called by the new `Rebus.InternalBeamOffsetSign` CVar refresh
>   sink.
> * `void ARebusFixtureActor::OptPrimitiveOutOfInternalBeamShadow(UPrimitiveComponent*)`
>   (public). Single-primitive opt-out + cache for the BuildMeshes defensive path.
> * `void URebusSceneSettingsSubsystem::PushLightFunctionAtlasForInternalBeam(bool)`
>   (private). Cached + restored r.LightFunctionAtlas.Enabled push.
>
> **No new UPROPERTYs** (the offset-sign is a global CVar, not per-fixture; per-fixture
> override would be over-engineering for a sign flip that depends on GDTF authoring
> convention, which the operator can usually fix once at the portal level rather than
> on each fixture). The existing `InternalBeamMaxZoomFullDeg` UPROPERTY (45 deg fallback)
> is unchanged.
>
> **Console additions.**
>
> ```
> Rebus.InternalBeamOffsetSign [+1|-1]   # v1.0.89 -- flip the InternalBeam back-offset
>                                         # along the SpotLight local +X. Default +1
>                                         # (v1.0.89 corrected); -1 = legacy v1.0.87.
> ```
>
> Existing `Rebus.InternalBeam`, `Rebus.InternalBeamScatter` continue to work unchanged.
>
> **Operator checklist.**
>
> ```text
> # 1. Toggle InternalBeam ON. Confirm both warnings fire OR the prerequisite is met.
> Rebus.InternalBeam 1
> #    Expected:
> #    bInternalBeam=1 applied to N fixture(s) (Epic beam hidden ... + r.LightFunctionAtlas.Enabled forced to 1 ...).
> #    v1.0.89 InternalBeam: r.LightFunctionAtlas.Enabled was=<prior> now=1 ...
> #    Per fixture:
> #    Fixture <id> InternalBeam mode ENABLED (backOffset=+<cm> sign=+1 ...).
> #    Fixture <id> InternalBeam: scatter=1.00 castShadows=1 volumetric-shadow=1 allowMegaLights=<0 if gobo active, else 1> light-function=<MID name or <none>> gobo-active=<0|1>
> #    If volumetric fog is OFF you also see (Warning):
> #    bInternalBeam=1 but bVolumetricFog=0 on the scene fog -- ... enable volumetric fog ...
> SetSceneProperty bVolumetricFog true   # via portal data channel, OR
> r.VolumetricFog 1                       # console
>
> # 2. Verify the spotlight is BEHIND the lens. Look at the fixture from the side -- the
> #    visible volumetric shaft should EXIT the lens at the lens diameter (max zoom) /
> #    INSIDE the lens diameter (narrower zoom). If the shaft starts in mid-air past the
> #    lens, the offset went the wrong way; flip the sign:
> Rebus.InternalBeamOffsetSign -1
> #    Per fixture:
> #    Fixture <id> InternalBeam offset: applied -<cm> along LiveFwd=(...) (sign=-1, ...)
>
> # 3. Verify the gobo lands on the volumetric shaft (not just the floor).
> #    Select a gobo with a clear pattern (e.g. break-up). With InternalBeam ON, the
> #    pattern should ride the visible shaft. If it doesn't, dump the fixture state:
> Rebus.DumpGoboState
> #    Look for: SpotLight.LightFunctionMaterial bound + r.LightFunctionAtlas.Enabled=1.
> #    If both are correct and the shaft still has no cookie, the next step is the
> #    translucent-cone fallback (NOT in v1.0.89 -- documented as future work).
>
> # 4. Verify shadows carve through fog.
> #    Put a truss in the beam path. The shaft should show a clean gap where the truss
> #    occludes it. Confirm via:
> Rebus.DumpFixtureLights
> #    Look for: bCastShadows=1, bCastVolumetricShadow=1, on every InternalBeam fixture.
>
> # 5. Verify the lens does NOT shadow the spotlight. Look at the fixture from below
> #    (along the beam axis). The shaft should be uniformly bright across the lens
> #    aperture. A dark disc in the centre of the shaft would mean the v1.0.88 isBeam
> #    lens mesh is still shadowing -- check that the per-fixture log includes the
> #    isBeam mesh in the body-shadow opt-out:
> #    Fixture <id> InternalBeam: opted N body primitive(s) out of shadow casting (...
> #         v1.0.89 INCLUDES RebusIsBeamLens-tagged real <Beam> lens meshes ...).
>
> # 6. Toggle OFF cleanly. Every cached flag restored byte-exact.
> Rebus.InternalBeam 0
> #    Expected:
> #    bInternalBeam=0 applied to N fixture(s) (Epic beam restored ... + r.LightFunctionAtlas.Enabled restored ...).
> #    v1.0.89 InternalBeam: r.LightFunctionAtlas.Enabled was=1 restored=<prior> ...
> ```

> **Real `<Beam>` lens disc -- isBeam mesh flag + mirror/glass material (v1.0.88).**
> User: *"We are now sending a beam object (Lens Disc) from our portal. This needs to be included
> in the fixture and move with the head. It needs the lens mirror/glass material applied to it."*
>
> Portal-team plugin-team note (verbatim): *"In `ParseMeshes` / `FRebusMesh`, parse the two new
> optional fields: `GeometryType` (`FString`) and `bIsBeam` (`bool`; default false / 'unknown'
> when absent). Additive; older blobs simply omit them, no breakage. When building geometry,
> find the lens disc by `bIsBeam == true` instead of matching the mesh `Name`/`GeometryName`
> against `"Beam"`. Attach the emissive lens material / light-emitting origin to that mesh and
> place the `photometrics.lensDiameter` emissive lens-flare on the `isBeam` mesh when one
> exists; keep the synthetic `lensDiameter` disc as the fallback for GDTFs whose `<Beam>` has
> no `<Model>` (no lens mesh) and for blobs that lack the flag. Do NOT change motion handling.
> The `isBeam` mesh is bucketed to the head by the existing `geometryName` rule (the `<Beam>`
> node lives under the tilt axis), so it already pans/tilts. `isBeam` is an identification
> hint, not a motion flag -- do NOT special-case it out of `BuildBucketMap` /
> `affectedGeometryNames` bucketing or the lens will detach from the head. A simple moving
> head has exactly one `isBeam` mesh; an LED-matrix fixture (e.g. MAC Aura) emits one per
> pixel -- handle the multi-beam case (each is an emitter) rather than assuming a single
> lens."*
>
> **What it does.** The portal's `/meshes` blob now bumps `version` 2 -> 3 and stamps each
> emitted mesh with two new OPTIONAL identifier fields:
>
> | Field          | Type     | Meaning                                                                 |
> | -------------- | -------- | ----------------------------------------------------------------------- |
> | `geometryType` | string   | Raw GDTF XML type (`"Beam"`, `"Geometry"`, `"Axis"`, ...) -- diagnostic |
> | `isBeam`       | boolean  | True when the mesh IS the GDTF `<Beam>` lens disc -- authoritative      |
>
> Both fields are ADDITIVE: a v2 blob (or a v3 blob that came through the data-channel push
> without a `profile.fixtureParts` hint) simply omits them and the plugin treats the mesh as
> "not a known lens". That means the synthetic single-disc fallback (the pre-v1.0.88
> `BuildLensDisc` path) ALWAYS continues to work for older portals + for GDTFs whose `<Beam>`
> node has no `<Model>` child -- no breakage.
>
> **Plugin behaviour when `isBeam=true` meshes are present.**
>
> 1. For every isBeam mesh:
>    * The procedural mesh component is tagged `RebusIsBeamLens` (grep-friendly) and added to
>      `IsBeamLensComponents`.
>    * The v1.0.71 mirror/glass material is applied to EVERY material slot
>      (`FixtureLensMaterialOverride` when the user authored `/Game/REBUS/Materials/M_RebusFixtureLens`,
>      otherwise the runtime `FixtureLensMID` with `Metallic=1, Roughness=0.05`). When BOTH
>      are null the actor logs a Warning once per fixture and leaves the procedural-mesh
>      default in place.
>    * One emissive lens-flare disc (`M_RebusLensFlare`) is spawned and parented to the isBeam
>      mesh so the flare inherits the mesh's motion automatically -- no separate
>      `RefreshMotion` call required. Sizing:
>      * Single isBeam mesh: `photometrics.lensDiameter` when set, else mesh local-bounds
>        radius x 2.
>      * Multi isBeam meshes (LED matrix / MAC Aura): ALWAYS mesh local-bounds radius x 2
>        (the photometric diameter describes the whole-fixture lens hole and would
>        over-size per-pixel flares).
> 2. The synthetic `LensDisc` is HIDDEN (`SetVisibility(false)`, NOT destroyed -- kept around
>    for A/B at runtime).
> 3. `RefreshLensDisc` now drives BOTH the synthetic disc MID AND every per-beam flare MID
>    each call (one source of truth for `EmissiveColor` + `EmissiveStrength` =
>    `Dimmer.Current * shutter-gate * RebusLensFlareMaxEmissive`).
>
> **Motion preservation (regression-tested in code review).** The isBeam mesh is bucketed by
> `RebusMotion::ResolveAxisForMesh(Mesh.GeometryName, Mesh.ModelName)` in `BuildMeshes`,
> EXACTLY as every other mesh is. The new isBeam block deliberately does NOT touch that call
> -- `bIsBeam` is an identification hint for the lens-visual path, never a motion override.
> The GDTF `<Beam>` node lives under the tilt axis, so the mesh continues to pan/tilt with
> the head via the existing `MeshAxisBucket` mechanism. Per-beam flare discs are parented to
> the isBeam mesh component, so they inherit motion through the scene component hierarchy
> without any additional code path. Verified: no special-case branch in `BuildMeshes` /
> `RefreshMotion` / `MeshAxisBucket` keys off `bIsBeam`.
>
> **Self-heal forward-compat diagnostic.** `BuildMeshes` now logs the mesh-blob version + mesh
> count on every fixture:
>
> ```
> LogRebusVisualiser: Fixture <id>: MeshBundle version 3, 7 mesh(es).
> ```
>
> Grep `MeshBundle version` on startup to confirm v3 blobs are arriving end-to-end. A v2 line
> means the portal hasn't been refreshed since v1.0.88 -- the synthetic-disc fallback is in
> force for that fixture (everything still works, just no real `<Beam>` geometry rendered).
>
> **Operator A/B toggle.** `Rebus.ForceSyntheticLensFallback [0|1]` (default `0`).
>
> * `0` (default): when a fixture has isBeam meshes, the real geometry IS the lens disc
>   (mirror/glass material + per-beam emissive flare); the synthetic disc is hidden.
> * `1`: every isBeam mesh + per-beam flare is hidden (`SetVisibility(false)` --
>   "invisible/passthrough" on the visible surface), the synthetic `LensDisc` is re-shown.
>   Fixtures with no isBeam meshes are unaffected (the synthetic disc was already the visual).
>
> The toggle is fully live: changing the CVar walks `TActorIterator<ARebusFixtureActor>` and
> per-fixture flips `SetUseSyntheticLensFallback`, which only re-applies visibility (no
> rebuild). Per-fixture log:
>
> ```
> Rebus.ForceSyntheticLensFallback -> 1, refreshed N fixture(s) (M with isBeam meshes; N-M on v2-blob/no-flag fallback regardless).
> Fixture <id> lens-fallback toggle: FORCED synthetic disc (isBeam meshes hidden) (isBeamMeshes=1 perBeamFlares=1 syntheticDisc=visible).
> ```
>
> **Mirror/glass material source.** Re-uses the v1.0.71 lens-material pipeline verbatim --
> see the v1.0.71 changelog block below for the EnsureFixtureMIDs / FixtureLensMaterial
> Override resolution. The isBeam path does NOT introduce a new material asset; it just
> applies the existing one authoritatively (the pre-v1.0.88 path matched on the case-
> insensitive substring "lens"/"glass"/"crystal"/"optic"/"front" in the mesh name, which
> missed any GDTF whose `<Beam>` was named anything else).
>
> **Verification log lines.** Per fixture on Setup:
>
> ```
> LogRebusVisualiser: Fixture <id>: MeshBundle version 3, 7 mesh(es).
> LogRebusVisualiser: Fixture <id>: built 7 mesh proxies (1 tagged isBeam -- real <Beam> geometry will be the lens disc; synthetic disc hidden).
> LogRebusVisualiser: Fixture <id> isBeam flare[0]: spawned diam=15.00cm (src=photometrics.lensDiameter (single-beam)) planeScale=0.1500 localRadius=7.50cm
> LogRebusVisualiser: Fixture <id> isBeam flares: built 1/1 (synthetic LensDisc kept alive as fallback; toggle with Rebus.ForceSyntheticLensFallback).
> ```
>
> And the Verbose breadcrumb (enable `LogRebusVisualiser` at `Verbose`) confirms parse-side
> arrival:
>
> ```
> LogRebusVisualiser: Verbose: Mesh '<name>' tagged isBeam=1 (geometryType='Beam').
> ```
>
> **Operator checklist.**
>
> ```text
> # 1. Confirm v3 blobs are arriving (look for one line per fixture).
> grep "MeshBundle version" PIE.log
> #    Expect: ... MeshBundle version 3, N mesh(es).
> #    If you see "version 2" the portal hasn't been refreshed since v1.0.88 -- everything
> #    still works, just no real <Beam> geometry rendered (synthetic disc fallback in force).
>
> # 2. Confirm isBeam meshes were detected (a moving head -> 1; MAC Aura -> N per pixel).
> grep "tagged isBeam" PIE.log
> #    Expect: Fixture <id>: built K mesh proxies (M tagged isBeam -- real <Beam> geometry ...).
>
> # 3. A/B against the synthetic disc.
> Rebus.ForceSyntheticLensFallback 1   # hide real lens, show synthetic
> Rebus.ForceSyntheticLensFallback 0   # back to real lens (default)
>
> # 4. If the mirror/glass doesn't render, check the material chain.
> #    Author /Game/REBUS/Materials/M_RebusFixtureLens.uasset (any PBR material) OR rely on
> #    the runtime MID off BasicShapeMaterial (Metallic=1, Roughness=0.05). If neither
> #    resolves, the actor logs a Warning per isBeam mesh and the procedural-mesh default
> #    stays in place -- check /Engine/BasicShapes/BasicShapeMaterial is packaged.
> ```
>
> **C4458 build-blocking hotfix rolled in.** v1.0.88 also includes a one-line rename in
> `ARebusFixtureActor::GetBoundOrbitPrimitives`: the local `TArray<USceneComponent*> Children`
> is now `ChildComps`. The original name shadowed `AActor::Children` (`TArray<TObjectPtr<AActor>>`),
> which the Game target tolerated but the **Editor** target compiles with C4458 ("declaration
> hides class member") treated as an error -- so every Editor build from **v1.0.85 through
> v1.0.87 inclusive** failed with `RebusFixtureActor.cpp(2446,28): error C4458`. v1.0.88 is
> therefore the first **cleanly-buildable Editor tag** since v1.0.84. No behaviour change;
> the local is purely a holder for `GetChildrenComponents` output.

> **InternalBeam A/B mode -- SpotLight provides the volumetric beam, back-offset places the cone exit at the lens (v1.0.87).**
> User: *"I want temporary turn off the epicbeam and just use the spotlight we use for the floor
> to create the beam. this will have volumetrics and volumetric shadows. We want the beam to
> exit the fixture at the right size so can we add an offset position for the spotlight that
> pushes it back inside the fixture head. The offset is calculated by taking the max zoom range
> angle and the diameter of the lens to work out how far back it needs to be to exit the lens
> at the right size. Its own fixture head object can not cause shadowing on this spotlight
> otherwise the light couldnt output."*
>
> **What it does.** A new runtime, operator-toggleable A/B mode on `ARebusFixtureActor` --
> `bInternalBeam` -- hides the Epic DMX-Fixtures beam canvas + the procedural cone-mesh fallback
> and promotes the same `USpotLightComponent` that already lights the floor / projects gobos to
> ALSO be the visible volumetric shaft. Default OFF; the Epic beam remains the visible shaft on
> first load. Flip ON via `Rebus.InternalBeam 1` (console) or `SetSceneProperty bInternalBeam
> true` (data channel). The mode is intentionally TEMPORARY -- the Epic beam component is
> preserved on disable (visibility flipped off, NOT destroyed) and snaps back intact on
> `Rebus.InternalBeam 0`, so the Epic beam path can be re-evaluated without a rebuild.
>
> **Back-offset derivation.** With the SpotLight pushed back into the head by a fixed offset, a
> cone of outer half-angle `theta` at distance `D` from the source has radius `D * tan(theta)`
> on the lens plane. We want that radius to equal the lens radius at the MAX zoom half-angle
> (= the widest beam the fixture can produce); solve for `D`:
>
> ```
>   lens_radius / tan(maxZoomHalfAngle) = back-offset (cm)
>   e.g. lensRadius = 7 cm, maxZoomFull = 45 deg -> halfAngle = 22.5 deg ->
>        offset = 7 / tan(22.5 deg) = 7 / 0.4142 ~= 16.9 cm
> ```
>
> Lens radius comes from the SAME resolution chain the existing lens-flare disc + SpotLight
> SourceRadius already use (`Profile.Photometrics.LensDiameter` -> `Profile.Source.RadiusMeters` ->
> `Profile.Source.DiameterMeters` -> a clamped fraction of the fixture dimensions -> a 3 cm
> floor). Max zoom comes from `Profile.Zoom.MaxDeg` when the parsed GDTF / MVR profile carries
> a zoom range, otherwise the UPROPERTY `InternalBeamMaxZoomFullDeg` fallback (default **45
> deg**, a common stage moving-head MAX zoom).
>
> **Why MAX zoom and not the live zoom.** The offset is FIXED at construction time so the cone
> at the lens plane is EXACTLY the lens diameter at MaxZoom and strictly INSIDE the lens diameter
> at every narrower zoom. If we keyed the offset off the current zoom, a wide-then-narrow zoom
> would project a cone whose mouth started inside the lens but whose body widened past the lens
> as it travelled outward (because at narrower outer cone half-angle the back-offset would have
> to be LARGER to keep the lens-radius exit; instead we leave the back-offset at the largest
> value and tolerate the cone being narrower-than-lens at narrower zooms). The visual: at MAX
> zoom the cone meets the lens; at any narrower zoom there's a small unlit annulus inside the
> lens disc, which is correct for a moving-head fixture.
>
> **Volumetric state pushed onto the SpotLight.** When InternalBeam goes ON:
> - `VolumetricScatteringIntensity = Rebus.InternalBeamScatter` (default `1.0`; live-tunable
>   via the CVar).
> - `bCastVolumetricShadow = true` so the spotlight's shaft carves through the
>   exponential-height-fog froxels and gets light-blocking shadows from any non-fixture
>   geometry (trusses, set pieces) in the beam.
> - `CastShadows = true` (volumetric shadow needs the per-light shadow data; this is a
>   requirement of UE's VSM volumetric path).
>
> The mode does NOT touch `bVolumetricFog` on the scene `ExponentialHeightFog` -- the operator
> still owns that scene property (`SetSceneProperty bVolumetricFog`). If `bVolumetricFog=0`
> when `bInternalBeam=1`, the shaft is invisible and the subsystem logs a Verbose hint
> ("InternalBeam=1 but VolumetricFog=0 -- enable VolumetricFog to see the shaft") so the
> operator can self-diagnose without us re-asserting a fog flag against the operator's intent.
>
> **Head-body self-shadow opt-out.** With the SpotLight now sitting INSIDE the fixture head, the
> head's own body geometry would otherwise cast a shadow into the spot's cone and extinguish
> nearly all the light it tries to emit. Two ways to fix this in UE 5.7:
>
> 1. **Per-primitive `CastShadow=false`** on the head/yoke/base body meshes. Pros: surgical --
>    only this fixture's body opts out, world lights (sun / sky / key) still light the head
>    from outside, every other fixture / truss / set piece is untouched. Cons: the head no
>    longer casts a self-shadow back onto its own surface (a near-invisible loss because the
>    head is a small airborne object and most of its self-shadowing is invisible behind the
>    other body parts).
> 2. **Lighting channels** (the UE per-light primitive inclusion bitmask). Pros: the head can
>    still shadow OTHER lights' rays. Cons: requires every world light AND every other light
>    in the rig to share the channel, otherwise the head goes black to the sun / sky / fill --
>    a much larger blast radius for the same fix.
>
> v1.0.87 picks **(1)** because the blast radius is intentionally small: the SpotLight is the
> ONLY light that needs the head transparent to its rays, and per-primitive opt-out is the
> exact "make this fixture's body invisible to this fixture's spotlight" tool. The opt-out
> walks this actor's `UPrimitiveComponent`s and force-sets `CastShadow` + `bCastDynamicShadow`
> + `bCastHiddenShadow` + `bCastShadowAsTwoSided` to `false`, EXCLUDING the SpotLight (not a
> primitive anyway), the Epic beam canvas, the procedural cone, the lens-flare disc, and any
> debug primitive. Restore on OFF replays each cached flag set byte-exact.
>
> **Re-applied on zoom.** `ApplyZoom` calls `ApplyInternalBeamPose` while the mode is ON, even
> though the current implementation derives the back-offset from MAX zoom (a constant for the
> actor's lifetime). The redundant hook future-proofs a per-fixture descriptor that retunes
> MaxZoom between zoom messages.
>
> **C++ surface (`ARebusFixtureActor`).**
>
> * `void SetInternalBeamModeEnabled(bool bEnabled)` -- toggle the mode on a single fixture.
> * `bool IsInternalBeamModeEnabled() const` -- live read; used by the
>   `Rebus.InternalBeamScatter` CVar live-refresh sink to identify which fixtures need a
>   volumetric-state re-push when the scatter scalar is retuned.
> * `void ApplyInternalBeamPose()` -- private; hides Epic / cone, pushes the SpotLight back,
>   forces volumetrics on, opts the body meshes out of shadow casting. Snapshots the
>   construction-time SpotLight state into private cache fields on FIRST call so OFF restores
>   byte-exact.
> * `void RestoreInternalBeamPose()` -- private; replays the cached pose + Epic-beam visibility
>   + per-primitive shadow flags.
> * `float ComputeInternalBeamBackOffsetCm() const` -- private; the `lensRadius / tan(maxHalf)`
>   solve, clamped to non-negative.
> * `void SetBodyMeshesCastShadow(bool bRestoreOriginal)` -- private; the per-primitive
>   walk + cache.
>
> **Subsystem (`URebusSceneSettingsSubsystem`).**
>
> * New `bInternalBeam` scene property, default `false` -- shipped on every `SceneState`
>   read-back so the portal sees the current mode without polling.
> * `SetInternalBeamEnabled(bool)` walks `TActorIterator<ARebusFixtureActor>` and calls
>   `SetInternalBeamModeEnabled` per fixture (mirrors the existing `SetMeshBeamsEnabled`
>   pattern from v1.0.31 / v1.0.47).
> * `ApplySceneProperty` routes the new `bInternalBeam` name. `ReapplyAll` re-asserts the
>   value after every fixture (re)spawn so a freshly-spawned fixture inherits the live mode
>   without needing a manual recycle (mirrors how `bMeshBeams` / `bDriveOrbitModels` already
>   work).
>
> **Console.** `Rebus.InternalBeam [0|1]`. Routes through `ApplySceneProperty(bInternalBeam,
> ...)` so the SceneState reflects the new value (instead of pushing the per-fixture path
> directly, which would render correctly but leave `SceneState` and the respawn re-assertion
> path out of sync). `Rebus.InternalBeamScatter <float>` (live CVar) re-tunes the scatter on
> every fixture currently in InternalBeam mode.
>
> **Operator checklist.**
>
> ```text
> # 1. Enable. Verify the shaft is visible (requires the scene's volumetric fog to be enabled).
> Rebus.InternalBeam 1
> #    Expected log:
> #    bInternalBeam=1 applied to N fixture(s) (Epic beam hidden, SpotLight promoted ...).
> #    Per fixture: Fixture <id> InternalBeam mode ENABLED (backOffset=<cm> maxZoomFullDeg=<deg> scatter=1.00 bodyPrimsOptedOut=<n>).
>
> # 2. If the shaft is INVISIBLE, check VolumetricFog.
> #    The fog must be on for the shaft to scatter:
> SetSceneProperty bVolumetricFog true   # via the portal data channel, or
> r.VolumetricFog 1                       # via console (Editor / Game world)
>
> # 3. Verify shadows carve through the fog.
> #    Put a truss in the beam path -- a clean gap should appear in the shaft where the truss
> #    occludes the SpotLight. If the truss DOESN'T occlude, check the truss isn't an
> #    OrbitConnector-imported glTF without distance fields (those fall back to VSM, which
> #    the InternalBeam mode does enable).
>
> # 4. A/B against the Epic beam. The Epic beam component is preserved -- toggle OFF returns
> #    to the prior visible shaft.
> Rebus.InternalBeam 0
> #    Expected log:
> #    bInternalBeam=0 applied to N fixture(s) (Epic beam restored, SpotLight pose + body shadow flags reverted byte-exact).
>
> # 5. Tune the scatter live (default 1.0).
> Rebus.InternalBeamScatter 1.5    # punchier shaft in light fog
> Rebus.InternalBeamScatter 0.4    # subtle, for thicker fog
> ```

> **Floor textures tile at 1 m physical scale (v1.0.86).**
> User: *"can we make sure that the floor textures scale correctly on the infinite plane. They
> are currently being stretched but they need to be scaled to a 1m x 1m size and then spread
> across the plane."*
>
> **Why textures were stretched.** The BaseLevel floor is `/Engine/BasicShapes/Plane.Plane`
> (a 100 cm engine plane) at `actor_scale3d = (2000, 2000, 1)` -- a 2 km square. The plane's
> default UV span 0..1 is therefore stretched across 2000 m, so anything that sampled a
> texture with the default `TexCoord[0]` node read as a SINGLE texture repeat covering 2 km.
> The pre-v1.0.86 `M_RebusGround` master sidestepped this because its only colour input was
> a `MaterialExpressionNoise` node which already runs on `AbsoluteWorldPosition` -- but the
> moment you wired an actual bitmap sampler into an MI based on the master, the TexCoord
> default kicked in and the bitmap got stretched 2000x in both directions.
>
> **The fix.** Drive UVs from world position, not from the mesh's TexCoord. The v1.0.86
> ground master computes:
>
> ```
> WorldUVs = AbsoluteWorldPosition.xy / (TilingMeters * 100 cm)
> ```
>
> With `TilingMeters = 1.0` (the new default), one texture repeat covers 1 m of world space
> regardless of the floor mesh's actor scale. The 2 km floor now shows 2000 repeats per side.
> `TilingMeters = 0.5` doubles the repeat density; `10.0` coarsens by 10x. The C++ runtime
> can override this on a per-session basis without rebaking the .uasset.
>
> **Master graph changes (`_build_ground_master` in `build_rebus_base_level.py`).**
>
> ```
> BaseColor = lerp(ColorA, ColorB, Noise) * BaseColorTexture.Sample(WorldUVs)
> Roughness = Roughness scalar
> TilingMeters (default 1.0) divides world position to produce WorldUVs
> ```
>
> * New `TilingMeters` scalar parameter (default 1.0).
> * New `BaseColorTexture` texture parameter -- defaults to
>   `/Engine/EngineResources/WhiteSquareTexture` so untextured MIs multiply by white (1,1,1)
>   and behave identically to the pre-v1.0.86 procedural-only output. The four shipped
>   presets (Concrete / Tarmac / Sand / Grass) are untextured, so they look pixel-identical
>   to before on regen. MIs that BIND a real tileable bitmap to `BaseColorTexture` get the
>   new 1 m tiling behaviour for free.
> * `Noise` stays world-driven (always was; it's correct as-is).
>
> **Python self-heal.** `ensure_ground_materials()` (the startup-hook entry point) now
> inspects the on-disk master and detects the pre-v1.0.86 form (no `TilingMeters` scalar
> parameter). When found, it promotes itself to `force=True` and regenerates the master +
> all four preset instances. Anyone who hand-customised the master in the editor will lose
> those edits on first v1.0.86 startup -- this regen logs a `Warning` so the change isn't
> silent. Acceptable for an additive parameter upgrade; preserves the long-term invariant
> that the Python script is the source of truth for the procedural master.
>
> **C++ runtime additions (`URebusSceneSettingsSubsystem`).**
>
> * New `GroundTilingMeters` scene property, default `1.0` -- shipped on every `SceneState`
>   read-back so the portal sees the current tiling without polling.
> * `SetGroundSurface(Preset)` now wraps the loaded MI in a `UMaterialInstanceDynamic` after
>   `SetMaterial(0, ...)` and pushes the current `TilingMeters` to it. Discards any prior
>   MID so a surface swap can't leak param state from the previous surface.
> * `SetGroundTilingMeters(metres)` pushes the scalar to the cached MID (lazy-wrapped via
>   `EnsureFloorMID`). Clamps to `>= 0.01 m` so a zero from the portal can't divide-by-zero
>   the UV calc inside the material and render as a single texel stretched across the plane.
> * `ApplySceneProperty` routes the new `GroundTilingMeters` name to `SetGroundTilingMeters`.
> * Safe with pre-v1.0.86 materials: `SetScalarParameterValue` is a silent no-op when the
>   underlying material has no `TilingMeters` parameter. Operators on an old master see no
>   error; they just see no visible change until they regen via `build_rebus_base_level.
>   build()` in the editor (or restart so the self-heal regen kicks in).
>
> **Console.** `Rebus.SetGroundTiling <metres>`. Routes through `ApplySceneProperty` so the
> SceneState reflects the new value (instead of pushing the MID directly, which would render
> correctly but leave `SceneState` out of date).
>
> **Operator checklist.**
>
> ```text
> # 1. Confirm the master has been regenerated.
> #    Editor -> /Game/REBUS/Materials/M_RebusGround -> Parameters panel should list
> #    `TilingMeters` (default 1.0) and `BaseColorTexture` (default WhiteSquareTexture).
> #    If missing, restart the editor -- ensure_ground_materials() will self-heal on init.
>
> # 2. Author a textured surface preset.
> #    Editor -> /Game/REBUS/Materials/ -> duplicate any MI_RebusGround_<preset> ->
> #    open in the MI editor -> set `BaseColorTexture` to your tileable PBR colour map.
> #    The texture will immediately tile at 1 m physical scale on the floor.
>
> # 3. Runtime tile size adjustment without re-cooking.
> Rebus.SetGroundTiling 1.0         # 1 texture repeat per 1 m (default)
> Rebus.SetGroundTiling 0.5         # 2 repeats per 1 m (finer)
> Rebus.SetGroundTiling 10          # 1 repeat per 10 m (coarser)
>
> # 4. Portal can drive the same control as a scene property -- the SceneState read-back
> #    publishes GroundTilingMeters alongside GroundSurface / bGroundVisible.
> ```
>
> **Why we didn't switch the noise to also use TilingMeters.** The noise scale is already
> set such that the procedural variation reads correctly at typical floor sizes (scale=0.005
> -> ~2 m feature size on the world-driven position input). Re-binding it to TilingMeters
> would make the noise pattern stretch with the tile size, which is the opposite of what the
> noise is there for (large-scale colour variation that's INDEPENDENT of tile size). Kept
> world-driven so the noise's character is consistent whether you set `TilingMeters = 0.5`
> or `10` for the texture layer.

> **Truss / set-piece powdercoat material override (v1.0.85).**
> User: *"if we have truss geometry can that be assigned a default material which is a black
> powdercoating style texture."*
>
> Yes. v1.0.85 extends the v1.0.71 fixture-body / lens override system to a third category:
> every Orbit-imported primitive that ISN'T claimed by a fixture's bind set (the trusses, set
> pieces, ground rows, layout meshes -- everything that's not a moving head). They all get a
> matte black powdercoat material applied automatically.
>
> **How it picks "is this truss vs is this a fixture body".** `URebusVisualiserSubsystem`
> already iterates every `OrbitImportRoot` actor on a 1Hz cadence (the `RebindOrbitModels`
> timer); the truss pass piggybacks on that. Each `ARebusFixtureActor` publishes its bound-
> Orbit-component set via `GetBoundOrbitPrimitives(TSet<UPrimitiveComponent*>&)` -- which
> recursively walks descendants so nested mesh trees under a transform-only Orbit node are
> correctly classified as "fixture geometry". Anything in the OrbitImportRoot tree NOT in
> that bound set is treated as truss / set-piece and gets the powdercoat material. Result:
> the v1.0.71 fixture body+lens override and the v1.0.85 truss override are mutually
> exclusive per component; no double-application, no visible flicker on the 1Hz re-apply.
>
> **Material source, in order of preference.**
>
> 1. **`/Game/REBUS/Materials/M_RebusTruss.M_RebusTruss`** (user-authored .uasset). If
>    present, used verbatim -- no parameter mangling. The operator owns the look entirely.
>    Same convention as `M_RebusFixtureBody` / `M_RebusFixtureLens` from v1.0.71. To author:
>    in the editor, right-click `/Game/REBUS/Materials/`, New -> Material, save as
>    `M_RebusTruss`. Any PBR shading you want; no parameter naming requirement.
> 2. **Runtime fallback MID** built off `/Engine/BasicShapes/BasicShapeMaterial`. Same
>    pattern v1.0.71 uses for the fixture body fallback. PBR knobs tuned for a real
>    powdercoat finish:
>    * `Color` = `#040404` -- slightly above pure black so the surface catches highlights
>      and reads as "matte black material". Pure black crushes contrast and makes trusses
>      look 2D under stage light.
>    * `Roughness` = `0.55` -- powdercoat is microscopically textured; lands between satin
>      (0.4) and matte (0.7), matching what real Prolyte / Eurotruss profiles meter at.
>    * `Metallic` = `0.0` -- powdercoat is a polymer over aluminium; the visible surface is
>      dielectric. Setting Metallic > 0 would give a chrome sheen that breaks the illusion.
>    * `Specular` = `0.5` -- the polymer F0 (default water/plastic specular).
>
> **Console command.** `Rebus.OverrideTrussMaterial [0|1]`. Default ON. OFF restores every
> originally-imported slot material from the per-subsystem cache (captured slot-aligned the
> first time each component was overridden), so the toggle round-trips byte-exact.
>
> ```text
> # ON (default) -- repaints unbound Orbit geometry in powdercoat.
> Rebus.OverrideTrussMaterial 1
>
> # OFF -- restores original glTF / Orbit materials.
> Rebus.OverrideTrussMaterial 0
> ```
>
> **Lifecycle / what fires when.**
>
> * On `URebusVisualiserSubsystem::Tick` -> every 1 second (alongside `RebindOrbitModels`):
>   the truss pass runs. Cheap when nothing changed (the per-slot diff returns Touched=0,
>   `UPrimitiveComponent::SetMaterial` short-circuits when the slot already has the target
>   material). Re-uses the same cadence so a freshly-imported truss / set piece is in
>   powdercoat within one second of arriving.
> * On `SetTrussMaterialOverrideEnabled(true)`: immediate one-shot apply (returns the count
>   counters so the console command logs `scanned=N touched=M skippedFixtureBound=K`).
> * On `SetTrussMaterialOverrideEnabled(false)`: walk the per-subsystem cache, restore each
>   primitive's original slot materials, clear the cache. Components that have been GC'd
>   since the snapshot was taken are silently skipped (TWeakObjectPtr->IsValid check).
>
> **Why this lives in the subsystem instead of on the actor.** Truss components belong to the
> separately-owned `OrbitImportRoot` actor (we have zero compile / link dependency on the
> OrbitConnector plugin -- both this pass and v1.0.70's `Rebus.ShowOrbit` find the root by
> class-name string match). There's no per-actor owner to attach the material state to, so
> `URebusVisualiserSubsystem` owns a global per-component cache keyed by `TWeakObjectPtr<
> UPrimitiveComponent>`. Dead weak handles are pruned at the start of each pass.
>
> **Operator checklist for first show with a powdercoat look.**
>
> ```text
> # 1. Verify the override is live.
> Rebus.OverrideTrussMaterial 1     # logs "scanned=N touched=M skippedFixtureBound=K".
>                                    # M > 0 the first run, ~0 thereafter; K matches the
>                                    # count of fixture-bound Orbit components (sanity check).
>
> # 2. (Optional) Author a custom M_RebusTruss.uasset to override the parametric fallback.
> #    Editor -> /Game/REBUS/Materials/ -> right-click -> New -> Material -> save as
> #    M_RebusTruss. Restart the visualiser. The user .uasset is picked up at first use.
>
> # 3. A/B against the original glTF materials any time.
> Rebus.OverrideTrussMaterial 0     # logs "restored=K original Orbit material(s)".
> Rebus.OverrideTrussMaterial 1     # back to powdercoat.
> ```

> **Zoom wire convention fix -- `zoomDeg` is now FULL beam angle (v1.0.84).**
> User: *"the zoom range from 23ish to 45 makes no change to the beam."*
>
> **Root cause.** `URebusFixtureControlSubsystem::SetFixtureZoom` was forwarding the
> portal-supplied `zoomDeg` straight to `ARebusFixtureActor::ApplyZoom`, which interprets its
> argument as a HALF beam angle (matching `SpotLight->OuterConeAngle` semantics). The portal --
> like GDTF, every fixture spec sheet, and every DMX desk on earth -- treats the slider value as
> the FULL beam angle. The on-actor clamp inside `ResolveOuterHalfDeg`:
>
> ```cpp
> OuterHalf = FMath::Clamp(OuterHalf,
>     (float)(Profile.Zoom.MinDeg * 0.5),   // profile is FULL angle from GDTF
>     (float)(Profile.Zoom.MaxDeg * 0.5));  // half it for half-angle clamp
> ```
>
> ... meant that for a fixture documented as e.g. **8 to 45 deg full** (Mac Encore Wash, Sharpy
> Plus, Mac Aura PXL, etc.), the on-the-wire value was being silently clamped to **the 4 to
> 22.5 deg HALF range**. Portal slider values 4..22.5 worked (but described a cone twice as wide
> as the user expected -- 8..45 deg full, perfectly matching the profile range, just labelled
> wrong). Portal slider values 22.5..45 all saturated at the upper clamp -- producing the user-
> visible **"23 to 45 makes no change"**.
>
> **Fix.** Treat the wire field as FULL angle (the convention every other tool uses) and
> convert at the boundary:
>
> 1. `URebusFixtureControlSubsystem::SetFixtureZoom(Id, ZoomFullDeg, Fade)` -- parameter
>    renamed; body now does `ApplyZoom(ZoomFullDeg * 0.5f, Fade)`.
> 2. `FRebusFixtureStateSnapshot::ZoomDeg` -- outbound `FixtureStates.zoomDeg` snapshot field
>    now emits `ZoomDeg.Current * 2.f` so the portal's live-stream echo agrees with what it
>    sent. The internal `ZoomDeg.Current` storage stays half-angle (the geometry math, IES
>    selection, beam-cone build, and `SpotLight->OuterConeAngle` push are all simpler in
>    half-angle and don't need to flip).
> 3. Header comment on `SetFixtureZoom` documents the convention.
> 4. README field doc for `FixtureStates.zoomDeg` updated.
>
> **What this means for the portal.**
>
> | Wire field                              | Pre-v1.0.84 (broken)                          | v1.0.84+ (correct)            |
> |-----------------------------------------|-----------------------------------------------|-------------------------------|
> | `SetFixtureZoom.zoomDeg` (inbound)      | Half-angle (silently)                         | FULL beam angle               |
> | `FixtureStates.fixtures[].zoomDeg` (outbound) | Half-angle (silently)                   | FULL beam angle               |
>
> No portal-side code change is required -- portals already send and render full-angle values,
> they were just being misinterpreted by UE. After updating to a v1.0.84 binary the slider's
> full advertised range works on every fixture without any portal-side adjustment.
>
> **Compatibility note.** Any caller talking to the old binary that genuinely wanted "16 deg
> full" was sending `zoomDeg=8` (because the binary treated that as half-angle). On v1.0.84
> they should send `zoomDeg=16`. Same number a fixture spec sheet shows.

> **Fresh approach to rotating-gobo ghosting -- per-light Lumen isolation + operator-picked AA mode (v1.0.83).**
> User: *"Since we made the render setting changes to improve the gobo rotate ghosting, we are getting noise/flickering.
> The ghosting isnt fixed though. can we reset these render settings and come up with a fresh approach to solve the ghosting.
> If we move the camera, it temporarily stops ghosting."*
>
> **Why the v1.0.73/74/78 approach failed.** Those releases shipped two CVar packs auto-applied at
> `PostEngineInit`:
>
> * **GoboAntiGhost** (v1.0.73/74): `r.TSR.ShadingRejection.Flickering=1`, `r.TSR.History.UpdateRate=0.6`,
>   `r.LightFunctionAtlas.Enabled=0`, + a few related knobs. The theory: make TSR more aggressive at
>   rejecting "flickering" pixels so the rotating cookie wouldn't smear. The reality: TSR's shading
>   rejection cannot distinguish "rotating gobo" from "noisy pixel" without lowering thresholds globally,
>   so the cure introduced low-grade noise/shimmer across the whole frame -- on geometry that has nothing
>   to do with gobos.
> * **LumenFastResponse** (v1.0.78): `r.Lumen.ScreenProbeGather.Temporal=0`, `r.Lumen.Reflections.Temporal=0`,
>   `r.Lumen.Radiosity.Temporal=0`. The theory: kill Lumen's temporal accumulator so the cookie's bounce
>   contribution doesn't ghost in GI. The reality: disabling Lumen's temporal filters globally means
>   *every* indirect-lighting sample is now sparse and noisy. Stage scenes look grainy even when no
>   gobo is animating.
>
> The user's diagnostic gold: *"if we move the camera, it temporarily stops ghosting"*. This is the
> proof that TSR is the layer doing it. When the camera moves, the floor's motion vector is non-zero,
> TSR reprojects history to a different pixel (which had a different lit colour), the colour-match
> check fails, history is rejected, ghost vanishes. When the camera is still, motion vector is zero
> and TSR keeps blending stale lit pixels at the same screen position. **TSR is fundamentally not
> designed for stationary geometry with animated light functions**, and no amount of CVar tuning
> changes that without trading one artifact for another.
>
> **Fresh approach: attack the two ghost layers surgically, not globally.**
>
> 1. **Lumen ghost layer -- per-light isolation.** `ARebusFixtureActor::RefreshGoboLumenIsolation()`
>    flips `SpotLight->SetAffectGlobalIllumination(!bGoboActive)` on the *specific* spotlight projecting
>    a cookie. Called from `ApplyGobo()` (gobo bound -> Lumen off for THIS light) and from
>    `ClearGoboToOpen()` (cookie removed -> Lumen back on). The bounce contribution from a
>    cookie-projecting fixture no longer enters Lumen's temporal history, so there's nothing for Lumen
>    to ghost -- and every other light in the scene keeps full GI fidelity. No global CVar nuke. The
>    cost is invisible 95%+ of the time: spotlights with cookies are aimed at floor/set pieces, the
>    "missing bounce" is a slightly less-glowy surrounding room, and the direct light is unaffected.
> 2. **TSR ghost layer -- operator-picked AA mode.** `Rebus.AAMode tsr|taa|fxaa|msaa|off|status` flips
>    `r.AntiAliasingMethod`. The recommendation for shows featuring rotating gobos is **TAA** (mode 2):
>    older temporal AA, simpler history, more aggressive rejection -- trails MUCH less behind animated
>    lights at the cost of slightly softer static AA. Shows without animated cookies stay on TSR (the
>    default). FXAA / no-AA are also available for extreme cases. Portal can drive this from the UI
>    by sending `{"type":"ConsoleCommand","command":"Rebus.AAMode taa"}` over the existing
>    `ConsoleCommand` descriptor pipe (no new portal-side wiring required).
>
> **Reverted.** The `PostEngineInit` auto-apply of `ApplyGoboAntiGhost(true)` and
> `ApplyLumenFastResponse(true)` is removed. The packs and their console commands still exist
> (`Rebus.GoboAntiGhost [0|1]` / `Rebus.LumenFastResponse [0|1]`) for A/B testing, but they're OFF by
> default. Anyone who relaunches into v1.0.83 immediately gets engine-default TSR + full Lumen back.
> The CVars are snapshotted on first ON and restored byte-exact on OFF, so flipping the toggles
> mid-session is safe (the snapshot is empty until the first ON, so the new default has zero
> historical interference).
>
> **What changed in code.**
>
> * `RebusVisualiser.cpp` -- `OnPostEngineInit` no longer auto-applies the two CVar packs. Replaced
>   with a long comment explaining the v1.0.83 diagnosis. Added `Rebus.AAMode` console command
>   (registration + handler + unregister). Updated `Rebus.GoboAntiGhost` help text to reflect the
>   new default-OFF status.
> * `RebusFixtureActor.h` -- declared `RefreshGoboLumenIsolation()`.
> * `RebusFixtureActor.cpp` -- implemented `RefreshGoboLumenIsolation()` (single-line flip via
>   `SetAffectGlobalIllumination` with a no-op fast path). Called from `ApplyGobo` (after
>   `RefreshBeamShadowMode`) and from `ClearGoboToOpen` (after `RefreshBeamShadowMode`).
>
> **Operator checklist for ghost-free rotating gobos.**
>
> ```
> # 1. Confirm the v1.0.83 reset is live.
> Rebus.GoboAntiGhost 0         # should report "OFF" (it already is, this re-asserts)
> Rebus.LumenFastResponse 0     # ditto
>
> # 2. Pick the AA mode that fits the show.
> Rebus.AAMode status           # current setting (likely "tsr" -- UE default)
> Rebus.AAMode taa              # recommended for rotating-gobo shows
>
> # 3. Verify per-light Lumen isolation is firing.
> #    (Set a gobo on a fixture from the portal, then dump.)
> Rebus.DumpGoboState           # confirms bGoboActive=1 on the chosen fixture
> #    The hidden Verbose-level log line "gobo Lumen-isolation: SetAffectGlobalIllumination(0)"
> #    fires the moment a gobo is bound; clear the gobo -> the same line with (1) fires.
>
> # 4. (Optional) The legacy v1.0.73/74/78 packs still exist if you want them.
> Rebus.GoboAntiGhost 1         # re-enable the v1.0.73 pack (NOT recommended -- adds noise)
> Rebus.LumenFastResponse 1     # re-enable v1.0.78 pack (NOT recommended -- adds noise)
> ```
>
> **Why this is the right shape long-term.** Neither layer is solved with "one CVar fixes
> everything". The Lumen layer is solved per-light (the surgical change ships with every binary --
> no operator action). The TSR layer is solved per-show (operator picks AA mode based on whether
> rotating gobos are featured -- single command, persists for the session). The default-OFF reset
> means new operators get a clean engine baseline and can opt into the legacy packs only if they
> want to A/B-test the trade-offs themselves.

> **CameraState delivery diagnostics + first-spawn force-push (v1.0.82).**
> User: *"we are not seeing the camera data in our portal, we have used the notes you gave us"*.
>
> **Diagnosis.** Three plausible failure modes for "portal isn't receiving CameraState":
>
> 1. **Cine pawn not spawned yet at handshake.** `TrySendReady` gates on
>    `bChannelReady && bSceneLoaded && bEnvEnsured` -- it does NOT gate on
>    `bViewPositioned`. So `bReadySent` (and the handshake's `BroadcastCameraStateIfChanged(force=true)`)
>    can fire **before** `TryPositionPlayerView` has a `PlayerController` to possess. The
>    forced broadcast runs against a null pawn and silently no-ops; only the periodic delta
>    stream remains, and a static camera now sits at its default values so dead-zone
>    rejection never fires the second-chance send.
> 2. **No connected viewer at the moment of broadcast.** The PS2 streamer's
>    `SendAllPlayersMessage` is a no-op when `GetConnectedPlayers().Num() == 0`. The existing
>    "Sending 'CameraState' (Response, N players)" log line in `RebusDataChannel.cpp` already
>    reports `N`; if it's `0`, the message went to nobody.
> 3. **Portal frontend not listening for `type:"CameraState"`.** UE is shipping the event,
>    but the portal's event router has no handler bound.
>
> **Fixes shipped in v1.0.82.**
>
> - **First-spawn force-push.** `TryPositionPlayerView` now calls
>   `BroadcastCameraStateIfChanged(force=true)` immediately after a successful pawn spawn
>   IF `bReadySent && Channel.IsValid()`. Covers the race in (1) -- the moment the pawn
>   becomes the view target, the portal gets a complete snapshot.
> - **Inbound descriptor trace.** Every camera descriptor (Request / SetCamera*) logs
>   `HandleCameraDescriptor: type='X' pawn=Name|NULL readySent=0|1 channel=0|1`. Single line
>   per inbound -- portals send these at <30Hz so no spam.
> - **Pawn-null warning.** If a `SetCamera*` lands before the pawn exists,
>   `HandleCameraDescriptor` now logs a `Warning` explaining the descriptor was accepted but
>   had no effect (instead of silently dropping it).
> - **Broadcaster trace (`Verbose`).** Every actual `SendCameraState` logs the snapshot it
>   shipped. `Log`-level when something rejected the broadcast (channel null / pawn null on
>   a forced send).
>
> **New console commands.**
>
> ```
> Rebus.CameraStreamStatus
>   One-shot dump:
>     - subsystem alive yes/no
>     - cine pawn alive yes/no (with name)
>     - live camera snapshot the next broadcast WOULD ship
>   Pair this with the existing "Sending 'CameraState' (Response, N players)" log to
>   verify N >= 1 (otherwise no portal will see it).
>
> Rebus.SendCameraState
>   Force one CameraState onto the wire NOW (synthesises a portal-side RequestCameraState).
>   Use to verify the wire end-to-end without portal cooperation. Read the next "Sending
>   'CameraState'..." log line to see how many viewers it went to.
> ```
>
> **Operator checklist when the portal still doesn't see CameraState.**
>
> 1. Run `Rebus.CameraStreamStatus` -- if `cinePawn=NULL`, the stream physically can't
>    fire yet. Wait for the level to begin play (the log line `RebusCineCameraPawn spawned +
>    possessed` confirms it).
> 2. Run `Rebus.SendCameraState` -- if `Sending 'CameraState' (Response, 0 players)`, the
>    PS2 streamer has no connected viewer (the portal's data track hasn't opened, or the
>    `-PixelStreamingID=` token mismatches `streamerId` the portal targets).
> 3. If `players >= 1` but the portal UI doesn't update, the failure is portal-side. The
>    event ships as `type:"CameraState"` -- confirm the portal's response listener subscribes
>    to that exact type string (case-sensitive).

> **Live state stream for every portal-exposed control (v1.0.80).**
> User asked: *"all the controls we expose to our portal, can these live stream their state, so
> all the fixture settings are streamed so everything is always in sync"*. Yes. v1.0.80
> introduces a full read-back surface so the portal always agrees with UE -- including across
> multiple simultaneously-connected portal clients (operator + producer + tablet at FOH).
>
> **Why this was needed.** Pre-v1.0.80 the data channel was write-mostly: portal -> UE
> `SetFixture*` / `SetScene*` / `SelectFixtures` worked, but UE -> portal echoes only
> happened at three moments: (a) `Ready` + `FixtureRegistered` on initial handshake, (b)
> `SceneState` on explicit `RequestSceneState`, (c) the per-fixture selection re-apply at
> handshake. So:
>
> - Two portal clients controlling the same UE drifted apart -- client B never saw client
>   A's dimmer change.
> - Reconnecting was a tabula rasa for the live values: the portal had to remember its last
>   send and hope UE had nothing else going on.
> - Long fades (e.g. `SetFixtureDimmer 0.5 fade=3s`) were invisible to the portal until the
>   operator pinged something else.
>
> **What's added.** Three live read-back streams + two pull descriptors:
>
> - **`FixtureStates`** (UE -> portal, batched). Periodic ~10Hz with per-field per-fixture
>   dead-zone gating. Carries every live-faded value the portal can set (dimmer / pan / tilt /
>   zoom / iris / frost / focus / RGB / colour temp / shutter mode + rate / gobo index / gobo
>   wheel / gobo rotation speed / animation-wheel speed / selected / primary). A static rig
>   produces **zero traffic**. A fading rig produces one batched message every ~100ms with
>   only the fixtures whose values actually moved.
> - **`SelectionState`** (UE -> portal, one-shot). Pushed any time `SelectFixtures` lands so
>   every other portal client sees the operator's selection immediately, and on every
>   reconnect so a late-joining portal paints the right highlight.
> - **`SceneState`** (UE -> portal, already existed). Now also auto-pushed after every
>   inbound `SetScene*` descriptor, so multi-client portals stay in sync on environment
>   settings (volumetric fog, ground material, render quality, drive-orbit toggle, etc.)
>   without anyone having to call `RequestSceneState`.
> - **`RequestFixtureStates`** (portal -> UE). One-shot full broadcast of every spawned
>   fixture (`full: true`). Use when the portal just opened and wants a snapshot before the
>   first delta arrives, or as a "resync" button.
> - **`RequestSelectionState`** (portal -> UE). One-shot selection push.
>
> **Message shapes.**
>
> ```jsonc
> // UE -> portal, periodic. full=true => every spawned fixture is in this batch (handshake or
> // explicit RequestFixtureStates); full=false => delta only, absent ids are unchanged.
> { "type": "FixtureStates",
>   "full": false,
>   "fixtures": [
>     { "id": "abc",
>       "dimmer":  0.5,         // 0..1 live-faded value (not target)
>       "panDeg":  12.3,
>       "tiltDeg": -45.0,
>       "zoomDeg": 30.4,        // FULL beam angle deg (v1.0.84 -- matches the inbound SetFixtureZoom wire field; portal renders the slider value directly)
>       "iris":    1.0,
>       "frost":   0.0,
>       "focus":   0.5,
>       "color":   [1.0, 0.5, 0.2],     // linear RGB
>       "ctK":     5600,                // optional -- only emitted when colour-temp mode is on
>       "shutter": { "mode": 2, "rateHz": 5.0 },   // 0=Open 1=Closed 2=Strobe
>       "gobo":    { "index": 3, "wheel": 0, "rotSpeed": 0.4, "animSpeed": 0.0 },
>       "selected": true,
>       "primary":  false }
>   ] }
>
> // UE -> portal, on selection change + on reconnect.
> { "type": "SelectionState",
>   "ids":       ["abc", "def"],
>   "primaryId": "abc" }
>
> // portal -> UE, one-shot resync requests.
> { "type": "RequestFixtureStates" }
> { "type": "RequestSelectionState" }
> ```
>
> **Dead-zone thresholds (per-field).** Tuned so a smooth fade (~0.01/frame at 100ms tick)
> always goes out, but rounding jitter from `FInterpEaseInOut` does not:
>
> ```
> dimmer / iris / frost / focus / color.{R,G,B} / goboRotSpeed / animWheelSpeed  : 0.002
> panDeg / tiltDeg / zoomDeg                                                     : 0.05 deg
> shutterRateHz                                                                  : 0.01 Hz
> colorTempK                                                                     : 1 K
> shutterMode / goboIndex / goboWheelIndex / selected / primary                  : strict !=
> ```
>
> Any one field crossing its threshold flags the whole fixture (the full struct ships
> either way; per-field deltas wouldn't shrink the payload meaningfully). A typical
> 50-fixture rig with one channel fading sends ~100B/tick -> ~1KB/s; the same rig fully
> idle sends 0B/s.
>
> **Lifecycle hooks.**
>
> - Periodic tick `(1/10)s` runs once `Ready` has been sent. Each tick rebuilds the cache
>   from `SpawnedFixtures`, prunes stale ids (so `ClearScene` -> reload doesn't leak), and
>   diffs each live snapshot against `LastSentFixtureStates`. Forwards only the changed
>   fixtures with `full:false`.
> - `BroadcastHandshake()` (fires on initial `Ready` and on every viewer reconnect) now also
>   force-pushes `FixtureStates {full:true}` + `SelectionState` after the existing
>   `FixtureRegistered` loop so the portal can index by id before state arrives.
> - `URebusFixtureControlSubsystem::SelectFixtures` lands -> the data-channel router calls
>   `NotifySelectionChanged()` -> immediate `SelectionState` push + cache refresh.
> - `URebusSceneSettingsSubsystem::HandleSceneDescriptor` returns true -> the router calls
>   `NotifySceneSettingsChanged()` -> immediate `SceneState` push.
> - `ClearScene` resets `LastSentFixtureStates` so the next scene's fixtures emit a fresh
>   first-broadcast for every id (even if the id happens to collide with the previous scene).
>
> **What's NOT streamed.** The cinematic-camera stream from v1.0.79 (`CameraState`) is
> already covered. The scene properties surface (`SceneState`) was already a thing; v1.0.80
> just adds the auto-push trigger. Console-command toggles that aren't routed via
> `SetScene*` (e.g. `Rebus.OverrideFixtureMaterials`, `Rebus.LumenFastResponse`) don't
> stream -- those are diagnostic, not user-control. If you want any of those exposed to the
> portal, add a `SetScene*` descriptor for them and the auto-push falls into place.

> **Cinematic camera + portal-driven lens / exposure / transform stream (v1.0.79).**
> User asked for:
> *"We want to change the default camera to a Cinematic Camera which is controllable. We want
> to make it Manual Exposure. We want to expose basic settings like focal length, Aperture
> etc. We want to send our portal real time transform information that then allows us to
> change and see live updates."*
>
> All four landed in one pass: a new `ARebusCineCameraPawn` replaces UE's default `ADefaultPawn`,
> exposes manual exposure + cinematic lens, accepts six `SetCamera*` data-channel descriptors,
> and broadcasts a `CameraState` event back at ~30Hz (dead-zone-gated, so a stationary camera
> stays quiet).
>
> **Why a cine camera (not just style the default one).** `UCineCameraComponent` is the cinema
> camera UE ships for sequencer + virtual production work. Three concrete reasons it had to
> replace the default:
>
> - **Focal length / aperture / focus distance are first-class UPROPERTYs** with the right
>   units (mm, f-stop, cm) and physically-accurate cone math. Trying to map "focal length" onto
>   `UCameraComponent::FieldOfView` (the DefaultPawn camera) loses the sensor-size dependency
>   -- you can't say "I want this look on a 50mm S35 lens" with just FOV, because the same FOV
>   means a different shot on different sensor sizes. The portal sliders that already speak
>   mm + f-stop now drive the camera in its native units.
> - **`FCameraFocusSettings` provides real depth-of-field** out of the box (manual or
>   tracking AF) so f-stop changes actually change the blur, not just the exposure (which
>   matters because the DMX rig changes f-stop to "stop down" highlights).
> - **Post-process settings on the cine component are scoped to the camera**, so the manual-
>   exposure override doesn't fight an unbound PostProcessVolume (the visualiser's default
>   environment includes one for fog/colour grading).
>
> **Why manual exposure.** Auto-exposure was rebalancing the viewport every time the DMX rig
> cut lights, which read to the audience as the **camera** adapting rather than the **show**
> changing. Three concrete failure modes:
>
> - **Blackouts looked like fades.** Auto-exposure ramped the gain up over ~0.5s of darkness,
>   so the intended dramatic cut lost its punch.
> - **EV sliders on the portal had no effect.** Operators tried to grade by pushing the EV
>   slider; auto-exposure compensated in the opposite direction. v1.0.79 forces
>   `AutoExposureMethod = AEM_Manual` + `AutoExposureBias = 0` by default, so the EV slider
>   maps 1:1 to a perceptible change.
> - **Lumen fast response (v1.0.78) was being masked.** GI now cuts instantly when lights
>   toggle, but auto-exposure would then slowly re-expose, so the audience still saw a fade.
>   Manual exposure + the v1.0.78 Lumen pack now combine into instant on/off.
>
> **Data-channel surface (portal -> UE).** All six descriptors are routed in
> `URebusVisualiserSubsystem::HandleCameraDescriptor` and reach the camera through
> `ARebusCineCameraPawn::Set*`:
>
> ```jsonc
> // 6-DoF pose -- cm + degrees, UE world frame.
> { "type": "SetCameraTransform",
>   "loc": [0, -2000, 200], "rot": [0, 90, 0] }
>
> // Lens focal length (mm). Clamped to [4, 1000].
> { "type": "SetCameraFocalLength", "mm": 35 }
>
> // Iris (f-stop). Clamped to [1, 32]. Changes the depth-of-field, not just exposure.
> { "type": "SetCameraAperture", "fStop": 2.8 }
>
> // Manual focus (cm) -- enables manual focus and sets the focus plane distance.
> { "type": "SetCameraFocusDistance", "cm": 500 }
>
> // Tracking AF -- bypasses manual focus until next SetCameraFocusDistance.
> { "type": "SetCameraFocusDistance", "auto": true }
>
> // Manual exposure bias (EV stops, ±10). Forces manual mode if it wasn't already.
> { "type": "SetCameraExposure", "ev": 0.0 }
>
> // Sensor size (mm). Defaults Super35 24.89x18.66. Changes how focal length reads as FOV.
> { "type": "SetCameraSensor", "widthMm": 24.89, "heightMm": 18.66 }
>
> // One-shot CameraState push (e.g. portal just opened and wants the current state).
> { "type": "RequestCameraState" }
> ```
>
> **Outbound `CameraState` (UE -> portal).** Identical struct shape so the portal can hold one
> `CameraState` object and call either direction with the same fields. The subsystem ticks at
> ~30Hz, samples the live pawn, and only sends when something moved beyond a dead zone (0.1cm
> pos / 0.05° rot / 0.1mm focal / 0.01 f-stop / 0.5cm focus / 0.005 EV). A stationary,
> untouched camera produces **zero** messages -- there's no idle traffic. Re-sends are also
> forced (a) on every viewer reconnect (so a late-joining portal paints the live pose
> immediately, not after the first wiggle) and (b) right after any inbound `SetCamera*` lands
> (so the portal sees its own change confirmed). Shape:
>
> ```jsonc
> { "type": "CameraState",
>   "loc":   [0, -2000, 200],          // cm, UE world space
>   "rot":   [0, 90, 0],               // pitch / yaw / roll, deg
>   "focalMm": 35.0,                   // current lens focal length
>   "fStop":   2.8,                    // current aperture
>   "focusCm": 500.0,                  // manual focus distance (cm)
>   "manualFocus": true,               // false -> tracking AF active
>   "ev":      0.0,                    // current manual EV bias
>   "sensor":  { "wMm": 24.89, "hMm": 18.66 } }
> ```
>
> **Spawn / possess plumbing.** The visualiser subsystem's existing
> `TryPositionPlayerView()` (already retried each tick until the PlayerController exists) now
> also spawns + possesses an `ARebusCineCameraPawn` and destroys the default pawn. Done at
> runtime instead of via a custom `AGameModeBase` so the existing "wait for PC, then place
> the view" pattern is reused as-is and packaged builds don't need a GameMode redirect (which
> is also brittle when a level was authored against the engine default). The cine pawn is a
> subclass of `ADefaultPawn` -- this is intentional: we inherit `UFloatingPawnMovement` +
> mouse-look + WASD bindings for free. `ADefaultPawn` in UE 5.7 has NO `UCameraComponent`
> (the stock pawn drives its view straight from the controller's control rotation), so we
> just `CreateDefaultSubobject<UCineCameraComponent>` in the cine pawn's ctor, attach it to
> the inherited sphere, and set `bUsePawnControlRotation = true` so mouse-look writes through.
> The camera manager auto-finds the cine component via `APawn::bFindCameraComponentWhenViewTarget`
> (defaults to `true`) -- no `SetViewTarget` call needed.
>
> **Defaults baked in v1.0.79.** 35mm focal, f/2.8 aperture, manual focus @ 5m, Super35
> sensor (24.89x18.66mm), manual exposure +0 EV. Picked to look like a neutral cinema prime
> on a S35 body framing a typical mid-stage subject without bokeh-blurring the back of the
> set at f/2.8. `Rebus.CameraReset` restores these without moving the camera; the operator
> can then re-frame from the portal with `SetCameraTransform`.
>
> **Console aids.**
>
> ```
> Rebus.CameraSnapshot   # one-line dump of the live state (loc/rot/lens/EV/sensor)
> Rebus.CameraReset      # restore the v1.0.79 lens + exposure defaults (does NOT move)
> ```
>
> **Module dependency.** Adds `CinematicCamera` to `RebusVisualiser.Build.cs`
> `PrivateDependencyModuleNames`. Nothing else moved.

> **Lumen GI fast-response: disable temporal filters so lights respond instantly (v1.0.78).**
> User identified the actual root cause: *"the ghosting is to do with Global illumination and
> the camera. When we turn lights on and off there is a fade off of GI, we want this
> instant."* Correct -- the fade-off when lights toggle, AND the residual gobo trail on the
> floor that persisted through the v1.0.73/74 TSR fixes, are both Lumen temporal
> accumulation symptoms, not TSR symptoms.
>
> **Why it happens.** Lumen is a screen-probe + radiosity GI system that samples sparsely
> each frame and accumulates over many frames into a temporal history buffer (this is what
> makes Lumen affordable -- you couldn't sample densely each frame at real-time rates).
> Two consequences:
>
> - **Lights toggled off**: direct lighting flips instantly, but the screen-probe gather +
>   radiosity history hold onto samples from the previous lit state for many frames. The
>   floor glow "fades out" instead of cutting.
> - **Rotating gobo**: each frame Lumen samples the cookie-lit floor sparsely as an
>   indirect bounce source; prior-frame samples linger in history; the result is a GI
>   trail in the bounced light, ON TOP of any TSR-side smear (v1.0.73/74 fixed TSR; this
>   is the layer underneath).
>
> **The fix.** Disable Lumen's temporal filters so direct light changes propagate to GI at
> full strength on the very next frame. Pack pushed at `PostEngineInit`, snapshot/restore
> on the same machinery as `GoboAntiGhost` (which got refactored to share via
> `ApplyCVarPack`):
>
> ```
> r.Lumen.ScreenProbeGather.Temporal 0                       # primary -- screen-probe history off
> r.Lumen.Reflections.Temporal 0                             # reflection bounce -- catches gobo on shiny floors
> r.Lumen.Radiosity.Temporal 0                               # final-gather radiosity history off
> r.LumenScene.SurfaceCache.RecaptureLightingPerFrame 1      # force per-frame surface cache refresh
>                                                            #   (otherwise the cache ALSO holds stale direct
>                                                            #   lighting samples and compounds the fade)
> ```
>
> **Cost.** Noisier GI. The temporal filter was hiding the sparse-sampling noise; without it
> you can see grain on indirect-lit surfaces. For a stage lighting visualiser this trade-off
> is the right one: instant response trumps smoothness. The eye reads the slight grain as
> natural surface texture; the eye reads a slow GI fade-off as "the lights aren't actually
> responding". For cinematic scenes where smooth GI matters more, `Rebus.LumenFastResponse 0`
> restores the snapshot byte-exact.
>
> **Live toggle: `Rebus.LumenFastResponse [0|1]`.** Default ON since v1.0.78, auto-applied
> at `PostEngineInit`. Logs each CVar's before/after on every transition:
>
> ```
> LumenFastResponse ON [PostEngineInit]: r.Lumen.ScreenProbeGather.Temporal was=1 now=0
> LumenFastResponse ON [PostEngineInit]: r.Lumen.Reflections.Temporal was=1 now=0
> LumenFastResponse ON [PostEngineInit]: r.Lumen.Radiosity.Temporal was=1 now=0
> LumenFastResponse ON [PostEngineInit]: r.LumenScene.SurfaceCache.RecaptureLightingPerFrame was=0 now=1
> Rebus.LumenFastResponse 1 -> live state ON.
> ```
>
> **Refactor note.** v1.0.78 split the snapshot/restore implementation out into a generic
> `ApplyCVarPack(bEnable, Phase, PackLabel, &liveState, &priorState, defs, numDefs)`. Both
> `GoboAntiGhost` (TSR layer, v1.0.73/74) and `LumenFastResponse` (Lumen layer, v1.0.78) now
> thin-wrap that. Future "Rebus.XFastResponse" packs (e.g. virtual shadow map temporal,
> volumetric fog history) drop in as a new `FCVarPackDef` array + one call.

> **Build hotfix: bShouldClearRenderTargetOnReceiveUpdate is protected in UE 5.7 (v1.0.76).**
> v1.0.74 / v1.0.75 wrote `GoboRT->bShouldClearRenderTargetOnReceiveUpdate = true` directly,
> which failed to compile in UE 5.7 with `C2248: cannot access protected member` -- the
> field is `protected` on `UCanvasRenderTarget2D` in 5.7 (it's still UPROPERTY-exposed via
> `meta=(AllowPrivateAccess="true")` for Blueprint, but C++ outside the class is blocked).
>
> v1.0.76 routes through `FProperty` reflection (two tiny helpers,
> `SetGoboRTClearOnUpdate` + `ReadGoboRTClearOnUpdate`, in the anonymous namespace next to
> the canvas-RT code). Reflection-based writes bypass C++ access control because
> `FBoolProperty::SetPropertyValue_InContainer` operates on the UPROPERTY through the
> reflection system, not through a C++ member access. Semantics are unchanged:
> v1.0.74's defensive assertion still runs at every `EnsureGoboRT` and
> `RebuildGoboRTAtSize`, the `Rebus.DumpGoboState` line still reports the live flag value,
> and the engine-default `true` provides the no-op fallback if `FindPropertyByName` ever
> returns null (a future engine rename). No behavioural change vs. v1.0.74/75 -- this is
> the build fix only.

> **Gobo resolution 512 -> 1024 + mipmaps + DLSS scaffolding (v1.0.75).** User asked
> *"can we increase the resolution of the gobos, they look pixelated. Can we enable NVIDIA
> DLSS"*. Two unrelated requests, both addressed here.
>
> **Gobo resolution.** Pre-v1.0.75 the per-fixture `GoboRT` was hard-coded **512x512**, with
> no mipmaps and the default sampler filter. At a typical 60-degree stage throw at 8m, 512
> across the cookie gives ~3 cm/texel on the floor -- that's the pixelation. We now:
>
> - Default to **1024x1024** (4x area). Cost is ~6 MiB / fixture (RGBA8 + mip chain) and
>   the canvas redraw is bandwidth-bound rather than fill-bound at this size, so no
>   measurable per-frame cost on a 4080-class GPU.
> - Enable **`bAutoGenerateMips = true`** and **`Filter = TF_Trilinear`** on the canvas RT
>   so the LF sampler picks the right LOD by screen footprint. Pre-v1.0.75, a 1024 RT
>   projected onto a tiny floor patch aliased hard (every other texel skipped); mip-LOD
>   picks a 256 or 128 sample for tiny footprints and the small-on-screen lights stay
>   crisp.
> - New console knob `Rebus.GoboRTSize <pixels>` rebuilds every fixture's RT at a new
>   square pow2 size (clamped to `[128, 8192]`). Useful presets:
>
>   | Size  | VRAM / fixture | When                                                  |
>   |-------|---------------:|-------------------------------------------------------|
>   | 512   | ~1.5 MiB       | pre-v1.0.75 default; lowest cost, alias on close throws |
>   | 1024  | ~6 MiB         | v1.0.75 default; crisp at typical throws              |
>   | 2048  | ~25 MiB        | hero shows; virtually no aliasing even up close       |
>   | 4096  | ~100 MiB       | max useful; mip-pyramid bandwidth becomes noticeable  |
>
> - `Rebus.DumpGoboState` now reports the RT size + mip + filter state too, so the resize
>   is provable from one log line.
>
> Per-actor API: `ARebusFixtureActor::RebuildGoboRTAtSize(int32 RequestedSizePixels)`
> returns the resolved pow2 size. Caller is responsible for re-pushing the RT into the
> cookie + cone MIDs (the function tail-calls `ApplyCurrentGoboToEpicBeam` which does
> exactly that).
>
> **NVIDIA DLSS scaffolding.** The DLSS plugin is **not bundled** with this project (the
> .uproject has no DLSS entry, and `Plugins/` has no `DLSS*`/`NGX*` directory). v1.0.75
> adds the runtime plumbing so DLSS works the moment the plugin is dropped in:
>
> 1. Download the **NVIDIA DLSS Unreal plugin** from
>    https://developer.nvidia.com/rtx/dlss/get-started (or the UE Marketplace "DLSS"
>    listing).
> 2. Extract to `REBUS_Visualiser/Plugins/DLSS/`.
> 3. Add `{ "Name": "DLSS", "Enabled": true }` to the `Plugins` array in
>    `REBUS_Visualiser/REBUS_Visualiser.uproject`.
> 4. Restart UE.
> 5. From the portal console (or in-editor): `Rebus.DLSS quality`.
>
> The new console command `Rebus.DLSS [off|quality|balanced|performance|ultraperformance|dlaa]`
> detects the plugin by probing `r.NGX.DLSS.Enable` / `r.NGX.DLAA.Enable` -- if the CVar
> isn't registered, the plugin isn't loaded and the command logs the install instructions
> instead of failing silently. When present, the preset maps to `r.NGX.DLSS.Quality`:
>
> | Preset             | Internal scale | r.NGX.DLSS.Quality |
> |--------------------|---------------:|-------------------:|
> | `off`              | n/a            | DLSS disabled, TSR fallback |
> | `quality`          | 67%            | 2 (default if no arg)       |
> | `balanced`         | 58%            | 1                           |
> | `performance`      | 50%            | 0                           |
> | `ultraperformance` | 33%            | 3 (4K+ output only)         |
> | `dlaa`             | 100% (no upscale) | DLAA enabled instead     |
>
> Requires NVIDIA RTX hardware (RTX 20-series or newer).
>
> **DLSS + rotating gobo: the trade-off.** DLSS uses temporal accumulation (same family as
> TSR), so the rotating-gobo ghost-trail symptom v1.0.73/74 fixed for TSR can re-appear
> under DLSS. The `Rebus.GoboAntiGhost` CVars stay on -- they're TSR-specific and don't
> touch DLSS's internal accumulator. Practical mitigations:
>
> - **`Rebus.DLSS dlaa`** -- deep-learning AA at native resolution. No upscale = less
>   accumulation pressure. Closest DLSS preset to a "clean rotating gobo".
> - **`Rebus.DLSS off`** -- fall back to TSR. The `r.TSR.*` knobs in GoboAntiGhost apply
>   again and the v1.0.74 tuning takes effect.
> - **Per-show**: run gobo-light shows on TSR; switch to DLSS-quality for static / less
>   gobo-heavy scenes where the perf headroom matters more.

> **Rotating-gobo ghosting fix v2 -- TSR history weight + LightFunctionAtlas + explicit RT clear (v1.0.74).**
> User reported v1.0.73's TSR flicker-rejection push *"doesn't change the issue, we are seeing
> the gobo ghosting on the floor as it spins"*. That fix was correct (TSR's flicker rejection
> IS necessary for animated light functions on opaque surfaces) but insufficient -- biasing
> TSR to reject flickering pixels still leaves the underlying history weight at its default,
> so the rejected sample blends with the prior frame and a trail still smears.
>
> **What v1.0.74 adds to the pack.** Two more CVars in the same `ApplyGoboAntiGhost` push,
> applied on the same `PostEngineInit` boundary and tracked in the same snapshot/restore
> machinery:
>
> ```
> r.TSR.History.UpdateRate 0.6           // was 0.4 default -- new frames stand more on their own
> r.LightFunctionAtlas.Enabled 0         // bypass the atlas -- no chance of a stale cached LF sample
> ```
>
> `r.TSR.History.UpdateRate` is the dominant lever for "fast-moving on-screen content trails
> behind itself". 0.4 means the new frame contributes only ~40% to the temporal accumulator
> and prior frames contribute the rest -- great for static scenes (perfect AA from accumulated
> sub-pixel jitter) but exactly the wrong response for a fast-rotating cookie. 0.6 keeps
> enough history for clean static AA on truss / set / floor surfaces but cuts the visible
> trail noticeably. Higher than 0.7 starts to introduce sub-pixel shimmer on real static
> high-frequency detail, so 0.6 is the safer ceiling for the global default.
>
> `r.LightFunctionAtlas.Enabled 0` is defensive. UE 5.5+ caches LF samples into a single
> atlas to reduce shader permutations and improve perf; great for STATIC LFs but for a
> per-frame-changing cookie (our rotating GoboRT) any stale atlas entry reads as the gobo
> "lagging" its true rotation. `RebusFixtureActor.cpp:3063` already notes M_Light_Master
> isn't atlas-compatible AND the LF is forced through the legacy deferred path while a gobo
> is active (`bAllowMegaLights=0`), but disabling the atlas globally removes any chance the
> engine ever decides to route the LF through it for some future-version atlas-compat
> heuristic. Cost: per-pixel LF eval for any other LF in the scene is slightly higher.
>
> **Explicit RT clear assertion in `EnsureGoboRT` (v1.0.74).** We now set
> `GoboRT->bShouldClearRenderTargetOnReceiveUpdate = true` explicitly. UCanvasRenderTarget2D
> defaults this to true in 5.7, but a future engine default flip OR an external write would
> silently turn the RT into an accumulator -- successive `K2_DrawTexture` calls with
> `BLEND_Translucent` on top of the uncleared prior frame would build up a smear of every
> recent gobo orientation, looking exactly like the floor-projection ghosting the user
> reports. Explicit-set removes that failure mode regardless of any future default change.
>
> **New: `Rebus.DumpGoboState` diagnostic.** Per-fixture dump of every ingredient that
> determines whether the rotating-cookie pipeline can ghost on the floor. Prove or disprove
> each one from a single grep:
>
> ```
> DumpGoboState '54E648DF...': bGoboActive=1 srcTex=GoboRT_5 GoboRT=0x... (512x512 clearOnUpdate=1) GoboAngle=237.4deg goboSpd=0.800 animSpd=0.000 combined=0.800 -- SpotLight: allowMega=0 LightFn=GoboLightFnMID_2 LensFn-MID=0x... EpicBeamMID=0x...
> ```
>
> Reading the dump: `bGoboActive=1` + non-zero `combined` + `clearOnUpdate=1` + `allowMega=0`
> + non-null `LightFn` means the pipeline is wired correctly and any remaining ghost is in
> the temporal AA chain (i.e. the GoboAntiGhost CVars are doing the work). If `allowMega=1`
> ever shows up while `bGoboActive=1`, the MegaLights opt-out regressed and MegaLights'
> temporal denoiser is in the loop -- THAT is the canonical "rotating gobo ghosts on the
> floor" symptom and the fix is to repair the opt-out in `ApplyCurrentGoboTo*`. If
> `clearOnUpdate=0`, the RT itself is accumulating and v1.0.74's explicit-set didn't take
> (asset override, editor utility script, etc.).
>
> **Nuclear-option escalation** (not pushed by default -- prove the diagnosis first):
> ```
> r.AntiAliasingMethod 1         // FXAA -- zero temporal accumulation; if THIS fixes it, the issue is 100% TSR-side
> r.MegaLights.Volume 0          // disable MegaLights volumetric path -- if THIS fixes it, the ghost is in fog scatter, not floor projection
> r.TSR.History.UpdateRate 1.0   // no temporal blending at all -- prove TSR is the source
> ```

> **Rotating-gobo ghosting fix: TSR flicker rejection + full-res light functions (v1.0.73).**
> User reported *"When the gobo is rotating fast we are getting ghosting. Is it a historyweight?"*.
> Yes -- it's exactly that, and TSR's purpose-built mitigation for it has shipped since 5.3.
>
> **Why it ghosts.** TSR (UE's default upscaler since 5.2) is a temporal accumulator that relies
> on motion vectors to know "this pixel moved by N pixels last frame, so go fetch the value
> from there in history". Animated light functions (our rotating gobo, projected via a
> SpotLight cookie onto opaque floor / set geometry) violate the assumption: the LIGHTING
> moves but the SURFACE does not, so the motion vector for the lit floor pixel is zero. TSR
> sees "same pixel, different colour every frame", reads it as flickering shading, and the
> history rejection trails the new value as the pattern rotates -- visible as a smear behind
> the rotating cookie. (The gobo render target itself is fine: `OnGoboRTUpdate` redraws it
> rotated, transparent-cleared, every Tick; the smear is downstream in TSR's accumulator,
> not in the RT.)
>
> **The fix.** Three CVars, pushed automatically on `FCoreDelegates::OnPostEngineInit` (so
> the renderer module is guaranteed loaded and the CVars are registered):
>
> ```
> r.TSR.ShadingRejection.Flickering 1                    // enable flicker-aware shading rejection
> r.TSR.ShadingRejection.Flickering.AdjustToFrameRate 1  // scale threshold to fps (30fps shadows stay)
> r.LightFunctionQuality 2                               // full-res light functions (smoother per-frame)
> ```
>
> `r.TSR.ShadingRejection.Flickering` is TSR's purpose-built rejection path for "lighting
> changes on a static surface" -- introduced specifically for club / disco / stage lighting
> in 5.3 and tightened through 5.5. With it on, TSR detects the per-pixel colour oscillation
> from a moving gobo, rejects the stale history sample for those pixels, and the trail
> disappears (at a small per-pixel cost: the new sample stands more on its own jittered
> sub-pixel reconstruction instead of leaning on history).
>
> **Snapshot + restore.** The push uses `ECVF_SetByGameOverride` priority, and snapshots the
> prior `int` value of each CVar before writing. The `OFF` path restores each CVar to its
> snapshotted value byte-exact (per-CVar `bValid` guard skips ones that weren't registered
> at push time, so a hot-loaded renderer module is benign).
>
> **Live toggle: `Rebus.GoboAntiGhost [0|1]`.** Default ON since v1.0.73. Use it for A/B
> comparison or if a specific scene wants raw TSR back. Each toggle logs which CVar moved:
>
> ```
> GoboAntiGhost ON [PostEngineInit]: r.TSR.ShadingRejection.Flickering was=0 now=1
> GoboAntiGhost ON [PostEngineInit]: r.TSR.ShadingRejection.Flickering.AdjustToFrameRate was=0 now=1
> GoboAntiGhost ON [PostEngineInit]: r.LightFunctionQuality was=1 now=2
> Rebus.GoboAntiGhost 1 -> live state ON.
> GoboAntiGhost OFF [ConsoleCommand]: r.TSR.ShadingRejection.Flickering was=1 restored=0
> ...
> Rebus.GoboAntiGhost 0 -> live state OFF.
> ```
>
> **Out of scope.** We deliberately do NOT touch `r.AntiAliasingMethod` (the project might
> have a deliberate non-TSR setting), `r.TSR.History.UpdateRate` (lowering it reduces ghost
> at cost of generalised flicker on real moving content -- too situational to push globally),
> or `r.MegaLights.*` (light-function paths there have a different temporal pipeline; the
> flicker-rejection CVar above is the right knob for both classic and MegaLights paths in
> 5.5+). If a scene still ghosts on a non-light-function path (e.g. translucent particle
> trails) that's a separate issue and not covered by this toggle.

> **Rebus.* prefix tolerance on the fixture-channel ConsoleCommand path (v1.0.72).** Portal
> sent `{ "type":"ConsoleCommand", "command":"ShowOrbitFixtures 0" }` and the log line read
> `Fixture-channel ConsoleCommand: 'ShowOrbitFixtures 0' (success=0)`. Our IConsoleCommand
> objects (every `Rebus.*` command we register in `FRebusVisualiserModule::StartupModule`)
> live under the `Rebus.` namespace prefix -- `Rebus.ShowOrbitFixtures`, `Rebus.ShowOrbit`,
> `Rebus.OverrideFixtureMaterials`, `Rebus.DriveOrbitModels`, `Rebus.MeshBeams`, etc. --
> so `GEngine->Exec` on the bare name returns false (unknown command) and the toggle
> silently no-ops, just like the user observed.
>
> v1.0.72 makes the dispatcher prefix-tolerant: it tries `Exec` as-given first; if that
> returns false AND the first token (everything before the first space) has no `.` in it
> (so it can't already be a namespaced command), it retries once with `Rebus.` prepended.
> Either-path success counts as success. The log line reports which variant actually ran:
>
> ```
> LogRebusVisualiser: Fixture-channel ConsoleCommand: 'ShowOrbitFixtures 0' -> retried as 'Rebus.ShowOrbitFixtures 0' (success=1)
> LogRebusVisualiser: Fixture-channel ConsoleCommand: 'Rebus.ShowOrbitFixtures 0' (success=1)
> LogRebusVisualiser: Fixture-channel ConsoleCommand: 'stat fps' (success=1)
> ```
>
> Scope is intentionally narrow: ONE retry, ONE prefix (`Rebus.`), no token-by-token
> rewriting, no allowlist. Engine and other-plugin commands with a `.` in the name (e.g.
> `r.ScreenPercentage`, `t.MaxFPS`, `PixelStreaming2.AllowPixelStreamingCommands`) fall
> through the retry guard and behave exactly as before. The portal contract is unchanged
> -- `Rebus.<Name>` keeps working -- but the slightly looser form (`<Name>` only) now
> works too, which removes a class of "the toggle didn't fire" surprises across every
> future Rebus.* command we add.

> **Fixture body + lens material override: black satin plastic + mirrored glass (v1.0.71).**
> User asked *"can all fixtures have their texture/material overridden and can UE make a
> Fixture Material that we use. This will be a black satin plastic material. If there is a
> lens material, this can be swapped for a mirrored glass material."* -- pre-v1.0.71 the
> control-channel procedural body meshes shipped with whatever default `UProceduralMeshComponent`
> renders (the engine WorldGridMaterial checkerboard in most cases), and the Orbit-imported
> bodies kept whatever the glb shipped, which is usually nothing -- so the show looked like
> a pile of grey debug meshes instead of real fixtures.
>
> **What v1.0.71 changes.** Every fixture's body meshes (control-channel `UProceduralMeshComponent`
> *and* Orbit-imported components bound by `RebindOrbitModels`) now get a slot-0 material
> override at build/bind time. Two material variants, picked per mesh by a name/tag keyword
> scan:
>
> - **Body** (black satin plastic) -- applied to any mesh whose name/tag does NOT match a lens
>   keyword. `Color = #050505`, `Metallic = 0`, `Roughness = 0.35`. Near-black diffuse + a
>   soft sheen highlight -- reads as injection-moulded fixture housing under stage lights.
>   (Pure black crushes contrast and reads as "flat black" rather than "satin"; the slight
>   colour catches the sheen.)
> - **Lens** (mirrored glass) -- applied to any mesh whose name OR any of its ComponentTags
>   contains the substring `lens` / `glass` / `crystal` / `optic` / `front` (case-insensitive,
>   covers GDTF "Lens"/"Front Lens"/"Optic", glb "lens"/"glass", and decorative-crystal naming).
>   `Color = #F2F2F2`, `Metallic = 1`, `Roughness = 0.05` -- a polished chrome mirror. True
>   dielectric glass needs translucent shading which the runtime parent doesn't support; in a
>   venue under stage light a fixture lens reads visually as a chrome mirror anyway (because
>   the dark interior absorbs the back), so metallic-mirror is the right approximation.
>
> **How the materials get built (no new assets required by default).** The two MIDs are
> created lazily per fixture from `/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial`
> -- an engine-shipped parametric PBR parent that exposes `Color` (vector), `Metallic`
> (scalar), `Roughness` (scalar) as parameters. Ships with every UE install so there are no
> new content additions; the cooker captures it via the constructor `ConstructorHelpers::
> FObjectFinder` hard ref (cook-safe -- a runtime LoadObject is *not* a cook dependency).
> If the engine ever renames those parameters the `SetVectorParameterValue`/
> `SetScalarParameterValue` calls become benign no-ops and the MID renders as the parent's
> default look -- never crashes, never breaks the build.
>
> **User-overridable assets (optional, drop-in).** The constructor also probes
> `/Game/REBUS/Materials/M_RebusFixtureBody.M_RebusFixtureBody` and
> `M_RebusFixtureLens.M_RebusFixtureLens`. If you create either asset in the editor it takes
> precedence over the runtime MID -- the override uses your material verbatim, no parameter
> mangling, no naming requirements on the parameters. To author them: right-click
> `/Game/REBUS/Materials` -> New -> Material, save as `M_RebusFixtureBody`/`M_RebusFixtureLens`,
> author whatever PBR shading you want (anisotropic satin, translucent glass with proper
> IOR, painted texture maps, anything). Restart the editor or PIE and the new asset will be
> picked up by the next fixture spawn.
>
> **Live toggle: `Rebus.OverrideFixtureMaterials [0|1]`.** Default ON. OFF restores each
> mesh's pre-override material from a per-actor cache captured the first time the override
> was applied -- byte-exact restore. The summary log reports the per-fixture body/lens
> apply or restore count:
>
> ```
> Rebus.OverrideFixtureMaterials 1: 4 fixture(s) -- body=18 lens=6 meshes overridden.
> Rebus.OverrideFixtureMaterials 0: 4 fixture(s) -- restored=24 original material(s).
> ```
>
> Per-actor API (for portal-side fine-grain control later):
> `ARebusFixtureActor::SetFixtureMaterialOverrideEnabled(bool)` returns
> `FFixtureMaterialApplyCount{Body, Lens, Restored}`.
>
> **Scope notes.** Only slot 0 of each mesh is overridden -- multi-material meshes keep
> slots 1+ on their original materials. The emissive lens-flare disc (`LensDisc`,
> `M_RebusLensFlare`) is untouched -- that's an additive glow disc, not body geometry; the
> "lens material" the user asked to swap is a body-mesh lens (the front optic / decorative
> glass), distinct from the flare. The beam canvas (Epic `MI_Beam`/REBUS `M_RebusBeam`) is
> also untouched -- it's the volumetric shaft, not body.

> **Hide-Orbit console commands (v1.0.70).** User asked *"how do I hide the orbit lights on
> cmd"* -- there was no console toggle for this. v1.0.65's `Rebus.DriveOrbitModels 0` only stops
> driving (the orbit fixtures freeze at their imported pose but stay visible). Added two
> commands, scoped narrow -> broad:
>
> - `Rebus.ShowOrbitFixtures [0|1]` -- show / hide ONLY the Orbit-imported geometry bound to
>   each `ARebusFixtureActor` (the components matched + bound by `RebindOrbitModels`).
>   Anything else in the OrbitImportRoot -- trusses, set pieces, layout meshes -- stays
>   visible. Use this when you want to A/B between the control-channel mesh proxies and the
>   orbit-imported fixture bodies, or to silently "remove" duplicate fixture geometry when
>   the two are stacked on top of each other.
> - `Rebus.ShowOrbit [0|1]` -- sledgehammer: hide every actor of class `OrbitImportRoot` in
>   every Game/PIE world via `SetActorHiddenInGame`. Kills the entire Orbit scene -- fixtures
>   AND trusses / set / layout -- in one shot.
>
> Both default to *show* when invoked with no arg, matching the other `Rebus.*` toggles
> (`Rebus.MeshBeams`, `Rebus.DriveOrbitModels`). The fixtures command logs a per-invocation
> summary: `Rebus.ShowOrbitFixtures 0: 4 fixture(s), 20 Orbit component(s) hidden.` The root
> command logs the count of OrbitImportRoot actors touched.
>
> Implementation notes:
>
> - `ARebusFixtureActor::SetOrbitVisibility(bool)` walks `OrbitComponents` and calls
>   `Comp->SetVisibility(bVisible, /*bPropagateToChildren*/ true)`. Returns the count
>   toggled so the console summary is meaningful. Dead weak handles (post-reimport) are
>   silently skipped.
> - The drive loop is unchanged -- hidden components still receive transform updates, so
>   toggling visibility back on lands them in the current pose, not a stale one.
> - The root command matches `GetClass()->GetName() == TEXT("OrbitImportRoot")` to avoid a
>   compile/link dependency on the separately-owned OrbitConnector plugin (same pattern
>   `RebindOrbitModels` uses to find the root).

> **ID-only axis classification, no bounding box (v1.0.69).** v1.0.68 added per-component
> motion-axis bucketing but its position-fallback strategy misbehaved on the user's GLBs:
>
> ```
> Fixture 54E648DF-...: BOUND 5 Orbit-imported component(s) (drive=ON)
>   | axis-buckets: base=0 pan=0 tilt=5 other=0 default=0
>   | sample: 'StaticMeshComponent_3'->position(a=1) | ...->position(a=1)
>     | ...->position(a=1) | ...->position(a=1) | ...->position(a=1)
> ```
>
> Every component went to the tilt axis. Cause: typical moving-head GLBs cluster every mesh
> (base, yoke, head, sub-parts) within ~tens-of-cm of each other, all within the head volume.
> The nearest-pivot heuristic puts a mesh on the closest rig pivot; with comps stacked near
> the head, "nearest" was tilt for everyone, ties resolved deepest-first onto tilt. With the
> whole fixture on `Cumulative[tilt] = TiltSelf * PanCumulative`, the user saw the body rotate
> as one rigid lump driven by pan, while the *intended* per-part split was nowhere to be seen:
> *"the tilt is no longer moving the orbit fixture but the entire orbit fixture is rotating
> on pan. No individual yoke/head control. ... can we just use IDs and not location bounding
> box."*
>
> **What v1.0.69 changes.** The classification chain is reduced to **ID/name strategies only**;
> the position fallback and default-head safety net are removed.
>
>   1. **Tag-name** -- each `Comp->ComponentTags` entry tested against
>      `Profile.MotionRig.Axes[i].AffectedGeometryNames` via the existing
>      `ResolveAxisForMesh`. Picks the deepest matching axis.
>   2. **Comp-name** -- `Comp->GetName()` tested the same way.
>   3. **Keyword scan** -- name + tags lower-cased, then substring search:
>      `head`/`tilt` -> tilt axis, `yoke`/`arm`/`pan` -> pan axis,
>      `base`/`body` -> static (`INDEX_NONE`).
>   4. **Attach-hierarchy depth** -- if the components form an attach-tree with varying
>      `GetAttachParent` depths (importer preserved glTF node hierarchy), deepest = head,
>      max-1 = yoke, root = base. Skipped when all components are at the same depth (typical
>      for procedural importers that re-parent everything under one root).
>   5. *(no position fallback)*
>   6. *(no default head)*
>
> Components for which NO strategy fires bucket to **`INDEX_NONE` (static rest)**. "Nothing
> moves" is preferable to "wrong thing moves" -- a static base mesh is correct on the body
> and lets the user immediately see which meshes are unclassified.
>
> **New diagnostic warning.** When at least one component falls through to unclassified, the
> classifier emits a one-shot warning per fixture spelling out exactly what naming the
> orbit-cli / portal can adopt to enable motion:
>
> ```
> Fixture 54E648DF-...: 5/5 Orbit component(s) UNCLASSIFIED (static rest). To enable per-part
>   motion, the orbit-cli / portal should expose ONE of these per mesh component,
>   case-insensitive:
>   (1) a ComponentTag matching a GDTF AffectedGeometryNames entry on this fixture's rig,
>   (2) the component name matching the same,
>   (3) any tag OR name containing the substrings 'head'/'tilt' (-> tilt axis),
>       'yoke'/'arm'/'pan' (-> pan axis), or 'base'/'body' (-> static),
>   (4) a preserved glTF parent-child hierarchy under OrbitImportRoot (deepest = head,
>       mid = yoke, root = base).
>   Current component names are generic (e.g. 'StaticMeshComponent_3') -- recommend tagging
>   each mesh with 'base' / 'yoke' / 'head' on the orbit-cli side.
> ```
>
> **Recommended schema (easiest to adopt).** On the orbit-cli side, for each MVR fixture
> instance's GLB, add one of these per mesh component before publishing to Unreal:
> - **ComponentTag** = `base`, `yoke`, or `head` (lower-case, exact). Strategy 3 catches it.
> - OR **component Name** containing one of those substrings (e.g. `MovingHead_Head_001`).
>   Strategy 3 catches it.
> - OR preserve the MVR symdef's parent-child hierarchy when building the GLB nodes
>   (strategy 4 catches it).
>
> Tag-based is the simplest because it doesn't require renaming meshes -- one extra
> `ComponentTags.Add(TEXT("head"))` per mesh in the orbit-cli emit step and the visualiser
> immediately splits the fixture correctly.
>
> Healthy log result after orbit-cli adoption:
>
> ```
> Fixture <id>: BOUND 5 Orbit-imported component(s) (drive=ON)
>   | axis-buckets: base=1 pan=2 tilt=2 other=0 unclassified=0
>   | sample: '...'->keyword-base(a=-1) | '...'->keyword-pan(a=0)
>     | '...'->keyword-head(a=1) | ...
> ```

> **Per-component axis classification: base stays put, yoke pans, head pans+tilts (v1.0.68).**
> v1.0.67 finally got the Orbit-imported mesh bound to the control-channel fixture, but the
> user immediately reported: *"we now have control of the Orbit fixture mesh but it's treating
> it as one object so the whole fixture tilts instead of just the head. We are not seeing the
> orbit fixture being treated as base, yoke and head."*
>
> **Root cause.** The control-channel mesh proxies have always classified per-mesh-per-axis via
> `MeshAxisBucket[i] = RebusMotion::ResolveAxisForMesh(rig, geometryName, modelName)`, so the
> base sits static, the yoke rides `Cumulative[panAxis]`, and the head rides
> `Cumulative[tiltAxis]` (which is itself `tilt * pan` because `Cumulative` is parent-composed
> in `RebusMotion::Solve`). The Orbit-bind path (introduced in v1.0.35) cached one
> `OrbitHeadWorldRest` for the WHOLE bind and `DriveOrbitModel(headLocal)` did
> `Comp->SetWorldTransform(OrbitBindBase[i] * (HeadLocal * ActorWorld))` for every component
> using the same `HeadLocal` -- so every component, regardless of whether it was the base, yoke
> or head mesh, rode the head's pan*tilt cumulative. Visually that meant a moving-head fixture's
> entire body tilted as one rigid lump.
>
> **What v1.0.68 changes.** The Orbit drive now mirrors the control-channel pipeline: per
> component motion-axis bucket + drive each bucket from its own `Cumulative[axis]`.
>
> 1. **`OrbitAxisBucket[]`** (new state, parallel to `OrbitComponents[]`): per-component axis
>    index from `Profile.MotionRig.Axes` (or `INDEX_NONE` = static base). Built in
>    `BindOrbitComponents` by a six-strategy chain that tries name-based identification first
>    and only falls through to fuzzier heuristics if the importer hasn't surfaced GDTF names:
>
>    1. **Tag-name** -- each `Comp->ComponentTags` entry tested against the rig's
>       `AffectedGeometryNames` via the existing `ResolveAxisForMesh` (catches the easy case
>       where orbit-cli exposes GDTF geometry names directly as tags, e.g. `"yoke"`, `"head"`).
>    2. **Comp-name** -- `Comp->GetName()` tested the same way (covers importers that put the
>       GDTF name on the component itself rather than as a tag).
>    3. **Keyword scan** -- name + tags concatenated and case-folded, then substring-matched
>       for `head`/`tilt` -> tilt axis, `yoke`/`arm`/`pan` -> pan axis, `base`/`body` -> static.
>       Tolerates glb node names like `Light_Head_001` or `MovingHead.yoke.arm` that don't
>       match the GDTF vocabulary exactly.
>    4. **Hierarchy depth** -- if the components form an attach-tree with different
>       `GetAttachParent` depths (importer preserved the glTF node hierarchy), the deepest
>       component is the head, max-1 is the yoke, the rest are base. Skipped when everything
>       is flat at the same depth (typical for procedural importers that re-parent under one
>       root).
>    5. **Position-fallback** -- world-space distance from `Comp->GetComponentLocation()` to
>       each axis's pivot (Y-up metres in the rig, converted to engine cm + transformed by
>       `ActorWorld`); nearest pivot wins. `AxisPivots` is sorted deepest-first so a tie picks
>       head over yoke, matching how a real moving-head's pivots stack (tilt nested inside
>       pan). Works without any naming convention and without an orientation assumption (the
>       comparison is pure 3-D distance, so hanging and standing rigs both classify correctly).
>    6. **Default head** -- last resort, equivalent to pre-v1.0.68 behaviour; reported in the
>       log as `default-head` (or `default-static` if the rig has no head axis) so the user
>       can see we punted.
>
> 2. **`DriveOrbitModel(const TArray<FTransform>& Cumulative)`** -- signature changed from
>    `(const FTransform& HeadLocal)`. Per component, looks up
>    `OrbitAxisBucket[i]` and applies `Cumulative[axis]` (or `Identity` for base / invalid
>    bucket / empty Cumulative). The bind-base captured at rest collapses to
>    `CompRest * ActorWorld^-1` for every bucket because every axis's rest-cumulative is the
>    identity transform (verified: `RotateAboutPivot` with a zero-angle quat is the identity),
>    so ONE `OrbitBindBase` array still drives every axis -- no per-axis storage needed.
>
> 3. **All four call sites updated**:
>    - `RefreshMotion` no-rig path: pass `TArray<FTransform>{}` (empty -> base for everyone ->
>      every component holds rest).
>    - `RefreshMotion` full path: pass the already-solved `Cumulative` directly (no redundant
>      solve per tick).
>    - `BindOrbitComponents`, `SetDriveOrbitModel(true)`: route through the new
>      `DriveOrbitModelFromPanTilt(InPan, InTilt)` helper, which solves once and forwards.
>
> **What you should see in the log.** The bind line is now diagnostic:
>
> ```
> Fixture <id>: BOUND 6 Orbit-imported component(s) by objectId 'mvr-symdef-instance-...'
>   (drive=ON) | axis-buckets: base=1 pan=2 tilt=3 other=0 default=0
>   | sample: 'StaticMeshComponent_0'->position(a=-1) | ...->position(a=0) | ...->position(a=1)
> ```
>
> - `base=N` -- components held static (the body / clamp / base plate).
> - `pan=N` -- components riding the yoke axis (the arms that swing left/right).
> - `tilt=N` -- components riding the head axis (the lamp that tilts up/down).
> - `default=N` -- count we couldn't classify, defaulted to head; non-zero means we should
>   look at the sample line and tighten a strategy. Healthy result: `default=0`.
> - `sample:` -- first 6 components with the *strategy that fired* (`tag-name`, `comp-name`,
>   `keyword-head`, `depth-yoke`, `position`, `default-head`, ...) so you can see which signal
>   the importer is actually giving us.
>
> **If the per-fixture split still looks wrong**, send me the `axis-buckets:` line and the
> `sample:` line and I'll either tighten a strategy or add a portal-side override field on
> `FRebusSceneFixture`. The position fallback is generic enough to work on most moving heads
> regardless of naming, but a fixture geometry that happens to put the base and yoke at the
> same world-space distance from the pan pivot can fall over -- in that case strategies 1-3
> need explicit naming hints from the importer.

> **Position-based Orbit match: bridge the MVR-UUID vs Speckle-id namespace gap (v1.0.67).**
> The v1.0.66 id-shape diagnostic answered "why doesn't substring match work" -- the two sides
> are in totally different id namespaces:
>
> ```
> matched=0 (substring=0) unmatched=4 unmatchedFixtureIds=090be834..., 0bfe4640..., ...
> Orbit bind sample orbit-side ids (first 12/25): 01354FAB-15FC-4481-... | 053DA726-... |
>   465FFBF2-... | 54E648DF-... | Design Layer-1 |
>   mvr-symdef-instance-08AE2F2B-E78F-... | mvr-symdef-instance-394E793C-... |
>   mvr-symdef-instance-9A066770-... | node10 | node11 | node12 | node13
> ```
>
> Control-channel ids are 32-char lowercase hex Speckle node ids; Orbit-side tags are MVR
> instance UUIDs (8-4-4-4-12 uppercase, with or without `mvr-symdef-instance-` prefix). The
> Speckle id is content-hashed -- not derivable from the MVR UUID alone (verified offline: every
> casing/dash variant md5/sha1 we tried gave zero hits against the four portal ids). So no
> string transformation can bridge them.
>
> **But world POSITION can.** Both the portal and `orbit-cli` ingest the SAME MVR file, so both
> place each fixture at the same MVR-coordinate location. Position is the universal ground
> truth we can always bridge through.
>
> **What v1.0.67 changes in `RebindOrbitModels`.** The match loop is restructured into a
> three-strategy pipeline tried in order per fixture, with each Orbit instance bindable to AT
> MOST ONE fixture per pass:
>
> 1. **Exact** -- canonical: `OrbitIndex.Find(FixtureId)` (unchanged from v1.0.35).
> 2. **Substring** -- tag *contains* the fixture id (unchanged from v1.0.66; catches
>    `Light_001/090be834...`-style wrappers).
> 3. **Position (NEW)** -- nearest unbound instance-centroid within a 5 m tolerance.
>    Implementation:
>
>    - Build `InstanceCentroids` once per rebind: scan all Orbit tag groups, keep only the ones
>      whose key matches a UUID pattern (8-4-4-4-12 hex, anywhere in the string; catches both
>      bare `54E648DF-...` and prefixed `mvr-symdef-instance-08AE2F2B-...` shapes). Skip
>      "Design Layer-1"-style category tags so they don't pull every component into one giant
>      centroid. For each kept group, centroid = mean of bound components'
>      `GetComponentLocation()`.
>    - Per unmatched fixture, query `GetActorLocation()` (the actor was already spawned at the
>      MVR-derived world position from the scene matrix) and pick the nearest unbound centroid
>      within `kPosToleranceCm = 500` (5 m). Tolerance is generous enough for modest origin
>      offsets between MVR space and Unreal world space, tight enough that adjacent fixtures
>      in a typical truss don't cross-bind.
> 4. **`UsedKeys` tracking** -- across all three strategies, each Orbit instance can only be
>    consumed by ONE fixture per pass. Necessary because the position fallback would otherwise
>    let two fixtures with overlapping centroids both bind to the same group.
>
> **Diagnostics.** The summary line now reports per-strategy counts:
>
> ```
> Orbit bind: roots=1 taggedComps=23 distinctObjectIds=25 instanceCentroids=N
>   | fixtures matched=4 (exact=0 substring=0 position=4) unmatched=0
> ```
>
> A new `position-fallback hits` line names which fixture matched which Orbit key and at what
> distance (first 4 hits):
>
> ```
> Orbit bind position-fallback hits (first 4, tolerance=5.0m):
>   090be834...<-'mvr-symdef-instance-08AE2F2B-...' (0.00m) ;
>   0bfe4640...<-'mvr-symdef-instance-394E793C-...' (0.00m) ;
>   ...
> ```
>
> If a fixture still doesn't match after all three strategies, the existing
> `sample orbit-side ids` line dumps the actual format mismatch and the unmatched-ids list
> tells you which fixtures fell through -- either the fixture has no Orbit twin (legitimately
> orphaned) OR it's positioned > 5 m from any Orbit instance (mismatch tolerance, or a coordinate
> origin offset; tell us and we'll widen the tolerance or fix the origin transform).
>
> **No portal-contract change.** The portal still sends Speckle node ids; OrbitConnector still
> tags components with MVR UUIDs; v1.0.67 just bridges them through world position when ids
> don't line up. If a future portal release adds an explicit `orbitNodeId`/`mvrInstanceId`
> alias to the scene-fixture descriptor we can wire it into a new "alias" strategy ahead of
> position in the pipeline -- the substring + position fallbacks would then only fire when the
> alias is absent.

> **Orbit id-shape diagnostic + substring fallback (v1.0.66).** First v1.0.65 boot in the
> field surfaced the next-layer-down issue: the matching plumbing works, but the IDs on the two
> sides don't line up. Log line from the test scene with 4 control-channel fixtures + a populated
> Orbit import:
>
> ```
> Orbit bind: roots=1 taggedComps=23 distinctObjectIds=25 | fixtures matched=0 unmatched=4
>   unmatchedFixtureIds=090be834b3d5ddc368799972e799e570,981d1fadb2846bac4555ff9a0b6bacbc,
>                       9900f25ec38772e1b609351b76fd128f,0bfe4640283e3a8c7f3f94e7878a4b2c
> ```
>
> 23 Orbit components forming 25 distinct tag strings, none of which equal any of the 4 Speckle
> node ids the portal registered. So `OrbitIndex.Find(FixtureId)` returned null every time. The
> previous diagnostic told us "they don't match" without telling us what the OTHER side actually
> looks like, so we couldn't diagnose whether it was a path wrapper, a hash, a totally different
> namespace, or a casing issue.
>
> **What v1.0.66 changes in `RebindOrbitModels`.**
>
> 1. **Substring-fallback match.** When exact `OrbitIndex.Find(FixtureId)` misses, we now scan
>    the Orbit tag strings and accept the first one that CONTAINS the fixture id as a case-
>    sensitive substring (with an 8-char floor on the fixture id so accidental short-string
>    matches can't fire). This catches the most common glb-export wrappers we expect: tag of
>    the form `Light_001/090be834...`, `MovingHead.090be834...`, `/scene/fixtures/090be834...`,
>    `prefix-090be834...-suffix`, etc. Speckle node ids are hex digests so casing is stable;
>    only the surrounding decoration varies across exports. When the substring match fires, the
>    fixture is bound to the components under that *full Orbit tag* (not the bare fixture id),
>    so subsequent rebinds correctly notice "already bound to this key" and skip rebinding.
> 2. **Orbit-id sample dump.** When the summary log fires AND any fixture is still unmatched
>    after the fallback, a second log line dumps the first 12 (lexically sorted) Orbit-side ids
>    so the actual format mismatch is visible -- no more guessing what the importer wrote:
>
>    ```
>    Orbit bind sample orbit-side ids (first 12/25, lexical): <id-1> | <id-2> | ...
>    ```
> 3. **Substring-hit log.** When the fallback DOES match, a third log line names which fixtures
>    matched which Orbit tag (truncated to the first 4 hits) so you can see at a glance what
>    wrapper shape the importer is using:
>
>    ```
>    Orbit bind substring-fallback hits (first N): 090be834...<-'Light_001/090be834...' ; ...
>    ```
> 4. **Summary line now reports substring count.** `matched=N (substring=M)` so a glance tells
>    you whether your scene relies on the fallback or the canonical exact-equality match.
>
> Why substring rather than a heuristic strip (e.g. split on `/`, take the last token): substring
> match is the smallest viable change that covers ALL the wrapper shapes we've seen, with zero
> false positives at 8+ chars of hex (32-char Speckle ids have ~10^38 distinct values; the
> chance of one accidentally appearing as a substring of an unrelated tag is ~0). If the actual
> mismatch turns out to be a transformation (base64-vs-hex, or a hash of the Speckle id), the
> sample-id log dumped in step 2 gives us the evidence to add a targeted normalisation pass.
>
> **No portal-contract change.** The portal still sends Speckle node ids; OrbitConnector still
> tags components however it tags them; v1.0.66 just bridges the two when the importer's tag
> wraps the id in extra characters. Field test with the existing scene should jump straight from
> `matched=0` to `matched=N (substring=N)` and the per-update `drove Orbit model ...` log line
> starts firing for each fixture.

> **Orbit-imported fixtures now move in lockstep with the control channel by default (v1.0.65).**
> User question after the v1.0.64 focus fix: *"the fixtures that are imported via orbit and not
> over the control channel. Do these have the same IDs now as the control channel fixtures? if
> so can you wire them to exactly the same motion control so they move insync with the control
> channel ones."* Yes -- and the wiring was already there since v1.0.35; it just defaulted off.
>
> **ID match -- same Speckle node id on both sides.** Control-channel fixtures are registered
> under their Speckle node id in `URebusFixtureControlSubsystem::RegisterFixture(NodeId, Actor)`
> (called from `RebusVisualiserSubsystem`'s portal-scene spawn loop with `F.Id` -- the same id
> the portal references in every `SetFixture*` descriptor). On the OrbitConnector side, imported
> scene components are tagged with their glb node-name ancestry, which carries the same Speckle
> node id at one of the tag levels. `RebindOrbitModels` matches them with a plain string-equality
> `OrbitIndex.Find(FixtureId)` lookup -- nothing fancy, no portal contract changes, the two trees
> are addressed by the same canonical id.
>
> **Motion contract -- same matrix, not a parallel recomputation.** In `RefreshMotion` the head
> axis transform is solved exactly once per fixture per frame
> (`RebusMotion::Solve` → `Cumulative[HeadAxisIndex]`) and that EXACT `FTransform` is used for
> both:
>
> ```cpp
> SpotLight->SetRelativeTransform(BeamRestTransform * Head);  // control-channel head + beam
> ...
> DriveOrbitModel(OrbitHead);                                  // Orbit-imported geometry
> ```
>
> So the two render on top of each other and move in perfect lockstep -- they cannot drift
> because they consume the same matrix, not two parallel pan/tilt solves. `DriveOrbitModel`
> applies `CompRestWorld * HeadWorldRest^-1 * HeadWorldNow` per bound component (one matrix
> multiply per component per update, precomputed prefix), so each Orbit mesh follows the
> incremental delta from its imported (rest) pose.
>
> **What v1.0.65 changes.**
>
> 1. `URebusFixtureControlSubsystem::bDriveOrbitModels` default flipped from `false` → `true`.
> 2. `ARebusFixtureActor::bDriveOrbitModel` default flipped from `false` → `true` (defends the
>    1 Hz rebind gap -- the per-actor flag is set true on first match, but defaulting it true
>    means a freshly-spawned fixture starts driving the moment its components are bound, not on
>    whichever later frame `RebindOrbitModels` happens to set it explicitly).
> 3. `URebusSceneSettingsSubsystem` seed for `bDriveOrbitModels` flipped to `true` so the
>    SceneState round-trip reports the new default consistently (otherwise the portal would see
>    the seed as false on first query and could re-disable).
> 4. `RegisterFixture` now kicks an immediate `RebindOrbitModels` when driving is enabled, so a
>    freshly-spawned fixture binds on THE SAME frame instead of waiting up to ~1 s for the
>    periodic rebind timer in `URebusVisualiserSubsystem::Tick`. The periodic rebind now mostly
>    catches the inverse case (Orbit import re-arrives after fixtures already spawned).
> 5. Dropped the "Phase 1 A/B sync test" framing from comments and the `Rebus.DriveOrbitModels`
>    console-command help -- it's no longer an A/B test, it's the default sync. The console
>    command is preserved as the kill-switch for debugging (`Rebus.DriveOrbitModels 0` reverts
>    Orbit components to their imported rest pose so a user can compare the two trees side-by-
>    side if they ever suspect drift).
>
> **What you should see.** Spawn a scene, drag pan/tilt -- the Orbit-imported geometry rotates
> with the control-channel head meshes, identically. The throttled per-update log
> (`Fixture %s: drove Orbit model '%s' pan=%.1f tilt=%.1f headRot=...`) confirms each tick;
> the periodic match summary
> (`Orbit bind: roots=%d taggedComps=%d distinctObjectIds=%d | fixtures matched=%d unmatched=%d`)
> reports which fixtures found their Orbit twin and which didn't (the unmatched-ids list is
> truncated to 12 to keep the line readable). If a fixture stays unmatched, the portal's glb
> export likely dropped the Speckle node id from the imported component tags -- not a code bug,
> a data-pipeline issue.

> **Focus now pulls the beam / gobo in and out of focus (v1.0.64).** User report after the
> v1.0.63 iris+frost fix: *"Focus doesnt do anything. can it pull a beam or gobo in and out of
> focus."* Before v1.0.64 `ApplyFocus` only stored the value in `Focus.Current` for the visual
> pipeline to consume -- nothing read it. The fix wires Focus into BOTH the GoboRT multi-tap
> blur (v1.0.63's gobo softening) AND the SpotLight inner-cone / source-radius softening, so
> the effect is visible whether or not a gobo is loaded.
>
> **Convention: BIPOLAR around 0.5.** `Focus = 0.5` is sharp (matches the
> `ResetAnimatedToDefaults` snap value, so a freshly-spawned fixture is always in focus). Either
> direction from 0.5 progressively defocuses; `Focus = 0` or `Focus = 1` is maximum defocus.
> This mirrors how a real stage moving light's focus knob behaves -- sweeping through the focal
> plane goes from soft → sharp → soft. The portal contract is unchanged
> (`{"type":"SetFixtureFocus", "focus": 0..1, "fadeMs"?}`); only the visual interpretation gains
> teeth.
>
> **What v1.0.64 changes.**
>
> 1. **`OnGoboRTUpdate` blur fold.** `DefocusNorm = clamp(|Focus.Current - 0.5| * 2, 0, 1)`
>    additively combines with `FrostNorm` into a single `BlurNorm` (clamped to 1) that drives
>    the existing v1.0.63 multi-tap pass. Either alone reaches max softening; both together
>    still cap at `BlurNorm = 1` so the tap offset never escapes the gobo's circular boundary.
>    Re-using the same path avoids a second 8-tap draw call.
> 2. **`RecomputeConeAngles` soften fold.** Same `DefocusNorm` adds into the inner-cone /
>    source-radius `SoftenAmount`. Without a gobo, this is what makes "defocus the beam" visible
>    -- the penumbra widens, the inner cone contracts toward the outer, the source disc grows
>    (4× at max combined Frost+defocus). With a gobo, the cookie blur AND the cone softening
>    both apply.
> 3. **`ApplyFocus` redraw kick.** Mirrors `ApplyIris` / `ApplyFrost` from v1.0.63: instant
>    single-shot changes call `RecomputeConeAngles()` AND `GoboRT->UpdateResource()` so the cone
>    and cookie reflect the new focus immediately; fades fall through the `Tick` `bConeAnim`
>    path, which now includes `Focus.Tick()` (previously ticked separately with no down-stream
>    effect).
>
> **Why bipolar instead of monotonic.** A unipolar focus (0 = soft, 1 = sharp) is more natural
> on a generic slider, but stage moving lights physically have a focus position that sweeps
> THROUGH the focal plane -- 0 is "near focus", 1 is "far focus", and the gobo is sharpest
> somewhere in between (where the lens converges the image at the reference throw distance).
> Defaulting to 0.5 = sharp is the conservative interpretation that matches GDTF "Focus 1"
> conventions and gives the user predictable behaviour: sweep focus 0→1 and the gobo goes from
> very soft → sharp at midpoint → very soft. If the portal needs monotonic semantics, the simple
> change is `DefocusNorm = 1.f - Focus.Current` in `OnGoboRTUpdate` + `RecomputeConeAngles` (and
> drop the abs/×2).
>
> **Iris circular crop + Frost gobo blur (v1.0.63).** User report after the v1.0.62 shutter
> fix: *"When we have a gobo in. Iris is zooming instead of circular cropping like an iris
> would. Frost is not working when a gobo is in."* Both effects had the same root cause — the
> cookie LightFunction (M_Light_Master) only exposes `DMX Frost` (which governs the surrounding
> penumbra fall-off, not the gobo-texture sample), and iris was being applied as a SpotLight
> outer-cone pinch (`ResolveOuterHalfDeg` scaled `OuterHalf` by `Lerp(0.4, 1, Iris)`), so:
>
> - **Iris closing didn't crop the gobo, it shrank the cone.** A narrower cone projects the
>   SAME `GoboRT` onto a smaller floor area, so gobo features look BIGGER → exactly what the
>   user saw as "zooming". A real iris is an aperture stop inside the fixture: the gobo image
>   stays the same scale, but the outer rim is occluded so the projected pattern shows inside a
>   smaller circle, surrounded by darkness.
> - **Frost did nothing visible on the gobo.** `DMX Frost` on `M_Light_Master` (verified via
>   asset inspection) modulates penumbra/source-radius softening, not the cookie texture sample,
>   so a sharp stencil stays razor-sharp on the floor regardless of frost.
>
> **What v1.0.63 changes.** The fix bakes BOTH effects directly into the `UCanvasRenderTarget2D`
> (`GoboRT`) we already use for in-plane gobo rotation, so the projected cookie carries the
> iris crop and frost halo whatever cookie material the SpotLight is using.
>
> 1. **`OnGoboRTUpdate` pass 1 (multi-tap frost blur).** The gobo is no longer a single
>    `K2_DrawTexture` call. We do `N = 1 + 8·step(Frost > 0)` taps: one centre tap plus eight
>    ring taps at sub-pixel offsets, each drawn at `1/N` alpha so they additively average. The
>    ring offset scales with `Frost.Current` up to `~2.5%` of the RT side (~13 px on a 512 RT,
>    which projects to a noticeable halo at typical throws). At `Frost=0` only the centre tap
>    runs — identical to the pre-v1.0.63 single draw.
> 2. **`OnGoboRTUpdate` pass 2 (circular iris mask).** A new helper
>    `EnsureIrisMaskTexture(Iris01)` lazily generates a 128×128 `PF_B8G8R8A8` transient texture
>    (`IrisMaskTex`) with an anti-aliased disc whose radius scales with `Iris01`. All four
>    channels (RGBA) are set to the SAME circular alpha value so `BLEND_Modulate` (`Dst *= Src`
>    across all components) zeroes RGB and A in lockstep — `M_Light_Master` samples the cookie's
>    RGB to weight the light function output, so a pure alpha mask would leave the gobo pattern
>    bright in the cropped area. It's debounced — only regenerated when the quantised (`0.01`)
>    iris value changes, so a 1 s iris fade allocates ~100 frames of mask data instead of every
>    frame. The mask is drawn full-RT with `BLEND_Modulate`. Result: inside the iris circle, the
>    gobo passes through unchanged; outside the circle, the RT pixel collapses to (0,0,0,0) so
>    the cookie projects black at that position → SpotLight emits no light at that azimuth in
>    the cone. At `Iris >= 0.999` the mask pass is skipped entirely (fully open, no draw cost).
> 3. **`ResolveOuterHalfDeg` cone-pinch is conditional on `bGoboActive == false`.** When a
>    gobo is live, iris is now handled exclusively by the GoboRT mask — pinching the cone here
>    would DOUBLE-iris the footprint AND re-introduce the "gobo zooms" artefact. Without a
>    gobo, the cone-pinch is preserved (back-compatible) so iris-only still has a visible
>    effect on the lit pool.
> 4. **Redraw kicks.** Iris + frost previously only ran `RecomputeConeAngles` on change. Now
>    `ApplyIris` / `ApplyFrost` also call `GoboRT->UpdateResource()` on instant single-shot
>    changes (`FadeSeconds <= 0`), and `Tick` kicks the same redraw on every `bConeAnim` frame
>    while `bGoboActive` is true. Without these, the cookie stencil would freeze the moment
>    iris or frost changed and only refresh on the next gobo-spin / gobo-selection event.
>
> **Why bake into the RT instead of pushing more material params.** `M_Light_Master` has no
> iris parameter at all (verified via the `DMXFixtures` content inspection done for v1.0.58)
> and `DMX Frost` doesn't touch the cookie texture sample. Authoring our own cookie material
> with circular-mask + blur nodes would require shipping a new `.uasset`; doing it in the RT
> reuses the already-allocated 512×512 `UCanvasRenderTarget2D` and leaves the material binding
> untouched, so the cookie path stays a drop-in replacement for the Epic default. The volumetric
> beam mesh sees no change — it samples the same RT, so the iris-cropped + frost-blurred gobo
> projects through both the cone and the cookie consistently.
>
> **Diagnostics.** No new log lines; the existing gobo-RT redraw cadence implicitly traces the
> iris/frost changes (`OnGoboRTUpdate` already runs on every `UpdateResource()` call). If the
> floor stencil ever lags behind the iris/frost target, watch for missing `Iris.Tick`/`Frost.
> Tick` returning true (would mean `FAnimatedFloat` is being snapped without queuing a tick) or
> for `bGoboActive` being false (would mean iris is going through the cone-pinch back-compat
> path — the gobo state should always force `bGoboActive=true` in `ApplyCurrentGoboToEpicBeam`).

> **Empty-id descriptors now broadcast to current selection (v1.0.62).** User retest of v1.0.61
> with the new diagnostics finally surfaced the actual portal contract: every `SetFixtureShutter`
> message arrives with `id=''` (empty string), `mode` and `rateHz` populated correctly. The
> portal expects per-fixture commands to be applied to the **currently-selected fixtures**, a
> standard lighting-console convention — the v1.0.61 trace line `SetFixtureShutter parsed:
> id='' mode=2(Strobe) [source=number raw=2.00] rate=5.00Hz` followed by `NO FIXTURE FOUND.
> Known ids: [...4 valid ids...]` made the mismatch unambiguous: descriptor was well-formed,
> selection was non-empty, but `FindFixture('')` returned null and we silently bailed.
>
> **What v1.0.62 changes in `HandleControlDescriptor`.** Target resolution now lives at the
> top of the descriptor handler, before any per-type branch:
>
> 1. Try `fixtureId` (canonical) → `id` → `fixture` → `nodeId` for an explicit non-empty id.
>    First non-empty wins; logged source name is captured in `id-src` for the trace line.
> 2. If still empty, fall back to `CurrentSelection` (managed by `SelectFixtures`). All ids
>    in that array are guaranteed to be registered fixtures, so the per-id dispatch loop won't
>    trigger the v1.0.61 `NO FIXTURE FOUND` warning in the broadcast path.
> 3. If still empty (no explicit id AND empty selection), log a single Warning naming the
>    descriptor type and explaining how to fix the payload — then return false so downstream
>    handlers (scene/keepalive) can still try.
>
> Every per-fixture branch (`SetFixtureIntensity`, `SetFixtureColor`, `SetFixturePanTilt`,
> `SetFixtureZoom`, `SetFixtureGobo`, `SetFixtureIris`, `SetFixtureFocus`, `SetFixtureFrost`,
> `SetFixtureColorTemp`, `SetFixtureShutter`, `SetFixtureGoboRotation`, `SetFixtureAnimation
> Rotation`, `SetFixturePrism`, `SetFixtureBeamVolumetrics`) loops over the resolved list:
>
> ```cpp
> for (const FString& T : TargetIds) SetFixtureShutter(T, ModeInt, (float)Rate);
> ```
>
> So a single descriptor "shutter to Strobe@5Hz" hitting a 4-fixture selection now calls
> `ApplyShutter` 4× in the same Tick. `SelectFixtures` is routed FIRST, before id resolution,
> because its descriptor body carries a `fixtureIds[]` array (not a single id) — selection state
> is the input to the broadcast fallback, not a per-fixture command.
>
> **Updated diagnostic chain.** The trace line now exposes the resolution machinery so the
> portal team can immediately see which shape arrived:
>
> ```
> Descriptor type 'SetFixtureShutter'.
> SetFixtureShutter parsed: id-src=(empty -> selection broadcast) targetCount=4 (broadcast=yes) firstId='090be834b3d5ddc368799972e799e570' mode=2(Strobe) [mode-src=number raw=2.00] rate=5.00Hz [rate-src=rateHz] fadeIgnored=0.00
> SetFixtureShutter dispatch: id='090be834...' fixture=BP_RebusFixture_0 mode=2 rate=5.00Hz.
> Fixture 090be834... ApplyShutter: mode=Strobe rate=5.00Hz (changed: mode=yes rate=yes) -- gate now drives SpotLight->SetIntensity ...
> SetFixtureShutter dispatch: id='981d1fad...' fixture=BP_RebusFixture_1 mode=2 rate=5.00Hz.
> Fixture 981d1fad... ApplyShutter: ...
> ...(2 more for the remaining selected fixtures)
> ```
>
> If neither an explicit id nor a non-empty selection is present, a single line:
>
> ```
> SetFixtureShutter: no target -- id field is empty (id-src=(empty -> selection broadcast)) AND current selection is empty. Portal should send {"fixtureId":"<id>"} OR call SelectFixtures first.
> ```
>
> **Field-name aliases now in effect across every per-fixture descriptor.** Combined with the
> v1.0.61 mode/rate flexibility, the visualiser now accepts both the canonical contract and
> common variants without silent failure:
>
> | Field   | Canonical    | Accepted aliases                                                |
> | ------- | ------------ | --------------------------------------------------------------- |
> | id      | `fixtureId`  | `id`, `fixture`, `nodeId` (first non-empty wins; missing/empty → broadcast to `CurrentSelection`) |
> | mode    | `mode` (int) | `mode` as string (`"open" \| "closed" \| "strobe"` + aliases, case-insensitive) -- shutter only |
> | rate    | `rateHz`     | `rate`, `frequency`, `hz`, `freq` -- shutter only               |
> | fade    | `fadeMs`     | (no aliases) — absent ⇒ snap                                    |
>
> Canonical contract still favoured (zero alias-tax, predictable trace lines):
>
> ```json
> { "type": "SetFixtureShutter", "fixtureId": "<id>", "mode": 2, "rateHz": 5 }
> ```
>
> Or for the broadcast-to-selection convention the portal already uses:
>
> ```json
> { "type": "SelectFixtures", "fixtureIds": ["abc", "def"], "primaryFixtureId": "abc" }
> { "type": "SetFixtureShutter", "mode": 2, "rateHz": 5 }
> ```

> **Shutter / strobe diagnostics + field-name flexibility + Strobe-with-0Hz safety net (v1.0.61).**
> User report: "We cannot control strobe, here are the logs." Logs showed `Descriptor type
> 'SetFixtureShutter'` arriving correctly but no shutter effect, with absolutely nothing
> downstream — no parsed values, no fixture-match, no `ApplyShutter` echo. Three concrete bugs
> were hiding behind that silence, plus the diagnostic gap that prevented us from telling which
> of the three was actually firing in any given run.
>
> **Bug 1 — Strobe + `rateHz=0` was a silent no-op.** `ApplyShutter` stored the mode and rate but
> the per-Tick block that advances `ShutterPhase` requires `ShutterRateHz > KINDA_SMALL_NUMBER`:
>
> ```cpp
> if (ShutterMode == ERebusShutterMode::Strobe && ShutterRateHz > KINDA_SMALL_NUMBER)
> {
>     ShutterPhase += DeltaSeconds * ShutterRateHz;
>     ShutterPhase = FMath::Fmod(ShutterPhase, 1.f);
>     RefreshIntensity();
> }
> ```
>
> If the portal sent `{"mode": 2}` without `rateHz`, `ShutterRateHz = 0` and the branch never
> ran. `ShutterPhase` stayed at 0, `Gate = (ShutterPhase < 0.5f) ? 1.f : 0.f = 1`, light stayed
> continuously lit. v1.0.61 detects Strobe + non-positive rate and coerces to a sensible default
> (`RebusDefaultStrobeHz = 5.f`), logging a Warning so the portal team can see they need to send
> `rateHz` alongside `mode=2` for explicit control. Max is still 30Hz.
>
> **Bug 2 — every duplicate descriptor reset `ShutterPhase = 0`.** The user's logs show 4
> identical `SetFixtureShutter` messages arriving in the same millisecond (likely a Pixel
> Streaming duplication or multi-fixture broadcast). Previously `ApplyShutter` unconditionally
> reset `ShutterPhase = 0.f` on every call. With duplicates at ~700ms intervals AND a re-send
> every Tick from a portal that mirrors selection state, `ShutterPhase` was being whacked back
> to 0 faster than Tick could advance it. v1.0.61 only resets phase when **Mode or Rate actually
> changes** — "stay strobing at 5Hz" pushes are now phase-preserving no-ops on the accumulator.
>
> **Bug 3 — silent field-name mismatch.** The handler parsed only `mode` (numeric 0/1/2) and
> `rateHz` (numeric). If the portal used a string `"strobe"` for mode, or a different field
> name for the rate (`rate`, `frequency`, `hz`, `freq`), `TryGetNumber` returned false and the
> values silently fell back to `mode=0 (Open), rateHz=0` — descriptor "succeeded" but did
> nothing. v1.0.61 accepts both, in this precedence:
>
> | Field   | Accepted values                                                          |
> | ------- | ------------------------------------------------------------------------ |
> | `mode`  | `0/1/2` numeric, OR string `"open" \| "closed" \| "strobe"` (also `on`/`off`/`flash`/`pulse`/`true`/`false` aliases, case-insensitive, trimmed) |
> | rate    | `rateHz` (canonical) → `rate` → `frequency` → `hz` → `freq` (first match wins) |
>
> Unrecognised mode strings log a Warning and fall back to Open. Canonical contract still favoured:
>
> ```json
> { "type": "SetFixtureShutter", "id": "<fixtureId>", "mode": 2, "rateHz": 5 }
> ```
>
> **Full diagnostic chain.** Where there used to be only `Descriptor type 'SetFixtureShutter'`,
> we now log every step. Trace at `Log` level:
>
> ```
> Descriptor type 'SetFixtureShutter'.
> SetFixtureShutter parsed: id='ABC' mode=2(Strobe) [source=number raw=2.00] rate=5.00Hz [source=rateHz] fadeIgnored=0.00
> SetFixtureShutter dispatch: id='ABC' fixture=BP_RebusFixture_42 mode=2 rate=5.00Hz.
> Fixture ABC ApplyShutter: mode=Strobe rate=5.00Hz (changed: mode=yes rate=yes) -- gate now drives SpotLight->SetIntensity in RefreshIntensity + EpicBeamMID DMX Dimmer in UpdateEpicBeamParams (cookie inherits transitively).
> ```
>
> When the fixture id doesn't match anything in the `Fixtures` map (the silent failure mode that
> bit us in v1.0.49→v1.0.60), we now log a Warning with a snapshot of every known id:
>
> ```
> SetFixtureShutter id='wrong-id' mode=2 rate=5.00Hz: NO FIXTURE FOUND. Known ids: [ABC, DEF, GHI, JKL].
> ```
>
> When Strobe is requested without a usable rate, a Warning before the defaulting:
>
> ```
> Fixture ABC ApplyShutter(Strobe) with rateHz=0.00 -- defaulting to 5.0Hz so the strobe actually progresses. Portal should send {"mode":2,"rateHz":<1..30>} for explicit control.
> ```
>
> **Why we didn't see this before.** v1.0.56 wired `SetFixtureShutter` correctly for `mode + rateHz`
> exactly as the portal contract was originally agreed. v1.0.59 unified the shutter envelope across
> beam + cookie + SpotLight. v1.0.60 fixed the cookie double-dim. Through all three the path
> assumed the portal would always send a numeric `mode` AND a numeric `rateHz` for strobe — but
> when the portal team tested with the strobe-only payload they had on hand, the silent fallback
> defeated everything. v1.0.61 closes the silence on the visualiser side and is forgiving about
> the field names.

> **Cookie footprint double-dim fix: IES + 1/r² + dimmer now linear (v1.0.60).** User retest of
> v1.0.59: "can we look at the fade intensity of the footprint. Is this following the IES? The
> intensity doesnt match the beam or allow for distance change." Three observations, one root
> cause -- the cookie material was double-dimming the footprint.
>
> **What was actually happening.** A `LightFunctionMaterial` in UE is multiplied with the light's
> per-pixel illumination contribution. The SpotLight's `SetIntensity` (set by `RefreshIntensity`
> to `BaseCandela * Dim * Gate`) already contains the full dimmer + shutter envelope, and
> `SetIntensityUnits(Candelas)` engages physical inverse-square attenuation, while
> `SetIESTexture(prof) + bUseIESBrightness=false` handles the angular distribution. v1.0.59
> additionally pushed `DMX Dimmer = Dim * Gate` into `GoboLightFnMID`, so the per-pixel cookie
> result was:
>
> ```
> Footprint = (BaseCandela * Dim * Gate * IES(angle) * 1/r²) * (pattern * Dim * Gate)
>           =  BaseCandela * Dim² * Gate² * IES * 1/r² * pattern
> ```
>
> while the volumetric cone-mesh beam (`M_Beam_Master`) fades linearly via its own
> `DMX Dimmer = Dim * Gate` push, scaled by the artistic `DMX Max Light Intensity =
> RebusEpicBeamMaxIntensity (2000) * MeshBeamUserScale`. Concretely:
>
> | Dimmer | Beam mesh brightness | Cookie footprint brightness | Ratio |
> | -----: | -------------------: | --------------------------: | ----: |
> |    1.0 |               100 % |                       100 % |  1.00 |
> |    0.5 |                50 % |                        25 % |  0.50 |
> |    0.1 |                10 % |                         1 % |  0.10 |
> |   0.01 |                 1 % |                      0.01 % |  0.01 |
>
> The 1/r² falloff WAS being applied (it's an engine-level feature of `ELightUnits::Candelas`),
> but at low dimmer values the double-dim crushed the pattern to near-black regardless of where
> the floor was. The user read this as "footprint doesn't follow IES / doesn't match the beam /
> doesn't allow for distance change" -- IES was being respected, the cookie was just being
> scaled into invisibility before the IES distribution could be perceived.
>
> **Fix in `UpdateEpicLightFnParams`.** Make the SpotLight the **single source of truth** for
> dimmer + shutter + IES + 1/r² + colour. The cookie is now a pure spatial pattern: `DMX
> Dimmer = 1.0` is forced unconditionally, and the cookie just modulates the pre-attenuated
> per-pixel illumination by the gobo texture. Frost is still pushed live (`DMX Frost =
> Frost.Current`) because frost is a per-pixel material blur, not a light-source-level control:
>
> ```cpp
> GoboLightFnMID->SetScalarParameterValue(TEXT("DMX Dimmer"), 1.f);     // cookie is pure pattern
> GoboLightFnMID->SetScalarParameterValue(TEXT("DMX Frost"),  FrostNorm); // per-pixel blur
> ```
>
> Resulting per-pixel maths after v1.0.60:
>
> ```
> Footprint = BaseCandela * Dim * Gate * IES(angle) * (1/r²) * pattern
>           ↑ linear in Dim    ↑ shutter   ↑ angular  ↑ distance  ↑ gobo shape
> ```
>
> All four signals come from canonical UE engine paths (Candelas-mode SetIntensity, IES texture,
> physical inverse-square, LightFunction multiply). Shutter is still unified across the fixture
> (`SetFixtureShutter` from v1.0.56) -- the cookie inherits Gate **transitively** via the
> SpotLight intensity it modulates, instead of having Gate baked into the material a second time.
>
> **What about the beam-vs-footprint absolute brightness match?** With the double-dim removed,
> both now track the dimmer linearly so the *relative* fade matches. The *absolute* per-pixel
> intensities are different scales by design -- the beam mesh is an artistic emissive
> (`RebusEpicBeamMaxIntensity = 2000.f * MeshBeamUserScale`) tuned for the volumetric shaft
> look, while the footprint is physical candelas from `Profile.Photometrics.LuminousFlux /
> (2π * (1 - cos(FieldAngle/2)))`. If a specific scene needs them closer in absolute brightness,
> `MeshBeamUserScale` (`SetFixtureBeamVolumetrics`) is the per-fixture lever -- everything
> downstream of dimmer × shutter is now correctly proportional.
>
> Diagnostic line: `Fixture <id> gobo cookie params (v1.0.60): cookie is pure spatial pattern
> (DMX Dimmer=1 forced, no Dim/Gate baked into the material); SpotLight->SetIntensity =
> BaseCandela * <dim> * <gate> handles all dimmer + shutter + IES + 1/r^2 falloff. Frost=<f>
> (live).` Enable with `Log LogRebusVisualiser Verbose`. To confirm IES is loaded on a given
> fixture: `Rebus.DumpFixtureLights` prints `IES=<asset>` (or `IES=<none>`) and
> `intensity=<candelas>` -- the cookie now perfectly tracks both.

> **Cookie footprint flashing during fades + unified shutter (v1.0.59).** User retest of v1.0.58:
> "it working but the gobo on the floor is flashing. I think this is to do with Shutter. Can we
> we control shutter as a sperate parameter. The shutter applies to the beam as well as footprint
> as one control." Two distinct bugs were producing the flash, and the shutter control the user
> wanted **already exists** as the `SetFixtureShutter` descriptor — it was just being undermined
> by the bugs and by the v1.0.58 strobe-param push.
>
> **Bug 1 — per-frame `GoboRT->UpdateResource()` clears the RT to transparent every Tick.** The
> v1.0.53 `ApplyCurrentGoboToEpicBeam` unconditionally called `GoboRT->UpdateResource()` so the
> RT always held the latest source-gobo + rotation. That function is called per-frame from
> `UpdateEpicBeamParams` during any fade (dimmer, colour, motion, cone). `UCanvasRenderTarget2D
> ::UpdateResource()` clears to `ClearColor` (transparent) **before** firing `OnCanvasRender
> TargetUpdate` — there's a one-frame gap where the RT holds nothing. The cookie `LightFunction`
> material samples that "blank" frame between the clear and the redraw and the footprint
> projects nothing for one frame; the beam mesh hid the same gap inside translucent surface
> blending (which is why the user only reported the cookie flashing, not the beam). v1.0.59
> adds `LastGoboRTUpdateTex` on the actor and gates the redraw to the case where `CurrentGobo
> Texture` has actually changed since the last call:
>
> ```cpp
> if (GoboRT && CurrentGoboTexture != LastGoboRTUpdateTex)
> {
>     GoboRT->UpdateResource();
>     LastGoboRTUpdateTex = CurrentGoboTexture;
> }
> ```
>
> Reset to `nullptr` in `ClearGoboToOpen` so the next non-Open assignment is correctly detected
> as a change. The Tick spin block (`bGoboSpinActive`) is **unchanged** — it still kicks
> `UpdateResource()` every frame because the RT contents need to change to show the rotation;
> `LastGoboRTUpdateTex` is not touched there because the source texture hasn't changed.
>
> **Bug 2 — `DMX Strobe Open` push made `MF_DMXStrobe` self-modulate the cookie.** v1.0.58 added
> `DMX Strobe Open` / `DMX Strobe Frequency` / `DMX Strobe Disable Burst` pushes on `GoboLightFn
> MID`. Those scalars feed `MF_DMXStrobe` inside `M_Light_Master`, which combines them with an
> internal `MaterialExpressionTime`-driven `MaterialExpressionSine` to **produce** strobe
> oscillation (verified by enumerating the `MF_DMXStrobe.uasset` node graph string table: nodes
> include `MaterialExpressionSine`, `MaterialExpressionTime`, `MaterialExpressionMultiply`,
> `MaterialExpressionIf`, comment "Pulse,strobe,sinewave"). Pushing `Gate = 1` into `DMX Strobe
> Open` while leaving Frequency/Disable Burst at their MID defaults made the material strobe
> independently of our `ShutterMode` state machine. **The fix the user actually saw in v1.0.58
> wasn't `DMX Strobe Open` at all — it was the `DMX Gobo Disk` (clean disc) texture push** in
> `ApplyCurrentGoboToLightFn`. `M_Light_Master`'s `MF_DMXGobo` samples the CLEAN disc, not the
> FROSTED disc; we'd been writing only Frosted from v1.0.49 → v1.0.57, so the cookie sampled
> Epic's stock default texture and showed nothing user-specific. v1.0.58 added the missing
> Clean push and that's what made user gobos visible. The Strobe Open push was a coincidental
> co-change that introduced the flashing.
>
> **v1.0.59 collapses shutter into `DMX Dimmer`** to mirror how the beam already does it. The
> volumetric beam (`EpicBeamMID` / `M_Beam_Master`) has always handled shutter purely via
> `DMX Dimmer = Dim * Gate` and has never flashed — it doesn't push any `DMX Strobe *` params,
> just inherits the MID defaults. `UpdateEpicLightFnParams` now writes only:
>
> ```cpp
> const float Dim = FMath::Clamp(Dimmer.Current, 0.f, 1.f) * Gate;
> const float FrostNorm = FMath::Clamp(Frost.Current, 0.f, 1.f);
> GoboLightFnMID->SetScalarParameterValue(TEXT("DMX Dimmer"), Dim);
> GoboLightFnMID->SetScalarParameterValue(TEXT("DMX Frost"),  FrostNorm);
> ```
>
> The `DMX Strobe Open`, `DMX Strobe Frequency`, `DMX Strobe Disable Burst` pushes are removed.
> Shutter is now a single signed envelope (`Dim * Gate`) propagated identically to (a) `SpotLight
> ->SetIntensity` in `RefreshIntensity`, (b) `EpicBeamMID DMX Dimmer` in `UpdateEpicBeamParams`,
> and (c) `GoboLightFnMID DMX Dimmer` here. All three multiply by the same `Gate` computed in
> the same Tick from `ShutterMode` + `ShutterPhase`, so a portal `SetFixtureShutter` command is
> reflected simultaneously across the beam, the cookie footprint, and the SpotLight's raw output
> as the user requested ("one control" for the whole fixture).
>
> **`SetFixtureShutter` descriptor (already in v1.0.56, documented here).** The portal-facing
> shutter control was wired in `URebusFixtureControlSubsystem::SetFixtureShutter` in v1.0.56;
> this is the formal contract:
>
> ```json
> { "type": "SetFixtureShutter", "id": "<fixtureId>", "mode": 0, "rateHz": 0 }
> ```
>
> | Field    | Type   | Meaning                                                                   |
> | -------- | ------ | ------------------------------------------------------------------------- |
> | `mode`   | int    | `0` = Open (continuous), `1` = Closed (blacked-out), `2` = Strobe         |
> | `rateHz` | number | Strobe rate in Hz when `mode == 2`; clamped to `[0, 30]`; ignored otherwise |
>
> The control is unified: a single descriptor drives the entire fixture (beam + cookie + lens
> disc + spotlight intensity). `mode=1` blacks everything out in lockstep; `mode=2` strobes
> everything in lockstep at `rateHz`. `mode=0` (the default) leaves the fixture continuously lit.
> The same descriptor can be sent in a batch (`SetFixturesBatch`) for synchronised multi-fixture
> shuttering. Diagnostic line: `Fixture <id> gobo cookie params (v1.0.59): dimXgate=<d>
> frost=<f> gate=<g> -- shutter combined into DMX Dimmer (matches beam); DMX Strobe Open/
> Frequency/Disable Burst left at MID defaults to stop MF_DMXStrobe self-strobing the cookie.`
> Enable with `Log LogRebusVisualiser Verbose`.

> **Spotlight footprint gobo: corrected to actual M_Light_Master vocabulary (v1.0.58).** User
> retest of v1.0.57: "we are still seeing no gobos in the footprint of the light on the floor."
> v1.0.57's diagnosis (cookie multiplied by 0 by an unset gating scalar) was the right shape but
> the wrong vocabulary -- we mirrored `M_Beam_Master`'s param list verbatim, but verified by
> unpacking the on-disk `M_Light_Master.uasset` string table at
> `/DMXFixtures/LightFixtures/DMX_Materials/Masters/M_Light_Master` that the actual scalar /
> vector inputs on the light function material are:
>
> ```
> DMX Dimmer
> DMX Frost
> DMX Strobe Open                 (binary gate, set by Strobe_Component in stock Epic BPs)
> DMX Strobe Frequency
> DMX Strobe Disable Burst
> DMX Gobo Disk                    (MF_DMXGobo clean disc texture)
> DMX Gobo Disk Frosted            (MF_DMXGobo frosted disc texture)
> DMX Gobo Num Mask
> DMX Gobo Index
> DMX Gobo Disk Rotation Speed
> Use Gobo                         (StaticSwitchParameter, ON in MI_Light per the existence of
>                                   a separate MI_LightNoGobo override 5441 bytes larger)
> ```
>
> `DMX Color` / `DMX Max Light Intensity` / `DMX Max Light Distance` / `DMX Lens Radius` /
> `DMX Zoom` / `DMX Zoom Normalize` / `DMX Quality Level` -- which v1.0.57 pushed -- DO NOT
> EXIST on `M_Light_Master`. UE silently no-ops `SetScalarParameterValue` on an unknown
> parameter (which is normally a safety feature) so v1.0.57's pushes wrote to nothing and never
> moved the needle, even though the trace logs printed values. Cross-referenced by reading
> Epic's `Strobe_Component`, `Dimmer_Component`, `Frost_Component`, and `Color_Component`
> Blueprint binaries:
>
> | Component        | Pushes to DynamicMaterialBeam | DynamicMaterialLens | DynamicMaterialSpotLight |
> | ---------------- | :---------------------------: | :-----------------: | :----------------------: |
> | Strobe_Component | yes (Open/Freq/Burst)         | yes                 | yes (Open/Freq/Burst)    |
> | Dimmer_Component | yes (Dimmer)                  | yes                 | yes (Dimmer)             |
> | Frost_Component  | yes (Frost)                   | yes                 | yes (Frost)              |
> | Color_Component  | yes (Color)                   | yes                 | **NO** (omitted by design) |
>
> So Epic deliberately does NOT push `DMX Color` at the SpotLight light function -- the cookie
> picks up its tint from the SpotLight's own `SetLightColor`. That matches our `RefreshIntensity`
> path; pushing `DMX Color` on `GoboLightFnMID` was harmless (no-op) but redundant in concept.
>
> **The actual gate** that was killing the cookie all along: **`DMX Strobe Open`**. Epic's
> stock `Strobe_Component` writes it on every shutter-state change, defaulting to 1 (open)
> when not strobing. We never set it, so the MID held the implicit 0 (closed) and
> `M_Light_Master` multiplied its entire output by 0 -- no cookie, regardless of the gobo
> texture / dimmer / atlas / cast-shadows / MegaLights opt-out / reregister machinery being
> correct. This is why every previous diagnostic from v1.0.49 through v1.0.57 showed
> "lightFn=MI_Light tex=<gobo>" set correctly on the proxy and yet no floor pattern projected.
>
> **v1.0.58 fix** in `ARebusFixtureActor::UpdateEpicLightFnParams()`:
> - Drops the `M_Beam_Master`-only pushes (Color / Max Light Intensity / Max Light Distance /
>   Lens Radius / Zoom / Zoom Normalize / Quality Level).
> - Pushes the verified `M_Light_Master` vocabulary: `DMX Dimmer` = live `Dimmer.Current`, `DMX
>   Strobe Open` = our existing `Gate` variable (1 when shutter open, 0 closed, oscillating
>   during strobe), `DMX Strobe Frequency` = 0 (we drive strobe at the SpotLight intensity
>   level, not at the material level), `DMX Strobe Disable Burst` = 0, `DMX Frost` = live
>   `Frost.Current`. Multiplying `DMX Dimmer * DMX Strobe Open` inside the material reproduces
>   Epic's stock cookie-gating arithmetic exactly: cookie brightness now tracks dimmer in
>   lockstep with the SpotLight's own `SetIntensity`, and a closed/strobed shutter blacks out
>   the cookie in the same frame the spotlight goes dark.
> - In `ApplyCurrentGoboToLightFn` also pushes `DMX Gobo Disk` (the clean disc texture) in
>   addition to `DMX Gobo Disk Frosted` -- Epic's stock `GoboWheel_Component` writes BOTH, and
>   the `Use Gobo` static switch in `MI_Light` might route through either, depending on the
>   asset's internal blend (verified Use Gobo is ON; the separate `MI_LightNoGobo` carries a
>   `StaticSwitchParameter Use Gobo=false` override and an extra ~5KB of static shader
>   permutation cache, so the default has Use Gobo on).
>
> Diagnostic line updated: `Fixture <id> gobo cookie params (v1.0.58 corrected): dim=<d>
> strobeOpen=<so> frost=<f> gate=<g> -- M_Light_Master vocabulary (DMX Strobe Open was the
> missing gate that multiplied the cookie by 0 in v1.0.49->v1.0.57).` Surface with
> `Log LogRebusVisualiser Verbose` to confirm. If on the next test the cookie is now visible
> but at the wrong brightness / colour, the per-fixture
> `Rebus.HeroShadowScatter` / `SetLightColor` paths in `RefreshIntensity` are the levers
> -- the cookie is now passing through the same gate the user-visible beam already uses.

> **Spotlight footprint gobo via mirrored M_Light_Master DMX params (v1.0.57).** User report:
> "The Beam which is made from the Epic UE DMX fixture plugin works well with the GOBO sent from
> our portal. The spotlight part of the fixture is not working with GOBOS. We need to take the
> logic for gobos with the epic beam and apply the gobo to the other light element that makes up
> the fixture." Root cause traced end-to-end:
>
> 1. **Epic beam (cone) works** because `UpdateEpicBeamParams` pushes the FULL DMX param set onto
>    `EpicBeamMID` (`MI_Beam` / `M_Beam_Master`): `DMX Color`, `DMX Dimmer`, `DMX Max Light
>    Intensity`, `DMX Max Light Distance`, `DMX Lens Radius`, `DMX Zoom`, `DMX Zoom Normalize`,
>    `DMX Quality Level` -- plus the gobo atlas (`DMX Gobo Disk Frosted` / `Num Mask` / `Index` /
>    `Disk Rotation Speed`) via `ApplyCurrentGoboToEpicBeam`. The beam material samples the gobo
>    THEN multiplies by dimmer × colour × intensity, so all eight scalars contribute to the final
>    visible cone -- and we set every one of them.
> 2. **Spotlight cookie (footprint) was silently dark** because `ApplyCurrentGoboToLightFn` only
>    pushed the **gobo atlas** params onto `GoboLightFnMID` (`MI_Light` / `M_Light_Master`,
>    LightFunction domain). The DMX dimmer / colour / intensity scalars stayed at their MIC
>    defaults of **0** (which a freshly-created MID inherits until a `SetScalarParameterValue` call
>    overrides them). `M_Light_Master` internally multiplies the sampled gobo by `DMX Dimmer ×
>    DMX Color × DMX Max Light Intensity` exactly like the beam, so cookie = gobo_pixel × 0 ×
>    black × 0 = **0**. That's why every diagnostic from v1.0.49 through v1.0.56 showed
>    `lightFn=MI_Light tex=<gobo>` set correctly on the proxy and yet no floor pattern projected.
>    The v1.0.50 MegaLights opt-out + v1.0.51 reregister + v1.0.53 RT spin were all necessary
>    but not sufficient -- they got the cookie texture to the GPU; the gating arithmetic still
>    multiplied it to nothing.
> 3. **Fix.** New `UpdateEpicLightFnParams()` mirrors the FULL `UpdateEpicBeamParams` push onto
>    `GoboLightFnMID` (live colour / dimmer / shutter-gate / intensity / zoom / distance / lens
>    radius / quality / zoom-units) so the cookie inherits identical brightness/colour gating.
>    Wired in two places:
>    - **`UpdateEpicBeamParams` tail-calls `UpdateEpicLightFnParams`** after pushing the beam
>      scalars + gobo. Every dimmer / colour / shutter / zoom change already flows
>      `RefreshIntensity -> RefreshBeamEmissive -> UpdateEpicBeamParams`, so the cookie now
>      tracks live values automatically -- the cone and footprint can't diverge.
>    - **`ApplyCurrentGoboToLightFn` calls `UpdateEpicLightFnParams`** immediately after pushing
>      the gobo atlas, so the FIRST gobo apply (which lazy-MIDs `MI_Light` from disk on demand)
>      primes the scalars in the same frame as the texture -- no one-frame "cookie still dark
>      while we wait for the next param refresh" glitch.
>    - `UpdateEpicLightFnParams` itself early-outs when `GoboLightFnMID` is null (no active gobo,
>      nothing to push, no LightFunctionMaterial assigned).
>
> Diagnostic added: `Fixture <id> gobo cookie params: color=(r,g,b) dim=<d> gate=<g> zoomFullDeg=
> <z> distCm=<l> -- mirrored from EpicBeamMID so M_Light_Master no longer multiplies the cookie
> by 0.` (Verbose-tier; surface with `Log LogRebusVisualiser Verbose`). Together with the existing
> `gobo cookie: lightFn=MI_Light tex=...` line at Log tier, you can confirm both the texture and
> the gating scalars made it onto the MID. The cone+cookie scalar+texture mirroring is the
> closest we can get to Epic stock `BP_MovingHead` behaviour without throwing out our hybrid
> stack (custom Speckle/Orbit bodies, GDTF rig, runtime IES, motion solver) -- everything
> upstream of the materials stays unchanged.
>
> **Potential follow-up (only if testing reports the cookie reads darker than the beam):** the
> floor lighting is `(cookie_pixel × DMX_Color × DMX_Dimmer) × (SpotLightColor × SpotLightIntensity)`,
> so the live dim/colour is multiplied TWICE -- once via the LightFn material, once via the
> SpotLight's own `SetLightColor` + `SetIntensity` in `RefreshIntensity`. At typical operating
> values the perceptual difference is negligible (the gamma curve absorbs the double-multiply
> into a normal-looking pool), but if the user wants strict beam==cookie radiometric parity, the
> minimal change is `UpdateEpicLightFnParams` pushing `Col = white` / `Dim = 1` and letting the
> SpotLight be the sole colour/dim authority. Left as a one-line tweak rather than a default
> because matching Epic's stock M_Light_Master vocabulary is what the user explicitly asked for.

> **Gobo rotation via per-fixture render target (v1.0.53).** User feedback on v1.0.52: "You are
> rotating the gobo around x instead of the z. Can you not just rotate gobo texture so it spins."
> v1.0.52's component-roll approach (rolling the SpotLight around its local +X / emission axis)
> is fully REVERTED. The SpotLight's relative transform is now just `BeamRestTransform * Head`
> again, untouched by gobo state. Instead v1.0.53 spins the TEXTURE itself:
>
> 1. **Per-fixture `UCanvasRenderTarget2D GoboRT`** (512×512, `ClearColor = Transparent`,
>    lazily allocated by `EnsureGoboRT` on first non-Open gobo apply). Bound (instead of
>    `CurrentGoboTexture` directly) as Epic's `DMX Gobo Disk Frosted` texture param on both
>    `EpicBeamMID` (cone) and `GoboLightFnMID` (cookie). RT lives for the actor's lifetime.
>
> 2. **`OnGoboRTUpdate(UCanvas* Canvas, int32 Width, int32 Height)`** is bound to
>    `GoboRT->OnCanvasRenderTargetUpdate` at allocation time (UFUNCTION dynamic delegate). When
>    `GoboRT->UpdateResource()` is called, UE clears the RT to transparent
>    (`bShouldClearRenderTargetOnReceiveUpdate` default true) then fires this callback with a
>    fresh `UCanvas`. We call `Canvas->K2_DrawTexture(CurrentGoboTexture, …, Rotation =
>    GoboAngle, PivotPoint = (0.5, 0.5))` — one quad, centered, largest square that fits the
>    RT, rotated around its centre by the current accumulated angle. Result: a translucent
>    rotated-gobo RT that the cone + cookie materials sample exactly like the source texture.
>
> 3. **Per-tick angle integration**. Tick computes `CombinedSpin = Clamp(gobo + anim, -2, 2)`,
>    advances `GoboAngle += DeltaSeconds * CombinedSpin * RebusGoboMaxRotRateDegPerSec` (360
>    deg/sec at speed=1.0 = 1 rev/sec per wire unit; max ±2 rps when both wires are at full
>    deflection), wraps to `[-360, 360]`, then calls `GoboRT->UpdateResource()` to redraw.
>    Skipped when there's no active gobo (RT param is the parent MI default in that case).
>
> 4. **First-frame correctness**. `EnsureGoboRT` does an immediate `UpdateResource()` on
>    allocation so the initial material-param push isn't a blank RT, and
>    `ApplyCurrentGoboToEpicBeam` also kicks `UpdateResource()` AFTER `EnsureGoboRT` so a NEW
>    gobo replacing an existing one is drawn into the RT BEFORE the param push (otherwise the
>    cone + cookie would project the previous gobo for one frame).
>
> 5. **Open / clear handling**. `ClearGoboToOpen` sets `CurrentGoboTexture = nullptr` then
>    calls `GoboRT->UpdateResource()` once — `OnGoboRTUpdate` early-outs on
>    `CurrentGoboTexture==null` so only the clear-to-transparent step runs, blanking the RT so
>    the next non-Open assignment doesn't briefly flash the previous gobo.
>    `ApplyCurrentGoboToEpicBeam` then reverts the cone MID's texture param to
>    `EpicBeamDefaultGoboTex` (Epic's open frosted disc, snapshotted at `TryBuildEpicBeam`
>    time) and nulls the cookie's `SpotLight->LightFunctionMaterial` (via the tail-called
>    `ApplyCurrentGoboToLightFn` Open branch). RT stays allocated -- cheap to keep around,
>    expensive to recreate on every gobo change.
>
> 6. **Animation wheel** keeps the v1.0.50 fold-into-combined fallback (Epic exposes no
>    animation-disc rotation param; verified v1.0.52). Both wire descriptors drive the same
>    `GoboAngle` integrator → the same RT spin. One-time Warning still fires on first non-zero
>    animation speed so the user knows the cone+cookie spin at the combined rate rather than
>    showing a stacked two-disc effect.
>
> 7. **Sign convention**. Portal sends `+speed = clockwise looking DOWN the beam`. `UCanvas`
>    rotation is a screen-space yaw applied via `FRotator(0, Rotation, 0)`; in UE's screen space
>    (+X right, +Y down) a +yaw rotates the quad clockwise on screen. The cookie projects
>    "with" the texture orientation onto the floor, so the floor pattern rotates clockwise as
>    `GoboAngle` increases (looking down the beam). If empirical testing shows the pattern
>    rotating the wrong way, the only place to flip is the `Rotation` argument in
>    `OnGoboRTUpdate` -- the wire contract, integration, and clamping stay identical.
>
> **`DMX Gobo Disk Rotation Speed = 0` is still pinned** in both `ApplyCurrentGoboToEpicBeam`
> and `ApplyCurrentGoboToLightFn` (the v1.0.52 pinning was correct -- Epic's "Disk Rotation
> Speed" is a U-axis SCROLL through wheel slots, not an image rotation; per the M_Beam_Master
> HLSL `GoboUV.x = GoboUV.x + (Time*GoboScrollingSpeed); GoboUV.x = GoboUV.x / NumGobos`).
>
> **Diagnostic logs**. `Fixture <id> gobo RT allocated 512x512 at <ptr> (src=<tex> GoboAngle=
> 0.0deg)` (one-shot at first non-Open apply). Verbose: `Fixture <id> gobo TEX param: beamMID=
> set lightFnMID=set src=<src> push=<rtName> (RT=ready)` on every cone+cookie apply. Apply
> rotation lines now read `Fixture <id> SetFixtureGoboRotation: wheelIndex=<i> speed=<s>
> (signed[-1,1]) -> per-tick TEXTURE rotation in GoboRT (gobo=<sg> anim=<sa> combined=<sum>
> max=360deg/sec at speed=1) (no material param: Epic's 'DMX Gobo Disk Rotation Speed' is a
> U-scroll) beamMID=<p> lightFnMID=<p> GoboRT=<p>`. To verify the gobo is actually rotating
> live: stage `stat unit` + `r.Streaming.PoolSize` next to your fixture, run
> `SetFixtureGoboRotation{speed:0.5}`, and watch the floor pattern -- the apply line confirms
> the integrator is on, and the per-tick `GoboAngle` advancement is what changes the visible
> pattern (no log spam per redraw to avoid noise; enable `LogRebusVisualiser Verbose` if you
> want per-frame `gobo TEX param:` lines).

> **Gobo rotation spins the SELECTED image, not the wheel slot (v1.0.52).** User feedback on
> v1.0.50: "Gobo rotate, rotates through the various gobos. It's meant to rotate the actual gobo
> selected so it spins clockwise or anti-clockwise." Root cause: `SetFixtureGoboRotation` was
> wired to Epic's `DMX Gobo Disk Rotation Speed` scalar. Re-introspected
> `M_Beam_Master.uasset`, `MF_DMXGobo.uasset`, and `M_Light_Master.uasset` and enumerated their
> full parameter sets via the uasset string tables. Epic exposes only these gobo params:
>
> | Param | Role |
> | --- | --- |
> | `DMX Gobo Disk Frosted` (texture) | the gobo "wheel sheet" texture; in our single-slot atlas mapping this IS the selected gobo image |
> | `DMX Gobo Disk Rotation Speed` (scalar) | NOT an image rotation. HLSL: `GoboUV.x = GoboUV.x + (Time * GoboScrollingSpeed); GoboUV.x = GoboUV.x / NumGobos;` -- a U-axis SCROLL through the wheel slots. With `NumMask=1` this slides the single gobo image horizontally and wraps -- exactly what the user described as "rotates through the various gobos" |
> | `DMX Gobo Index` (scalar) | picks which slot on the wheel to display (`GoboUV.x += GoboIndex/NumGobos`); we always set 0 |
> | `DMX Gobo Num Mask` (scalar) | total slots on the wheel; we always set 1 (single-slot atlas) |
>
> **There is no image-rotation parameter in Epic's stock materials.** The same enumeration of
> `MF_DMXGobo` and `M_Light_Master` returns the same list (no separate `Index Rotation` /
> `Image Rotation` / `Spin` / `Anim Disk Rotation` parameter). Material edits + re-bake are out
> of scope for this fix, so the only path to actually SPIN the selected gobo is at the COMPONENT
> level. The fix:
>
> 1. **`DMX Gobo Disk Rotation Speed` is now pinned to 0** in both `ApplyCurrentGoboToEpicBeam`
>    (cone) and `ApplyCurrentGoboToLightFn` (cookie). This stops the U-scroll cycling regardless
>    of wire speed.
>
> 2. **Tick integrates the combined signed speed into `GoboAngle`** (`deg`, modulo 360):
>    `GoboAngle += DeltaSeconds * (CurrentGoboRotationSpeed + CurrentAnimationWheelSpeed) *
>    RebusGoboMaxRotRateDegPerSec` (default 360 deg/sec at speed=1 = 1 revolution per second per
>    wire). The spin block runs BEFORE the motion-refresh block so the next `RefreshMotion` call
>    picks up the new angle.
>
> 3. **`RefreshMotion` composes a roll around the SpotLight's local +X (its emission axis)** onto
>    its relative transform: `SpotLight->SetRelativeTransform((BeamRestTransform * Head) *
>    FTransform(FQuat(FVector::ForwardVector, FMath::DegreesToRadians(GoboAngle))))`. FTransform
>    composition order: the roll is applied FIRST in SpotLight-local space (rotates the local
>    Y/Z axes around X; X stays X), then the head transform places the rolled frame in
>    fixture-local space. The SpotLight's WORLD emission direction is unchanged (X axis is the
>    axis of roll, so it's invariant) -- only the local frame around the beam is rolled.
>
> 4. **The cookie projection on the lit floor rotates because** the `LightFunctionMaterial`'s UV
>    is sampled in the SpotLight's local-projection space; rolling the local frame around +X
>    rolls the cookie UV → the cookie image rotates on the floor at the integrated angle. The
>    IES roll too, but spotlight IES profiles are typically rotationally symmetric so it's a
>    no-op visually.
>
> 5. **The in-cone gobo rotates because** the Epic beam canvas (`EpicBeamComp`) is PARENTED
>    UNDER the SpotLight (`TryBuildEpicBeam:1195` -- `SetupAttachment(BeamParent=SpotLight)`) with
>    a fixed `FindBetweenNormals(+Z, +X)` relative rotation. When the SpotLight rolls around its
>    +X, the canvas's world transform inherits the roll. `M_Beam_Master`'s GoboUV samples in the
>    canvas's mesh-local transverse plane (`float2 GoboUV = pos.xy`), so the in-cone gobo image
>    rotates with the canvas mesh's local frame. The canvas mesh is rotationally symmetric about
>    its own emission axis (a cone), so the geometry doesn't visibly shift -- only the gobo
>    rotates.
>
> 6. **`Tick` also triggers `RefreshMotion()` when the spin is active even if pan/tilt is
>    stable**, so the rolled angle takes effect tick by tick without needing motion input. When
>    speed returns to 0, `GoboAngle` holds at its last value and no further work is done.
>
> 7. **`SetFixtureAnimationRotation` keeps the v1.0.50 fold-into-combined fallback** -- Epic
>    still has no separate animation-wheel disc parameter, so the animation speed adds to the
>    gobo speed (`CombinedSpin = CurrentGoboRotationSpeed + CurrentAnimationWheelSpeed`,
>    clamped to `[-2, +2]`). One-time Warning still fires on first non-zero animation speed.
>
> **Sign convention unchanged**: `+1.0` = clockwise looking down the beam at full speed (1 rps),
> `-1.0` = counter-clockwise at full speed, `0.0` = stop. Combined gobo+animation maxes at ±2
> wire units (= ±2 rps).
>
> **Per-call log updated** to make the mapping unambiguous:
> `Fixture <id> SetFixtureGoboRotation: wheelIndex=<i> speed=<s> (signed[-1,1]) -> per-tick
> SpotLight roll around emission axis (gobo=<s_gobo> anim=<s_anim> combined=<s_sum>
> max=360deg/sec at speed=1) (no material param: Epic's 'DMX Gobo Disk Rotation Speed' is a
> U-scroll, not an image rotation) beamMID=<p> lightFnMID=<p>`. The cone/cookie apply lines now
> end with `(rotation via component-roll, material wheel-scroll pinned to 0)` so the diagnostic
> trail proves we're no longer touching the wheel-scroll param.

> **Footprint cookie diagnostics + MegaLights reregister + verified-no-aux-lights (v1.0.51).**
> v1.0.50 set `bAllowMegaLights=0` + `MarkRenderStateDirty()` to route the SpotLight through the
> standard deferred path so `LightFunctionMaterial` (the `MI_Light` cookie MID) would actually
> render. The user reported the cookie still wasn't visible on the lit floor pool, suspecting
> competing/duplicate light sources. This release does three things in priority order:
>
> 1. **Verified — no aux lights enter from the Orbit import path.** Walked
>    `OrbitConnector/Source/OrbitConnectorRuntime/Private/OrbitImportSubsystem.cpp`
>    (`SpawnNodeRecursive`). The recursive node walker only acts on `Node.MeshIndex != INDEX_NONE`
>    and constructs `UStaticMeshComponent` only — it never calls
>    `UglTFRuntimeAsset::LoadPunctualLight`. So even if an Orbit-imported glTF carries
>    `KHR_lights_punctual` nodes (some fixture exports do), they are NOT spawned as
>    `UPointLightComponent`/`USpotLightComponent` siblings on the import root, and therefore are
>    not tagged with the fixture's objectId, and therefore are not bound onto the
>    `ARebusFixtureActor` via `BindOrbitComponents`. This rules out the "aux import light washes
>    out the cookie" hypothesis at the architectural level. We still report any sibling light
>    found at runtime as a Warning (see (3)) — if a future code path ever attaches one, we'll see
>    it immediately.
>
> 2. **Stronger MegaLights opt-out via `ReregisterComponent` on the transition.**
>    `bAllowMegaLights` is read by `FLightSceneInfo` at PROXY-CREATION time
>    (`Engine/Source/Runtime/Renderer/Private/LightSceneInfo.cpp:55` —
>    `bAllowMegaLights = InLightSceneInfo->Proxy->AllowMegaLights()`), so the flag must be present
>    on a freshly-created proxy to take effect. `MarkRenderStateDirty()` SHOULD schedule a
>    deferred proxy recreate via `LightComponent`'s render-state lifecycle, but the user's
>    report suggests that in practice the proxy state was stale. v1.0.51 now calls
>    `SpotLight->ReregisterComponent()` specifically on the OFF→ON / ON→OFF transition (NOT every
>    gobo update — that would be wasteful), which guarantees `DestroyRenderState_Concurrent` +
>    `CreateRenderState_Concurrent` and a fresh proxy with the new `bAllowMegaLights`. Cost: a
>    single one-frame light blackout on the toggle. Within-gobo-active updates (texture / rotation
>    changes while the cookie is already on) still use the lightweight `MarkRenderStateDirty()`.
>
> 3. **`Rebus.DumpFixtureLights` console command (always-on diagnostic).** Dumps to
>    `LogRebusVisualiser` for every fixture in every game/PIE/editor world: SpotLight properties
>    (`visible / intensity / units / attenRadius / inner+outerCone / castShadows /
>    bCastVolumetricShadow / bAllowMegaLights / LightFunctionMaterial path / IESTexture / mobility`),
>    ANY sibling `ULightComponent` on the actor (Warning level — these are the duplication
>    smoking-gun), the bound Orbit components (count, and any that are `ULightComponent` — flagged
>    as Warning), and total `USceneComponent` count. World-level header line lists competing scene
>    lights (sky / directional, anything not on a fixture) and reports the relevant CVars:
>    `r.SupportLightFunctions`, `r.LightFunctionAtlas.Enabled`, `r.MegaLights.Allow`,
>    `r.MegaLights.LightFunctions`, `r.MegaLights.Volume`. Run after setting a gobo (`SetFixtureGobo`
>    with a real `index`) and paste the output — we can tell at a glance whether the proxy
>    state is what we set, whether a sibling light is leaking, and which scalability CVar is in
>    the way.
>
> Also added: a **next-tick verification log** (`FTimerManager::SetTimerForNextTick`) inside
> `ApplyCurrentGoboToLightFn` that re-reads the SpotLight one frame after the
> reregister/markdirty and emits:
> `Fixture <id> cookie NEXT-TICK verify: bAllowMegaLights=%d castShadows=%d
> castVolumetricShadow=%d intensity=%.1f units=%d attenRadius=%.0f outerCone=%.1f LightFn=%s
> IES=%s`. The component-thread value above prints the GAME-thread state; this prints what the
> NEXT tick sees (after the deferred render-state work has run on the render thread). The
> existing apply-cookie log now ends with `REREGISTERED` (transition) or
> `MarkRenderStateDirty` (within-gobo update) instead of the v1.0.50 unconditional `toggled for
> light function`.
>
> **What to look for in the output.** If `bAllowMegaLights=0` on the NEXT-TICK line and
> `LightFn=/Game/.../MI_Light` and `castShadows=1`, the SpotLight is correctly set up — any
> remaining cookie-invisible symptom is either (a) a competing scene light (check the world's
> competing-lights count and dirlight/sky values), (b) a SIBLING LIGHT Warning on the fixture
> (regression — paste it back), (c) `r.SupportLightFunctions=0` (scalability low), or (d) the
> floor material doesn't receive standard deferred lighting. None of those are this plugin's
> fault and we have direct evidence pointing at the right place.

> **Footprint gobo via MegaLights opt-out + gobo/animation-wheel rotation wire (v1.0.50).** Two
> deliverables:
>
> 1. **Lit-pool cookie now actually projects.** v1.0.49 MID'd `MI_Light` and assigned it to
>    `SpotLight->LightFunctionMaterial`, but the cookie still wasn't visible on the floor. Root
>    cause: every fixture light is `bAllowMegaLights=1` (asserted in `BuildSpotLight`), and
>    MegaLights in UE 5.7 renders light functions **only** through `LightFunctionAtlas` — gated
>    by `r.MegaLights.LightFunctions` AND `FMaterial::MaterialIsLightFunctionAtlasCompatible`
>    (engine source: `Engine/Source/Runtime/Renderer/Private/LightFunctionAtlas.cpp` lines
>    218–490 + `MegaLights.cpp` line 172 / 1457). `M_Light_Master`'s `MF_DMXGobo` (runtime UV
>    rotation + texture sampling) is NOT atlas-compatible, so the cookie was silently dropped
>    even though MID creation succeeded. Fix: `ApplyCurrentGoboToLightFn` now sets
>    `SpotLight->bAllowMegaLights = 0` + `MarkRenderStateDirty()` while a gobo is active so the
>    light routes through the standard deferred path, which renders `LightFunctionMaterial`
>    directly. Restored to `1` on Open/clear. Trade-off: that light loses MegaLights' clustering
>    perf while a gobo is up — acceptable; gobo lights are typically hero fixtures.
>
> 2. **`SetFixtureGoboRotation` + new `SetFixtureAnimationRotation` wire descriptors.** Both are
>    per-fixture signed normalised speeds in `[-1, 1]` (clamped); sign = direction (+ CW looking
>    down the beam, − CCW), 0 = stop. `SetFixtureGoboRotation` adds an optional `wheelIndex`
>    field (0-based into the full `wheels[]`, matches `RegisterFixtureGobos`; informational/logged
>    today since the actor pushes one rotation to Epic's single `DMX Gobo Disk Rotation Speed`).
>    `SetFixtureAnimationRotation` is new; Epic's reference materials don't model a separate
>    animation-wheel disc, so the visualiser folds it into the same MID rotation as a best-effort
>    fallback (combined = gobo + anim; a Warning fires once per fixture on first non-zero apply
>    so the user knows the cone+pool won't show a stacked two-disc effect). Both push to BOTH the
>    cone MID (`EpicBeamMID`) AND the cookie MID (`GoboLightFnMID`) so the in-cone gobo and the
>    lit-pool gobo always rotate together. `UpdateEpicBeamParams` already re-applies via
>    `ApplyCurrentGoboToEpicBeam`, so beam (re)builds, zoom changes, and re-aims don't drop the
>    rotation. JSON examples + sign/unit contract are in the README §"Gobo / animation-wheel
>    rotation wire" bullet.
>
> **Diagnostics.** Existing gobo pipeline lines extended with `bAllowMegaLights=<new> (was <prev>,
> toggled for light function)` on every cookie apply/clear and per-component rotation values
> (`gobo=… anim=… combined=…`) on every cone/cookie push. Two new always-on lines per fixture:
> `SetFixtureGoboRotation: wheelIndex=… speed=… -> combined=… (gobo=… anim=…) beamMID=… lightFnMID=…`
> and `SetFixtureAnimationRotation: speed=… -> combined=… (gobo=… anim=…) …`. The Warning on
> first non-zero animation speed: `SetFixtureAnimationRotation: speed=… -- Epic M_Beam_Master has
> no animation-wheel param, folding into DMX Gobo Disk Rotation Speed as a best-effort fallback…`

> **Gobo Open-slot clear + lit-pool cookie (v1.0.49).** Two follow-ups to v1.0.48:
>
> 1. **Open slot** — picking the no-gobo position used to leave the LAST gobo stuck in the cone.
>    Root cause: the v1.0.48 finalizer dropped any inline entry with empty bytes AND empty url (so
>    Open entries disappeared from the cache), then `AssignGobo` fell to `FetchAndAssignGobo`,
>    which on a missing profile URL only nulled the legacy (always-null) `GoboMID`'s light
>    function — the Epic cone's `DMX Gobo Disk Frosted` was never reverted. Fix: detect Open from
>    `slotName/name` via the new `ARebusFixtureActor::IsOpenSlotName` (recognises
>    `"Open"/"None"/"Empty"/"Clear"/"No Gobo"/"NoGobo"/"Open Hole"/"OpenHole"/"Off"/"0"`,
>    case-insensitive, trimmed exact match), KEEP Open entries in the finalize cache with a new
>    `bIsOpen=true` flag, and route every clear path (`SetFixtureGobo(!bHasIndex)`, inline Open
>    entry, inline empty entry, missing profile URL, missing profile wheel) through a single
>    `ClearGoboToOpen(reason)` helper that drops `CurrentGoboTexture`, reverts the Epic beam MID to
>    its MI parent default (Epic's open frosted disc), nulls the SpotLight cookie, clears
>    `bGoboActive`, and re-asserts `RefreshBeamShadowMode`. The finalize log now reports
>    `isOpen=1` so Open detection is provable per slot.
>
> 2. **Lit-pool cookie** — the gobo was visible in the cone (v1.0.48) but not projected on the lit
>    pool. Pre-v1.0.48 the legacy `GoboMID` (declared but never instantiated, no `M_RebusGobo`
>    asset existed) silently no-oped the cookie path. Fix: instantiate Epic's **`MI_Light`** (MID
>    of `M_Light_Master`, `MD_LightFunction` domain, samples the SAME `MF_DMXGobo` material
>    function as `M_Beam_Master`) lazily on first gobo apply and assign it as
>    `SpotLight->LightFunctionMaterial`. A new `ApplyCurrentGoboToLightFn` pushes the SAME
>    single-cell atlas params (`DMX Gobo Disk Frosted` + `Num Mask=1` + `Index=0` +
>    `Disk Rotation Speed`) so one decoded texture drives both the cone and the cookie. On Open
>    the cookie is set to `nullptr` (a transparent light function would dim the lit pool — null is
>    the true "no gobo"). `RefreshBeamShadowMode` now ORs `bGoboActive` into
>    `SpotLight->SetCastShadows(…)` so every fixture with an active gobo gets shadow-casting on
>    (a SpotLight light function only projects when the light is also casting regular shadows —
>    it's sampled via the shadow render target), regardless of hero-beam status. Cleared back when
>    the gobo clears.
>
> **Diagnostics.** New cookie + Open lines listed in the §"Gobo pipeline diagnostics" recipe —
> twelve lines total now cover wire arrival → finalize+isOpen → SetFixtureGobo → Open-detect →
> clear → AssignGobo source → decode → cookie MID lazy-create → cookie apply (with `castShadows`
> reported) → cookie clear → URL fetch fallback. The first missing or off line identifies the
> failing link.

> **Gobos through Epic's beam + full pipeline diagnostics (v1.0.48).** Two co-existing breaks made
> gobos invisible since the v1.0.43 Epic beam swap. (1) `GoboMID` (the legacy SpotLight light-function
> MID) was declared but **never instantiated** — `M_RebusGobo` doesn't exist in content, so
> `ApplyGoboTextureFromBytes`'s `if (Tex && GoboMID)` always fell through and the projection path
> silently no-oped from day one. (2) Since v1.0.43 the **visible cone is Epic's `M_Beam_Master`**,
> which exposes first-class gobo params (`DMX Gobo Disk Frosted` texture + `DMX Gobo Num Mask`,
> `DMX Gobo Index`, `DMX Gobo Disk Rotation Speed`) — but we wrote NONE of them. Net effect: the gobo
> rendered in zero places.
>
> **Fix.** `ApplyGoboTextureFromBytes` decodes the slot image (`FImageUtils::ImportBufferAsTexture2D`,
> auto-detects PNG/JPEG/etc) into `CurrentGoboTexture` (`UTexture2D` cached on the actor) and calls
> `ApplyCurrentGoboToEpicBeam`, which pushes onto `EpicBeamMID`:
> ```
> DMX Gobo Disk Frosted     = CurrentGoboTexture  (or EpicBeamDefaultGoboTex on clear)
> DMX Gobo Num Mask         = 1                    (single-cell atlas == our whole texture)
> DMX Gobo Index            = 0                    (cell 0 of 1)
> DMX Gobo Disk Rotation Speed = CurrentGoboRotationSpeed
> ```
> `TryBuildEpicBeam` snapshots the MI parent's default `DMX Gobo Disk Frosted` into
> `EpicBeamDefaultGoboTex` once, so a `SetFixtureGobo(clear)` reverts to Epic's open disc instead of
> leaving the last-picked image stuck. `UpdateEpicBeamParams` calls `ApplyCurrentGoboToEpicBeam` on
> every refresh so beam (re)builds, zoom changes, and re-aims don't drop the live gobo.
> `ApplyGoboRotation(speed)` writes the rotation directly to the MID *and* caches it in
> `CurrentGoboRotationSpeed`. The legacy light-function write is preserved (guarded) so a future
> `M_RebusGobo` content asset would light up automatically — today it stays a no-op.
>
> **Diagnostics.** Always-on `LogRebusVisualiser` lines at every link of the pipeline
> (`RegisterFixtureGobos chunk N/M`, per-slot `finalized: …`, `complete: …`, per-fixture
> `SetFixtureGobo:`, `AssignGobo: source=…`, `gobo decode OK: → texture(W×H)`). The README's
> §"Gobo pipeline diagnostics" bullet lists each line and the failure mode each missing line implies,
> so the user can run once with the editor's Output Log filtered to `LogRebusVisualiser` and identify
> the broken link top-to-bottom.

> **Visible beam shadow gaps (v1.0.47).** Epic's `M_Beam_Master` is a pure unshadowed additive
> raymarch (no `DistanceField`/VSM/`SceneTexture` sampling — verified by inspection), so the truss
> "gap" shafts inside the cone come **exclusively** from the SpotLight's VSM-shadowed fog
> scattering coincident with the cone (our v1.0.36 hero-beam hybrid). Three changes pair this with
> the ~2000-candela Epic cone so the gaps actually read:
> - **`RebusHeroShadowScatter` lifted 0.8 → 4.0** (default), now CVar-backed and live-tunable via
>   **`Rebus.HeroShadowScatter <float>`**. The OnChanged sink walks every live `ARebusFixtureActor`
>   and re-applies `RefreshBeamShadowMode()` so a new value takes effect without restart.
> - `RefreshBeamShadowMode` now also enables **`SpotLight->SetCastShadows(true)`** on hero beams.
>   `bCastVolumetricShadow` only carves the fog when the light is also casting regular shadows —
>   without this, the flag was a no-op. (Cleared back to `false` when the beam isn't a hero.)
> - **`Rebus.MeshBeams [0|1]`** (default 1) — live A/B toggle that hides the Epic canvas on every
>   fixture, so only the SpotLight's shadowed-fog beam renders and the truss shafts are obvious.
>   Routes through the existing `URebusSceneSettingsSubsystem::SetMeshBeamsEnabled` path (same
>   plumbing as the `SetSceneProperty bMeshBeams` wire toggle — they don't fight).
>
> **Diagnostics.** Two log lines tell you whether shadow gaps are blocked upstream:
> - Per-call: `Fixture %s SetFixtureBeamVolumetrics: intensity=… castVolumetricShadow=… -> bWantsVolumetricShadow=… bGrantedShadowHero=… (heroBudget=N/6, activeFogScatter=…, Rebus.HeroShadowScatter=…)`
> - Per spawn batch (in `URebusVisualiserSubsystem` after the `Spawned %d fixtures` line):
>   `Spawn batch shadow budget: spawned=… wantsShadow=… grantedHero=N/6 (Rebus.HeroShadowScatter=…)`.
>   If `wantsShadow=0` the portal isn't sending `castVolumetricShadow=true`. If `grantedHero<wantsShadow`,
>   the hero budget (`RebusMaxShadowFogBeams = 6`) is filtering some beams out.
>
> **How to see shadow gaps (recipe):**
> 1. Send `castVolumetricShadow=true` for the fixtures you want shadowed (via `SetFixtureBeamVolumetrics`).
> 2. Watch the spawn-batch log for `grantedHero≥1`. If it's 0 either the wire flag isn't true or all
>    six budget slots are already claimed.
> 3. Tune `Rebus.HeroShadowScatter` upward if the Epic cone overwhelms the gaps (try 4–8); downward
>    if the fog itself reads too bright.
> 4. `Rebus.MeshBeams 0` to confirm the shadowed-fog beam in isolation; `Rebus.MeshBeams 1` to restore.
> 5. The existing `r.VolumetricFog.GridPixelSize=4` + `HistoryWeight=0.95` (v1.0.37) give crisp
>    enough froxels; if gap edges look soft, try `r.VolumetricFog.GridPixelSize 2` at the console
>    (cost goes up).

> **Epic beam emission axis (v1.0.46).** v1.0.45 inferred the canvas's emission axis as **−Z** from
> the vertex extent (`SM_Beam_RM` geometry spans `Z 0..−1`), but with that sign the beam emitted
> **180° out the back** of the lens while pan/tilt still tracked the head correctly. In practice
> `M_Beam_Master` raymarches along canvas-local **+Z** — the pivot/apex is the `Z=−1` end and the tube
> extends downstream toward `+Z`. v1.0.46 flips `RebusEpicBeamLocalEmission` from `(0,0,−1)` to
> `(0,0,+1)` so the fixed relative `+Z → +X` mapping aims the beam through the lens. The existing
> v1.0.45 alignment log still reads `dot=1.000` (it measures the *configured* emission axis, which
> now matches both the math and the rendered output); the visible change is that the beam exits
> forward through the lens instead of backwards.

### `RenderQuality` scene property (runtime tiers)

Push `SetSceneProperty name="RenderQuality" value="<tier>"` (case-insensitive; unknown values
fall back to `live`). Each tier re-applies **only** the MegaLights volume grid + sample count via
a console override (and re-asserts `Allow`/`Volume`); it does **not** touch `r.VolumetricFog.*`:

| Tier | `NumSamplesPerPixel` | `Volume.GridPixelSize` | `Volume.GridSizeZ` | Use |
| --- | --- | --- | --- | --- |
| **`live`** *(default)* | 2 | 8 | 64 | lightest — live previs streaming |
| `previs` | 4 | 4 | 128 | the "start here" baseline |
| `final` | 8 | 2 | 192 | heavy — final renders |

`live` is the **runtime default**: it is seeded in `URebusSceneSettingsSubsystem::Initialize`
(overriding the MegaLights volume `[SystemSettings]` baseline for the live stream) and stored in
`SceneState`, so the portal's control hydrates to `live` and every tier switch is logged. All
tiers also re-assert `r.MegaLights.Allow=1` + `r.MegaLights.Volume=1`. The `r.VolumetricFog.*`
froxel grid is untouched by tier switches and remains at the fixed defaults above.

### Volumetric fog + per-fixture beam scattering defaults

The **ExponentialHeightFog** component is configured for haze/beam visibility, both at authoring
time (`build_rebus_base_level.py`) and defensively on fresh spawn (`EnsureSceneEnvironment`):

- `bEnableVolumetricFog = true` (`SetVolumetricFog`)
- `VolumetricFogDistance = 35000` cm (`SetVolumetricFogDistance`)
- `VolumetricFogExtinctionScale = 0.3` (`SetVolumetricFogExtinctionScale`)
- `VolumetricFogScatteringDistribution = 0.4` (`SetVolumetricFogScatteringDistribution`)

Existing fog density/colour scene properties (`FogDensity`, `InscatteringColor`,
`bVolumetricFog`, `VolumetricScatteringDistribution`, `VolumetricExtinctionScale`, ...) still
drive the same component and can override these at runtime.

Each per-fixture `USpotLightComponent` (`BuildSpotLight`) gets:

- `VolumetricScatteringIntensity = 0` while the **hybrid cone-mesh beam** is on (the default,
  §8.4a) — the mesh shaft is the visible beam, so this light's fog scattering is suppressed to
  avoid a competing noisy froxel beam. The fog value (2.5) is restored if `bMeshBeams` is toggled
  off. **Exception (Phase 2)**: a hero shadow-casting beam re-enables a modest scattering (1.5) +
  `Cast Volumetric Shadow` so the native VSM fog carves light-blocking truss gaps (§8.4a).
- `bAllowMegaLights = true` — opts the light into MegaLights (5.7's per-light flag; defaults
  true, asserted so the project-level `r.MegaLights.Allow` governs the whole rig).
- **Hero-beam volumetric-shadow cap**: `SetCastVolumetricShadow(true)` for only the **first 8**
  spotlights created per spawn batch (`RebusMaxVolumetricShadowBeams`); the rest still scatter
  but skip the volumetric shadow pass. The session subsystem resets the budget
  (`ARebusFixtureActor::ResetVolumetricShadowBudget`) before every (re)spawn, so each fresh
  scene gets its own 8 hero beams. (With the mesh beam on, scattering is 0 so the hero-beam
  volumetric-shadow pass is effectively idle; it applies to the fog beam when `bMeshBeams` is off.)
- `SetFixtureBeamVolumetrics` (`ApplyBeamVolumetrics`) is **re-pointed (§8.4a)**: it now tunes the
  **mesh beam** intensity (a multiplier on `BeamIntensity`) rather than the fog scattering. The
  same value is stored so a `bMeshBeams=false` toggle restores an equivalent fog beam; the
  `castVolumetricShadow` flag (Phase 2) opts the fixture into the **native VSM fog volumetric-shadow
  hybrid** for light-blocking truss gaps, gated by a hero budget (`RebusMaxShadowFogBeams = 6`).

> **Caveat:** UE 5.7 MegaLights + volumetric fog can show artefacts with some sky / height-fog
> and GPU/driver combinations (flicker, sample noise, banding in the fog volume). The tiers
> exist to dial cost vs quality — drop to `live` if the live stream shows noise, raise to
> `final` for clean stills.

### Moving-head parity: the `profile` + `meshes` the portal must push

The plugin already parses and drives the full GDTF rig — to make the **physical heads move**
(not just the beam), the portal must include each fixture's **`motionRig`** *and* its
**`meshes`** in the push. Field names below are exactly what `RebusJson::ParseFixtureProfile`
/ `ParseMeshBundle` read; unknown fields are ignored, missing optionals stay unset.

**Coordinate conventions (do not mix these up):**
- `scene.fixtures[].matrixZUpMeters` — **RH Z-up metres**, column-major (or row-major when
  `matrixSource == "transform-row"`). This is the only Z-up payload.
- `motionRig` pivots/axes/`pivotOffset`, mesh `vertices`, and beam vectors /
  `worldMatrixMeters` — **RH Y-up metres**. (Most common mistake: the rig vectors are Y-up.)

```jsonc
// profiles["<libraryFixtureId>"]  — identical to GET /api/ue/fixtures/{libraryId}
{
  "schema": "rebus-ue-fixture/v5",
  "id": "<libraryFixtureId>",            // must equal scene.fixtures[].fixtureId
  "manufacturer": "...", "fixtureName": "...",
  "dimensions": { "x": .., "y": .., "z": .. },        // metres, optional

  "motionRig": {                                       // REQUIRED for head/yoke motion
    "pivotOffset": { "x":0, "y":0, "z":0 },            // RH Y-up metres, optional
    "axes": [
      { "kind": "pan",                                 // "pan" | "tilt" | other
        "pivot": { "x":0, "y":0,   "z":0 },            // RH Y-up metres, fixture-local
        "axis":  { "x":0, "y":1,   "z":0 },            // RH Y-up unit direction
        "minDeg": -270, "maxDeg": 270, "defaultDeg": 0,
        "geometryName": "yoke", "parentGeometryName": "", // empty parent = base axis
        "affectedGeometryNames": ["yoke","head"] },     // links to mesh geometryName/modelName
      { "kind": "tilt",
        "pivot": { "x":0, "y":0.2, "z":0 },
        "axis":  { "x":1, "y":0,   "z":0 },
        "minDeg": -135, "maxDeg": 135, "defaultDeg": 0,
        "geometryName": "head", "parentGeometryName": "yoke", // tilt is a CHILD of pan
        "affectedGeometryNames": ["head"] }
    ]
  },

  "fixtureParts": [                                      // optional; gives the beam its aim (§7.7)
    { "name": "Beam", "type": "Beam",
      "worldMatrixMeters": [ /* 16, RH Y-up */ ],
      "beamDirectionWorld": { "x":0, "y":-1, "z":0 },    // RH Y-up; -Y = points down
      "beamUpWorld":        { "x":0, "y":0,  "z":1 } }
  ],

  "photometrics": { "luminousFlux":.., "beamAngle":.., "fieldAngle":.., "colorTemperature":.., "cri":.., "hasIesProfile":true,
                    "lensDiameter":0.18 },   // v6 additive (metres); diameter of the luminous opening → lens-flare disc (§8.3a)
  "zoom":   { "minDeg":.., "maxDeg":.. },
  "source": { "radiusMeters":.., "diameterMeters":.. },
  "wheels": [ { "name":"Gobo 1", "kind":"gobo", "slots":[ { "name":"..","color":"..","imageUrl":".." } ] } ],
  "iesProfiles": [ { "zoomDmx":0, "zoomAngleDeg":.., "beamAngleDeg":.., "fieldAngleDeg":.., "iesUrl":".." } ]
}
```

```jsonc
// meshes["<libraryFixtureId>"]  — identical to GET /api/ue/fixtures/{id}/meshes
{
  "version": 1,
  "meshes": [
    { "name": "head", "geometryName": "head", "modelName": "...",
      "vertices": [ x,y,z,  x,y,z, ... ],   // metres, engine Y-up
      "faces":    [ 0, i0,i1,i2,  1, j0,j1,j2,j3, ... ] }  // 0=tri, 1=quad, else explicit count
  ]
}
```

**Axis identity field names:** each axis's identity/parent link is read from **either**
`nodeName`/`parentNodeName` **or** `geometryName`/`parentGeometryName` (the portal sends the
latter). The explicit `nodeName`/`parentNodeName` win when both are present. These names are
matched (`parentGeometryName` → `geometryName`) to resolve the parent-first axis order and the
tilt-under-pan compensation, so the head's tilt chains under the pan. `affectedGeometryNames`
remains the separate mesh→axis link field (unchanged).

**The linking rule that makes geometry move:** a mesh is bucketed onto a motion axis when its
`geometryName` *or* `modelName` (compared case-insensitively) appears in that axis's
`affectedGeometryNames`. The **deepest** matching axis wins; a mesh that matches nothing stays
on the static base; the beam/head tracks the deepest axis automatically. So the portal must
keep `affectedGeometryNames` consistent with the mesh `geometryName`s it pushes — otherwise the
geometry parents to the base and won't move even though the beam does.

> Watch message size: a full mesh bundle can be large. For big fixtures push each profile (with
> its meshes) via `RegisterFixtureProfile` first, then a final `LoadScene` carrying just `scene`.

## Compile-time touch-points to verify (Pixel Streaming 2)

PS2's C++ surface shifted across 5.5→5.7 and was not compiler-checked on the authoring
machine (no local UE). If the module fails to compile, these are the only spots to adjust —
all isolated to `RebusDataChannel.cpp` + the `Build.cs` module list:

- **Module names** in `RebusVisualiser.Build.cs`: `PixelStreaming2`, `PixelStreaming2Core`,
  `PixelStreaming2Input`. Some 5.7 layouts fold Core/Input differently.
- **Streamer discovery**: `IPixelStreaming2Module::Get()` / `IsAvailable()`,
  `FindStreamer(Id)`, `GetDefaultStreamerID()`.
- **Input handler**: `IPixelStreaming2Streamer::GetInputHandler()` (assumed `TWeakPtr` →
  `.Pin()`), `IPixelStreaming2InputHandler::RegisterMessageHandler("UIInteraction", Fn)` with
  `Fn = TFunction<void(FString SourceId, FMemoryReader)>`.
- **Send**: `IPixelStreaming2Streamer::SendAllPlayerMessage(TEXT("Response"), Json)` — the PS2
  frontend delivers `Response`-typed messages to the portal's response listener.

The migration reference: <https://github.com/EpicGamesExt/PixelStreamingInfrastructure/blob/master/Docs/pixel-streaming-2-migration-guide.md>.

## Known best-effort areas

- **Runtime IES** (`RebusIes.cpp`) uses `FIESConverter` (Engine `IESConverter.h`). With editor
  data it uses the `Source` path; cooked builds fill platform data directly. If a shipping
  build can't load IES at runtime, pre-bake the profiles or fall back to the synthesized cone
  (already the default when no IES exists).
- **Gobo projection** (v1.0.48 cone, v1.0.49 cookie). One decoded texture drives both the visible
  beam cone (Epic's `M_Beam_Master` via `MI_Beam`) AND the SpotLight cookie projected on the lit
  floor (Epic's `M_Light_Master` via `MI_Light` — `MD_LightFunction` domain). Both materials sample
  the same `MF_DMXGobo` material function, so a single set of texture + atlas + rotation params
  fans out to both surfaces.
  - `RebusFixtureActor::ApplyGoboTextureFromBytes` decodes the inline (or fetched) wheel image to a
    `UTexture2D`, caches it as `CurrentGoboTexture`, latches `bGoboActive=true`, and calls
    `ApplyCurrentGoboToEpicBeam` which:
    1. Pushes `DMX Gobo Disk Frosted = <our texture>`, `DMX Gobo Num Mask = 1`, `DMX Gobo Index = 0`,
       `DMX Gobo Disk Rotation Speed = ApplyGoboRotation(speed)` onto `EpicBeamMID` (the cone).
    2. Tail-calls `ApplyCurrentGoboToLightFn`, which lazily MIDs `MI_Light` on first use and pushes
       the same four params, then assigns `SpotLight->LightFunctionMaterial = GoboLightFnMID` so the
       cookie projects identically.
  - `RefreshBeamShadowMode` ORs `bGoboActive` into `SpotLight->SetCastShadows(…)`. A SpotLight light
    function only renders when the light is also casting regular shadows (it's sampled via the
    shadow render target), so every fixture with an active gobo gets `CastShadows=true` regardless
    of hero-beam status. Cleared back when the gobo clears.
  - **MegaLights opt-out while a gobo is active (v1.0.50).** Every fixture light is normally
    `bAllowMegaLights=1` (asserted in `BuildSpotLight`). MegaLights in UE 5.7 renders light
    functions ONLY through `LightFunctionAtlas` (gated by `r.MegaLights.LightFunctions` AND
    `FMaterial::MaterialIsLightFunctionAtlasCompatible` — see
    `Engine/Source/Runtime/Renderer/Private/LightFunctionAtlas.cpp` lines 218–490). Epic's
    `M_Light_Master` uses `MF_DMXGobo`'s runtime UV rotation + texture sampling, which is **not**
    atlas-compatible — so the cookie pre-v1.0.50 was silently dropped even though `MI_Light` MID
    creation + `SetLightFunctionMaterial` succeeded. Fix: `ApplyCurrentGoboToLightFn` now sets
    `SpotLight->bAllowMegaLights=0` + `MarkRenderStateDirty()` while a gobo is active so the light
    routes through the standard deferred path, which renders `LightFunctionMaterial` directly.
    Restored to `1` on Open/clear. Cost: this light loses MegaLights' clustering perf while a gobo
    is up — acceptable; gobo lights are typically heroes. Every cookie log line reports
    `bAllowMegaLights=<new> (was <prev>, …)` for verification.
  - `UpdateEpicBeamParams` re-pushes the cached gobo on every beam refresh, so motion/zoom changes
    don't drop the cone or the cookie.
  - `EpicBeamDefaultGoboTex` snapshots the MI parent's default `DMX Gobo Disk Frosted` at
    `TryBuildEpicBeam`, so a `SetFixtureGobo` clear reverts the cone to Epic's open frosted disc.
    The cookie clears by nulling `SpotLight->LightFunctionMaterial` (a transparent light function
    would dim the lit pool — null is the true "no gobo").
  - The legacy `GoboMID` member is preserved (no-op when its `M_RebusGobo` content asset doesn't
    exist) so a future light-fn material can light up without re-wiring.
- **Gobo / animation-wheel rotation wire** (v1.0.50 wire, v1.0.52 mapping investigation, v1.0.53
  texture-rotation fix). Two descriptors drive the in-plane rotation of the SELECTED gobo image
  -- both inside the cone (Epic beam) and on the lit-pool cookie. Both are signed normalised
  speeds in `[-1, 1]` (clamped); sign is direction (+ = CW looking down the beam, − = CCW),
  `0` = stop. **v1.0.53 implementation**: a per-fixture `UCanvasRenderTarget2D GoboRT` holds the
  source gobo redrawn rotated by an integrated `GoboAngle` (deg, mod 360) every tick. `GoboRT`
  is bound (instead of `CurrentGoboTexture` directly) as Epic's `DMX Gobo Disk Frosted` texture
  param on both `EpicBeamMID` (cone) and `GoboLightFnMID` (cookie); the cone + cookie sample
  the rotated translucent RT and the projected pattern spins in plane around the cookie's
  out-of-screen axis. NO component transform is touched by gobo spin (v1.0.52's SpotLight roll
  was reverted -- user feedback "rotating around x instead of z"). Epic's `DMX Gobo Disk
  Rotation Speed` is pinned to 0 because per `M_Beam_Master`'s HLSL it's a U-axis SCROLL through
  wheel slots, not an image rotation; verified v1.0.52 by enumerating the uasset string tables
  of `M_Beam_Master`, `MF_DMXGobo`, `M_Light_Master`. Integration rate: 360 deg/sec per wire
  unit (= 1 rev/sec at speed=1); combined gobo+anim maxes at ±2 rps. See v1.0.53 release block
  above for the RT lifecycle (lazy `EnsureGoboRT`, `OnGoboRTUpdate` callback, first-frame
  redraw, Open/clear blanking) and sign-convention reasoning.

  ```jsonc
  // Gobo wheel rotation. Signed normalised speed in [-1, 1].
  // wheelIndex is optional, 0-based into the full wheels[] (matches RegisterFixtureGobos);
  // today the actor pushes one rotation per fixture so wheelIndex is logged for debugging.
  { "type": "SetFixtureGoboRotation",
    "fixtureId": "<id>",
    "speed": 0.5,            // -1..+1; 0 = stop, sign = direction
    "wheelIndex": 0          // optional, default 0 (first gobo-kind wheel)
  }

  // Animation-wheel rotation. Same units. Epic's reference materials don't model a separate
  // animation-wheel disc (no animation-disc rotation param in M_Beam_Master / MF_DMXGobo /
  // M_Light_Master, verified v1.0.52), so the visualiser folds this into the same SpotLight
  // roll as the gobo wire (combined = gobo + anim). A Warning fires once per fixture on first
  // non-zero apply so the user knows the cone and pool spin at the combined rate rather than
  // showing a stacked two-disc effect.
  { "type": "SetFixtureAnimationRotation",
    "fixtureId": "<id>",
    "speed": -0.25            // -1..+1; 0 = stop, sign = direction
  }
  ```

  Combined rotation = `Clamp(speed_gobo, -1, 1) + Clamp(speed_anim, -1, 1)` (so the wire range
  effectively widens to `[-2, 2]` only when both wheels are simultaneously driven). v1.0.52
  routes this into a per-tick SpotLight roll (see v1.0.52 release block above), so a combined
  ±1 = 1 rev/sec and a combined ±2 = 2 rev/sec around the beam emission axis. When a future
  material adds a real second animation disc with its own image-rotation parameter we'll split
  the push and the wire contract stays unchanged.

- **Open-slot detection** (v1.0.49). Real fixtures have an OPEN slot on every gobo wheel (the
  no-gobo position) that carries no image data. Pre-v1.0.49 the finalizer DROPPED entries with no
  bytes + no url, so Open vanished from the cache and selecting it silently fell through to the
  LAST gobo (cone never cleared). Now:
  1. `ARebusFixtureActor::IsOpenSlotName` recognises `"Open" | "None" | "Empty" | "Clear" | "No Gobo"
     | "NoGobo" | "Open Hole" | "OpenHole" | "Off" | "0"` (case-insensitive, trimmed exact match).
  2. `URebusVisualiserSubsystem` finalize sets `FRebusInlineGobo::bIsOpen` on slots whose `slotName`
     or `name` matches, and KEEPS them in the cache even with empty bytes/url. The finalize log
     reports the marker: `… slot=N slotName='Open' bytes=0 urlFallback=0 isOpen=1`.
  3. `AssignGobo` detects `Inline->bIsOpen` (or a slotName match, or an inline entry with no bytes
     and no url and no tag) and routes through `ClearGoboToOpen("inline Open slot")`, which drops
     `CurrentGoboTexture`, reverts the cone to its MI default, nulls the cookie, clears
     `bGoboActive`, and re-asserts `RefreshBeamShadowMode`.
  4. `FetchAndAssignGobo` (profile-URL fallback path) ALSO routes through `ClearGoboToOpen` on a
     missing slot URL — pre-v1.0.49 it only nulled the (always-null) legacy `GoboMID`, so the cone
     kept the last gobo. Now both surfaces revert together.
  5. `SetFixtureGobo(!bHasIndex)` likewise routes through `ClearGoboToOpen` for the explicit clear
     path. A spawn-with-Open also reaches the same clear via `AssignGobo` ↔ inline Open entry.
- **Gobo pipeline diagnostics** (v1.0.48 + v1.0.49 cookie/Open). Always-on `LogRebusVisualiser`
  lines, in order of arrival:
  1. `RegisterFixtureGobos '<libraryId>' chunk N/M (E entr(ies) in msg, A accumulated).`
  2. `RegisterFixtureGobos '<libraryId>' finalized: wheelIndex=W wheel='WN'(kind=K) slot=S slotName='SN' bytes=B urlFallback=0|1 isOpen=0|1` — one line per slot the worker assembled (v1.0.49 adds `isOpen`).
  3. `RegisterFixtureGobos '<libraryId>' complete: N gobo image(s) across W wheel(s), B total bytes.`
  4. `Fixture <id> SetFixtureGobo: bHasIndex=… goboIndex=… wheelIndex=… wheelName=… inlineGobos=N epicBeamMID=set|absent lightFnMID=set|lazy` — every wire-side `SetFixtureGobo` arrival.
  5. (Open slot) `Fixture <id> gobo OPEN slot detected (wheelIndex=W slot=S slotName='SN' name='N') -> applying clear.`
  6. (Clear) `Fixture <id> gobo OPEN: clearing cone+cookie (reason=…)` — explains which path drove the clear (`SetFixtureGobo(!bHasIndex)`, `inline Open slot`, `inline empty slot`, `profile slot has no media (Open)`, `no profile wheel`).
  7. `Fixture <id> AssignGobo: source=inline|inline-url|fallback …` — which path resolved.
  8. `Fixture <id> gobo decode OK: B bytes -> texture(W×H) -> epicBeamMID=set|absent lightFnMID=set|lazy` — decode + Epic MID push receipt.
  9. (Cookie MID lazy-create, once per fixture) `Fixture <id> gobo cookie: MID'd /DMXFixtures/LightFixtures/DMX_Materials/MI_Light.MI_Light for SpotLight->LightFunctionMaterial.`
  10. `Fixture <id> gobo cookie: lightFn=MI_Light tex=<name>(W×H) rot=<r> castShadows=0|1` — per cookie apply.
  11. (On clear) `Fixture <id> gobo cookie: lightFn=nullptr (Open / clear).`
  12. (URL fallback only) `Fixture <id> gobo URL fetched: B bytes, applied=0|1`.

  **Recipe.** Open the editor's Output Log, filter `LogRebusVisualiser`, change a gobo on a fixture
  in Orbit, and read top-to-bottom. The first missing line identifies the broken link:
  - no chunk = wire not arriving;
  - chunk but no finalize = base64/parse failure;
  - finalize but `isOpen=0` on what should be Open = portal `slotName/name` doesn't match any of
    the recognised "no-gobo" strings — add a portal-side rename or extend `IsOpenSlotName`;
  - finalize but no `SetFixtureGobo` = control-channel `setFixtureGobo` not being sent;
  - `SetFixtureGobo` but `inlineGobos=0` = the fixture's `libraryFixtureId` doesn't match any
    cached library;
  - `AssignGobo source=fallback` and no decode = no inline match for `(wheelIndex, slot)` so it
    falls to the REST URL fetch;
  - cookie line says `lightFn=nullptr` and you expected one = `CurrentGoboTexture` is null (Open
    path), check the preceding lines for which clear triggered;
  - cookie applied but no projection visible on the floor = either `MI_Light` failed to load
    (warning line says so) or `SpotLight->CastShadows` is false (the cookie line reports it; if 0
    we missed a refresh path).
- **Spotlight source size (§8.3)** (`RebusFixtureActor::BuildSpotLight`) sizes the
  `USpotLightComponent` emitter so the **beam starts at the lens diameter** (a finite disc, not a
  point) — the beam and its volumetric scattering emanate from the lens and gain soft-shadow
  penumbrae. `SourceRadius` (UE cm) is resolved by this precedence, the same diameter source order
  as the lens-flare disc but converted to a **radius**: `photometrics.lensDiameter / 2` (the IES
  lens opening) → `source.radiusMeters` → `source.diameterMeters / 2` → **leave the engine default
  untouched** (never fabricated). `SourceLength = 0` whenever a radius is set (circular GDTF beam,
  no second axis). The resolved radius is cached and reused as the base for the frost penumbra
  scaling, so the beam-origin diameter stays consistent with the lens-flare disc.
- **Emissive lens-flare disc (§8.3a)** (`RebusFixtureActor::BuildLensDisc`) spawns a thin
  `/Engine/BasicShapes/Plane` at the **`<Beam>` node origin**, parented under `FixtureRoot` and
  composed with the head motion (`LensDiscRest * Head`) so it tracks pan/tilt and stays
  perpendicular to the v1.0.21 beam direction (plane normal along the beam aim). Its **diameter
  source order** is `photometrics.lensDiameter` (metres, v6) → `source.radiusMeters * 2` →
  `source.diameterMeters` → a **synthetic dimensions fallback** (`0.4 × min(width, height)`,
  clamped 3–50 cm, so a disc + finite source always show when the portal sends no lens/source
  size) → skip only if even dimensions are absent. The plane (100 uu base) is scaled to
  `diameterCm / 100` and pushed slightly proud of the lens plane along the aim so head geometry
  can't clip it. The material is the committed **unlit, two-sided, ADDITIVE** master
  `/Game/REBUS/Materials/M_RebusLensFlare` (vector `EmissiveColor`, scalar `EmissiveStrength`,
  radial UV mask → round soft-edged glow). Additive (not translucent) means the disc **vanishes
  when the fixture is dark** instead of showing a black card. It is authored by the editor Python
  script `build_rebus_base_level.py` and the baked `.uasset` is committed. **Cook-safety:** the
  material is loaded by path (not referenced by any map), so the actor **hard-refs the mesh +
  material from its CDO** *and* `Config/DefaultGame.ini` lists `/Game/REBUS` + `/Engine/BasicShapes`
  under `DirectoriesToAlwaysCook` — otherwise the cooker strips the material and the disc never
  appears in `-game`/packaged builds. A per-fixture `UMaterialInstanceDynamic` is driven from the
  **live output** on the **same path that updates the SpotLight** (`RefreshIntensity` →
  `RefreshLensDisc`): `EmissiveColor` = the current linear fixture colour, `EmissiveStrength ∝
  dimmer × shutter-gate` (bright at full output, dark when fully dimmed, strobes in lockstep). It
  is purely **additive** — it never reshapes the SpotLight/IES beam. `BuildLensDisc` logs a single
  consolidated diagnostics line (`lens disc: SPAWNED ... meshOk/matOk ... relScale ... SourceRadius`)
  so a missing asset or zero scale is provable from logs.
- **Hybrid cone-mesh volumetric beam (§8.4a)** (`RebusFixtureActor::BuildBeamCone`) adds a visible
  volumetric **shaft** rendered as a procedural mesh — *alongside* (not replacing) the
  `USpotLightComponent`, which keeps surface lighting + IES + soft shadows. This gives an
  accurate, **IES-sized beam without the froxel-fog noise**.
  - **Geometry**: a **truncated cone (frustum)** built with `UProceduralMeshComponent` —
    **base radius** = the lens radius (`ResolveLensDiameterMeters()/2`, the same value driving
    `SourceRadius`/the lens disc, so the shaft starts exactly at the lens), **far radius** =
    `Length × tan(fieldHalfAngle)` where `fieldHalfAngle` is the current outer/field cone
    half-angle (`ResolveOuterHalfDeg`, shared with the SpotLight cone; zoom-range-clamped, iris-
    pinched), **length** = the SpotLight `AttenuationRadius` (the throw). Open sides, no caps,
    outward-radial smooth normals, collision **off**, `CastShadow(false)`. It is parented under
    `FixtureRoot` and composed with the head motion (`BeamConeRest × Head`, mesh `+Z` → beam
    forward) so it tracks pan/tilt and matches the v1.0.21 beam direction. The frustum is
    **regenerated on zoom/iris** (`RecomputeConeAngles → UpdateBeamConeGeometry`, gated to skip
    rebuilds when the far radius is ~unchanged).
  - **Material** (`/Game/REBUS/Materials/M_RebusBeam`, authored in `build_rebus_base_level.py`,
    baked + committed, CDO hard-ref + `/Game/REBUS` cook dir for cook-safety): an **unlit,
    two-sided, ADDITIVE** shader whose body is a **true N-step view-ray raymarch** in a single
    Custom HLSL node (Phase 2, v1.0.33). For each pixel it marches `StepCount` (~32) samples along
    the view ray from the cone's front face downrange, accumulating **front-to-back with
    transmittance** (`trans *= 1 − (1 − exp(−d·dt))`, energy-conserving) a density
    `d = BeamDensity × core × lenA`, where `core = pow(saturate(1 − radial/radiusAt), BeamSharpness)`
    is the **on-axis radial profile** (bright core → soft edge, `radiusAt` interpolated lens→far so
    it matches the mesh) and `lenA = pow(saturate(1 − axial/Length), BeamFalloff)` is the **length
    attenuation**. Output is `float4(BeamColor × BeamIntensity × coverage, coverage)` → Emissive +
    Opacity. **Camera scene-depth occlusion** clips the march at the opaque scene (`SceneDepth`
    converted to a distance along the view ray via the front-face `PixelDepth`), so the shaft
    disappears behind geometry from the camera; a **near-face soft clip** (`PixelDepth` ramp) stops
    popping when flying through the cone. MID params: `BeamColor`, `BeamIntensity`, `BeamSharpness`,
    `BeamFalloff`, plus the raymarch params `StepCount` + `BeamDensity` and the geometry feeds
    `BeamOrigin`/`BeamDir` (world, pushed each `RefreshMotion` so the marched cone matches the mesh
    after pan/tilt), `BeamLength`, `LensRadius`, `FarRadius`. *The Custom HLSL compiles cleanly in
    the headless bake (validated — `Success - 0 error(s), 0 warning(s)`).*
  - **Driven live**: `SetFixtureColor` → `BeamColor`; `RefreshIntensity` (dimmer × shutter-gate)
    → `BeamIntensity` (× `SetFixtureBeamVolumetrics` multiplier), so the shaft fades to nothing
    when dimmed/closed and strobes in lockstep; zoom/iris → frustum + half-angle; pan/tilt → head
    parenting. Initialised from current state at spawn.
  - **Coexistence + toggle**: while the mesh beam is on, the SpotLight's fog
    `VolumetricScatteringIntensity` is **forced to 0** by default (no competing noisy froxel beam).
    The `SetSceneProperty` boolean **`bMeshBeams`** (default `true`,
    `RebusSceneSettingsSubsystem::SetMeshBeamsEnabled`) toggles every fixture's cone on/off and,
    when off, **restores** the SpotLight fog scattering (the old fog beam) for runtime A/B. The
    lens-flare disc sits at the cone base (unchanged). `BuildBeamCone` logs a consolidated line
    (`beam: SPAWNED matOk ... baseRadius/farRadius/length/halfAngle ... BeamIntensity ...
    meshBeams=... coneFwd/spotFwd`).
  - **Direction = the live spotlight emission (ground truth, v1.0.34)**: the cone mesh and the
    raymarch are no longer oriented from a *constructed* rest basis (which could still render the
    shaft 180° opposite the real emission). Each `RefreshMotion`, `DriveBeamConeFromSpotLight`
    reads the **live `USpotLightComponent` world transform** *after* the spotlight is positioned —
    `GetForwardVector()` (the `+X` axis that actually lights the floor) and `GetComponentLocation()`
    (the lit origin) — orients the cone so its `+X` (frustum opening) **is** that vector, and feeds
    the same vector to the material `BeamOrigin`/`BeamDir`. So the spotlight forward, the cone mesh
    forward and the material `BeamDir` are provably the **same** vector. `RefreshBeamSpatialParams`
    emits a throttled proof line per aim change: `beam align: spotFwd=… coneFwd=… beamDir=…
    dot(spot,cone)=… dot(spot,beamDir)=…` (both dots must read `~1.000`, never `-1`).
  - **Light-blocking volumetric shadows (Phase 2, the must-have) — native VSM fog hybrid**: the
    trusses/set are **runtime-imported via glTFRuntime**, which has **no distance-field import
    option** (`FglTFRuntimeStaticMeshConfig` has no `bGenerateDistanceField`; mesh distance fields
    are an editor/DDC build step), so those meshes are **absent from the Global Distance Field** and
    a material can't `DistanceToNearestSurface`-trace them — and a material also can't sample a
    specific light's shadow map. The technique that **actually** carves a truss gap on runtime
    geometry is therefore **Virtual Shadow Maps**, which render *all* geometry regardless of
    distance fields. So a fixture that requests volumetric shadows
    (`SetFixtureBeamVolumetrics(castVolumetricShadow=true)`) and wins a **hero budget** slot
    (`RebusMaxShadowFogBeams = 6` per spawn batch, reset with the v1.0.19 cap) gets a **modest
    SpotLight `VolumetricScatteringIntensity` (1.5) + `Cast Volumetric Shadow`** re-enabled
    (`RefreshBeamShadowMode`): the native fog volume then shows the **real light-blocking truss
    gaps**, while the **mesh cone provides the crisp shaft**. All other beams stay mesh-only
    (scattering 0). **Trade-off / limit**: the shadow gaps come from the (slightly noisier, froxel)
    fog volume, not the crisp mesh cone, and only the few hero beams pay the cost; a per-fixture
    `USceneCaptureComponent2D` depth-shadow was rejected as higher-risk to validate headlessly.
    Gated by `bWantsVolumetricShadow` + the hero budget; `ApplyBeamVolumetrics` logs
    `wantsShadow/shadowHero/heroBudget`.
  - **A fixture does NOT self-shadow its own beam (v1.0.36)**: with the hero fog re-enabled, a
    fixture's OWN body geometry sits right at the light source and was carving mottled gaps into the
    base of its own beam (made worse by the v1.0.35 bound Orbit model stacking on top — two
    near-source occluders). Patchiness therefore tracked the hero/shadow-casting beams, not the
    mesh-only ones. Fix: `DisableSelfBeamVolumetricShadow` clears `bCastDynamicShadow` (keeping
    `CastShadow` for contact/RT grounding) on **both** the control-channel body `MeshComponents` (on
    `BuildMeshes`) **and** the matched/bound Orbit model components (on `BindOrbitComponents`). A
    movable spotlight's volumetric fog is shadowed by its VSM/shadow depth, which only includes
    `CastShadow && bCastDynamicShadow` primitives — UE5.7 has **no** per-primitive volumetric-fog
    flag (`bCastVolumetricShadow`/`SetCastVolumetricShadow` are **light-only**), so the dynamic-shadow
    opt-out is the lever. **Trade-off**: the fixture body no longer casts a dynamic shadow into ANY
    beam (incl. neighbours) or onto the floor — acceptable since fixture bodies are small/airborne.
    The trusses/set are OTHER actors and keep their dynamic shadows, so the wanted truss
    self-shadowing (the whole point of the hybrid) is **unaffected**.
- **Mesh→axis bucketing** matches GDTF `affectedGeometryNames`; opaque MVR proxy names
  (`mvr-glb-<uuid>`) that match nothing fall to the static base. The guide's height-plane
  split (§7.6) is the more robust fallback to add if needed.
- **Volumetric beams under MegaLights** (§5.7) — beams scatter through the level's volumetric
  height fog with MegaLights + its lighting volume (`r.MegaLights.Volume=1`) enabled. See the
  `RenderQuality` tiers and the artefact caveat above if the fog volume shows noise/banding.

## Orbit-imported model binding (Phase 1 A/B sync test)

The OrbitConnector import brings in the **light-fixture models** alongside the trusses/set. Those
models share the **same object id** as the fixtures delivered over the control channel, so we can
drive the imported model with the **same motion solve** as its `ARebusFixtureActor` and confirm
they move in sync. This is **Phase 1**: an overlay/A-B test — the control-channel mesh proxies stay
visible and authoritative; the Orbit overlay is **off by default** and toggled live. (Phase 2 —
dropping the control-channel `RegisterFixtureMeshes` import in favour of the Orbit models — is **not
done** here.)

- **Identification / matching (by object id).** On import, every Orbit static-mesh component is
  tagged (`UActorComponent::ComponentTags`) with the **names of its glb-node ancestry** — the
  Speckle object id is expected to be one of those node names. `URebusFixtureControlSubsystem::`
  `RebindOrbitModels` finds the import actor **generically by class name** (`"OrbitImportRoot"`, so
  RebusVisualiser keeps **no compile/link dependency** on the separately-owned OrbitConnector
  plugin), groups the tagged components into an `objectId → components` index, and binds each group
  to the registered fixture whose **`FixtureId` (Speckle node id)** equals that object id.
- **How the model is driven.** `ARebusFixtureActor::BindOrbitComponents` caches each component's
  imported (rest) world transform and the **head world transform at the rest pose** (`pan=tilt=0`),
  precomputing `OrbitBindBase = CompRestWorld · HeadWorldRest⁻¹`. Each `RefreshMotion`,
  `DriveOrbitModel(HeadLocal)` sets every bound component's world transform to
  `OrbitBindBase · (HeadLocal · ActorWorld)` — i.e. it applies **only the head's delta from rest**
  using the *same* `Cumulative[HeadAxisIndex]` solve that moves the control-channel head meshes, so
  the overlay tracks pan/tilt identically. No-rig fixtures drive with an identity head (the control
  meshes don't move either, so they stay in lock-step). The control-channel meshes are **not
  hidden** — both render together for the comparison.
- **Toggle (live).** Off by default. Enable either way:
  - **Scene property:** `SetSceneProperty name="bDriveOrbitModels" value=true|false` (round-trips in
    `SceneState`, re-asserted on respawn via `ReapplyAll`).
  - **Console:** `Rebus.DriveOrbitModels 1` / `Rebus.DriveOrbitModels 0`.
  Disabling restores every bound model to its imported pose.
- **Late binding (both orders).** Binding runs on a **1 Hz retry** (`URebusVisualiserSubsystem::
  Tick` → `RebindOrbitModels`, only while driving is enabled) plus on toggle and on (re)spawn. So a
  fixture that spawns **after** the import binds on the next pass, an import that arrives **after**
  the fixtures binds on the next tick, and a **re-import** (which destroys the components — held as
  weak pointers) rebinds when the live binding goes stale. Re-binding an already-bound id is skipped
  so the rest pose is never re-captured mid-motion.
- **Diagnostics to watch.** `Orbit bind: roots=… taggedComps=… distinctObjectIds=… | fixtures
  matched=… unmatched=… unmatchedFixtureIds=…` (match summary, throttled to changes);
  `Fixture <id>: BOUND <n> Orbit-imported component(s) by objectId '<id>'`; and per-update
  `Fixture <id>: drove Orbit model '<id>' pan=… tilt=… headRot=(P=… Y=… R=…) comps=…` (throttled to
  pan/tilt changes) so the overlay motion can be diffed against the control meshes.
- **Caveats / assumptions for the portal/import team to confirm.**
  - **The Orbit object id == the fixture `FixtureId`** (Speckle node id). Matching is exact-string
    against the glb node-name ancestry; if the portal exports the model under a *different* id (e.g.
    a Speckle *application* id or a decorated name), the `unmatchedFixtureIds` log will show it and a
    normalisation step is needed.
  - **Object-id tagging lives in OrbitConnector.** The import must tag each imported mesh component
    with its glb node-name ancestry (as this build does locally) for matching to work. That change
    is **not committed here** (OrbitConnector is owned by `orbit-connectors`); it must be upstreamed.
    Until then, a clean checkout binds **0** models (logged) and the overlay is simply inert.
  - **Whole-model overlay.** Phase 1 drives the *entire* matched model with the head solve (it can't
    tell yoke from base from generic tags), which is correct for pure pan/tilt sync but would also
    rotate a static base; per-part (head-only) driving is a Phase-2 refinement.

## Acceptance mapping (§12)

Fixtures spawn from `/scene` at placement · all `SetFixture*` keyed by node id, last-writer-
wins, optional `fadeMs` ease · selection highlight via custom-depth stencil (needs an outline
post-process material in content) · pan keeps base static (parent-first compose +
tilt-under-pan) · zoom reshapes cone + zoom-keyed IES with portal brightness authority ·
source radius → soft penumbrae · scene/quality applies, unknown names ignored · `Ready` →
`RequestSceneState` → `SceneState` read-back · streamer id from `-PixelStreamingID`.
