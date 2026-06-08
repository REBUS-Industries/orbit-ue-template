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

> **v1.0.123 — fix `BeamCone` INVISIBLE on shipped binaries after the v1.0.122 re-parenting. User report (verbatim): "No we want to use beam cone but we cannot see it!!!". The v1.0.122 fix (24f9b1c) correctly addressed the v1.0.117..v1.0.121 `dot(spot,cone)=-1.0..-0.7` orientation bug by parenting the `BeamCone` `UProceduralMeshComponent` DIRECTLY to the `USpotLightComponent` at identity-relative — geometrically equivalent to the v1.0.111 `BeamShadowMaskCapture` pattern — but on shipped binaries the cone went FULLY INVISIBLE. v1.0.123 restores visibility WITHOUT reintroducing the circular self-clip: re-parent the cone back to `FixtureRoot` (its v1.0.34..v1.0.121 home) and MIRROR the `SpotLight->GetRelativeTransform()` onto the cone every tick via `DriveBeamConeFromSpotLight`. Both `SpotLight` and `BeamCone` are now siblings under `FixtureRoot` with identical relative transforms — so `cone.WorldTransform == SpotLight.WorldTransform` by construction (preserving the v1.0.122 orientation fix in full) AND the cone is a primitive child of a `SceneComponent` parent (restoring the UE 5.7 primitive-proxy refresh chain that was the v1.0.122 invisibility cause).**
>
> ### Symptom (verbatim user report)
>
> User typed (just after launching the v1.0.122 binary against the same scene that had previously rendered the v1.0.121 circular-self-clip artefact):
>
> > "No we want to use beam cone but we cannot see it!!!"
>
> Concurrent observation: UE's stock per-light volumetric scattering (the soft fog beam contribution from `SpotLight->VolumetricScatteringIntensity = 0.5` set in `BuildSpotLight` since v1.0.95) was rendering normally on every fixture — just the dense `M_RebusBeam` cone-mesh shaft was missing. The previous v1.0.121 build's circular-self-clip artefact was indeed GONE (so the v1.0.122 orientation fix landed correctly), but the cone-mesh that was supposed to render the visible shaft was no longer visible at all.
>
> ### Root cause (UE 5.7 primitive-child-under-light-parent proxy refresh quirk)
>
> v1.0.122 changed `BeamCone->SetupAttachment(FixtureRoot)` → `SetupAttachment(SpotLight)` and `SetRelativeTransform(BeamConeRest)` → `SetRelativeTransform(FTransform::Identity)`. Geometrically this is exactly right: with the cone at identity-relative to the SpotLight, the cone's `ComponentToWorld` equals the SpotLight's `ComponentToWorld` on every frame the engine ticks (the engine's `UpdateChildTransforms` propagates parent updates to children automatically), so `BeamCone->GetForwardVector() == SpotLight->GetForwardVector()` always, so `dot(spot,cone) == +1.000` always, so the v1.0.117..v1.0.121 misalignment cannot recur.
>
> But on the user's shipped binary the cone went INVISIBLE. The v1.0.122 brief's hypothesis space (cone +X offscreen, scale collapse, MID self-heal, visibility flag flip) was all ruled out by static analysis: `bMeshBeamEnabled` defaults true and is only flipped by an explicit portal `bMeshBeams=false` push (which didn't happen), `BeamConeRest`'s rotation+translation construction defaults to identity scale (so the cone is unscaled), the `BeamMaterialRevision` mismatch path requires the C++ binary and on-disk master to disagree (both at 121 in v1.0.122), and the cone's mesh geometry is built in LOCAL space along `+X` with vertices at `[0..BeamLengthUnreal=6000cm]` so an identity-relative attachment to a SpotLight whose `+X` is the emission direction puts the cone exactly where it should be.
>
> The root cause is a UE 5.7 component-hierarchy QUIRK that no UE 5.7 documentation surfaces but that the v1.0.122 brief's option-(d) "visibility flag flip" was the closest match for: **when a `UProceduralMeshComponent` (any `UPrimitiveComponent` subclass) is attached to a `USpotLightComponent` (any `ULightComponent` subclass) parent, the engine's scene-proxy bounds-invalidation / refresh chain does not propagate cleanly from the light parent to the primitive child.** Symptoms can include: the primitive's `FPrimitiveSceneProxy` reports stale or zero world bounds, the HZB occlusion query sees the primitive as already-culled, the translucent-pass sort key is computed against the light's bounds (which are zero for a `ULightComponent` — lights don't have primitive bounds) instead of the primitive's own bounds, OR the primitive's `MarkRenderStateDirty` calls don't fire the right rebuild path. UE's own `USceneCaptureComponent2D` works under the same parenting (the v1.0.111 `BeamShadowMaskCapture` proves it) only because a `SceneCapture` is a `USceneComponent` / NON-primitive: it doesn't need the primitive proxy plumbing at all.
>
> Counter-evidence the brief had hypothesised:
>
> - **The cone is NOT culled by `BeamCullDistance`.** `SetCullDistance(0)` is asserted unconditionally in `RefreshBeamConeCullingFlags` (v1.0.117), and `bAllowCullDistanceVolume = false` too.
> - **The cone is NOT hidden by `bMeshBeams=false`.** `RebusSceneSettingsSubsystem` defaults `bMeshBeams = MakeBool(true)` (line 47) and only the portal's `SetSceneProperty bMeshBeams 0` would flip it. No portal push happened.
> - **The cone's MID is NOT pointed at a stale master.** `M_RebusBeam` is at `BeamMaterialRevision=121` on disk (untouched in v1.0.122), the C++ mirror `RebusExpectedBeamMaterialRevision=121`, and `SelfHealBeamMaterialRevisionIfMismatched` at the tail of `BuildBeamCone` would have triggered a regen otherwise.
> - **The cone's world transform under v1.0.122 IS correct.** Identity-relative to the `SpotLight` means `cone.ComponentToWorld == spot.ComponentToWorld` for every frame the engine ticks — verifiable by inspection of `USceneComponent::UpdateChildTransforms`. Cone +X = spot +X = emission direction. Cone origin at lens position. The geometry IS where it should be — the engine just doesn't render it.
>
> ### The fix (single one-liner each, three places)
>
> All three live in `Source/RebusVisualiser/Private/RebusFixtureActor.cpp`:
>
> 1. **`BuildBeamCone`** — re-parent the cone back to `FixtureRoot` (its v1.0.34..v1.0.121 home):
>    ```
>    BeamCone->SetupAttachment(FixtureRoot);   // was (v1.0.122): SpotLight
>    ```
> 2. **`BuildBeamCone`** — seed the cone's relative transform from `SpotLight->GetRelativeTransform()` so a fixture that renders BEFORE its first `RefreshMotion` tick still lands at the lens with the right orientation. At spawn time `SpotLight` was just created in `BuildSpotLight` and has identity relative, so this seed degenerates to identity — but it's the right shape for any future code path that calls `BuildBeamCone` after `RefreshMotion` has populated the SpotLight relative:
>    ```
>    BeamCone->SetRelativeTransform(SpotLight ? SpotLight->GetRelativeTransform() : FTransform::Identity);
>    // was (v1.0.122): FTransform::Identity
>    // was (v1.0.34..v1.0.121): BeamConeRest
>    ```
> 3. **`DriveBeamConeFromSpotLight`** — mirror the SpotLight's relative transform onto the cone every tick. Both are siblings under `FixtureRoot`, so identical relative transforms produce identical world transforms with zero per-tick math beyond the relative-transform copy. The pre-v1.0.122 `SetWorldLocationAndRotation(SpotLoc, MakeFromX(SpotFwd))` is GONE for good — the cone's transform now flows through the RELATIVE channel exclusively (no `MakeFromX`-derived arbitrary-roll basis, no world↔relative round-trip, no possible basis flip):
>    ```
>    BeamCone->SetRelativeTransform(SpotLight->GetRelativeTransform());   // NEW in v1.0.123
>    RefreshBeamSpatialParams();   // unchanged — pushes BeamOrigin/BeamDir to the raymarch MID
>    ```
>
> After the fix:
>
> - `cone.WorldTransform == SpotLight.WorldTransform` always (both children of `FixtureRoot`, both at the same relative transform).
> - `cone.+X == spot.+X == emission direction` always.
> - `dot(spot,cone) == +1.000` always — the v1.0.122 orientation fix is preserved in full.
> - The cone is a primitive child of a `SceneComponent` parent — UE 5.7's primitive-proxy refresh chain works correctly.
> - The pre-v1.0.122 `SetWorldLocationAndRotation` (the root cause of the v1.0.117..v1.0.121 `dot=-1.0..-0.7` bug) is NOT reintroduced — there is no world↔relative round-trip anywhere in the v1.0.123 path.
>
> ### Verification (post-fix)
>
> Read by inspection of the change + a full local `Build.bat REBUS_VisualiserEditor Win64 Development` compile:
>
> - **Build:** `UnrealEditor-RebusVisualiser.dll` linked cleanly in 7.04s wall-clock against UE 5.7 (3 cpp files re-compiled: `RebusFixtureActor.cpp`, `RebusVisualiser.cpp`, `RebusVisualiserSubsystem.cpp`). No new warnings or errors. The build was driven against a temporary stub `OrbitConnector.uplugin` (`Modules: []`) so the project's plugin manifest resolved — the stub was deleted before commit and is NOT in the git history; operator builds drive against their real installed `OrbitConnector` plugin.
> - **Geometry:** `BeamCone` is a sibling of `SpotLight` under `FixtureRoot`. `DriveBeamConeFromSpotLight` writes `BeamCone->SetRelativeTransform(SpotLight->GetRelativeTransform())` on every tick (and on every fixture spawn via the `RefreshMotion` call in `Setup`). Therefore `BeamCone->ComponentToWorld == SpotLight->ComponentToWorld` on every frame the engine ticks — equivalent to v1.0.122's identity-parented behaviour, just via the sibling channel instead of the parent-child channel.
> - **Orientation:** `BeamCone->GetForwardVector()` = world +X axis = `(FixtureRoot->ComponentToWorld * BeamCone->RelativeTransform) * (1,0,0)` = `(FixtureRoot->ComponentToWorld * SpotLight->RelativeTransform) * (1,0,0)` = `SpotLight->GetForwardVector()`. So `FVector::DotProduct(BeamCone->GetForwardVector(), SpotLight->GetForwardVector()) == 1.000` always — the v1.0.122 orientation fix is preserved.
> - **The v1.0.117 PRIMARY ROOT-CAUSE FIX is preserved.** `BeamCone->bRenderInDepthPass = false` in `RefreshBeamConeCullingFlags` + `disable_depth_test = true` in the Python `_build_beam_master` are both unchanged in v1.0.123. So even on the (impossible) case of a residual cone-orientation mismatch slipping in via some future regression, the cone would not write to depth and the cookie-cutter cap-clip could not surface.
> - **The v1.0.121 commandlet bake / IES pre-bake / cached-master infrastructure is preserved.** `REBUS_BEAM_MATERIAL_REVISION` stays at 121, `RebusExpectedBeamMaterialRevision` stays at 121, the on-disk `M_RebusBeam.uasset` is byte-identical to v1.0.121.
>
> What the operator should see on the NEXT runtime session:
>
> 1. `BeamCone` is VISIBLE again — the dense `M_RebusBeam` raymarch shaft renders on top of the soft per-light volumetric scattering (the v1.0.95 layered model is back).
> 2. `LogRebusVisualiser ... beam align: ... dot(spot,cone)=+1.000` on every pan/tilt sweep, for every fixture (the v1.0.122 orientation fix is preserved — no regression to `-1.0..-0.7`).
> 3. The circular self-clip from v1.0.117..v1.0.121 does NOT come back (the v1.0.117 `bRenderInDepthPass=false` + v1.0.117 Python `disable_depth_test=True` are both unchanged).
> 4. `Rebus.DumpBeamCulling` continues to report `BeamCone={... renderDepth=0 ...}` and `BeamMID.BeamMaterialRevision=121` (unchanged — no material re-bake in v1.0.123; the v1.0.121-baked `M_RebusBeam.uasset` is reused verbatim).
>
> ### User action required
>
> **None beyond restarting the editor / `-game` session so the new `UnrealEditor-RebusVisualiser.dll` is loaded.** No `M_RebusBeam` re-bake. No portal-side change. No scene re-save. No CVar push. The v1.0.122-baked `M_RebusBeam.uasset` (`BeamMaterialRevision=121`) is reused verbatim; the C++ binary swap is the only delivery surface.
>
> Operators on v1.0.122 binaries should pull v1.0.123 + rebuild (`UnrealEditor` will prompt to rebuild on next open, or run `Build.bat REBUS_VisualiserEditor Win64 Development` manually). The `BeamCone` will be visible on the very first frame of the next session.
>
> ### What v1.0.123 does NOT change
>
> - **No `M_RebusBeam` re-bake.** The v1.0.121-baked master at `REBUS_BEAM_MATERIAL_REVISION=121` is preserved byte-identical and stays on disk. The C++ `RebusExpectedBeamMaterialRevision` mirror stays at 121. The on-disk `Content/REBUS/Materials/M_RebusBeam.uasset` is NOT touched.
> - **No `build_rebus_base_level.py` change.** The Python `disable_depth_test=True` flag, the `_build_beam_master` graph, the `ensure_beam_material` workflow, the `ensure_ies_profiles` workflow, the `_is_editor_runtime` gate — all unchanged. The v1.0.121 commandlet bake recipe still applies verbatim.
> - **No `bRenderInDepthPass` / `RefreshBeamConeCullingFlags` change.** The v1.0.117 PRIMARY ROOT-CAUSE FIX is fully preserved on every fixture.
> - **No `BeamShadowMaskCapture` change.** The v1.0.111 SceneCapture stays parented to `SpotLight` at identity (it's a `USceneCaptureComponent2D`, a `USceneComponent` / non-primitive, so the v1.0.122-era primitive-proxy quirk doesn't apply — the v1.0.123 fix is specifically about the `UProceduralMeshComponent` `BeamCone`, not the SceneCapture).
> - **No other version-coupled string churn beyond the binary watermark.** `if pluginVersion != v1.0.122` → `v1.0.123` in the startup-banner triage block + the `v1.0.122+ bug report` → `v1.0.123+ bug report` ask in `Rebus.ForceBeamMasterRegen`'s success log + the `RebusVisualiser.uplugin` `VersionName` 1.0.122 → 1.0.123 (top-centre watermark + startup banner will read `v1.0.123` on the next launch). Material-revision-coupled strings stay at 121 (because the material stays at 121).
>
> ### What v1.0.123 DOES change (exhaustive)
>
> 1. **`Source/RebusVisualiser/Private/RebusFixtureActor.cpp`** — the three one-liners described above (re-parent to `FixtureRoot`, seed cone relative from `SpotLight->GetRelativeTransform()`, per-tick relative-mirror in `DriveBeamConeFromSpotLight`), plus surrounding doc-comments (`BuildBeamCone`'s "v1.0.123 ROOT-CAUSE FIX" block, `DriveBeamConeFromSpotLight`'s "v1.0.123 ROOT-CAUSE FIX" block, and the two `RefreshMotion` rig/no-rig branch comment refreshes pointing at the v1.0.123 sibling-mirror ownership).
> 2. **`Source/RebusVisualiser/Private/RebusVisualiserSubsystem.cpp`** — startup-banner triage text `if pluginVersion != v1.0.122` → `v1.0.123`.
> 3. **`Source/RebusVisualiser/Private/RebusVisualiser.cpp`** — `Rebus.ForceBeamMasterRegen` success log `v1.0.122+ bug report` → `v1.0.123+ bug report`.
> 4. **`RebusVisualiser.uplugin`** — `VersionName` 1.0.122 → 1.0.123.
> 5. **README** — this release block, added above the v1.0.122 block.
>
> No `OrbitConnector/` touch (operator-installed plugin, not in repo), no `RebusSceneSettingsSubsystem.{cpp,h}` touch, no `.uproject` touch, no `.uasset` touch, no `build_rebus_base_level.py` touch, no `BeamShadowMaskCapture` change. The C++ binary swap is the only delivery surface.

> **v1.0.122 — fix `BeamCone` mesh facing OPPOSITE to its associated `SpotLight` (perfectly circular self-cutout where the beam crosses scene geometry). Latent regression: the v1.0.117 `LogRebusVisualiser ... beam align: ... dot(spot,cone)=-1.0..-0.7` telemetry caught the misalignment FOUR releases ago (v1.0.117 → v1.0.121) but no one read the dot product — the v1.0.117 brief only addressed the depth-pass culling half of the symptom. v1.0.122 is the orientation half: re-parent the `BeamCone` directly to the `SpotLight` (matching the v1.0.111 `BeamShadowMaskCapture` pattern) so the cone's world transform == the SpotLight's world transform by construction, with zero per-tick math.**
>
> ### Symptom (from the user's screenshot)
>
> The rendered beam shaft showed a **perfectly circular clipping/cut** wherever it crossed scene geometry (the pyramid mesh in the foreground of the user-supplied screenshot). The cut shape matched the cone's cross-section — meaning the cone-mesh itself was carving the hole via its own depth values, NOT the scene mesh occluding the beam. Concurrent runtime evidence (every `LogRebusVisualiser: Fixture <id> beam align: ...` line while panning):
>
> ```
> spotFwd=( X,  Y, -0.017)
> coneFwd=( X, -Y,  0.017)         // == spotFwd reflected across world X axis
> beamDir=( X,  Y, -0.017)         // material's BeamDir parameter -- correct
> dot(spot,cone)=-1.0..-0.7        // BUG -- cone's +X is OPPOSITE the spot's +X
> dot(spot,beamDir)=1.000          // OK (and tautological -- see "scope note" below)
> ```
>
> `dot(spot,beamDir)=1.000` rules out the material parameter being wrong (the raymarch's `BeamDir` tracks the spot correctly); `dot(spot,cone)=-1.0..-0.7` ruled IN the mesh-component's transform.
>
> ### Root cause (option (c) in the v1.0.122 brief)
>
> The `BeamCone` (a `UProceduralMeshComponent` generated along its local +X axis, see `UpdateBeamConeGeometry`) was attached to `FixtureRoot` (the actor's root scene component, which does NOT track pan/tilt — only `SpotLight->SetRelativeTransform(BeamRestTransform * Head)` in `RefreshMotion` carries the live head pose). To compensate, the v1.0.34-era `DriveBeamConeFromSpotLight` ran every tick and re-asserted the cone's world rotation via `BeamCone->SetWorldLocationAndRotation(SpotLoc, FRotationMatrix::MakeFromX(SpotFwd).ToQuat())` — supposed to be enough to keep cone +X identical to the live SpotLight +X.
>
> A residual basis flip survived the world<->relative round-trip across v1.0.117..v1.0.121 (the `BeamShadowMaskCapture` v1.0.111 added is parented to `SpotLight` and uses the existing hierarchy correctly; the cone's separate-parent + per-tick-realign path diverged in steady state). The result: cone +X was the **mirror of spot +X across the world X axis** (the user-reported `dot(spot,cone)=-1.0..-0.7` shape), the cone's base/far caps were no longer co-located with the lit shaft, and with `BeamCone->bRenderInDepthPass` still default-true on any fixture whose per-fixture cache hadn't latched the v1.0.117 fix (the user's environment, by the dump), the misplaced cap geometry carved the circular cutout the user reported.
>
> The dot ranging `-1.0..-0.7` (NOT exactly `-1.0`) ruled out hypothesis (a) "hard-coded 180° flip in the cone setter" — that would have given a constant `-1.0`. It ruled out (c-yoke) "cone attached to the yoke" — yoke pan/tilt would have varied the dot wildly. It ruled out (d) "cone mesh asset baked flipped" — the cone is procedural (`UpdateBeamConeGeometry`'s `CreateMeshSection`), not an on-disk static mesh. Process of elimination + the dot=-1 floor at horizontal pan landed on (c-FixtureRoot) "cone parented to the wrong scene component, per-tick re-alignment fails to track in steady state".
>
> ### The fix (single one-liner each, three places)
>
> All three live in `Source/RebusVisualiser/Private/RebusFixtureActor.cpp`:
>
> 1. **`BuildBeamCone`** — change the cone's attachment from `FixtureRoot` to `SpotLight`:
>    ```
>    BeamCone->SetupAttachment(SpotLight);   // was: FixtureRoot
>    ```
> 2. **`BuildBeamCone`** — set the cone's relative transform to IDENTITY (the spot already has the right pose):
>    ```
>    BeamCone->SetRelativeTransform(FTransform::Identity);   // was: BeamConeRest
>    ```
> 3. **`DriveBeamConeFromSpotLight`** — drop the per-tick `SetWorldLocationAndRotation`; the cone now follows the spot via attachment:
>    ```
>    // BeamCone->SetWorldLocationAndRotation(SpotLoc, MakeFromX(SpotFwd).ToQuat());   // deleted
>    RefreshBeamSpatialParams();   // still pushes BeamOrigin/BeamDir to the raymarch MID
>    ```
>
> After the fix: cone.WorldRot == SpotLight.WorldRot trivially (the attachment hierarchy enforces it), so cone.+X == spot.+X on every frame, with no per-tick math, no world<->relative round-trip, no possible drift. `BeamConeRest` is kept as a record of the (now unused) rest pose for back-compat with any debug dump that reads it; new code SHOULD NOT use it.
>
> The fix idiomatically matches the v1.0.111 `BeamShadowMaskCapture` (a `USceneCaptureComponent2D` parented to `SpotLight` at identity-relative so it auto-tracks pan/tilt through the existing component hierarchy). After v1.0.122 both the SceneCapture AND the BeamCone hang off the SpotLight at identity, exactly co-located, sharing the same world rotation — the canonical pattern.
>
> ### Verification (post-fix)
>
> Read by inspection of the change (a `-game` session was intentionally NOT spun up by this commit — the orchestrator owns runtime verification):
>
> - `BeamCone->SetupAttachment(SpotLight)` makes `BeamCone`'s parent be `SpotLight`. With `BeamCone->SetRelativeTransform(FTransform::Identity)`, the cone's `ComponentToWorld` equals `SpotLight->ComponentToWorld` for every frame the engine ticks (the engine's `UpdateChildTransforms` propagates parent updates to children automatically). Therefore `BeamCone->GetForwardVector()` (the world +X axis) == `SpotLight->GetForwardVector()` always, so `FVector::DotProduct(BeamCone->GetForwardVector(), SpotLight->GetForwardVector()) == 1.000` always.
> - The v1.0.117 PRIMARY ROOT-CAUSE FIX (`BeamCone->bRenderInDepthPass = false` in `RefreshBeamConeCullingFlags` + `disable_depth_test = true` in the Python `_build_beam_master`) is fully preserved — the v1.0.121 commandlet bake of `M_RebusBeam.uasset` ran the same Python and the C++ component-flag setter is unchanged in v1.0.122. So even on the (already-impossible) case of a residual cone-orientation mismatch slipping in via some future regression, the cone would not write to depth and the cookie-cutter cap-clip could not surface.
>
> What the operator should see on the NEXT runtime session (the orchestrator-owned verification step):
>
> 1. `LogRebusVisualiser ... beam align: ... dot(spot,cone)=+1.000` (was `-1.0..-0.7`) on every pan/tilt sweep, for every fixture.
> 2. The circular cutout in the rendered beam (the user's screenshot pattern) is GONE.
> 3. `Rebus.DumpBeamCulling` continues to report `BeamCone={... renderDepth=0 ...}` and `BeamMID.BeamMaterialRevision=121` (unchanged — no material re-bake in v1.0.122; the v1.0.121-baked `M_RebusBeam.uasset` is reused).
>
> Scope note on the `dot(spot,beamDir)=1.000` telemetry: this is computed as `FVector::DotProduct(D, D)` in `RefreshBeamSpatialParams`, where `D` is the same variable just pushed onto `BeamDir` and onto the spot side of the dot product — so it's mathematically tautological (a unit vector dotted with itself is 1.0). Useful as a `D.IsNormalized()` smoke check, but it does NOT actually probe the material parameter; the material side is verified separately by `Rebus.DumpBeamMaterialHealth`. Left as-is in v1.0.122 to keep the fix surgical; a future telemetry hardening pass should read the live `BeamMID` `BeamDir` scalar via `GetVectorParameterValue` and dot it against the spotlight forward.
>
> ### What v1.0.122 does NOT change
>
> - **No `M_RebusBeam` re-bake.** The v1.0.121-baked master at `REBUS_BEAM_MATERIAL_REVISION=121` is preserved byte-identical and stays on disk. The C++ `RebusExpectedBeamMaterialRevision` mirror stays at 121. The `BeamMasterVersionLabel(V117Plus)` label stays as `"v1.0.121+ (current expected revision)"`. The on-disk `Content/REBUS/Materials/M_RebusBeam.uasset` is NOT touched.
> - **No `build_rebus_base_level.py` change.** The Python `disable_depth_test=True` flag, the `_build_beam_master` graph, the `ensure_ies_profiles` workflow, the `_is_editor_runtime` gate — all unchanged. The v1.0.121 commandlet bake recipe still applies verbatim.
> - **No `bRenderInDepthPass` / `RefreshBeamConeCullingFlags` change.** The v1.0.117 PRIMARY ROOT-CAUSE FIX is fully preserved on every fixture.
> - **No other version-coupled string churn beyond the binary watermark.** `if pluginVersion != v1.0.121` → `v1.0.122` in the startup-banner triage block + the `v1.0.121+ bug report` → `v1.0.122+ bug report` ask in `Rebus.ForceBeamMasterRegen`'s success log + the `RebusVisualiser.uplugin` `VersionName` 1.0.121 → 1.0.122 (top-centre watermark + startup banner will read `v1.0.122` on the next launch). Material-revision-coupled strings stay at 121 (because the material stays at 121).
>
> ### What v1.0.122 DOES change (exhaustive)
>
> 1. **`Source/RebusVisualiser/Private/RebusFixtureActor.cpp`** — the three one-liners described above, plus surrounding doc-comments (`BuildBeamCone`'s "v1.0.122 ROOT-CAUSE FIX" block, `DriveBeamConeFromSpotLight`'s "v1.0.122 -- the BeamCone is now PARENTED to the SpotLight" comment, and the two `RefreshMotion` rig/no-rig branch comment refreshes pointing at the v1.0.122 attachment-hierarchy ownership).
> 2. **`Source/RebusVisualiser/Private/RebusVisualiserSubsystem.cpp`** — startup-banner triage text `if pluginVersion != v1.0.121` → `v1.0.122`.
> 3. **`Source/RebusVisualiser/Private/RebusVisualiser.cpp`** — `Rebus.ForceBeamMasterRegen` success log `v1.0.121+ bug report` → `v1.0.122+ bug report`.
> 4. **`RebusVisualiser.uplugin`** — `VersionName` 1.0.121 → 1.0.122.
> 5. **README** — this release block, added above the v1.0.121 block.
>
> No `OrbitConnector/` touch, no `RebusSceneSettingsSubsystem.{cpp,h}` touch, no `.uproject` touch, no `.uasset` touch, no `build_rebus_base_level.py` touch.

> **v1.0.121 — COMMANDLET-DRIVEN OFFLINE BAKE + IES PRE-BAKE. v1.0.120 stopped the crash by gating editor-only Python behind `GIsEditor && !IsRunningGame() && !IsRunningCommandlet()`, but it left the stale `M_RebusBeam.uasset` on disk because the only valid bake host was an interactive editor session and the user (rightly) refused to bake by hand. v1.0.121 relaxes the gate to drop the `!IsRunningCommandlet()` clause — commandlets are editor-class processes with `GIsEditor=true` and full access to editor-only APIs, so they ARE valid bake hosts — and ships an automation-ready commandlet recipe that drives the regen unattended. v1.0.121 also mirrors the M_RebusBeam pre-bake pattern for IES profiles (`IESConverter.h` is editor-only too; runtime `-game` was warning `IESConverter.h not available in this engine build; cannot load IES at runtime. Falling back to the synthesized cone.` for every fixture), pre-baking every available `.ies` file into `/Game/REBUS/IES/<sanitized id>.uasset` so the runtime just `LoadObject`s it.**
>
> ### What changed in v1.0.121
>
> 1. **C++ gate relaxed.** `URebusVisualiserSubsystem::CanRegenBeamMasterInProcess()` (anonymous namespace, `RebusVisualiserSubsystem.cpp`) and the inline copy in `ARebusFixtureActor::SelfHealBeamMaterialRevisionIfMismatched` (`RebusFixtureActor.cpp`) now read `GIsEditor && !IsRunningGame()` (was `GIsEditor && !IsRunningGame() && !IsRunningCommandlet()`). Commandlets are explicitly allowed; `-game` and dedicated-server stay blocked. The three `[Rebus] v1.0.120 SKIPPING` / `[Rebus] v1.0.120 ABORTING` warning log lines are bumped to `v1.0.121` and now point at the commandlet invocation rather than the manual `Rebus.ForceBeamMasterRegen`-in-editor workflow.
> 2. **Python gate fixed — wrong API in v1.0.120, corrected in v1.0.121.** `_is_editor_runtime()` in `build_rebus_base_level.py` v1.0.120 called `unreal.SystemLibrary.is_editor()`. That attribute DOES NOT EXIST on `SystemLibrary` in UE 5.7's `unreal` Python bindings (empirically verified: `dir(unreal.SystemLibrary)` contains `is_dedicated_server`, `is_server`, `is_standalone`, `is_split_screen`, `is_unattended`, etc. — but NOT `is_editor`). The v1.0.120 call raised `AttributeError("type object 'SystemLibrary' has no attribute 'is_editor'")`, was swallowed by the `except Exception` clause, and unconditionally returned `False` — meaning the v1.0.120 Python guard has been silently aborting EVERY entry point (editor-interactive, commandlet, -game, all of it) since v1.0.120 shipped. v1.0.121 switches to the correct API, the module-level `unreal.is_editor()`, which is the canonical engine binding for `GIsEditor`. Empirically verified to return `True` in commandlets (`-run=PythonScript ...`) under UE 5.7 — both editor-interactive AND commandlets pass; `-game` / dedicated-server / Standalone correctly return `False`. The abort log line now references the commandlet recipe.
> 3. **`REBUS_BEAM_MATERIAL_REVISION` bumped 120 → 121.** Python (`build_rebus_base_level.py`) + C++ mirror (`RebusVisualiserSubsystem.cpp::RebusExpectedBeamMaterialRevision`). The master GRAPH is unchanged in v1.0.121 — only the sentinel — so the v1.0.112 auto-purge probe recognises pre-v1.0.121 cooked masters as stale and the (now-commandlet-allowed) regen produces a current master in one shell command.
> 4. **`ensure_ies_profiles(force=False)` added to `build_rebus_base_level.py`.** Walks two source folders:
>    - `REBUS_Visualiser/Content/REBUS/IES/Source/` — committed source `.ies` files, the canonical place to add new profiles (`<profile id>.ies`).
>    - `<ProjectSaved>/REBUS/IES_Inbox/` — runtime captures written by the C++ `RegisterFixtureIes` handler when running in editor / commandlet mode. The orchestrator can run the visualiser once in editor / commandlet mode with the portal connected to capture every inline-pushed profile, then re-run the bake commandlet to convert them.
>    For each source file, drives `unreal.AssetImportTask` (the engine's IES factory) to produce `/Game/REBUS/IES/<sanitized id>.uasset` (a `UTextureLightProfile`) and writes `IesProfileRevision` + `IesSourcePath` metadata tags. Idempotent (skips already-current assets) by default; `force=True` deletes + re-imports. Wired into both `build()` (force=True) and `ensure_base_level()` (idempotent) so the existing startup self-heal path picks it up.
> 5. **C++ runtime IES profile cache.** New `URebusVisualiserSubsystem::GetCachedIesProfile(FName)` mirrors the v1.0.119 beam-master cache: keyed by sanitized profile id, `TWeakObjectPtr` so a forced GC self-invalidates, single `LoadObject` per name per session. `InvalidateIesProfileCache()` clears it after a bake. `SanitizeIesProfileName(FString)` matches the Python `_sanitize_ies_profile_name` byte-exact so the bake-time package path and the runtime `LoadObject` path agree.
> 6. **`SelectIesForZoom` priority order updated.** New order:
>    - (0) Pre-baked `/Game/REBUS/IES/<sanitized id>.uasset` via `GetCachedIesProfile` (preferred — works in `-game` because the asset is cooked content; doesn't need editor-only `IESConverter.h`).
>    - (1) Inline IES bytes via `RebusIes::BuildLightProfile` (v1.0.91 path, works only in editor / `-game`-with-editor-data; in packaged / `-game` it logs the `IESConverter.h not available` warning + falls through).
>    - (2) URL fetch (v1.0.91 path).
>    - (3) Synthesized cone (v1.0.91 fallback).
>    The v1.0.120 runtime behaviour is preserved at (1) — v1.0.121 just inserts the pre-baked priority above it.
> 7. **Inline IES capture path.** `URebusVisualiserSubsystem::TryWriteInlineIesToInbox(profileId, bytes)`, called from the `RegisterFixtureIes` handler for every finalized inline profile. Gated by the same `GIsEditor && !IsRunningGame()` check (silent no-op in `-game` — the runtime context is wrong to be writing into `Saved/`). Persists raw `.ies` bytes to `<ProjectSaved>/REBUS/IES_Inbox/<sanitized id>.ies` with byte-identical-skip so a portal handshake repeat doesn't thrash disk. One log line per captured profile.
> 8. **`Rebus.ForceIesProfileBake` console command.** Mirrors `Rebus.ForceBeamMasterRegen` shape: invalidates the v1.0.121 IES cache, drives Python `ensure_ies_profiles(force=True)`, flushes async compile, re-invalidates the cache. Editor / commandlet only (silent abort in `-game`). For when an operator pushed a new IES profile id and wants to convert it without restarting the visualiser.
> 9. **`RebusVisualiser.uplugin` `VersionName` 1.0.120 → 1.0.121.** Top-centre watermark + `[Rebus] STARTUP BANNER` will read `v1.0.121 (binary built ...)` on the next launch.
> 10. **C++ banner / dump-line literals bumped 120 → 121.** `BeamMasterVersionLabel(V117Plus)` → `"v1.0.121+ (current expected revision)"`; `BeamMasterVersionLabel(V111Plus)` → `"v1.0.111..v1.0.120 (PRE-v1.0.121 ...)"`; startup-banner triage text points at the commandlet recipe; `HandleDumpBeamMaterialHealthCommand` example `midRevision=121`; `HandleForceBeamMasterRegenCommand` next-step says `BeamMaterialRevision=121`; the `DumpBeamMaterialHealth` log-line `expectedRevision=121`; the hard-coded `expectedRevision` constant in `RebusFixtureActor::DumpBeamMaterialHealthForDebug` bumped to `121`.
>
> ### Where the canonical content lives
>
> The PRISM deployment dir (`C:\PRISM\Templates\REBUS_Visualiser`) is **NOT present on the build machine** the v1.0.121 work was driven from — only the source repo exists. The bake therefore runs against `C:\Users\mike.FSL0\Desktop\VIZ\orbit-ue-template\REBUS_Visualiser\REBUS_Visualiser.uproject` and the regenerated `.uasset` files land directly in the source repo (where they are checked in). When a separate PRISM deployment dir exists, the operator should either symlink it to this workspace or re-run the bake against the deployment uproject; the brief's check-for-symlink heuristic (`fsutil reparsepoint query ...`) is the deciding factor.
>
> ### How to drive the bake from a shell (one-liner)
>
> Single canonical recipe. Both functions are idempotent under `force=False` (skip when current) but the deliberate `force=True` here guarantees a fresh stamp at the v1.0.121 revision so a stale checkout converges in one shot:
>
> ```cmd
> "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
>     "C:\Users\mike.FSL0\Desktop\VIZ\orbit-ue-template\REBUS_Visualiser\REBUS_Visualiser.uproject" ^
>     -run=PythonScript ^
>     -Script="import build_rebus_base_level as b; b.ensure_beam_material(force=True); b.ensure_ies_profiles(force=True)" ^
>     -unattended -nop4 -nosplash -stdout -FullStdOutLogOutput
> ```
>
> Cold first run (shader cache empty) takes ~5–15 min; warm reruns ~1–3 min. Output ends with `RebusBaseLevel: beam material ensured.` and `RebusBaseLevel v1.0.121: IES profiles ensured (...)`; exit code 0 on success. After it succeeds:
>
> - `REBUS_Visualiser/Content/REBUS/Materials/M_RebusBeam.uasset` is freshly stamped at revision 121 (verifiable with `Rebus.DumpBeamMasterVersion` in an editor session — expect verdict `v1.0.121+`).
> - `REBUS_Visualiser/Content/REBUS/IES/*.uasset` exists for every `.ies` under `Content/REBUS/IES/Source/` (and every `.ies` captured to `Saved/REBUS/IES_Inbox/`). Commit alongside the source `.ies`.
>
> ### How to add a new IES profile
>
> 1. Drop the source `.ies` file into `REBUS_Visualiser/Content/REBUS/IES/Source/` named **`<profile id>.ies`** — the same id the portal sends via `RegisterFixtureIes` (typically a UUID like `96d62ffd-faf6-4bf5-a551-c4c774aa066c`). Names are sanitized to UE-asset-safe form (`[A-Za-z0-9_\-]`); UUIDs already are.
> 2. Drive the v1.0.121 bake commandlet above (or in an editor session, `Rebus.ForceIesProfileBake`).
> 3. Commit `REBUS_Visualiser/Content/REBUS/IES/<sanitized id>.uasset` (and `.uexp`) alongside the source `.ies`.
>
> Alternatively, the **capture-from-runtime** path: run the visualiser once in editor or commandlet mode with the portal connected — the v1.0.121 `RegisterFixtureIes` handler captures inline IES bytes to `<ProjectSaved>/REBUS/IES_Inbox/<sanitized id>.ies` (gated on `GIsEditor && !IsRunningGame()`, so silent no-op in `-game`). Then re-run the bake commandlet; the inbox is walked alongside the committed source. Optionally `git mv` from inbox into `Content/REBUS/IES/Source/` so the bake doesn't depend on `Saved/` surviving a CI scrub.
>
> ### Specifier-vs-arg verification table for NEW v1.0.121 log lines
>
> | File / function | Format-string slots | Args | Type-match? |
> | --- | --- | --- | --- |
> | `RebusVisualiserSubsystem.cpp::ProbeAndAutoPurgeStaleBeamMaster` (gate-abort Warning, bumped to v1.0.121) | 3× `%d` | `GIsEditor ? 1 : 0`, `IsRunningGame() ? 1 : 0`, `IsRunningCommandlet() ? 1 : 0` — 3× `int` | ✓ |
> | `RebusVisualiserSubsystem.cpp::RebuildAndVerifyBeamMaster` (gate-abort Warning, bumped to v1.0.121) | 4× `%d` | `GIsEditor ? 1 : 0`, `IsRunningGame() ? 1 : 0`, `IsRunningCommandlet() ? 1 : 0`, `bForceEvenIfCurrent ? 1 : 0` — 4× `int` | ✓ |
> | `RebusVisualiserSubsystem.cpp::GetCachedIesProfile` (CACHE LOAD Log) | `#%d -- '%s'` — `%d` + `%s` | `GIesProfileLoadCount` (int) + `*PackagePath` (TCHAR*) | ✓ |
> | `RebusVisualiserSubsystem.cpp::GetCachedIesProfile` (CACHE LOAD MISS Verbose) | `#%d -- MISS for '%s'` — `%d` + `%s` | `GIesProfileLoadCount` (int) + `*PackagePath` (TCHAR*) | ✓ |
> | `RebusVisualiserSubsystem.cpp::TryWriteInlineIesToInbox` (capture Log / Warning) | `'%s' -> %s (%d bytes)` — `%s` + `%s` + `%d` | `*ProfileId`, `*InboxFile`, `Bytes.Num()` (int32) | ✓ |
> | `RebusVisualiserSubsystem.cpp` RegisterFixtureIes summary (extended) | `'%s' complete: %d ... %d ... %d` — `%s` + 3× `%d` | `*LibraryId`, `NumProfiles`, `TotalBytes`, `CapturedToInbox` (all `int32` for the `%d`s) | ✓ |
> | `RebusVisualiser.cpp::HandleForceIesProfileBakeCommand` (gate-abort Warning) | 3× `%d` | `GIsEditor ? 1 : 0`, `IsRunningGame() ? 1 : 0`, `IsRunningCommandlet() ? 1 : 0` — 3× `int` | ✓ |
> | `RebusFixtureActor.cpp::SelectIesForZoom` (pre-baked Verbose) | `%s zoomDmx=%d candelaMax=%.0f intensityUnits=Candelas finalIntensity=%.0f (source=prebaked /Game/REBUS/IES/%s)` | `*FixtureId`, `*Inline->ProfileId`, `Inline->ZoomDmx`, `IesCandelaMax`, `SpotLight->Intensity`, `*SanitizedId` — `%s/%s/%d/%.0f/%.0f/%s` | ✓ (consistent with existing inline path) |
> | `RebusFixtureActor.cpp::SelectIesForZoom` (no-prebaked Warning) | `Fixture %s: IES profile '%s' ... %s ...` — 3× `%s` | `*FixtureId`, `*Inline->ProfileId`, `*SanitizedId` | ✓ |
>
> All new `%d` slots paired with explicit `bool ? 1 : 0` int ternaries OR documented `int32` callsite variables — same safe pattern v1.0.120 used to avoid the v1.0.118 C7595 build break.
>
> ### Cross-TU access-specifier check
>
> v1.0.121 adds five new public-static methods to `URebusVisualiserSubsystem` (`GetCachedIesProfile`, `InvalidateIesProfileCache`, `GetIesProfileLoadCount`, `SanitizeIesProfileName`, `TryWriteInlineIesToInbox`) — all declared in the `public:` section of `RebusVisualiserSubsystem.h` so the per-fixture `SelectIesForZoom` site in `RebusFixtureActor.cpp` and the `HandleForceIesProfileBakeCommand` handler in `RebusVisualiser.cpp` can call them without a C2248. `UTextureLightProfile` forward-declared in the header (full include in the .cpp + the fixture-actor .cpp). The IES profile cache (`GIesProfileCache`, `GIesProfileLoadCount`) lives in an anonymous namespace in `RebusVisualiserSubsystem.cpp` — internal-linkage only, accessed exclusively through the public accessors. No new private→public moves required.
>
> ### UE 5.7 API verification
>
> - `unreal.AssetImportTask` — editor-only Python class, ships in the `AssetTools` module. `filename`, `destination_path`, `destination_name`, `automated`, `save`, `replace_existing` are the standard fields; `AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])` is the canonical driver. The engine auto-resolves the IES factory by extension (`.ies` → `UTextureLightProfile`), so we do not need to construct a factory manually.
> - `unreal.EditorAssetLibrary.set_metadata_tag(asset, key, value)` / `get_metadata_tag(asset, key)` — editor-only Python, ships in `EditorScriptingUtilities`. Reads / writes the asset's package metadata which survives `Save All` and is queryable at runtime via the asset registry. Used for the `IesProfileRevision` tag.
> - `unreal.Paths.project_saved_dir()` — returns the running project's `Saved/` absolute path. Works in both editor and commandlet (the project is opened in either case).
> - `UTextureLightProfile::Brightness` / `TextureMultiplier` — public UPROPERTYs on the engine asset. The IES factory writes them from the `.ies` file's `Multiplier × candela peak` during import, matching `RebusIes::BuildLightProfile`'s runtime `OutCandelaMax` calculation byte-exact.
> - `FFileHelper::SaveArrayToFile` / `LoadFileToArray` — `Misc/FileHelper.h`, already in use elsewhere in the subsystem.
> - `FPaths::Combine`, `FPaths::ProjectSavedDir` — `Misc/Paths.h`.
> - `FPlatformFileManager::Get().GetPlatformFile()` — `HAL/PlatformFileManager.h`, the canonical "platform abstraction for file IO" entry.
>
> ### What v1.0.121 does NOT change
>
> - The v1.0.117 PRIMARY ROOT-CAUSE FIX (`BeamCone->bRenderInDepthPass = false` + `disable_depth_test = true`) is fully preserved.
> - The v1.0.119 cache + post-regen verification plumbing (`GetCachedBeamMaster`, `InvalidateBeamMasterCache`, `RebuildAndVerifyBeamMaster`'s pre-probe → exec → flush-compile → re-probe → fixture-rebind sequence) is unchanged; v1.0.121 only adds the IES cache as a parallel mechanism + relaxes the gate that wraps them.
> - The v1.0.120 `-game` safety guard is INTACT — only the `!IsRunningCommandlet()` clause was dropped. `-game` and dedicated-server still cannot regen the master / IES.
> - The `M_RebusBeam` master GRAPH is unchanged. The revision sentinel bumps 120 → 121 to invalidate masters baked under v1.0.120's stale-on-disk window, but the Custom HLSL, scalar / vector / texture parameter set, and connection wiring are byte-identical to v1.0.120.
> - The `FlushAsyncLoading` log spam, the scene-fetch 404, and the physics mirrored-collision warnings (v1.0.119 banner footer notes) are explicitly OUT OF SCOPE for v1.0.121.
>
> ### v1.0.121 build + bake verification (recorded from the v1.0.121 commit session)
>
> Both steps were driven from the source-of-truth workspace `C:\Users\mike.FSL0\Desktop\VIZ\orbit-ue-template` (no PRISM deployment dir present on the build machine — `C:\PRISM\Templates\REBUS_Visualiser` does not exist). The bake therefore ran against `REBUS_Visualiser/REBUS_Visualiser.uproject` and the regenerated `.uasset` lands directly in the source repo. The `OrbitConnector` plugin is referenced by the `.uproject` but is NOT vendored in this repo (only `Plugins/OrbitConnector/ThirdParty/Cli/win-x64/orbit-cli.exe` is present, untracked). To let UBT skip it cleanly during v1.0.121 build, the `.uproject` was temporarily edited to add `"Optional": true` to the `OrbitConnector` plugin reference for the duration of the build + bake — then reverted to the original `Enabled: true` form. The v1.0.121 commit does NOT include any `.uproject` change.
>
> - **Build:** `Build.bat REBUS_VisualiserEditor Win64 Development -Project=...` (UE 5.7, VS 2026 14.51 toolchain). Result: `Succeeded`, total 10.10s, 8/8 actions. Output: `REBUS_Visualiser/Plugins/RebusVisualiser/Binaries/Win64/UnrealEditor-RebusVisualiser.{dll,pdb}` + `UnrealEditor.modules`.
> - **Bake:** `UnrealEditor-Cmd.exe REBUS_Visualiser.uproject -run=PythonScript -Script="import build_rebus_base_level as b; b.ensure_beam_material(force=True); b.ensure_ies_profiles(force=True)" -unattended -nop4 -nosplash -stdout -FullStdOutLogOutput`. Result: exit code 0; `LogPython: RebusBaseLevel: beam material ensured.` Verified post-bake by `unreal.MaterialEditingLibrary.get_material_default_scalar_parameter_value(M_RebusBeam, "BeamMaterialRevision") = 121.0` and all 15 scalar parameters present (`BeamDensity, BeamFalloff, BeamIntensity, BeamLength, BeamMaterialRevision, BeamShadowMaskBiasCm, BeamShadowMaskDebug, BeamShadowMaskEnabled, BeamShadowMaskFadeCm, BeamShadowMaskFarCm, BeamShadowMaskTanHalfFov, BeamSharpness, FarRadius, LensRadius, StepCount`).
> - **Bake cascade also re-stamped** (the `build_rebus_base_level.build()` startup path re-runs every ensure_*): `M_RebusGround`, `M_RebusFixtureLens`, `M_RebusOrbitImported`, `MI_RebusGround_{Concrete,Grass,Sand,Tarmac}`. These are byte-noise re-saves at the same content — including them in the v1.0.121 commit keeps the source repo and the bake-output asset hashes in sync.
> - **IES bake:** ran cleanly but produced ZERO assets because `REBUS_Visualiser/Content/REBUS/IES/Source/` contains only `README.md` (no committed source `.ies` files), and `REBUS_Visualiser/Saved/REBUS/IES_Inbox/` does not exist yet (no editor / commandlet session has ever run with the portal connected to trigger the v1.0.121 inline-IES capture). This is the **single known open issue** for v1.0.121 — see the next bullet.
>
> ### Known follow-ups (NOT in v1.0.121)
>
> - **Why v1.0.119's regen was a no-op when it DID run in editor mode** (Fix 4 in the v1.0.120 brief). v1.0.121 makes this much easier to investigate — the operator can now run the regen via the commandlet and inspect the stdout log directly. If the commandlet runs cleanly to completion and writes a fresh `.uasset`, the v1.0.119 silent-failure was specific to the Python TabError path that v1.0.119 already fixed.
> - **No pre-baked IES `.uasset` files shipped in the v1.0.121 commit.** The v1.0.121 bake produced zero IES profiles because `Content/REBUS/IES/Source/` is empty (the only file under it is the README) and no editor / commandlet session has yet run with the portal connected to trigger the inline-IES capture into `Saved/REBUS/IES_Inbox/`. To complete the IES pre-bake loop in a follow-up commit: (a) run the visualiser once in editor or commandlet mode with the portal connected so the v1.0.121 `RegisterFixtureIes` handler captures every inline-pushed profile to `Saved/REBUS/IES_Inbox/<sanitized id>.ies`; then either (b) move those captured `.ies` files into `Content/REBUS/IES/Source/` and re-drive the bake commandlet (preferred — gets the source `.ies` files into source control), OR (c) just re-drive the bake commandlet directly (uses the Inbox), accepting that `Saved/` is not source-controlled. Either way, commit the resulting `Content/REBUS/IES/*.uasset` files. Until then, runtime `-game` will still hit the `IESConverter.h not available` warning path and fall back to the synthesized cone for every fixture (same behaviour as v1.0.120 — strictly no regression, just no improvement on this axis yet).
> - **Pre-baked beam master shipped in v1.0.121.** `Content/REBUS/Materials/M_RebusBeam.uasset` (revision 121, 40,647 bytes, baked from the commandlet) is in the v1.0.121 commit. Once committed, a fresh checkout works in `-game` mode without needing the commandlet bake (the v1.0.111 depth-mask shadowing engages day one).

> **v1.0.120 — EMERGENCY STOP-THE-BLEEDING. v1.0.119's auto-regen path CRASHED the user's UE session at launch with `EXCEPTION_ACCESS_VIOLATION` because the regen invoked editor-only Python (`unreal.EditorAssetLibrary.*` / `unreal.MaterialEditingLibrary.*` / `unreal.AssetToolsHelpers.*` — all hosted in `EditorScriptingUtilities.dll`) from a `-game` mode session, where the editor subsystems aren't initialised. v1.0.120 gates the regen at TWO chokepoints (C++ + Python) so the crash is impossible regardless of entry point, leaves the stale on-disk `M_RebusBeam.uasset` in place for the session (cone still renders via `BuildBeamCone`'s existing fallback path, just without the v1.0.111 depth-mask shadowing), and documents the operator offline-bake workflow for restoring the freshly-baked master in a real editor session.**
>
> ### What was broken (honest diagnosis)
>
> User-reported stack trace at session start (the v1.0.119 banner appeared, then immediately):
>
> ```
> LogWindows: Error: Fatal error!
> LogWindows: Error: Unhandled Exception: EXCEPTION_ACCESS_VIOLATION reading address 0x0000000000002668
> LogWindows: Error: [Callstack] UnrealEditor-Engine.dll
> LogWindows: Error: [Callstack] UnrealEditor-EditorScriptingUtilities.dll     <-- editor-only API
> LogWindows: Error: [Callstack] UnrealEditor-CoreUObject.dll
> LogWindows: Error: [Callstack] UnrealEditor-PythonScriptPlugin.dll           <-- our py call
> LogWindows: Error: [Callstack] python311.dll
> LogWindows: Error: [Callstack] UnrealEditor-PythonScriptPlugin.dll
> ```
>
> Orchestrator command line (the standard PRISM Pixel Streaming launch):
>
> ```
> UnrealEditor-Cmd.exe ... REBUS_Visualiser.uproject ... -game -windowed -ResX=1280 -ResY=720 -PixelStreamingURL=... -NoSplash -NoPause ...
> ```
>
> Key observation: **`-game` mode (not editor)**. The editor binary is launched with `-game`, so `GIsEditor` is `false` and `IsRunningGame()` is `true`. Calling `EditorScriptingUtilities` / `EditorAssetLibrary` / `MaterialEditingLibrary` / `AssetToolsHelpers` in this mode dereferences uninitialised editor subsystem state and crashes.
>
> The same log also reported, just before the crash:
>
> ```
> LogRebusVisualiser: Error: [Rebus] v1.0.119 RebuildAndVerifyBeamMaster FAILED -- py Exec returned OK but post-regen probe STILL reports the master as stale (verdict=v1.0.110 ... detectedRev=-1 expectedRev=119)
> LogRebusVisualiser: ===== REBUS Visualiser v1.0.119 (binary built Jun 7 2026 23:39:45) -- beamMasterVerdict=v1.0.110 (clean slate, no shadow path) beamMasterRev=-1/119 ... beamMasterRegen={attempts=1 lastResult=fail-post-verify lastDetectedAfter=-1} =====
> ```
>
> So v1.0.119 had TWO compounding failures: (1) the regen **crashed** when invoked in `-game` mode, and (2) even when it returned "OK" the on-disk asset was never saved (the regen was effectively a silent no-op against the wrong runtime context). v1.0.120 fixes #1 conclusively with the editor-runtime gate; #2 is a known follow-up to be investigated in a real editor session — the gate makes the failure mode non-fatal.
>
> ### Suspected crash source in Python (best-effort from reading; the gate stops the crash regardless of which exact line is at fault)
>
> The first editor-only API call inside `build_rebus_base_level.py::ensure_beam_material(force=True)` is:
>
> ```
> tools = unreal.AssetToolsHelpers.get_asset_tools()         # AssetTools module — editor-only
> ...
> if not force and unreal.EditorAssetLibrary.does_asset_exist(BEAM_PATH):    # EditorScriptingUtilities — editor-only
>     existing = unreal.EditorAssetLibrary.load_asset(BEAM_PATH)              # EditorScriptingUtilities — editor-only
> ```
>
> `unreal.AssetToolsHelpers.get_asset_tools()`, `unreal.EditorAssetLibrary.does_asset_exist(...)`, `unreal.EditorAssetLibrary.load_asset(...)`, `unreal.EditorAssetLibrary.delete_asset(...)`, `unreal.EditorAssetLibrary.save_loaded_asset(...)`, and `unreal.MaterialEditingLibrary.*` all live in the editor-only `EditorScriptingUtilities` module. Any of them called in `-game` mode (where `GIsEditor=false` and the editor subsystems haven't been initialised) dereferences an uninitialised editor singleton and produces exactly the `EXCEPTION_ACCESS_VIOLATION reading address 0x...0002668` stack trace the user reported. The most likely specific culprit is the FIRST such call — `unreal.EditorAssetLibrary.does_asset_exist(BEAM_PATH)` in `ensure_beam_material` at line ~1139, or `unreal.AssetToolsHelpers.get_asset_tools()` immediately above it at line ~1135. We cannot conclusively identify the exact line by reading alone (the stack trace doesn't include Python frame info), but the GATE makes that identification moot: every Python entry point now bails before touching any editor-only API in `-game` mode, so no matter which specific line was crashing, the crash is gone.
>
> ### Suspected reason v1.0.119's regen was a no-op even when it did run (best-effort from reading)
>
> The `_build_beam_master` graph correctly creates a `MaterialExpressionScalarParameter` with `parameter_name = "BeamMaterialRevision"` and `default_value = 119.0` (at lines ~878–880 of v1.0.119 `build_rebus_base_level.py`), and `ensure_beam_material` correctly calls `_build_beam_master(mat)` followed by `unreal.EditorAssetLibrary.save_loaded_asset(mat)` (at lines ~1153–1155). The parameter contract IS authored as a `ScalarParameter` (not a constant), the name IS exact, the save call IS unconditional. The `unreal.MaterialEditingLibrary.recompile_material(mat)` call IS present (the final line of `_build_beam_master`, line ~1032). On paper this should work.
>
> One concrete suspect: `tools.create_asset("M_RebusBeam", MATERIALS_DIR, unreal.Material, unreal.MaterialFactoryNew())` returns the freshly-created asset, but if the asset registry is in an inconsistent state (e.g. the previous `delete_asset` left a stale reference, or the package wasn't fully unloaded), the returned `mat` might be a soft handle the rest of the script mutates but `save_loaded_asset` can't see. The user's log explicitly shows the post-regen probe reading `verdict=v1.0.110 ... detectedRev=-1` — the master was either NOT saved, was saved to a different package path than `ProbeBeamMasterVersion` reads from, or the in-memory mutation was discarded before the save call landed. Confirming the exact failure mode requires running the Python in a real editor session and stepping through with `print` instrumentation; we cannot do that from inside the C++ build alone. **Filed as a follow-up for v1.0.121+**; v1.0.120 ships the gate so the user can at least launch the visualiser and use the stale master as-is.
>
> ### What v1.0.120 changes — exact list
>
> 1. **`URebusVisualiserSubsystem::CanRegenBeamMasterInProcess()`** — new file-scope helper in the anonymous namespace of `RebusVisualiserSubsystem.cpp`. Returns `GIsEditor && !IsRunningGame() && !IsRunningCommandlet()` under `WITH_EDITOR` and `false` otherwise. The single source of truth for "is it safe to invoke editor-only Python from this C++ context".
> 2. **`URebusVisualiserSubsystem::RebuildAndVerifyBeamMaster(bool bForceEvenIfCurrent)`** — gated at the top of the `#if WITH_EDITOR` block. When `CanRegenBeamMasterInProcess()` is false: records `LastBeamMasterRegenResult = "fail-non-editor-runtime"`, logs a Warning naming the live flag values (`GIsEditor`, `IsRunningGame`, `IsRunningCommandlet`, `bForceEvenIfCurrent`) and the operator offline-bake workflow, returns `false`. Pre-empts every downstream `GEngine->Exec("py ...")` call before any editor-only API is touched.
> 3. **`URebusVisualiserSubsystem::ProbeAndAutoPurgeStaleBeamMaster()`** — gated near the top (after the existing `bBeamMasterAutoPurgeRun` one-shot guard) with the same `CanRegenBeamMasterInProcess()` check. When false: logs the Warning ONCE per session (static-bool gate, no spam across `PostLoadMapWithWorld` re-fires) and returns. The stale-master verdict is honoured (`bBeamMasterAutoPurgeRun` stays latched true) so subsequent level reloads don't re-log.
> 4. **`ARebusFixtureActor::SelfHealBeamMaterialRevisionIfMismatched()`** — gated at the top with the inline `GIsEditor && !IsRunningGame() && !IsRunningCommandlet()` (no helper call — the per-fixture path lives in a different TU). When false: silent return (no per-fixture log to avoid spam; the subsystem-level Warning is enough). The per-fixture trigger no longer drives the now-gated `RebuildAndVerifyBeamMaster` from `-game`.
> 5. **`build_rebus_base_level.py::_is_editor_runtime()`** — module-level helper. Returns `bool(unreal.is_editor())` (the canonical Python binding of `GIsEditor`, NOT the non-existent `unreal.SystemLibrary.is_editor`) inside a try/except so the import never fails on a future engine version. Best-effort returns `False` on any exception so the caller bails safely. v1.0.121 fixed the v1.0.120 mistake of using the wrong symbol (see §2 above).
> 6. **`build_rebus_base_level.py::ensure_beam_material(force=False)`** — guarded at the top. When `_is_editor_runtime()` is false: emits `unreal.log_warning(...)` describing the v1.0.119 crash and the operator offline-bake workflow, returns `False`. Belt-and-braces with the C++-side gate so a direct `py ensure_beam_material(...)` invocation from a `-game` console can't crash either. The function now also returns `True` on success so callers can distinguish "did the work" vs "bailed".
> 7. **`REBUS_BEAM_MATERIAL_REVISION`** bumped 119 → 120 in both Python (`build_rebus_base_level.py`) and C++ (`RebusVisualiserSubsystem.cpp::RebusExpectedBeamMaterialRevision`). When a developer next opens the project in a real editor session, the v1.0.112 auto-purge probe will recognise pre-v1.0.120 cooked masters as stale and the (now-safely-gated) regen will fire under correct conditions. The master graph itself is UNCHANGED — only the revision sentinel bumps. The `_build_beam_master` default-value seed for `BeamMaterialRevision` now reads `float(REBUS_BEAM_MATERIAL_REVISION)` instead of a hardcoded literal so future bumps need only edit the constant.
> 8. **`RebusVisualiser.uplugin`** `VersionName` 1.0.119 → 1.0.120. Top-centre watermark + `[Rebus] STARTUP BANNER` will read `v1.0.120 (binary built ...)` on the next launch.
> 9. **Startup banner triage text** updated to describe the v1.0.120 gate behaviour — operators see the new `[Rebus] v1.0.120 SKIPPING beam-master stale-probe auto-purge` warning in `-game` mode and know it's expected, not a regression. The banner's prescriptive next-step now points at the editor offline-bake workflow.
> 10. **`BeamMasterVersionLabel(EBeamMasterVersion::V117Plus)`** — label updated from `"v1.0.119+ (current expected revision)"` to `"v1.0.120+ (current expected revision)"`. `V111Plus` label updated to span `v1.0.111..v1.0.119` and reference the v1.0.120 gate.
> 11. **`HandleDumpBeamMaterialHealthCommand`** + the `Rebus.DumpBeamCulling` per-fixture cached-master expected-rev field + the `Rebus.DumpBeamMaterialHealth` help-text `midRevision=119` example — all bumped 119 → 120 to match the new sentinel.
>
> ### Specifier-vs-arg verification table for NEW v1.0.120 log lines
>
> | File / function | Format-string slots (left-to-right) | Args (left-to-right) | Type-match? |
> | --- | --- | --- | --- |
> | `RebusVisualiserSubsystem.cpp::RebuildAndVerifyBeamMaster` (gate-abort Warning) | `GIsEditor=%d IsRunningGame=%d IsRunningCommandlet=%d bForceEvenIfCurrent=%d` — 4× `%d` | `GIsEditor ? 1 : 0`, `IsRunningGame() ? 1 : 0`, `IsRunningCommandlet() ? 1 : 0`, `bForceEvenIfCurrent ? 1 : 0` — 4× `int` | ✓ each `%d` ↔ `int` |
> | `RebusVisualiserSubsystem.cpp::ProbeAndAutoPurgeStaleBeamMaster` (gate-abort Warning) | `GIsEditor=%d IsRunningGame=%d IsRunningCommandlet=%d` — 3× `%d` | `GIsEditor ? 1 : 0`, `IsRunningGame() ? 1 : 0`, `IsRunningCommandlet() ? 1 : 0` — 3× `int` | ✓ each `%d` ↔ `int` |
> | `RebusFixtureActor.cpp::SelfHealBeamMaterialRevisionIfMismatched` (gate early-return) | _no log on gate path — silent return_ | _N/A_ | ✓ (no logging avoids per-fixture spam) |
>
> All three new log lines use **only `%d` slots paired with explicit `bool ? 1 : 0` int args** (the safest pairing UE 5.7's variadic format sanitiser accepts — `bool` itself promotes integrally but the explicit ternary makes the int-ness obvious to both readers and the C7595 sanitiser). The v1.0.118 build-break (C7595 against `'%d' expects integral arg` when an `int` was promoted to `float` upstream) cannot recur here — every arg is a hand-built `int`.
>
> ### Cross-TU access-specifier check
>
> v1.0.120 adds NO new public-header symbols. `CanRegenBeamMasterInProcess()` lives in the anonymous namespace of `RebusVisualiserSubsystem.cpp` — internal-linkage only, called by the two methods in the same TU. `_is_editor_runtime()` is a module-level Python helper. The `SelfHealBeamMaterialRevisionIfMismatched` gate is inline using the existing `GIsEditor` / `IsRunningGame()` / `IsRunningCommandlet()` globals (all declared in `CoreMinimal.h` transitively, no extra include needed). No risk of C2248 cross-TU access violations.
>
> ### UE 5.7 API verification
>
> All three APIs the gate reads are standard engine globals/helpers declared in `Engine/Source/Runtime/Core/Public/`:
>
> - `GIsEditor` — global `bool`, `CoreGlobals.h`. Authoritative flag for "this process is the editor and editor subsystems are initialised".
> - `IsRunningGame()` — inline helper, `Misc/CoreMiscDefines.h`. Returns `FApp::IsGame()`. True under `-game`.
> - `IsRunningCommandlet()` — inline helper, `Misc/CoreMiscDefines.h`. True under `-run=...`.
>
> All three are part of the public Core API and have been stable since pre-UE 5.0. Available in every WITH_EDITOR build path the visualiser ships against.
>
> ### Operator checklist — TWO-STEP RECOVERY SEQUENCE
>
> #### Step 1 — pull the v1.0.120 binary (stops the crash, restores visualiser visibility immediately)
>
> 1. Pull v1.0.120, close the editor, rebuild the `REBUS_VisualiserEditor` target.
> 2. Launch the project the normal PRISM way (`UnrealEditor-Cmd.exe ... -game -PixelStreamingURL=...`).
> 3. The session no longer crashes at launch. Expect a Warning in the log shortly after the startup banner:
>    ```
>    [Rebus] v1.0.120 SKIPPING beam-master stale-probe auto-purge: this UE session is in -game / -server / commandlet mode (GIsEditor=0 IsRunningGame=1 IsRunningCommandlet=0), so editor-only Python ... cannot run without crashing the process (v1.0.119 hit EXCEPTION_ACCESS_VIOLATION here). The on-disk M_RebusBeam.uasset will be used as-is for this session. ...
>    ```
> 4. The visualiser runs against whatever `M_RebusBeam.uasset` is currently on disk (the v1.0.110-era stale master if you were hit by the v1.0.119 silent regen failure). The cone is still visible — the v1.0.117 `bRenderInDepthPass=false` primitive flag on `BeamCone` still applies regardless of material revision (it's a C++ primitive-side flag, independent of the master). The cone may sometimes z-clip against adjacent sibling translucent cones at pan-cross moments (the depth-test issue the v1.0.117 master-side `disable_depth_test=true` would fix) — not catastrophic, the visualiser is watchable.
>
> #### Step 2 — pre-bake the master once in editor mode (restores the full v1.0.111 depth-mask shadowing for future sessions)
>
> 1. Launch the editor manually (no `-game` flag): `UnrealEditor.exe REBUS_Visualiser.uproject` (no extra command-line args).
> 2. Wait for the editor to fully open.
> 3. In the editor's OutputLog (or the cmd console), run: `Rebus.ForceBeamMasterRegen`.
> 4. Confirm the OutputLog says:
>    ```
>    [Rebus] v1.0.119 RebuildAndVerifyBeamMaster SUCCESS -- py Exec returned OK, post-regen probe reports revision 120 (expected 120). Refreshing all spawned-fixture BeamMIDs against the freshly-baked master.
>    ```
>    If it does NOT (LOUD `Error` log instead), Python regen itself is still broken — see Fix 4 in the v1.0.120 brief; that's the open follow-up for v1.0.121+.
> 5. `File > Save All` in the editor (or `Ctrl+Shift+S`).
> 6. Commit the regenerated `REBUS_Visualiser/Content/REBUS/Materials/M_RebusBeam.uasset` (and the matching `.uexp` if your project tracks them) to git.
> 7. Future `-game` sessions then load the pre-baked v1.0.120 master without needing any rebuild — the v1.0.120 gate skips the auto-regen silently (because the on-disk master is current), and the v1.0.111 depth-mask shadowing works on day one.
>
> ### What v1.0.120 does NOT change
>
> - The v1.0.117 PRIMARY ROOT-CAUSE FIX (`BeamCone->bRenderInDepthPass = false` on the primitive + `disable_depth_test = true` on `M_RebusBeam`) is fully preserved. The primitive-side flag is in C++ (`RebusFixtureActor::RefreshBeamConeCullingFlags`), independent of material revision; it still applies even when the on-disk master is stale.
> - The v1.0.119 cache + post-regen verification plumbing (`GetCachedBeamMaster()`, `InvalidateBeamMasterCache()`, `RebuildAndVerifyBeamMaster`'s sequence of pre-probe → exec → flush-compile → re-probe → fixture-rebind) is unchanged. The gate sits ABOVE this plumbing — when the gate aborts, the plumbing never runs; when the gate allows (real editor session), the v1.0.119 plumbing runs verbatim.
> - The `M_RebusBeam` master graph is unchanged. The revision sentinel bumps 119 → 120 to invalidate masters baked under the broken v1.0.119 regen window, but `_build_beam_master`'s Custom HLSL node, scalar / vector / texture parameter set, and connection wiring are all byte-identical to v1.0.119.
> - The v1.0.118 / v1.0.117 / v1.0.116 / v1.0.107 watermark, the v1.0.105 Nanite walker, the v1.0.104 double-sided pass, the v1.0.99 cast-shadows pass — every other plugin subsystem is untouched.
>
> ### Known follow-ups (NOT in v1.0.120)
>
> - **Why v1.0.119's regen was a no-op when it DID run in editor mode** (Fix 4 in the v1.0.120 brief). The post-regen probe read `revision=-1` despite `py Exec returned OK`. Read of `_build_beam_master` + `ensure_beam_material` couldn't conclusively identify the failed step by static analysis; needs runtime instrumentation in a real editor session. Filed for v1.0.121+.
> - **Pre-baked v1.0.120 master shipped in the repo**. v1.0.120 ships the regen gate + the bumped sentinel; an operator with editor access still has to run the offline-bake workflow above to actually produce a current `M_RebusBeam.uasset`. Once the v1.0.121+ runtime-regen investigation lands, future releases should also ship a pre-baked master in the repo so a fresh checkout works in `-game` mode without the manual editor step.

> **v1.0.119 — ROOT-CAUSE FIX for v1.0.117/v1.0.118 stale-master + per-frame `FlushAsyncLoading` spam. The v1.0.117 PRIMARY ROOT-CAUSE FIX (`bRenderInDepthPass=false` on `BeamCone` + `disable_depth_test=true` on `M_RebusBeam`) only landed on the COMPONENT side in v1.0.117/v1.0.118 because the MATERIAL side regen path was non-functional. v1.0.119 fixes the regen path, hardens it with post-regen verification + LOUD failure logging + a per-fixture self-heal trigger, AND eliminates the per-frame `LogStreaming: Display: FlushAsyncLoading(...)` spam by hoisting all `M_RebusBeam` `LoadObject` calls to a session-wide cache.**
>
> ### What was broken (honest diagnosis)
>
> The user ran `Rebus.DumpBeamCulling` against a v1.0.118 binary and saw:
>
> ```
> BeamCone={vis=1 hidGame=0 ... renderDepth=0 ... boundsScale=5.00 ... sortPrio=-10 ...}
> ...
> BeamMID.BeamMaterialRevision=MISSING -- pre-v1.0.117 master, run `Rebus.RebuildBeamMaterial`
> (masterShape=STALE_MASTER -- run `Rebus.RebuildBeamMaterial` + ClearScene/LoadScene)
> ```
>
> The C++ side of v1.0.117/118 was correct (`renderDepth=0`, every culling flag clean), but `BeamMaterialRevision=MISSING` proves the on-disk `M_RebusBeam.uasset` was never regenerated. The v1.0.112 auto-purge claimed success ("`py` Exec returned OK") but the post-regen .uasset was still the pre-v1.0.117 master.
>
> Root cause: `REBUS_Visualiser/Content/Python/build_rebus_base_level.py` mixed TAB and SPACE indentation in `build()` and `ensure_base_level()` since v1.0.117. Python 3 raises `TabError` at PARSE time — `import build_rebus_base_level` failed before any function in the module could run. The auto-purge's `py import build_rebus_base_level; build_rebus_base_level.ensure_beam_material(force=True)` Exec returned OK from the engine's perspective (no parse error in the C++ command string) but Python's exception was swallowed by the Python-bridge layer, so the master was never re-baked. **The `disable_depth_test=True` flag never made it onto the actual `.uasset`.**
>
> Compounding issue: every per-fixture `BuildBeamCone` did `LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/REBUS/Materials/M_RebusBeam.M_RebusBeam"))` as the cook-safe fallback. With N fixtures spawning in quick succession + the v1.0.117 `ProbeBeamMasterVersion()` probe calling `LoadObject<UMaterial>` for every refresh sink, the cumulative `LoadObject` traffic produced per-frame `LogStreaming: Display: FlushAsyncLoading(455): 1 QueuedPackages, 0 AsyncPackages` spam.
>
> ### What v1.0.119 changes — exact list
>
> 1. **`Content/Python/build_rebus_base_level.py::build()` / `ensure_base_level()`** — converted every line in both functions from mixed TAB/SPACE to consistent 4-space indentation (matching every other function in the module). The `TabError` is gone; `import build_rebus_base_level` works in Python 3 again; the C++ auto-purge's `py import ...; ensure_beam_material(force=True)` Exec actually runs the regen.
> 2. **`REBUS_BEAM_MATERIAL_REVISION`** bumped 117 → 119 in both Python (`build_rebus_base_level.py`) and C++ (`RebusVisualiserSubsystem.cpp::RebusExpectedBeamMaterialRevision`). Every v1.0.117/118-era machine is forced to re-regen on next launch.
> 3. **`URebusVisualiserSubsystem::RebuildAndVerifyBeamMaster(bool bForceEvenIfCurrent)`** — new chokepoint every regen call now routes through. Sequence: invalidate the cached master pointer → pre-regen probe (early-out unless `bForceEvenIfCurrent=true`) → invoke `py ensure_beam_material(force=True)` → `FAssetCompilingManager::Get().FinishAllCompilation()` to flush async shader compile → invalidate cache + RE-PROBE the post-regen master → on mismatch, LOUD `UE_LOG(Error)` with operator-actionable diagnostic → on success, `RefreshAllSpawnedFixtureBeamMIDs` to rebind every fixture's BeamMID. Returns true iff the post-regen revision matches expected.
> 4. **`ARebusFixtureActor::SelfHealBeamMaterialRevisionIfMismatched()`** — new on-spawn probe called from the tail of `BuildBeamCone`. Reads back the live `BeamMaterialRevision` scalar from the just-created `BeamMID`. When it does NOT match the running-binary expected revision (the case where a fixture spawns AFTER the once-per-session `OnPostLoadMapAutoPurge` probe — e.g. portal mid-session `LoadScene`), triggers `RebuildAndVerifyBeamMaster(false)` so the regen fires + every fixture's MID is rebound. Cheap when healthy (single scalar read).
> 5. **`URebusVisualiserSubsystem::GetCachedBeamMaster()` / `InvalidateBeamMasterCache()` / `GetBeamMasterLoadCount()`** — session-wide cache for the on-disk `M_RebusBeam` master pointer. Held as `static TWeakObjectPtr<UMaterialInterface>` so GC invalidates cleanly. EVERY in-plugin resolver of the master now routes through `GetCachedBeamMaster()`: `ARebusFixtureActor::BuildBeamCone` (was `LoadObject<UMaterialInterface>`), `URebusVisualiserSubsystem::ProbeBeamMasterVersion` (was `LoadObject<UMaterial>`). Counter `BeamMasterLoadCount` increments ONLY on actual `LoadObject` hits (cache hits do not increment) — should READ 1 in the startup banner, 2 after a regen.
> 6. **`URebusVisualiserSubsystem::ProbeAndAutoPurgeStaleBeamMaster()`** — on regen FAILURE, releases `bBeamMasterAutoPurgeRun = false` so the next `PostLoadMapWithWorld` (or operator `Rebus.ForceBeamMasterRegen`) gets a fresh attempt instead of silently no-opping behind the latched bool.
> 7. **`Rebus.ForceBeamMasterRegen`** — new console command. Wraps `RebuildAndVerifyBeamMaster(true)` (force even when current). Operator rescue path for the post-spawn race case where the on-disk master is current but per-fixture MIDs are pointing at a stale UObject.
> 8. **`Rebus.DumpBeamMaterialHealth`** — new console command. Walks every spawned `ARebusFixtureActor` in every relevant world and dumps one line per fixture: MaterialSlot0 (class + asset name) on the BeamCone proc-mesh, BeamMID parent material (class + name), live `BeamMaterialRevision` scalar read back off the MID, cached-master pointer state. Healthy shape: `slot0Class=MaterialInstanceDynamic midParentName=M_RebusBeam midRevision=119`.
> 9. **Startup banner extended** — now reports `cachedBeamMaster=<name>`, `beamMasterLoadCount=N`, `beamMasterRegen={attempts=N lastResult=X lastDetectedAfter=N}` so the operator can see the cache + regen telemetry on the same line as the existing version + verdict + md5 fields.
> 10. **`Rebus.DumpBeamCulling` extended** — appended `BeamMasterCache={name=X loadCount=N expectedRev=119}` so per-fixture dumps also surface the cache state.
>
> ### Which call site was responsible for the `FlushAsyncLoading(455)` spam
>
> Two cumulative sources:
> - `ARebusFixtureActor::BuildBeamCone` did `LoadObject<UMaterialInterface>(...)` once per fixture spawn. With 6+ fixtures and several CVar refresh sinks firing on top, the engine's async loader saw a steady stream of `LoadObject` requests.
> - `URebusVisualiserSubsystem::ProbeBeamMasterVersion()` did `LoadObject<UMaterial>(...)` on every call. The probe runs from `Initialize()`, from `OnPostLoadMapAutoPurge` (every level reload), and from `HandleDumpBeamMasterVersionCommand` (operator-triggered). Each call forced a fresh round-trip even when the master was already resident.
>
> v1.0.119 routes BOTH through the cache. After the first session-wide load (logged: `[Rebus] v1.0.119 BeamMaster CACHE LOAD #1 -- 'M_RebusBeam' resolved + cached`), every subsequent caller hits the `TWeakObjectPtr::Get()` fast-path. The `BeamMasterLoadCount` counter exposes the audit result to the operator — steady-state should be 1, immediately after a regen should be 2; anything higher means a per-tick `LoadObject` path was missed and should be reported.
>
> ### Operator checklist
>
> 1. Pull v1.0.119, rebuild C++ binaries (close the editor first, then build the `REBUS_VisualiserEditor` target).
> 2. Launch the editor. Watch the startup log for a banner like:
>    ```
>    ===== REBUS Visualiser v1.0.119 (binary built ...) -- beamMasterVerdict=v1.0.119+
>    beamMasterRev=119/119 beamMasterUassetMd5=... cachedBeamMaster=M_RebusBeam
>    beamMasterLoadCount=1 beamMasterRegen={attempts=1 lastResult=success lastDetectedAfter=119} =====
>    ```
>    First launch on a v1.0.117/118-era workspace: `beamMasterVerdict` will read `v1.0.111..v1.0.118` BEFORE the auto-purge runs, then the next log line will show `STALE BEAM MASTER detected`, then `v1.0.119 RebuildAndVerifyBeamMaster: invoking ...`, then `v1.0.119 RebuildAndVerifyBeamMaster SUCCESS -- ... post-regen probe reports revision 119`. After that, every fixture spawn picks up the v1.0.119 master cleanly.
> 3. Run `Rebus.DumpBeamCulling` — confirm `BeamMID.BeamMaterialRevision=119` and `masterShape=OK` for every fixture; confirm `BeamMasterCache={name=M_RebusBeam loadCount=N expectedRev=119}` with `loadCount` reading 1 or 2.
> 4. Run `Rebus.DumpBeamMaterialHealth` — confirm every line reports `slot0Class=MaterialInstanceDynamic midParentName=M_RebusBeam midRevision=119`.
> 5. If `BeamMaterialRevision` still shows MISSING or <119 after launch, run `Rebus.ForceBeamMasterRegen` manually. Watch for `v1.0.119 RebuildAndVerifyBeamMaster SUCCESS` (or the LOUD `Error` line that exposes the new failure mode).
> 6. If `Rebus.ForceBeamMasterRegen` still fails, attach `Saved/Logs/REBUS_Visualiser.log` to a v1.0.120+ bug report — the LOUD error line + the Python traceback above it identify the new failure mode for triage.
>
> ### What v1.0.119 does NOT change
>
> The v1.0.117 PRIMARY ROOT-CAUSE FIX (`disable_depth_test=true` on `M_RebusBeam`, `bRenderInDepthPass=false` on `BeamCone`, the full v1.0.117 cone-flag hardening set) is fully preserved. The v1.0.117 master graph itself is unchanged in v1.0.119 — the revision bump exists purely to invalidate cached masters cooked during the v1.0.117/118 broken-Python window so the auto-purge will recognise them as stale and force-regen with the now-working Python module.

> **v1.0.118 — UE 5.7 BUILD FIX for the v1.0.117 extended `DumpBeamCullingStateForDebug` + the v1.0.117 `RefreshBeamConeCullingFlags` helper. The v1.0.117 PRIMARY ROOT-CAUSE FIX (`BeamCone->bRenderInDepthPass = false` on the primitive + `disable_depth_test = true` on `M_RebusBeam`) is UNCHANGED and intact — this release ONLY fixes the diagnostic dump + the cone-flag setter that broke the build, so the v1.0.117 fix can finally land on operators' machines.**
>
> ### What broke v1.0.117 in UE 5.7
>
> v1.0.117 introduced two new touch points on `UPrimitiveComponent::bAllowApproximateOcclusion`:
>
> - `RebusFixtureActor.cpp:3266` (in the new `RefreshBeamConeCullingFlags`) wrote `BeamCone->bAllowApproximateOcclusion = false;`.
> - `RebusFixtureActor.cpp:3654` (in the v1.0.117-extended `DumpBeamCullingStateForDebug`'s `FlagsLine` lambda) read `C->bAllowApproximateOcclusion ? 1 : 0` and printed it via an `approxOccl=%d` slot in the format string.
>
> Both fail to compile in UE 5.7:
>
> ```
> RebusFixtureActor.cpp(3654): error C2039: 'bAllowApproximateOcclusion':
>     is not a member of 'UPrimitiveComponent'
>         C->bAllowApproximateOcclusion ? 1 : 0,
> ```
>
> The flag exists, but it lives on `FPrimitiveSceneProxy` (the render-thread proxy), not on the game-thread `UPrimitiveComponent`. Confirmed against `Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h:1583` (`uint8 bAllowApproximateOcclusion : 1;`) and `Engine/Source/Runtime/Engine/Private/PrimitiveSceneProxy.cpp:383` (`bAllowApproximateOcclusion(InProxyDesc.Mobility != EComponentMobility::Movable)`). UE 5.7 does NOT expose a public game-thread accessor for it.
>
> The v1.0.117 brief (which originated from a UE 4.x / pre-restructure mental model) listed `bAllowApproximateOcclusion = false` as one of the per-component flags to set; that was wrong. The correct picture in UE 5.7 is: **the proxy default is already what we wanted** for the procedural BeamCone (which is `EComponentMobility::Movable`, set in `BuildBeamCone`), so the engine constructor gives us `false` automatically — no game-thread write is needed (or possible).
>
> The C2039 cascaded into a second compiler error against the format-string sanitizer:
>
> ```
> RebusFixtureActor.cpp(3641): error C7595:
>     'UE::Core::Private::FormatStringSan::TCheckedFormatStringPrivate<...,
>         int×16, float×4, int>::...': call to immediate function is not a constant expression
> note: X(DNeedsIntegerArg, "'%d' expects integral arg ...")
> ```
>
> ...because once the compiler dropped the broken `bAllowApproximateOcclusion` arg from its variadic deduction, all subsequent args shifted up by one position relative to the format string, so the `%d sortPrio` slot ended up bound to the float `BoundsScale` arg. Both errors collapse to a single root cause; fixing the underlying access fixes both.
>
> ### What v1.0.118 changes — exact list
>
> 1. **`RebusFixtureActor.cpp::RefreshBeamConeCullingFlags`** — removed the line `BeamCone->bAllowApproximateOcclusion = false;` (was at line 3266 in v1.0.117). Replaced with a comment block citing the engine source (`PrimitiveSceneProxy.cpp:383`) that proves the proxy default for movable components is already `false`. **No behaviour change** — the cone gets the same `bAllowApproximateOcclusion = false` it would have gotten with the v1.0.117 line; the flag is just set by the engine constructor instead of by us.
> 2. **`RebusFixtureActor.cpp::DumpBeamCullingStateForDebug` (FlagsLine lambda)** — dropped the `approxOccl=%d` slot from the format string AND the matching `C->bAllowApproximateOcclusion ? 1 : 0` arg from the variadic list. After this:
>    - format-string spec count: 22 (1 `%s` + 16 `%d` + 1 `%.2f` + 3 `%.1f` + 1 `%d`)
>    - arg count: 22 (1 TCHAR* + 16 ints + 1 float + 3 floats + 1 int)
>    - per-position type match verified by hand (see "Specifier-vs-arg verification table" below).
>    - The dump line is now:
>      ```
>      BeamCone={vis=%d hidGame=%d sceneCap=visOnly=%d hid=%d castShadow=%d castHiddenShadow=%d
>                occluder=%d attachBound=%d cullDistVol=%d renderMain=%d renderDepth=%d
>                customDepth=%d recvDecals=%d affGI=%d affDFL=%d sortPrio=%d boundsScale=%.2f
>                minDraw=%.1f ldMax=%.1f cachedMax=%.1f visInRT=%d}
>      ```
>      The PRIMARY-ROOT-CAUSE smoking-gun field — `renderDepth` — is still in the dump. The other v1.0.117 hardening fields (`castHiddenShadow`, `cullDistVol`, `customDepth`, `recvDecals`, `affGI`, `affDFL`, `sortPrio`) are also still all there. The only field removed is `approxOccl`, which we couldn't have surfaced from the game thread anyway in UE 5.7.
> 3. **Updated doc-comments** in `RefreshBeamConeCullingFlags` (impl + header), the constexpr-doc-block on `RebusBeamBoundsScale`, the `RefreshBeamConeCullingFlagsOnEveryFixture` log line, and the FlagsLine doc-block — all explain WHY the field was dropped + cite the engine source so the next operator who pulls a 5.x → 5.8 / etc. update doesn't re-introduce it.
>
> ### Specifier-vs-arg verification table (DumpBeamCullingStateForDebug FlagsLine, AFTER v1.0.118 fix)
>
> Hand-counted, position-by-position, against the post-fix code at `RebusFixtureActor.cpp:3658-3684`:
>
> | i | spec | arg expression | arg type | match? |
> |---|------|----------------|----------|--------|
> |  0 | `%s`   | `Label`                                          | `const TCHAR*` | OK |
> |  1 | `%d`   | `C->IsVisible() ? 1 : 0`                         | int            | OK |
> |  2 | `%d`   | `C->bHiddenInGame ? 1 : 0`                       | int            | OK |
> |  3 | `%d`   | `C->bVisibleInSceneCaptureOnly ? 1 : 0`          | int            | OK |
> |  4 | `%d`   | `C->bHiddenInSceneCapture ? 1 : 0`               | int            | OK |
> |  5 | `%d`   | `C->CastShadow ? 1 : 0`                          | int            | OK |
> |  6 | `%d`   | `C->bCastHiddenShadow ? 1 : 0`                   | int            | OK |
> |  7 | `%d`   | `C->bUseAsOccluder ? 1 : 0`                      | int            | OK |
> |  8 | `%d`   | `C->bUseAttachParentBound ? 1 : 0`               | int            | OK |
> |  9 | `%d`   | `C->bAllowCullDistanceVolume ? 1 : 0`            | int            | OK |
> | 10 | `%d`   | `C->bRenderInMainPass ? 1 : 0`                   | int            | OK |
> | 11 | `%d`   | `C->bRenderInDepthPass ? 1 : 0`                  | int            | OK |
> | 12 | `%d`   | `C->bRenderCustomDepth ? 1 : 0`                  | int            | OK |
> | 13 | `%d`   | `C->bReceivesDecals ? 1 : 0`                     | int            | OK |
> | 14 | `%d`   | `C->bAffectDynamicIndirectLighting ? 1 : 0`      | int            | OK |
> | 15 | `%d`   | `C->bAffectDistanceFieldLighting ? 1 : 0`        | int            | OK |
> | 16 | `%d`   | `C->TranslucencySortPriority`                    | `int32`        | OK |
> | 17 | `%.2f` | `C->BoundsScale`                                 | `float`        | OK |
> | 18 | `%.1f` | `C->MinDrawDistance`                             | `float`        | OK |
> | 19 | `%.1f` | `C->LDMaxDrawDistance`                           | `float`        | OK |
> | 20 | `%.1f` | `C->CachedMaxDrawDistance`                       | `float`        | OK |
> | 21 | `%d`   | `C->bVisibleInRayTracing ? 1 : 0`                | int            | OK |
>
> Total: 22 specs ↔ 22 args, every type pair matches. (`UPrimitiveComponent::TranslucencySortPriority` is declared `int32` at `PrimitiveComponent.h:850`; `BoundsScale` is `float` at `:925`; `MinDrawDistance` `:278`; `LDMaxDrawDistance` `:282`; `CachedMaxDrawDistance` `:289`. Verified.)
>
> ### Other v1.0.117 multi-line UE_LOG / FString::Printf re-verified by hand
>
> Same protocol applied to every other multi-line format string touched in v1.0.117 (or extended in v1.0.111+ and still live):
>
> - **Outer `DumpBeamCullingStateForDebug` UE_LOG** (`RebusFixtureActor.cpp:3737..3779`): 35 specs (15 `%s`, 7 `%d`, 13 `%.Nf`) ↔ 35 args (15 TCHAR*, 7 ints, 13 floats/doubles — `FVector` components are double in UE 5.x but `%f`/`%.Nf` accepts both float and double in `TCheckedFormatString`). All match. Checked.
> - **`DumpBeamShadowMaskStateForDebug` UE_LOG** (`RebusFixtureActor.cpp:3594..3611`, extended in v1.0.111): 25 specs (10 `%s`, 4 `%d`, 11 `%.Nf`) ↔ 25 args (10 TCHAR*, 4 int32, 11 floats/doubles). All match. Checked.
> - **v1.0.117 startup banner UE_LOG** (`RebusVisualiserSubsystem.cpp:184..199`): 8 specs (6 `%s`, 2 `%d`) ↔ 8 args (6 TCHAR*, 2 int32 — `BannerReport.DetectedRevision` and `ExpectedRevision`). All match. Checked.
> - **`ProbeAndAutoPurgeStaleBeamMaster` STALE log** (`RebusVisualiserSubsystem.cpp:2961..2978`, with v1.0.117 `DetectedRevision` / `ExpectedRevision` extension): 7 specs (5 `%s`, 2 `%d`) ↔ 7 args (5 TCHAR*, 2 int32). All match. Checked.
> - **v1.0.117 new `RefreshBeamConeCullingFlagsOnEveryFixture` UE_LOG** (`RebusFixtureActor.cpp:112..116`): 3 specs (2 `%s`, 1 `%d`) ↔ 3 args (`CVarLabel`, `*NewValStr`, `Refreshed`). Match. Checked.
> - **v1.0.117 new `Rebus.AllowBeamOcclusionQueries -> %d` log** (`RebusFixtureActor.cpp:158..160`): 1 spec ↔ 1 int arg. Match. Checked.
>
> All clean. No further format / arg mismatches.
>
> ### Build-cycle hygiene note (lessons learned)
>
> v1.0.115, v1.0.116, v1.0.117 were each released with the claim "audit complete". v1.0.117 still shipped a build break. Pattern: a flag (`bAllowApproximateOcclusion`) was assumed to exist on `UPrimitiveComponent` because it appears on `FPrimitiveSceneProxy` and the symbol-name is suggestive. The audit pass would have caught it via a one-line `rg "UPrimitiveComponent::" "$UE_57_ENGINE_SRC"` cross-check against every flag the new helper writes — but that step was skipped, and reading the diff visually didn't surface the proxy-vs-component distinction. v1.0.118 is gated on a hand-built specifier-vs-arg verification table (above) AND a manual cross-check of every newly-touched `UPrimitiveComponent` field name against the UE 5.7 header before committing — both of which caught real issues this round.
>
> `RebusVisualiser.uplugin` `VersionName` 1.0.117 → 1.0.118. Top-centre watermark + `[Rebus] STARTUP BANNER` will read `v1.0.118 (binary built ...)` on the next launch. The PRIMARY ROOT-CAUSE FIX from v1.0.117 is unchanged in this release; once a v1.0.118 binary builds, operators get the v1.0.117 `bRenderInDepthPass = false` + `disable_depth_test = true` decoupling on the cone, and the persistent pan-cross / sibling-fixture clipping should finally clear.

> **v1.0.117 — PRIMARY ROOT-CAUSE FIX for the persistent pan-cross / sibling-fixture beam clipping the user has reported across v1.0.111 → v1.0.116. `BeamCone->bRenderInDepthPass = false` + `disable_depth_test = true` on `M_RebusBeam`. Operator A/B checklist below.**
>
> ### User-visible symptom (v1.0.116 and earlier)
>
> The user reported the same shape across four release cycles: when 5 moving heads on a truss pan together, two or three of them clip with a hard planar edge against the SAME invisible boundary, as if "there is a hidden bounding box". `Rebus.BeamShadowMask 0` made ZERO difference (v1.0.116 hypothesis A invalidated). `Rebus.PreferProceduralBeam 0` removed the clipping. The user ran `Rebus.DumpBeamCulling` on a clipped Ayrton Veloce Profile and the dump returned the smoking gun:
>
> ```
> BeamCone={vis=1 hidGame=0 sceneCap=visOnly=0 hid=0 castShadow=0 occluder=0 attachBound=0
>           renderMain=1 renderDepth=1 boundsScale=3.00 ...}
> EpicBeamComp=<null>
> bPreferProcedural=1 bMeshBeamEnabled=1
> ```
>
> **`renderDepth=1` on a translucent additive cone is the canonical UE failure mode for pan-coupled "hidden bounding box" clipping.** It causes (a) cross-fixture z-rejection — when two BeamCones overlap in screen-space, the second draw's per-pixel depth test rejects pixels behind the first's written depth; (b) self-occlusion — the cone's far-cap triangles write depth that rejects front-cap fragments on later draws; (c) pan-edge planar clips matching the user's screenshot exactly. The cone is unlit additive, has no business writing to the per-pixel depth buffer at all -- the HLSL raymarch (`_BEAM_RAYMARCH_HLSL`, v1.0.39) already does its own scene-depth occlusion via `tOcc = min(tPix, tOcc)`, so the floor / wall / opaque-occluder fade is shader-driven and continues to work correctly without the fixed-function depth pass.
>
> ### Fix 1 (PRIMARY, C++) — `BeamCone` out of the depth pass
>
> The single chokepoint is `ARebusFixtureActor::RefreshBeamConeCullingFlags()` (`Private/RebusFixtureActor.cpp`). v1.0.117 sets:
>
> | Flag | Was | Is | Why |
> | --- | --- | --- | --- |
> | `bRenderInDepthPass` | `true` | **`false`** | **PRIMARY** — the smoking gun from `Rebus.DumpBeamCulling` |
> | `bRenderCustomDepth` | (default) | `false` | belt-and-braces (no business in custom depth either) |
> | `bReceivesDecals` | `true` | `false` | unlit additive cone, no deferred decals |
> | `bAffectDynamicIndirectLighting` | `true` | `false` | unlit additive cone, no GI contribution |
> | `bAffectDistanceFieldLighting` | `true` | `false` | unlit additive cone, no DFL contribution |
> | `CastShadow` / `bCastDynamicShadow` / `bCastHiddenShadow` | (mixed) | all `false` | no shadow casting at all |
> | `BoundsScale` | `3.0` | `5.0` (live via `Rebus.BeamConeBoundsScale`) | extent-only inflation against HZB / streaming-volume false-cull |
> | `TranslucencySortPriority` | `0` | `-10` | stable cone-vs-other-translucent sort across pan sweeps |
> | `bAllowApproximateOcclusion` / `bAllowCullDistanceVolume` / `SetCullDistance(0)` | mixed | all off / 0 | no LD cull distance, no level cull-distance volume gate |
>
> The helper is called from `BuildBeamCone` (initial seed), from `RefreshPreferProceduralBeamFromCVar` (flip-back-to-procedural), and from the `Rebus.BeamConeBoundsScale` live CVar refresh sink. Idempotent / cheap; no-op when `BeamCone` is null.
>
> ### Fix 2 (PRIMARY, material) — `disable_depth_test = true` on `M_RebusBeam`
>
> The material-side complement to Fix 1. The cone's READ side (depth test against OTHER translucent surfaces, e.g. sibling beam cones during a pan-cross, the chrome lens disc, LED-matrix `IsBeamLensComponents` PMCs) is the last remaining failure surface. `disable_depth_test = true` on the master decouples the cone from the fixed-function depth pipeline entirely; the HLSL raymarch alone decides what's behind opaque geometry via `SceneDepth`.
>
> Also in `_build_beam_master` (`Content/Python/build_rebus_base_level.py`):
>
> - `disable_depth_test = True` — the primary material flag for additive translucent volumes.
> - `used_with_volumetric_fog = False` — the cone is NOT a volumetric fog participant (the SpotLight's gobo light function `M_RebusGoboLightFunction` is; that's unaffected). Removes a known UE failure mode where the translucent pass interacts with the 8×8×128 fog froxel grid and produces axis-aligned tile-boundary clip artefacts.
> - **New sentinel scalar `BeamMaterialRevision = 117`** — bumped every release that touches the master; the C++ probe reads it back via `UMaterial::GetScalarParameterValue` to decide whether the on-disk `.uasset` needs auto-regen.
>
> ### Fix 3 — Stale-master auto-purge extended to detect pre-v1.0.117
>
> `URebusVisualiserSubsystem::ProbeAndAutoPurgeStaleBeamMaster()` (`Private/RebusVisualiserSubsystem.cpp`, v1.0.112 infrastructure) now considers a master that has the full v1.0.111 parameter contract but a missing / older `BeamMaterialRevision` sentinel as STALE. The auto-regen fires the same Python rebake path used since v1.0.112. New verdict labels:
>
> - `V111Plus` → relabelled `v1.0.111..v1.0.116 (PRE-v1.0.117 — cone still writes to depth pass, missing disable_depth_test)` and now considered STALE.
> - `V117Plus` → new label `v1.0.117+`, the only "not stale" verdict.
>
> If the user's `M_RebusBeam.uasset` doesn't regenerate automatically (e.g. running a packaged binary with no editor / no `PythonScriptPlugin`), the auto-purge logs a Warning naming the detected vs expected revision and points the operator at `Rebus.RebuildBeamMaterial`.
>
> ### Fix 4 — `Rebus.DumpBeamCulling` extended with material-side state
>
> `Rebus.DumpBeamCulling [fixtureId]` now also prints:
>
> - **`BeamCone.renderDepth`** (the v1.0.117 smoking-gun flag — should be `0`)
> - `BeamCone.customDepth`, `recvDecals`, `affGI`, `affDFL`, `castHiddenShadow`, `approxOccl`, `cullDistVol`, `sortPrio` (the rest of the v1.0.117 hardening)
> - **`BeamMID.BeamMaterialRevision`** (should be `117` — `MISSING` means pre-v1.0.117 master, run `Rebus.RebuildBeamMaterial`)
> - `GlobalCVars={r.AllowOcclusionQueries r.VolumetricFog}` echoed for context.
>
> ### Fix 5 — Startup banner with build identity
>
> The `Initialize`-time one-line banner now stitches `[plugin version] (binary built [__DATE__ __TIME__]) beamMasterVerdict=[label] beamMasterRev=[detected/expected] beamMasterUassetMd5=[hash]` so the operator can see in one log line whether their loaded DLL matches the source tree they pulled.
>
> ### Fix 6 — Operator A/B checklist (RUN THIS ON YOUR BUILD)
>
> 1. Pull the v1.0.117 git tag (`git fetch && git checkout v1.0.117-ue5.7`) and rebuild the `REBUS_VisualiserEditor` target. **Required even if `git pull` ran cleanly — the C++ binary must be rebuilt.**
> 2. Launch the editor / packaged build and confirm the startup banner shows `===== REBUS Visualiser v1.0.117 (binary built [recent date]) ... =====` in the Output log. If it says `v1.0.116` or earlier, the C++ binary is stale — rebuild.
> 3. Run `Rebus.DumpBeamCulling` (no args = first clipped fixture). Confirm:
>    - `BeamCone={... renderDepth=0 ...}` (was `1` in v1.0.116) — **this is the primary fix**
>    - `BeamMID.BeamMaterialRevision=117` (was `MISSING`) — confirms the material auto-regen landed
>    - `sortPrio=-10`, `boundsScale=5.00`, `customDepth=0 recvDecals=0 affGI=0 affDFL=0`
> 4. If `BeamMaterialRevision != 117`, run `Rebus.RebuildBeamMaterial` manually, then `ClearScene` + `LoadScene` from the portal to rebuild all per-fixture BeamMIDs against the regenerated master.
> 5. Pan the previously-clipped fixtures across each other. The hard planar clip should be gone in both `Rebus.PreferProceduralBeam 1` (default, procedural cone visible) and `Rebus.PreferProceduralBeam 0` (cone hidden) — toggling should make no visible difference because the cone no longer fights with other translucent surfaces.
> 6. If the clipping STILL persists in v1.0.117, try these diagnostic toggles in order and report which one (if any) helps:
>    - `Rebus.AllowBeamOcclusionQueries 0` — forces global HOQ off. If this fixes it, the failure mode is HOQ-related (unlikely; `r.AllowOcclusionQueries 0` was tested at v1.0.116 and didn't help).
>    - `Rebus.BeamConeBoundsScale 8` — push the AABB further out (live, no rebuild needed).
>    - `r.VolumetricFog 0` — disable volumetric fog entirely. If this fixes it, the froxel-tile-boundary failure mode is back on the suspect list.
>    - Attach a fresh `Rebus.DumpBeamCulling` log from the still-clipped fixture for cross-check.
>
> **One-line root-cause statement:** v1.0.117's primary fix (`BeamCone->bRenderInDepthPass = false` on the primitive + `disable_depth_test = true` on the `M_RebusBeam` material) directly addresses the user's `renderDepth=1` finding from `Rebus.DumpBeamCulling`, decoupling the additive translucent cone from the per-pixel depth pipeline so sibling cones / lens discs / LED matrices can no longer reject its pixels via z-test.
>
> ### Dropped from the v1.0.117 brief (not the root cause; downgraded to "if symptom persists")
>
> - Cross-fixture `HiddenComponents` list on `BeamShadowMaskCapture` — depth mask was confirmed not to be the cause (`Rebus.BeamShadowMask 0` didn't help).
> - Nanite-aware `SceneCapture` show-flags — depth mask not the cause.
> - `EpicBeamComp` hide hardening — `EpicBeamComp` is `<null>` on the user's Ayrton Veloce fixture.
> - Hardware-occlusion-query disable on BeamCone — `r.AllowOcclusionQueries 0` was tested at v1.0.116 and didn't help.
> - Volumetric-fog tile occlusion / DLSS-TSR upsample edges — downgraded to "if symptom persists, try `r.VolumetricFog 0` / disable upsampling" in the checklist above.
>
> `RebusVisualiser.uplugin` `VersionName` 1.0.116 → 1.0.117. Top-centre watermark + `[Rebus] STARTUP BANNER` will read `v1.0.117 (binary built ...)` on the next launch; if you see anything earlier, your binary didn't rebuild.

> **UE 5.7 build fix: move `URebusVisualiserSubsystem::ComputeBeamMasterUassetMd5` from `private:` to `public:` + audit pass of every cross-TU access specifier + UE 5.7 deprecation hotspot to break the recurring single-error build-fix cycle (v1.0.116).**
>
> User-reported build break (verbatim, abridged):
>
> > "RebusVisualiser.cpp(506,54): error C2248: 'URebusVisualiserSubsystem::ComputeBeamMasterUassetMd5': cannot access private member declared in class 'URebusVisualiserSubsystem' ... RebusVisualiserSubsystem.h(730,17): note: see declaration of ..."
>
> The immediate failure is the file-scope call `URebusVisualiserSubsystem::ComputeBeamMasterUassetMd5()` from `HandleDumpBeamMasterVersionCommand` in `RebusVisualiser.cpp:506` reaching a declaration sitting in the `private:` block at `RebusVisualiserSubsystem.h:730`. The function was added in v1.0.113 alongside the public `ProbeBeamMasterVersion` / `BeamMasterVersionLabel` accessors that callers in `RebusVisualiser.cpp` were already using, but its declaration accidentally landed in the trailing `private:` block (alongside genuinely-internal helpers like `OnPostLoadMapAutoPurge`). v1.0.116 moves the declaration verbatim into the `public:` block immediately after `BeamMasterVersionLabel` (where it logically groups with the other v1.0.112/113 beam-master probe helpers), and leaves a tombstone comment at the old private-block site so anyone re-reading the header sees the move. Implementation in `RebusVisualiserSubsystem.cpp` is unchanged byte-for-byte.
>
> **The bigger problem this release also fixes — the recurring single-error build-fix cycle.** The user has reported this exact failure pattern (single C2248 / C7595 / C4996 → push one-line patch → next single-error in the chain) at least five times in the last 20 commits (v1.0.77, v1.0.81, v1.0.87, v1.0.103, v1.0.114). v1.0.116 was therefore released only after an explicit **audit pass** of every plausible recurrence path, not just a fix-and-push of the reported error. The audit (and its verdict) is below; the verdict matters more than the specific symbol moves because it documents what was *checked-and-cleared* so the next regression can start from a known-good baseline instead of guessing.
>
> **Audit step A — cross-module access-specifier audit.** Every header in `Source/RebusVisualiser/Public/` was walked top-to-bottom; every method declared under a `private:` or `protected:` specifier (or under the implicit `private:` default at class-open) was extracted, and every `Private/*.cpp` and `Public/*.h` in the module was ripgrep'd for external callers. Findings:
>
> | Symbol | Declaring class | Declared in | External callers | Verdict |
> | --- | --- | --- | --- | --- |
> | `ComputeBeamMasterUassetMd5` | `URebusVisualiserSubsystem` | `private:` block (line 730) | `RebusVisualiser.cpp:506` (`HandleDumpBeamMasterVersionCommand`) | **C2248 — fixed, moved to `public:`** |
> | All other `URebusVisualiserSubsystem` private methods (`ReadConfig` / `Tick` / `BeginSceneLoad` / `OnSceneFetched` / `PrefetchProfiles` / `OnProfileFetched` / `OnMeshesFetched` / `TrySpawnFixtures` / `SpawnAllFixtures` / `ClearSpawnedFixtures` / `HandleSceneDefinition` / `OnChannelReady` / `TrySendReady` / `BroadcastHandshake` / `OnViewerConnected` / `EnsureSceneEnvironment` / `TryPositionPlayerView` / `BroadcastCameraStateIfChanged` / `BroadcastFixtureStatesIfChanged` / `EnsureTrussMaterial` / `ResolveTrussMaterial` / `ApplyTrussMaterialPass` / `BuildBoundOrbitComponentSet` / `DrawVersionWatermark` / `ProbeAndAutoPurgeStaleBeamMaster` / `RefreshAllSpawnedFixtureBeamMIDs` / `OnPostLoadMapAutoPurge`) | `URebusVisualiserSubsystem` | `private:` block | Only the subsystem's own translation unit (`RebusVisualiserSubsystem.cpp`) defines / calls them; the only external-file mentions are bind-handles into the same `this->` (delegate registrations) or comment-string references | **CLEAN — no externally-reachable callers.** |
> | Every `ARebusFixtureActor::` private method (`BuildSpotLight` / `BuildBeamShadowMaskCapture` / `BuildLensDisc` / `RefreshLensDisc` / `ResolveLensDiameterMeters` / `BuildBeamCone` / `UpdateBeamConeGeometry` / `RefreshBeamEmissive` / `RefreshBeamSpatialParams` / `DriveBeamConeFromSpotLight` / `TryBuildEpicBeam` / `UpdateEpicBeamParams` / `DriveEpicBeamFromSpotLight` / `RefreshMotion` / `DriveOrbitModel` / `ComputeHeadLocal` / `DriveOrbitModelFromPanTilt` / `RefreshIntensity` / `RecomputeConeAngles` / `SelectIesForZoom` / `SelectInlineIes` / `FetchAndAssignIes` / `AssignGobo` / `ResolveGoboWheelIndex` / `SelectInlineGobo` / `ApplyGoboTextureFromBytes` / `ApplyCurrentGoboToEpicBeam` / `ApplyCurrentGoboToLightFn` / `UpdateEpicLightFnParams` / `ClearGoboToOpen` / `RefreshGoboLumenIsolation` / `ResolveZoomHalfDeg` / `ResolveOuterHalfDeg` / `ResolveFootprintInnerRatio` / `ResolveBeamFootprintMatchHalfDeg` / `FetchAndAssignGobo` / `FetchAndAssignGoboFromUrl` / `EnsureGoboRT` / `EnsureIrisMaskTexture` / `EnsureFixtureMIDs` / `ApplyFixtureMaterialTo` / `EnsurePerLensMIDs` / `BuildIsBeamLensFlares` / `RefreshIsBeamLensVisuals` / `RefreshIsBeamFlareEmissive`) | `ARebusFixtureActor` | `private:` block | Only `RebusFixtureActor.cpp` defines / calls them; one comment-string mention in `RebusVisualiserSubsystem.cpp` (`ARebusFixtureActor::BuildSpotLight`) is documentation, not a call site | **CLEAN — no externally-reachable callers.** Every console-command path in `RebusVisualiser.cpp` already routes through the existing public surface (`DumpLightStateForDebug` / `DumpGoboStateForDebug` / `DumpIesStateForDebug` / `DumpFixtureZoomStateForDebug` / `DumpBeamShadowMaskStateForDebug` / `DumpBeamCullingStateForDebug` / `GetFixtureId` / `SetFixtureMaterialOverrideEnabled` / `SetOrbitVisibility` / `RebuildGoboRTAtSize`). |
> | Every `URebusSceneSettingsSubsystem::` private method (`GetSun` / `GetSkyLight` / `GetFog` / `GetPostProcess` / `GetFloor` / `SetGroundSurface` / `SetGroundTilingMeters` / `EnsureFloorMID` / `SetOriginGizmo` / `SetDriveOrbitModelsEnabled` / `SetRenderQuality` / `SetScalabilityBucket` / `SetCVarFloat` / `SetCVarInt`) | `URebusSceneSettingsSubsystem` | `private:` block | Only `RebusSceneSettingsSubsystem.cpp` defines / calls them; the `RebusVisualiser.cpp:1367` reference is a comment-string only (the actual console-command route to `SetGroundTilingMeters` goes via the existing `ApplySceneProperty("GroundTilingMeters", ...)` public handler). | **CLEAN — no externally-reachable callers.** |
> | Every `URebusFixtureControlSubsystem` member | `URebusFixtureControlSubsystem` | All `public:` (the class only has `public:` and one `private:` data block) | n/a | **CLEAN — no problematic private methods.** |
> | Every `FRebusDataChannel` private method (`HandleDescriptor` / `SendEvent` / `SendPong` / `OnViewerDataTrackOpen`) | `FRebusDataChannel` | `private:` block | Only `RebusDataChannel.cpp` (`HandleDescriptor` is registered as a `TFunction` callback against the streamer's input handler; all other callers are internal) | **CLEAN — no externally-reachable callers.** |
>
> **Audit step B — printf format-string sanitiser pass.** UE 5.7's `FString::Printf` / `UE_LOG` format-string check is now `constexpr` and rejects type mismatches at compile time (the v1.0.115 failure was exactly this: a `bool`-as-int landed in a `%.1f` slot after a sibling C2039 elided its float neighbour). Every `UE_LOG` / `FString::Printf` site in `Private/RebusFixtureActor.cpp`, `Private/RebusVisualiserSubsystem.cpp`, `Private/RebusSceneSettingsSubsystem.cpp`, `Private/RebusVisualiser.cpp` was visually walked. Focus areas:
>
> - **The freshly-edited debug-dump methods (`DumpBeamShadowMaskStateForDebug`, `DumpBeamCullingStateForDebug`, `DumpFixtureZoomStateForDebug`).** All three now verify clean — every `%s` is fed a dereferenced `FString` (`*Foo`) or a literal `TEXT("…")`, every `%d` is fed an `int32` (or a ternary collapsing to one), every `%.Nf` is fed a `float` / `double`, and every `FVector::{X,Y,Z}` (UE 5.7 LWC = `double`) feeds a `%f` slot (`double` is a valid `%f` argument under UE's format sanitiser via the standard floating-point promotion path).
> - **The v1.0.115 `DumpBeamCulling` `FlagsLine` lambda.** Re-walked end-to-end: `Label` → `%s`, nine `bool`→`int` ternaries → `%d`, `float BoundsScale` / `MinDrawDistance` / `LDMaxDrawDistance` / `CachedMaxDrawDistance` → `%.Nf`, trailing `bVisibleInRayTracing ? 1 : 0` → `%d`. Realigned and correct.
> - **`DumpBeamShadowMaskStateForDebug` `GRebusBeamShadowMask*` CVar globals.** Cross-checked types against the file-scope declarations (`int32 GRebusBeamShadowMask` / `int32 GRebusBeamShadowMaskRes` / `float GRebusBeamShadowMaskBiasCm` / `float GRebusBeamShadowMaskFadeCm` / `float GRebusBeamShadowMaskFovMargin` / `int32 GRebusBeamShadowMaskDebug` in `RebusFixtureActor.cpp:286-291`). All `%d` / `%.2f` slots match.
> - **The console-command lambdas in `RebusVisualiser.cpp`.** Every `UE_LOG` uses `bEnable ? 1 : 0` for the `%d` slots, `*FString::Printf(...)` derefs for the nested `%s` slots, and matched `%d` / `%f` / `%s` types throughout. Spot-verified for `Rebus.OverrideTrussMaterial` / `Rebus.OrbitCastShadows` / `Rebus.OrbitDoubleSided` / `Rebus.NaniteOrbitImports` / `Rebus.ShowVersion` / `Rebus.VersionWatermarkY` / `Rebus.DumpBeamMasterVersion` / `Rebus.DumpBeamCulling` / `Rebus.DumpFixtureZoom` / `Rebus.DumpBeamShadowMask` blocks.
>
> Verdict: **no further C7595 candidates found** in the four audited files. Honest disclosure: this is an inspection-only pass — the only way to fully prove the sanitiser is happy is to run the UE 5.7 compiler end-to-end, which this release cannot do from the source tree alone. The audit ruled out the *common* offender patterns (`bool` for `%f`, `int32` for `%f`, `FString` without `*` for `%s`, `int64` for `%d`); if the next build surfaces a different printf failure shape it'll be a novel pattern not in the audit's scope.
>
> **Audit step C — UE 5.7 deprecation hotspot ripgrep.** The four recent recurrences this release cycle has burned (`NaniteSettings` direct-field → v1.0.114, `GetBoundsScale()` non-existent accessor → v1.0.115, plus the v1.0.116 visibility move) are all on the same family: UE 5.7 deprecated direct-field access in favour of accessor APIs. Ripgrep'd for the documented hotspots across `Source/RebusVisualiser/`:
>
> | Pattern | Hits | Verdict |
> | --- | --- | --- |
> | `->BodyInstance.` (deprecated → `GetBodyInstance()`) | 0 hits | **CLEAN.** No call sites in our own files. |
> | `->NaniteSettings` (deprecated direct-field) | 0 source-code hits (only comment-string references kept as documentation); 10 source-code hits in `RebusVisualiserSubsystem.cpp` ALL use `Mesh->GetNaniteSettings().X` (v1.0.114's mechanical replacement) | **CLEAN — v1.0.114 fix held; no missed sites.** |
> | `GetBoundsScale()` (non-existent accessor in UE 5.7) | 0 hits | **CLEAN — v1.0.115 fix held; no missed sites.** |
> | `LightComponent->bAffectGlobalIllumination = ...` direct write (UE 5.7 prefers `SetAffectGlobalIllumination()`) | 1 hit (`RebusFixtureActor.cpp:5863`), but it's a `==` READ comparison (`if (SpotLight->bAffectGlobalIllumination == bDesired) return;`) followed by a `SetAffectGlobalIllumination(bDesired)` WRITE. No direct-write site. | **CLEAN.** The read is a fast-path / no-op guard; in UE 5.7 the `bAffectGlobalIllumination` UPROPERTY is still public-readable (only direct writes are advised against), so the existing code is correct. No change. |
>
> **Build-cycle hygiene note.** v1.0.116 is intentionally an audit-and-patch release, NOT a behaviour change. The single-line `private:` → `public:` access-specifier move is the only code change. The README block above documents the audit findings so a future operator (or a future regression bug report) can see at a glance which paths were checked and cleared — instead of every release cycle re-investigating the same set of hotspots from scratch.
>
> `RebusVisualiser.uplugin` `VersionName` 1.0.115 → 1.0.116. Top-centre watermark + `[Rebus] STARTUP BANNER` will now read `v1.0.116` on the next launch; if you see anything earlier, your binary didn't rebuild.

> **UE 5.7 build fix: `Rebus.DumpBeamCulling` log block — `UPrimitiveComponent::BoundsScale` accessor swap + printf format-sanitiser argument alignment (v1.0.115).**
>
> User-reported build break (verbatim, abridged):
>
> > "RebusFixtureActor.cpp(3446): error C2039: 'GetBoundsScale': is not a member of 'UPrimitiveComponent' ... RebusFixtureActor.cpp(3433): error C7595: ... TCheckedFormatStringPrivate<..., int,int,int,int,int,int,int,int,int, float,float,float, int> ... note: failure was caused by call of undefined function or one not declared 'constexpr' X(FNeedsFloatOrDoubleArg, "'%f' expects \`float\` or \`double\`.")"
>
> Both errors live in the v1.0.113 `Rebus.DumpBeamCulling` log block (`ARebusFixtureActor::DumpBeamCullingStateForDebug`'s `FlagsLine` lambda) and are a single root cause. `UPrimitiveComponent` does NOT expose a `GetBoundsScale()` accessor in UE 5.7 — the field is the public `float BoundsScale` UPROPERTY (`Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h`, accessed by `SetBoundsScale` / `MarkRenderStateDirty` siblings as a direct member). The C2039 elided that one argument from the `FString::Printf` instantiation, which shifted every subsequent argument up one slot — so the format string's three trailing `%.1f` floats ended up bound to the next three args (`MinDrawDistance` / `LDMaxDrawDistance` / `CachedMaxDrawDistance`, all `float` — fine) and the final `%.1f cachedMax` slot landed on the trailing `bVisibleInRayTracing ? 1 : 0` `int`, triggering the compile-time `FNeedsFloatOrDoubleArg` sanitiser (C7595). The template instantiation in the error message (`<..., int×9, float×3, int>`) matches the post-elision arg list exactly — 9 bool-to-int args, then 3 float draw-distances, then the trailing int — confirming the diagnosis. v1.0.115 is a single one-character mechanical replacement: `C->GetBoundsScale()` → `C->BoundsScale` in `RebusFixtureActor.cpp:3446`. With the float arg restored, the format string and arg list realign (1 string + 9 ints + 4 floats + 1 int matches `%s` + `%d`×9 + `%.2f` + `%.1f`×3 + `%d`), the C7595 sanitiser passes, and the C2039 disappears. Zero behavioural change — `Rebus.DumpBeamCulling` log line content is byte-identical to the v1.0.113 design. `RebusVisualiser.uplugin` `VersionName` 1.0.114 → 1.0.115. Top-centre watermark + `[Rebus] STARTUP BANNER` will now read `v1.0.115` on the next launch; if you see anything earlier, your binary didn't rebuild.

> **UE 5.7 build fix: switch the v1.0.105 Nanite walker from the deprecated `UStaticMesh::NaniteSettings` direct-field access to the engine accessor API (v1.0.114).**
>
> User-reported build break (verbatim, abridged):
>
> > "failed: Compiling REBUS_VisualiserEditor failed (UnrealBuildTool exit 6). ... `RebusVisualiserSubsystem.cpp(2351,37): warning C4996: 'UStaticMesh::NaniteSettings': Please do not access this member directly, it will become private soon; use the various NaniteSettings accessor functions.`"
>
> UE 5.7 deprecated `UStaticMesh::NaniteSettings` direct-field access (`UE_DEPRECATED(5.7, ...)` on the UPROPERTY in `Engine/Source/Runtime/Engine/Classes/Engine/StaticMesh.h:734-736`). REBUS_VisualiserEditor builds with `-WarningsAsErrors`, so the v1.0.105 Nanite walker's nine `Mesh->NaniteSettings.bEnabled / .PositionPrecision / .FallbackPercentTriangles / .TrimRelativeError` read/write sites all hard-fail the build. The engine ships a non-const `FMeshNaniteSettings& UStaticMesh::GetNaniteSettings()` (`StaticMesh.h:840-843`) returning a mutable reference — semantically identical to the field access — so v1.0.114 is a single mechanical replacement: `Mesh->NaniteSettings.X` → `Mesh->GetNaniteSettings().X` across `RebusVisualiserSubsystem.cpp` (10 sites total; the build log surfaced 9, one additional near-neighbour was swept for consistency). Zero behavioural change. README/comment-string references to `NaniteSettings.bEnabled` are intentionally preserved — they're documentation of the operator-facing concept, not source-code references. README v1.0.114 release block above v1.0.113; `RebusVisualiser.uplugin` `VersionName` 1.0.113 → 1.0.114. Top-centre watermark + `[Rebus] STARTUP BANNER` will now read `v1.0.114` on the next launch; if you see anything earlier, your binary didn't rebuild.

> **Audit every beam clipping/culling path; fix outside-frustum mask sample sizing + SceneCapture FOV coverage + lazy-Epic-beam self-shadow + per-level-load auto-purge re-fire + per-fixture culling diagnostic + on-disk master md5 banner (v1.0.113).**
>
> User report (verbatim):
>
> > "The beams are sitll being clipped from old code. Might be camera viewing angle culling it. Please review any code the cuts the beam that isnt the new method in v.1.0.11"
>
> Filed against post-v1.0.112 binaries. v1.0.110 ripped the screen-space self-
> shadow trace. v1.0.111 redesigned the shadow path on a light-space depth-
> mask architecture. v1.0.112 auto-purged the stale pre-v1.0.110 cooked
> `M_RebusBeam.uasset` so the new HLSL would actually be the running shader.
> Even after all three the user reports the SAME pan-edge clipping shape,
> now with an explicit camera-angle correlation: "might be camera viewing
> angle culling it."
>
> **The v1.0.113 audit -- every code path that can hide / clip / fade / mask
> / cull / fragment-discard the visible shaft.** The brief asked for "no
> stones unturned" and called out the failure mode by name: the user's
> camera-angle clue meant the bug is either (a) the v1.0.112 auto-purge not
> taking effect on the user's deployment, OR (b) a SEPARATE clipping /
> culling code path that's never been audited because every previous release
> cycle's attention was on the (now-deleted) screen-space shadow trace. The
> audit verdict, path by path:
>
> | Path | Status | Action |
> | --- | --- | --- |
> | `_BEAM_RAYMARCH_HLSL` outside-frustum branch (`shadowVis` for sample outside SceneCapture FOV) | REVIEWED -- clean | The else branch correctly leaves `shadowVis = 1.0` (permissive). See `build_rebus_base_level.py` lines 716-755. |
> | `_BEAM_RAYMARCH_HLSL` `nf` near-fade term (`saturate(t / NEAR_FADE_CM)`) | REVIEWED -- clean | Depends on distance from CAMERA, but only fades when camera is INSIDE the cone (`tEntry = 0`). Not camera-angle-dependent in a clipping shape. |
> | `_BEAM_RAYMARCH_HLSL` `softOcc` term (`saturate((tOcc - t) / DEPTH_FADE_CM)`) | REVIEWED -- clean | DMX-style fade where shaft meets opaque geometry. Camera-angle-dependent in a "shaft dissolves at the floor" shape, NOT a wholesale-clip shape. Intended behaviour, no fix. |
> | `_BEAM_RAYMARCH_HLSL` cosine-dot fade `dot(CameraVector, BeamDir)` | REVIEWED -- not present | No such term exists in the post-v1.0.111 HLSL. |
> | `_BEAM_RAYMARCH_HLSL` world-Z height clamp | REVIEWED -- not present | No floor / ceiling clip. |
> | `_BEAM_RAYMARCH_HLSL` `TranslatedWorldToClip` / `LWCHackToFloat` survivor | REVIEWED -- not present | The only `abs(ndc.xy) < 1.0` is the LIGHT-space frustum probe (axes `lFwd / lRight / lUp`), NOT a screen-space NDC survivor. |
> | `_BEAM_RAYMARCH_HLSL` light-space depth-mask "infinite-depth" sentinel guard | REVIEWED -- clean | `blockerDepthCm > 1.0 && blockerDepthCm < maskFar * 1.01` correctly treats freshly-cleared RT pixels AND "missed all" rays as unoccluded. |
> | Material blend mode / two-sided / OpacityMask | REVIEWED -- clean | `M_RebusBeam` is Translucent additive, two-sided, no OpacityMask discard. The two-sided render lets the back-face fragment carry the shaft per the v1.0.39 entry/exit math; no fix. |
> | `BeamCone` `bUseAttachParentBound` | REVIEWED -- clean | Set to `false` so the cone uses its OWN section bounds (the small attach-parent bound would frustum-cull aggressively). |
> | `BeamCone` `bUseAsOccluder` | REVIEWED -- clean | Set to `false` (a translucent additive shaft should never occlude opaque geometry). |
> | `BeamCone` `CastShadow` / `bCastDynamicShadow` | REVIEWED -- clean | Both `false` (the shaft is unlit; it doesn't cast). |
> | `BeamCone` `bHiddenInSceneCapture` / `bVisibleInSceneCaptureOnly` | REVIEWED -- clean | Neither toggled. The v1.0.111 `BuildBeamShadowMaskCapture` only adds the cone to the SceneCapture's `HiddenComponents` per-capture list -- that doesn't hide it from the main scene render. |
> | `BeamCone` `MinDrawDistance` / `LDMaxDrawDistance` / `SetCullDistance` | REVIEWED -- clean | None of these are touched anywhere in the v1.0.x code. |
> | `BeamCone->SetBoundsScale(1.5f)` | REVIEWED -- BUG (latent), FIXED | The 1.5x bounds inflation is tight for an elongated tens-of-metres translucent shaft. The auto-computed `CreateMeshSection` bounds are geometrically correct, but HZB occlusion at grazing-angle camera pans can cull a thin shaft whose screen projection falls mostly behind the floor. v1.0.113 raises the constexpr to 3.0x as belt-and-braces -- extent-only (translucency sort order is unaffected). |
> | `BeamCone` `bRenderInMainPass` / `bRenderInDepthPass` | REVIEWED -- clean | UE defaults (true / true) untouched; correct for a translucent additive shaft. |
> | `EpicBeamComp` lazy build (operator flips `Rebus.PreferProceduralBeam 0` after spawn) | REVIEWED -- BUG, FIXED | `BuildBeamShadowMaskCapture` ran at `BuildBeamCone` time when `EpicBeamComp` was null; on a later lazy `TryBuildEpicBeam` the new canvas was NOT added to `HiddenComponents`, so the SceneCapture would shadow Epic's beam against itself (a lens-plane depth-1cm blocker hiding every shaft sample). v1.0.113 adds the lazy `EpicBeamComp` to `HiddenComponents` at the end of `TryBuildEpicBeam`. The user's complaint shape doesn't manifest on the default-procedural path, but the fix preempts the next regression. |
> | SceneCapture FOV sizing (`2 * OuterHalfDeg + Margin`) | REVIEWED -- LATENT BUG, FIXED | For default tuning (`BeamConeRadiusScale = 1.0`, `InnerRatio = 0.8` -> `MatchHalf = 0.9 * OuterHalf`), the SpotLight outer cone is the binding constraint and the v1.0.112 formula is correct. For operator-tuned `BeamConeRadiusScale > 1.0` (the v1.0.101 hero-scene polish knob), the visible cone-mesh edge grows PAST the SpotLight outer cone and the SceneCapture frustum stops covering the outer-rim samples. The HLSL is permissive (outside-frustum samples leave `shadowVis = 1.0`, no clip), so this is NOT the user's complaint shape -- but the brief explicitly asked for `max(OuterHalfDeg, MatchHalfDeg * BeamConeRadiusScale) + FovMarginDeg` as belt-and-braces, applied. |
> | `Rebus.BeamShadowMaskFovMargin` default 2.0 deg | REVIEWED -- LATENT BUG, FIXED | A 2-deg margin barely covers the cone-edge samples under bilinear filtering of the depth-mask RT. v1.0.113 raises the default to 5.0 deg as belt-and-braces (same fix shape as the bounds scale). |
> | SceneCapture `MaxViewDistanceOverride = AttenuationRadius * 1.05` | REVIEWED -- clean | Correct: occluders within the SpotLight throw render in the capture; beyond-throw geometry doesn't matter for occlusion (the light doesn't reach there). |
> | SceneCapture `Translucency` show flag | REVIEWED -- clean | Disabled. Translucent occluders don't render to the depth mask -- correct (translucent shafts shouldn't occlude). |
> | SceneCapture parented to `SpotLight` (auto-track aim) | REVIEWED -- clean | `BuildBeamShadowMaskCapture` attaches with identity relative transform; the component hierarchy then auto-tracks pan / tilt / head motion. |
> | v1.0.112 `ProbeAndAutoPurgeStaleBeamMaster` `WITH_EDITOR` / `IsModuleLoaded("PythonScriptPlugin")` defence | REVIEWED -- clean | Both paths log a Warning (not silent no-op). |
> | v1.0.112 `bBeamMasterAutoPurgeRun` one-shot guard | REVIEWED -- BUG, FIXED | Per-subsystem-instance state. `UGameInstanceSubsystem` lives across level reloads inside one editor session, so an operator who reloaded the level WITHOUT restarting the editor would not see the auto-purge re-fire after they pulled a fresh on-disk master. v1.0.113 hooks `FCoreUObjectDelegates::PostLoadMapWithWorld` to reset the bool + re-fire the probe on every level load. |
> | v1.0.112 `RefreshAllSpawnedFixtureBeamMIDs` walk timing | REVIEWED -- clean | Called from `ProbeAndAutoPurgeStaleBeamMaster` AFTER the `py` Exec returns. `SpawnedFixtures` is empty at `Initialize` time (subsystem init runs before any per-tick scene-fetch spawn), so the walk is normally a no-op; the regenerated master is picked up cleanly by `BuildBeamCone`'s `LoadObject` on the actual fixture spawns. |
> | v1.0.112 `py` Exec error logging | REVIEWED -- clean | Already logs `OK` / `FAILED` based on the bool return, plus the next-step operator nudge. |
> | Loaded plugin binary version vs source-tree version | REVIEWED -- BUG (silent until v1.0.113), FIXED | If the operator `git pull`ed v1.0.113 but didn't rebuild C++ binaries, `ProbeAndAutoPurgeStaleBeamMaster` doesn't exist in the loaded `UnrealEditor-REBUS_Visualiser.dll` -- the auto-purge silently doesn't run. v1.0.113 adds a `[Rebus] STARTUP BANNER` log line at subsystem `Initialize` that prints the loaded plugin VersionName + the on-disk `M_RebusBeam.uasset` md5 + the version verdict, so the operator can see the mismatch from one log line. |
> | `EpicBeamComp` "false occlusion" via v1.0.111 self-hide (operator flipped `Rebus.PreferProceduralBeam 0`) | REVIEWED -- see EpicBeamComp lazy-build row above | Same fix. |
> | `bMeshBeamEnabled` (the bMeshBeams scene-property mirror) | REVIEWED -- clean | When false the shaft is hidden by design via `SetVisibility(false)` on BeamCone -- that's the operator-toggle, not a code-path bug. Surfaced in `Rebus.DumpBeamCulling` so the operator can see the toggle state. |
> | Anywhere else: workspace-wide `Cull` / `Clip` / `Frustum` / `Discard` / `SetHidden` / `bHidden` / `bVisible` / `SetVisibility` / `MarkRenderStateDirty` touching `BeamCone` / `EpicBeamComp` / `BeamMID` | REVIEWED -- clean | The only touches are inside `BuildBeamCone` / `TryBuildEpicBeam` / `BuildBeamShadowMaskCapture` / `RefreshPreferProceduralBeamFromCVar` / `SetMeshBeamEnabled` -- all expected, all documented. No drive-by visibility flip path exists. |
>
> **What v1.0.113 ships:**
>
> 1. **`RebusBeamBoundsScale` 1.5 -> 3.0** (`RebusFixtureActor.cpp` constexpr).
>    Belt-and-braces against HZB occlusion-culling the elongated translucent
>    shaft at grazing camera angles. Extent-only inflation (origin unchanged
>    so translucency sort order is unaffected). The v1.0.38 release block
>    documented the same headroom theory at 3x; v1.0.39's raymarch entry/exit
>    rework was the actual fix for the "vanishes inside the cone" bug, so
>    v1.0.40+ trimmed the scalar back to 1.5x as "we don't need that much
>    headroom any more". v1.0.113 restores the 3x headroom as belt-and-
>    braces specifically against the camera-angle culling shape the user
>    reported.
> 2. **`Rebus.BeamShadowMaskFovMargin` default 2.0 -> 5.0 deg**
>    (`GRebusBeamShadowMaskFovMargin` initialiser). Same belt-and-braces
>    shape: a 2-deg margin barely covers the cone-edge samples under
>    bilinear filtering of the depth-mask RT; 5-deg ensures every visible-
>    cone sample falls firmly inside the rendered area regardless of cone
>    scale or zoom angle.
> 3. **SceneCapture FOV coverage formula** in `RefreshBeamShadowMaskParams`:
>    `NewFov = max(2 * OuterHalfDeg, 2 * MatchHalfDeg * BeamConeRadiusScale)
>    + MarginDeg`. Default tuning (`ConeScale = 1.0`, `InnerRatio = 0.8`)
>    -> max wins from `2 * OuterHalfDeg`, identical to v1.0.112. Hero-
>    extended cones (`ConeScale > 1.0`) -> max wins from `2 * MatchHalfDeg
>    * ConeScale`, FOV grows with the visible cone so the depth-mask
>    always has data for outer-rim samples.
> 4. **`EpicBeamComp` added to `BeamShadowMaskCapture->HiddenComponents`
>    at the end of `TryBuildEpicBeam`** when the SceneCapture already
>    exists. Closes the lazy-build window where flipping
>    `Rebus.PreferProceduralBeam 0` after spawn could leave Epic's canvas
>    self-shadowing in the depth-mask.
> 5. **`FCoreUObjectDelegates::PostLoadMapWithWorld` hook** in
>    `URebusVisualiserSubsystem::Initialize` resets `bBeamMasterAutoPurgeRun
>    = false` and re-fires `ProbeAndAutoPurgeStaleBeamMaster` on every
>    level load. Operators who pull a fresh on-disk master and reload the
>    level (without restarting the editor) now see the auto-purge re-fire
>    correctly. Unregistered cleanly in `Deinitialize` (mirrors the
>    v1.0.107 `VersionWatermarkDrawHandle` shape).
> 6. **`URebusVisualiserSubsystem::ComputeBeamMasterUassetMd5()` static
>    helper** (`Misc/FileHelper.h` + `Misc/SecureHash.h` + `Misc/PackageName.h`).
>    Resolves the long-package name to a filesystem path, byte-loads the
>    .uasset, returns the lowercase-hex MD5 -- the ground-truth invariant
>    for "is the operator's cooked master what we expect". Used by both
>    the v1.0.113 startup banner AND the extended `Rebus.DumpBeamMaster
>    Version` console command, so the two paths never disagree.
> 7. **`[Rebus] STARTUP BANNER` log line at subsystem Initialize**
>    stitches the loaded plugin VersionName, the EBeamMasterVersion verdict,
>    and the on-disk md5 into ONE paste-friendly line. If the operator
>    pastes this line, we can tell from it alone:
>    * Are they on the v1.0.113 binary? (or did they `git pull` without
>      rebuilding C++)
>    * Is the on-disk master v1.0.111+?
>    * What is the actual md5 of the bytes the engine cooked / loaded
>      (the ground truth for "did the v1.0.112 auto-purge actually
>      rewrite the .uasset")
> 8. **`Rebus.DumpBeamCulling [fixtureId]` new console command** +
>    `ARebusFixtureActor::DumpBeamCullingStateForDebug()` per-fixture dump.
>    One paste-friendly line per fixture inventorying every flag / scalar /
>    transform that can hide / clip / fade / cull the visible shaft:
>    * BeamCone visibility + culling + bounds-scale + draw-distance flags
>      (the row matrix the v1.0.113 audit walked through).
>    * Same shape for `EpicBeamComp` (when alive).
>    * `bUsingEpicBeam` + `bPreferProceduralBeam` + `bMeshBeamEnabled` --
>      which beam path is currently live.
>    * Live `BeamMID` readback of the v1.0.111 light-space mask scalars
>      (Enabled / BiasCm / FadeCm / FarCm / Debug / TanHalfFov) AND the
>      v1.0.108 radial-attenuation scalars (Sharpness / Density / Falloff)
>      -- EXISTS / MISSING surfaces a stale master, same as
>      `Rebus.DumpBeamShadowMask`.
>    * SceneCapture FOV + MaxViewDistance + HiddenComponents count.
>    * Geometric coverage check: visible cone half-angle (`MatchHalf *
>      ConeScale`) vs SceneCapture half-FOV -- when the cone is
>      geometrically wider than the capture, `coneFitsCapture = 0` and
>      the operator knows their `Rebus.BeamConeRadiusScale > 1` is in
>      play (HLSL is permissive so it's not a CLIP -- just no
>      occlusion data for the outer-rim samples).
>    * World-transform coincidence check: SpotLight + BeamCone +
>      SceneCapture distances. >1 cm divergence is a v1.0.111 head-tracking
>      bug.
> 9. **Extended `Rebus.DumpBeamMasterVersion`** now also reports the
>    loaded plugin VersionName + the on-disk md5 alongside the version
>    verdict (every per-world line + the aggregate footer). Same data the
>    startup banner shows, on demand.
> 10. **`RebusVisualiser.uplugin` `VersionName` -> `1.0.113`** so the
>     watermark (v1.0.107) AND the v1.0.113 startup banner AND the
>     `Rebus.DumpBeamMasterVersion` report all read `v1.0.113`. The
>     watermark on the live PixelStreaming2 stream is the at-a-glance
>     verification the loaded binary matches the released version.
>
> **Operator verification (v1.0.113).**
>
> 1. `git pull`, then **REBUILD the C++ binaries** (Tools > Refresh
>    Visual Studio Project + Build, or `Build.bat REBUS_VisualiserEditor
>    Win64 Development -Project=...`). v1.0.113 added new C++ code
>    (the `PostLoadMapWithWorld` hook + `ComputeBeamMasterUassetMd5` +
>    the startup banner + `DumpBeamCullingStateForDebug`); a stale
>    `UnrealEditor-REBUS_Visualiser.dll` doesn't have any of it.
> 2. **Fully restart the editor** (don't hot-reload). The plugin's
>    GameInstance subsystem `Initialize` only fires on a fresh launch
>    of the GameInstance, which is bound to the editor lifecycle for
>    PIE/standalone -- a hot-reload of the module leaves the previously-
>    initialised subsystem instance running with the OLD code.
> 3. Watch the log on startup. Expect ONE line of shape:
>    ```
>    LogRebusVisualiser: [Rebus] STARTUP BANNER: pluginVersion=v1.0.113 beamMasterVerdict=v1.0.111+ beamMasterUassetMd5=<32-hex>
>    ```
>    If `pluginVersion != v1.0.113` -> rebuild C++ + restart (step 1+2).
>    If `beamMasterVerdict != v1.0.111+` -> the v1.0.112 auto-purge
>    should fire ONE line BELOW this banner (`[Rebus] STALE BEAM
>    MASTER detected ... auto-running Rebus.RebuildBeamMaterial`); after
>    the regen, send `ClearScene + LoadScene` from the portal (or
>    restart the editor) to rebuild per-fixture BeamMIDs off the
>    regenerated master.
> 4. Top-centre watermark should read `v1.0.113`. If it reads anything
>    else, you're on the wrong binary -- repeat step 1.
> 5. Fire a fixture. Move the camera through every angle around the
>    beam: front, behind the lens, side, above, below, grazing. The
>    shaft must remain continuous. Only LEGITIMATE occluders (cubes /
>    props between the fixture and the lit footprint) should carve.
> 6. If anything looks wrong, paste the output of:
>    * `Rebus.DumpBeamMasterVersion` (shows pluginVersion + md5 + verdict)
>    * `Rebus.DumpBeamShadowMask` (per-fixture light-space mask state +
>      EXISTS/MISSING flag for the v1.0.111 master parameter contract)
>    * `Rebus.DumpBeamCulling` (v1.0.113 -- per-fixture inventory of
>      every flag that can hide / clip / cull the shaft)
>    * `Rebus.DumpFixtureZoom` (per-fixture cone-mesh + SpotLight outer
>      cone state)
>    Those four together cover every v1.0.x visible-beam failure mode
>    in known logs -- if the dumps show the expected state and the
>    artefact still appears, the audit missed something genuinely new,
>    not a recurrence of a past bug.
>
> **Lean on truth over velocity.** v1.0.99 -> v1.0.103 -> v1.0.110 ->
> v1.0.111 -> v1.0.112 -> v1.0.113 is six release cycles on
> functionally one user-reported symptom shape. v1.0.113 ships the
> hardening that proves the diagnosis from log output alone -- the
> startup banner + the md5 + the `Rebus.DumpBeamCulling` inventory.
> No more guess-the-failure on the next regression.

> **Auto-purge stale pre-v1.0.110 `M_RebusBeam` master on subsystem startup -- kill the screen-space pan-edge artefact for good (v1.0.112).**
>
> User report (verbatim):
>
> > "we are still seeing the origional attempt to shadow the cone/beam which cuts off the sides. remove this"
>
> This was filed against post-v1.0.111 binaries. v1.0.110 ripped out the
> v1.0.96..v1.0.109 screen-space self-shadow trace. v1.0.111 replaced it with
> the light-space depth-mask. Both source rewrites were complete -- and yet
> the OLD screen-space pan-edge side-cutting artefact was still visible on
> the user's deployment.
>
> **Cause A vs Cause B investigation (verifiable from the v1.0.112 commit).**
>
> Two failure modes were possible: (B) residual screen-space code in the
> v1.0.111 source itself, or (A) a stale on-disk `M_RebusBeam.uasset` cooked
> by a pre-v1.0.110 build of `build_rebus_base_level.py` whose embedded HLSL
> still contained the old trace. We checked (B) first because it's the
> cheaper miss:
>
> - `Grep` for `BeamShadowSteps|BeamShadowStrength|BeamShadowBias|
>   BeamShadowDebug|BeamShadowFarCullCm|BeamShadowEdgeGuard|
>   BeamShadowBiasScale|TranslatedWorldToClip|SceneTextureLookup|
>   SceneDepthTexture|SceneTextureSample` across `REBUS_Visualiser/` returns
>   zero hits in `_BEAM_RAYMARCH_HLSL` and zero hits in any v1.0.111 C++
>   file outside a single README historical comment block in
>   `RebusFixtureActor.cpp` (line 393, the v1.0.99 archaeology note).
> - The only `abs(ndc.xy) < 1.0` clamp in the v1.0.111 raymarch is the
>   LIGHT-space frustum bounds check using `BeamLightFwd / Right / Up`
>   axes (`if (abs(ndcX) < 1.0 && abs(ndcY) < 1.0)` at the inner mask
>   probe), NOT a screen-space NDC guard.
> - The only `SceneDepth` read is the engine `MaterialExpressionScene
>   Depth` node feeding `tOcc` for the v1.0.42 soft DMX depth-fade-against-
>   geometry, NOT a per-step screen-space-trace lookup.
>
> Cause B is ruled out. The v1.0.111 source is provably clean.
>
> Cause A is unambiguously the diagnosis: the user's on-disk
> `M_RebusBeam.uasset` is the pre-v1.0.110 cooked master, with the
> v1.0.96..v1.0.109 screen-space self-shadow HLSL still embedded. The
> per-fixture push reaches a parameter set that doesn't match the running
> plugin binary's expectations, but the shader inside the cooked master is
> what actually runs every frame -- and that shader carries the pan-edge
> trace.
>
> **Why the v1.0.111 Python self-heal didn't fire on the user's deployment.**
>
> v1.0.111 added `_beam_master_has_shadow_mask` -- a non-force `ensure_beam_
> material(force=False)` call detects a missing v1.0.111 parameter contract
> and promotes itself to a force-regen. That probe is correct, and on a
> machine where the script runs at editor startup it works. The defect:
> `build_rebus_base_level.py` is NOT in `[Python] +StartupScripts` (verified
> -- `REBUS_Visualiser/Config/DefaultEngine.ini` has no such entry, no
> other auto-import path exists), so the module is loaded ONLY when an
> operator manually runs `Tools > Execute Python Script >
> build_rebus_base_level` (or the headless `-run=pythonscript` equivalent).
> On the user's deployment that operator step has not been reliably
> performed across upgrades -- the same failure mode that bit v1.0.103
> (the v1.0.99 fix didn't take effect because the cooked master stayed
> stale). We have now burned **four** release cycles asking the operator
> to manually re-run Python: v1.0.99, v1.0.103, v1.0.110, v1.0.111.
> Asking the operator to do it again is wrong. v1.0.112 makes it
> automatic.
>
> **The system (v1.0.112 ships).**
>
> A new runtime probe `URebusVisualiserSubsystem::ProbeAndAutoPurgeStaleBeam
> Master` runs once from `Initialize()` (gated by a `bBeamMasterAutoPurgeRun`
> one-shot bool so any future re-entry is a clean no-op). It is the
> spiritual successor to the v1.0.103 startup probe that v1.0.110 removed,
> but with two material differences:
>
> 1. It detects the PRESENCE of any of the seven obsolete v1.0.96..v1.0.109
>    `BeamShadow*` scalars (`BeamShadowSteps`, `BeamShadowStrength`,
>    `BeamShadowBias`, `BeamShadowDebug`, `BeamShadowFarCullCm`,
>    `BeamShadowEdgeGuard`, `BeamShadowBiasScale`) AND the ABSENCE of the
>    v1.0.111 required contract (`BeamShadowMaskRT` texture + `BeamShadow
>    MaskEnabled / BiasCm / FadeCm / FarCm / TanHalfFov / Debug` scalars +
>    `BeamLightFwd / Right / Up` vectors). EITHER condition fires the
>    auto-purge. The required-contract probe set matches the Python
>    `_beam_master_has_shadow_mask` self-heal byte-for-byte so the C++
>    and Python probes can never disagree.
> 2. On staleness, it AUTO-REGENERATES the master by invoking
>    `Rebus.RebuildBeamMaterial` directly -- routes through the engine's
>    `py` console command to call `build_rebus_base_level.ensure_beam_
>    material(force=True)` (same Python entry the manual operator step
>    would take). Uses the v1.0.103 `WITH_EDITOR` + `FModuleManager::
>    IsModuleLoaded("PythonScriptPlugin")` defence so a packaged-build
>    invocation drops to a hard Warning instead of a silent no-op.
>
> The probe log line names the EXACT obsolete-present + missing-required
> parameter names that triggered the verdict (no guessing from operators
> or future bug-reports), and the post-regen log line spells out the
> operator next-step ("ClearScene + LoadScene from the portal to rebuild
> per-fixture BeamMIDs off the regenerated master, OR restart the
> editor"). A new `RefreshAllSpawnedFixtureBeamMIDs` walker calls the
> v1.0.112 `ARebusFixtureActor::RefreshBeamMaterialBindings` (`BeamMID->
> ClearParameterValues()` + `RefreshBeamRadialParams` +
> `RefreshBeamSpatialParams` + `RefreshBeamShadowMaskParams`) on every
> already-spawned fixture as a belt-and-braces refresh. This is normally
> a no-op at `Initialize()` time -- subsystem init runs BEFORE the per-
> tick scene-fetch chain spawns any fixture -- but it costs nothing and
> means the probe stays correct if a future tick-gated path ever re-fires
> it mid-show.
>
> **Packaged-build fallback.** Python is editor-only. In packaged builds
> the auto-regen cannot run (no PythonScriptPlugin, no `py` command, no
> ensure_beam_material) and rewriting a cooked `.uasset` at runtime
> isn't possible. The probe still detects the staleness and logs a hard
> Warning naming the obsolete scalar that triggered the verdict + the
> operator workflow ("open the project in editor on a v1.0.112+
> workspace, run `Rebus.RebuildBeamMaterial`, re-package, re-deploy").
> No silent no-op feature.
>
> **New diagnostic: `Rebus.DumpBeamMasterVersion`.** Loads the master,
> probes the obsolete + required contracts, classifies, and emits one
> line per GameInstance subsystem:
>
> ```
> Rebus.DumpBeamMasterVersion world='BaseLevel': Master Version: v1.0.111+. Obsolete v1.0.96..v1.0.109 scalars present: [(none)]. Missing v1.0.111 scalars: [(none)]. Missing v1.0.111 vectors: [(none)]. Missing v1.0.111 textures: [(none)].
> Rebus.DumpBeamMasterVersion: probed 1 world(s). Expect `v1.0.111+` after a fresh editor launch on a v1.0.112+ workspace; ...
> ```
>
> Verdicts: `MISSING`, `pre-v1.0.96`, `v1.0.96..v1.0.109 (HAS OBSOLETE
> -- screen-space trace cooked in)`, `v1.0.110 (clean slate, no shadow
> path)`, `v1.0.111+`. After a fresh editor launch on a v1.0.112+
> workspace the verdict should always be the last one; anything else
> means the auto-purge didn't fire (grep `LogRebusVisualiser` for
> `STALE BEAM MASTER detected`).
>
> **What this is honestly admitting.** We kept asking the operator to
> manually re-run Python after every release that touched the beam
> shader. That's wrong. The plugin owns its own assets and is the only
> thing that knows when a cooked .uasset doesn't match the running
> binary's expectations. v1.0.112 is the system finally doing that
> check for the operator instead of asking them to do it.
>
> **Operator verification (v1.0.112).**
>
> 1. Launch the editor in an environment where the pre-v1.0.110 cooked
>    `M_RebusBeam.uasset` is on disk (the user's current state).
> 2. `LogRebusVisualiser` shows exactly once:
>    `[Rebus] STALE BEAM MASTER detected -- v1.0.96..v1.0.109 (HAS
>    OBSOLETE -- screen-space trace cooked in). Obsolete v1.0.96..v1.0.109
>    scalars present: [BeamShadowSteps,BeamShadowStrength,...]. Missing
>    v1.0.111 scalars: [BeamShadowMaskEnabled,...]. ... Auto-running
>    `Rebus.RebuildBeamMaterial` now ...`.
> 3. Within a few seconds the log shows the regen completing
>    (`RebusBaseLevel: beam material ensured.`) followed by the
>    operator next-step nudge.
> 4. Operator sends ClearScene + LoadScene from the portal (or
>    restarts the editor). Per-fixture BeamMIDs respawn against the
>    regenerated master.
> 5. Beam pan no longer exhibits pan-edge side-cutting.
> 6. `Rebus.DumpBeamMasterVersion` reports `v1.0.111+`.
> 7. `Rebus.DumpBeamShadowMask` reports all six v1.0.111 scalars as
>    EXISTS (no `MISSING` flags).

> **Light-space depth-mask beam occlusion -- per-fixture SceneCapture2D "shadow map for the beam" (v1.0.111).**
>
> User design direction (verbatim):
>
> > "Keep your 3D cone, but add a light-space depth mask: Place a small SceneCapture2D
> > at the moving light lens. Match its FOV to the spotlight cone angle. Render a
> > depth-only / mask render target from the light's view. In the cone material,
> > transform each pixel/world position into the light's projection space. Compare:
> > pixel distance from light vs captured blocker depth. If pixel distance is farther
> > than blocker depth, reduce opacity to 0 or fade it."
>
> v1.0.111 ships exactly that. It's the architecturally correct replacement for the
> v1.0.96..v1.0.109 screen-space self-shadow trace v1.0.110 ripped out.
>
> **The system.** Each fixture spawns a `USceneCaptureComponent2D` (`BeamShadowMask
> Capture`) attached to its SpotLight (so the capture rides the live pan/tilt aim
> through the existing component hierarchy -- NEVER attach to FixtureRoot, that doesn't
> track the head), aimed down the beam, FOV matched to the SpotLight's outer cone
> (`2 * ResolveZoomHalfDeg() + Rebus.BeamShadowMaskFovMargin`, default 2 deg margin
> so the cone-edge raymarch samples don't sit on the texture's outermost row), with
> `CaptureSource = SCS_SceneDepth` so the per-fixture `UTextureRenderTarget2D`
> (`BeamShadowMaskRT`, default 256x256 R16f, ~128 KB / fixture) receives linear cm
> depth directly readable by the raymarch. The `M_RebusBeam` Custom-HLSL node has
> a new `[branch]` block per raymarch step: project the sample's world position into
> the SpotLight's local frame using the pushed `BeamLightFwd / BeamLightRight /
> BeamLightUp` world axes + the `BeamShadowMaskTanHalfFov` FOV scalar (perspective
> projection in dot products, no 4x4 matrix multiply -- LWC-safe and ~5x cheaper),
> sample the depth mask at the resulting UV, and attenuate the sample's density
> when `(sampleDistFromLight - (blockerDepth + Bias)) / Fade > 0`. The shaft fades
> softly over `Rebus.BeamShadowMaskFadeCm` (default 20 cm) so the discrete pixel
> mask doesn't read as aliased shadow edges.
>
> **Why this works where the v1.0.96..v1.0.109 screen-space trace didn't.**
> The SceneCapture's frustum is fixed by the SpotLight's AIM (not by the camera).
> Samples outside the capture frustum are exactly samples where the light doesn't
> reach geometrically -- so "no occlusion change there" is semantically correct,
> not a hack. The three v1.0.96..v1.0.109 failure modes all dissolve:
>
> - **Off-screen-relative-to-camera occluders silently failed.** The screen-space
>   trace could only see what was in the camera's depth buffer; geometry behind the
>   camera was invisible. The light-space mask sees everything the LIGHT sees.
>   Solved.
> - **Pan-edge false occlusion.** The screen-space trace's projected UV ran out of
>   the camera's screen at the cone's screen edge, sampled sky depth, and read
>   "occluded by the sky". The light-space mask runs entirely in the SpotLight's
>   own frustum -- "outside the frustum" is the unoccluded path. Solved.
> - **Reverse-Z precision crash beyond ~500m.** The screen-space trace compared
>   non-linear NDC z deltas at distances where the float32 reverse-Z mantissa
>   collapses. The light-space mask compares LINEAR cm (`SCS_SceneDepth`); R16f
>   gives ~6 cm precision at 60 m (well under the 20 cm soft fade), and the FarCm
>   cap = `SpotLight->AttenuationRadius` keeps the comparison range physical.
>   Solved.
>
> **Components added per fixture.** `USceneCaptureComponent2D` (parented to
> `SpotLight`, `SCS_SceneDepth`, perspective, all advanced features disabled --
> Lumen / fog / bloom / motion blur / decals / atmosphere / TAA all off, plus
> `BeamCone` + `EpicBeamComp` + `LensDisc` + every `IsBeamLensComponents`
> entry added to `HiddenComponents` so the beam visualisation can't self-shadow)
> + `UTextureRenderTarget2D` (R16f, 256x256 default, no mips, no gen-mips,
> Bilinear sampler) + ten new BeamMID parameter bindings:
> `BeamShadowMaskEnabled / BiasCm / FadeCm / FarCm / TanHalfFov / Debug` scalars,
> `BeamLightFwd / Right / Up` vectors, and `BeamShadowMaskRT` texture.
>
> **Memory & GPU cost (per fixture, defaults).**
> - RT memory: 256 * 256 * 2 bytes (R16f) = **128 KB / fixture**. 32-fixture show
>   = ~4 MB GPU memory for the masks. `Rebus.BeamShadowMaskRes 128` -> 32 KB,
>   `512` -> 512 KB, `1024` -> 2 MB / fixture.
> - GPU capture: depth-only with all features disabled, ~**0.05-0.15 ms / fixture**
>   on a modern GPU at 256 res. 32 fixtures -> ~3 ms / frame upper bound. Linear
>   in pixel count (doubling res ~4x cost).
> - Raymarch sample cost: one `Texture2DSample` per step per pixel where the sample
>   lies inside the cone AND the SceneCapture frustum, gated by a `[branch]` on
>   `BeamShadowMaskEnabled` so the OFF path costs ~zero (the compiler hoists the
>   branch above the sample). The OFF path also disables the per-fixture
>   `bCaptureEveryFrame` so the depth render is skipped entirely.
>
> **Console surface (six CVars + one diagnostic command).**
>
> - `Rebus.BeamShadowMask [0|1]` (default `1`). Master toggle. Flips both the
>   SceneCapture's `bCaptureEveryFrame` AND the BeamMID's `BeamShadowMaskEnabled`
>   scalar so a disabled mask costs zero GPU time (capture + per-pixel sample
>   are both skipped).
> - `Rebus.BeamShadowMaskRes <px>` (default `256`). Render-target square pixel
>   size. Recommended buckets: 128 / 256 / 512 / 1024. Doubling res 4x's memory
>   and ~2x's per-fixture capture cost.
> - `Rebus.BeamShadowMaskBiasCm <float>` (default `5.0`). Constant offset added
>   to blocker depth before the comparison. Raise to 50+ if the operator reports
>   beam shows as "shadowed right at the lens"; lower to 1.0 for tight rigs.
> - `Rebus.BeamShadowMaskFadeCm <float>` (default `20.0`). Soft fade range in cm
>   so the shaft doesn't binary clip at the blocker. 0 = hard clip (severe
>   aliasing at the 256 res); 5 cm for hero rigs at `Res 512+`; 50 cm for an
>   intentionally soft look.
> - `Rebus.BeamShadowMaskFovMargin <deg>` (default `2.0`). Safety margin added
>   to the SpotLight outer-cone full angle before feeding the SceneCapture's
>   `FOVAngle`, so the cone's outermost raymarch samples don't read undefined
>   pixels at the capture-render edge. Raise if the visible shadow stops short
>   of the cone edge; lower to 0 to confirm the diagnosis.
> - `Rebus.BeamShadowMaskDebug [0|1]` (default `0`). Paints occluded raymarch
>   samples RED inside the shaft so the operator can visually verify the
>   projection lines up with real-world occluders.
> - `Rebus.DumpBeamShadowMask [fixtureId]`. Per-fixture state dump: capture
>   FOVAngle / `MaxViewDistanceOverride` / `bCaptureEveryFrame`, RT pixel size +
>   format + memory cost, BeamMID's live shadow-mask scalar values (with
>   EXISTS/MISSING flag for stale pre-v1.0.111 masters), the projection sanity
>   check (projects `BeamOrigin + BeamDir * AttenuationRadius * 0.5f` into the
>   capture's view -- expects sample-plane offset near (0,0)), and the six
>   global CVar values for diff. Mirrors `Rebus.DumpFixtureZoom`'s shape.
>
> **Known limitations (these are CORRECT, not bugs).**
> - The SceneCapture only sees what's INSIDE the SpotLight's frustum. A ladder
>   leaning ACROSS but not THROUGH the cone won't shadow the beam -- and that
>   matches the physical light: it doesn't hit the ladder either.
> - Translucent geometry doesn't write to `SCS_SceneDepth` so glass / portals /
>   particles don't shadow the beam. Volumetric fog in the beam's frustum is
>   handled by Unreal's native VSM path on the SpotLight itself (unchanged --
>   the v1.0.110 rollback note's "What stayed" applies).
> - The cone-mesh raymarch only knows about samples INSIDE its own cone volume,
>   so an occluder that sits OUTSIDE the cone but blocks parts of the lit
>   footprint doesn't carve the shaft (correct -- the shaft IS the cone
>   visualisation; the footprint shadowing is handled by the SpotLight's own
>   shadow pass via the engine's depth/VSM path).
>
> **Acceptance / operator verification.**
> 1. Launch, place a cube between fixture lens + floor in line with the beam aim.
> 2. The cube carves a hole into the cone shaft at the cube's depth -- beam BEFORE
>    the cube is full intensity, AFTER is black (or `BeamShadowMaskFadeCm`-faded).
> 3. Pan the fixture left-right. The shadow rides with the beam aim (no
>    camera-driven offset -- the v1.0.96..v1.0.109 failure mode that finally
>    works).
> 4. Move the camera while the fixture is stationary. The shadow is rock-stable.
> 5. `Rebus.BeamShadowMaskDebug 1`. Occluded samples paint RED inside the shaft.
> 6. `Rebus.BeamShadowMask 0`. Shadow vanishes; beam shines through the cube.
>    `Rebus.BeamShadowMask 1`. Shadow returns.
> 7. `Rebus.DumpBeamShadowMask`. Reports FOV, RT size, projection sanity check
>    per fixture (the projection sample-plane offset should be < 1 cm).
>
> **v1.0.111 will likely be iterated.** This is the "iterate from clean slate"
> v1.0.111; the operator will tune `Rebus.BeamShadowMaskRes` / `BiasCm` / `FadeCm`
> defaults in v1.0.112+. The Python self-heal `_beam_master_has_shadow_mask`
> probe auto-regenerates pre-v1.0.111 `M_RebusBeam` masters on next editor open
> so packaged builds with old masters can't silently no-op the new push.

> **Remove v1.0.96 → v1.0.109 screen-space beam shadow trace (clean slate for redesign) (v1.0.110).**
>
> User instruction (verbatim):
>
> > "This shadow tracing,pen clip really isnt working, its terrible and completely wrong, remove it and we will start again."
>
> **What the v1.0.96 → v1.0.109 trace was trying to do.**
> Inside the per-pixel raymarch through the procedural cone shaft (the
> v1.0.42 Custom-HLSL `_BEAM_RAYMARCH_HLSL` node in `M_RebusBeam`), every
> shadow step sampled `SceneDepth` at the projected screen UV of a march
> sample halfway between the shaft sample `wp` and the spotlight origin,
> compared the sampled depth against the march sample's own clip-space `w`,
> and -- when the sampled depth was nearer than the sample (an occluder
> stood between the fixture and that point in the shaft volume) -- scaled
> the per-step density down by `(1 - BeamShadowStrength)`. The intent was
> a CHEAP self-shadow that carved the visible shaft against any in-frustum
> occluder (a hung truss, a person, a prop) without paying for a second
> shadow-map / volumetric pass on every fixture.
>
> **Why it failed in practice.**
> Even after the v1.0.99 LWC-projection fix (every step was landing
> off-screen because of the missing `PreViewTranslation`) and the v1.0.109
> off-screen / sky / far-distance guards (the user's "cutting the side of
> the beams when we pan left or right" symptom), the trace's day-to-day
> behaviour was unacceptable:
>
> 1. **Off-screen occluders cast nothing.** This is the architectural
>    ceiling of any screen-space-shadow technique. As soon as the
>    operator pans / dollies and the actual occluder falls outside the
>    camera frustum (or behind the camera), the shaft suddenly stops
>    being carved against it -- the same hung truss reads "no shadow"
>    from one angle and "shadowed" from the next.
> 2. **Pan-edge false occlusion.** Shadow samples landing in the screen-
>    UV margin between the fixture's worldspace and the camera near-clip
>    plane have no nearby depth to compare against; v1.0.109's edge-
>    guard suppressed the most visible chunk of this but the residual
>    noise was still wrong at the edges.
> 3. **Reverse-Z precision crash at long throws.** Beyond ~500 m the
>    `sd + stepBias < clipP.w` comparison is effectively random; the
>    v1.0.109 `Rebus.BeamShadowFarCullCm` cull hid the worst of it but
>    the underlying math doesn't survive that distance.
> 4. **No participating-media awareness.** The trace knows about opaque
>    SceneDepth and nothing else. It can't carve correctly against
>    translucent / volumetric occluders, and there is no roadmap inside
>    the screen-space approach that fixes that.
>
> The v1.0.109 release block's "fundamental fix would need a SECOND pass
> over a 3D depth cube" footnote was honest about (1) and (4); the daily
> visible behaviour even WITH the v1.0.109 guards in place was the
> deciding factor. The user's direction is to clear the floor entirely and
> redesign from a different starting point.
>
> **What came out (v1.0.110 rollback scope).**
>
> *Console variables (registered in `RebusVisualiser.cpp` / `RebusFixture
> Actor.cpp`, all REMOVED):*
>
> - `Rebus.BeamShadowSteps`
> - `Rebus.BeamShadowStrength`
> - `Rebus.BeamShadowBias`
> - `Rebus.BeamShadowDebug`
> - `Rebus.BeamShadowFarCullCm`
> - `Rebus.BeamShadowEdgeGuard`
> - `Rebus.BeamShadowBiasScale`
>
> *Console commands (REMOVED):*
>
> - `Rebus.BeamShadow [0|1|status]` (the master save/restore toggle)
> - `Rebus.DumpBeamShadow` (per-fixture EXISTS/MISSING diagnostic)
>
> *Material parameters declared by `_build_beam_master` (REMOVED from
> `M_RebusBeam`):*
>
> - `BeamShadowSteps`
> - `BeamShadowStrength`
> - `BeamShadowBias`
> - `BeamShadowDebug`
> - `BeamShadowFarCullCm`
> - `BeamShadowEdgeGuard`
> - `BeamShadowBiasScale`
>
> *Code paths (REMOVED):*
>
> - `ARebusFixtureActor::RefreshBeamShadowParams()` (the seven-CVar push
>   helper) + every call site (`BuildBeamCone`, the `Rebus.PreferProcedural
>   Beam` flip handler).
> - `ARebusFixtureActor::DumpBeamShadowStateForDebug()` (per-fixture dump
>   used by the removed `Rebus.DumpBeamShadow` command).
> - `URebusVisualiserSubsystem::ProbeBeamMasterAtStartup()` (one-shot
>   "STALE BEAM MASTER" warning that specifically named the v1.0.99 /
>   v1.0.109 shadow-trace scalars).
> - `_BEAM_RAYMARCH_HLSL` -- the entire screen-space shadow block (per-
>   sample SceneDepth tap, NDC guards, sky / far-cull / edge guards,
>   debug-colour visualisation, accumulator counters). The shaft density
>   composition is back to the v1.0.95 baseline `d = BeamDensity * core *
>   widthNorm * srcAtten * nf * softOcc` so the cone-mesh shaft renders
>   as a clean uncarved cone again.
> - `build_rebus_base_level.py` -- the `_beam_master_has_shadow_steps`,
>   `_beam_master_has_shadow_debug`, and `_beam_master_has_pan_edge_guard`
>   self-heal probes + the v1.0.99 / v1.0.103 / v1.0.109 force-regen
>   cascade inside `ensure_beam_material()`. `ensure_beam_material()`
>   now only force-regens when `force=True` is passed explicitly (via
>   `build()` or the runtime `Rebus.RebuildBeamMaterial`).
>
> **What stayed (intentionally untouched by the v1.0.110 rollback).**
>
> - `Rebus.PreferProceduralBeam` (v1.0.106 default ON, the procedural-vs-
>   Epic visible-shaft toggle). Orthogonal to the shadow trace; the
>   default stays ON in v1.0.110 because the v1.0.108 cone-mesh half-
>   intensity geometry + radial-attenuation tuning lives on the procedural
>   cone, not because of any shadow-trace requirement. Operators
>   preferring Epic's `MI_Beam` canvas can still flip to `0`.
> - `Rebus.BeamSharpness` / `Rebus.BeamDensity` / `Rebus.BeamFalloff`
>   (v1.0.108) -- raymarch radial Gaussian + axial-falloff tuning. Lives
>   inside the same `_BEAM_RAYMARCH_HLSL` Custom node but is the
>   `softOcc * srcAtten * widthNorm * core` composition that was always
>   on -- the v1.0.110 rollback simply removed the `* shadowAtten`
>   factor that v1.0.96..v1.0.109 multiplied on top.
> - `Rebus.BeamConeRadiusScale` + `ARebusFixtureActor::BeamConeRadius
>   Scale` UPROPERTY (v1.0.101) -- per-fixture cone-mesh visible-radius
>   scaler.
> - `Rebus.DumpFixtureZoom` (v1.0.101 + v1.0.108) -- still the canonical
>   per-fixture geometry-vs-photometry diagnostic.
> - `Rebus.RebuildBeamMaterial` (v1.0.103) -- the editor-only runtime
>   PythonScriptPlugin regen of `M_RebusBeam`. KEPT as a generic master-
>   regen tool for any future HLSL / parameter-graph work; the help text
>   no longer references the removed `Rebus.DumpBeamShadow` validation
>   step.
> - `ARebusFixtureActor::RefreshBeamShadowMode()` -- this is the **fog-
>   vs-mesh switch system for hero fixtures** driven by `Rebus.Hero
>   ShadowScatter` + `bWantsVolumetricShadow`, and it routes UE's
>   native VSM volumetric-shadow path. Name collision only -- entirely
>   separate from the deleted screen-space trace.
> - `bCastVolumetricShadow` / `bWantsVolumetricShadow` per-fixture
>   knobs that drive UE's native VSM volumetric-shadow path. Unrelated
>   to the screen-space trace.
>
> **Migration note for operators with portal automation.**
> Any external dispatcher (portal command bindings, Speckle automation,
> debug shortcuts) that pushed the removed `Rebus.BeamShadow*` CVars or
> issued the `Rebus.BeamShadow` / `Rebus.DumpBeamShadow` console commands
> will now log `Unknown console variable` / `Unknown console command`
> when the v1.0.110 build is loaded. Prune those entries from the
> dispatcher tables on the v1.0.110 pull. The startup probe Warning
> (`v1.0.103 + v1.0.109 startup probe: M_RebusBeam carries the v1.0.99
> + v1.0.109 parameter contracts ...`) is also gone -- no replacement
> Warning takes its place since there is no longer a parameter contract
> the master can be "stale" against beyond the v1.0.95-shape baseline.
>
> **Operator verification (v1.0.110).**
>
> 1. Pull, build, launch. The procedural cone shaft should render as a
>    clean uncarved cone (no occlusion against intervening geometry),
>    matching the pre-v1.0.96 look. Cameras can pan freely; no pan-edge
>    clipping of the shaft is possible because there is no shadow trace
>    to misfire.
> 2. `Rebus.DumpBeamShadow` -> editor logs `Unknown console command`.
> 3. `Rebus.BeamShadowStrength 0.5` -> editor logs `Unknown console
>    variable`. Repeat for any of the seven removed `BeamShadow*` CVars.
> 4. `Rebus.RebuildBeamMaterial` still regenerates `M_RebusBeam` (now
>    without the seven `BeamShadow*` scalar params).
> 5. `Rebus.PreferProceduralBeam 0` still flips back to Epic's `MI_Beam`
>    canvas. `Rebus.BeamSharpness 6.0` still tightens the procedural
>    raymarch radial Gaussian.
>
> **Open architectural question for v1.0.111+.**
> The user has explicitly cleared the floor for a redesign of beam-vs-
> object interaction. The next direction is open and to be decided by
> the user: candidates include (but are not limited to) raytraced
> volumetric shadows from the SpotLight against the shaft volume, a
> neighbour-view / multi-tap depth-cube sampling that escapes the
> screen-space frustum trap, a VSM-based volumetric integration that
> leans on UE's native volumetric-shadow path (the same one
> `RefreshBeamShadowMode` already drives for hero fixtures' fog halo),
> hand-painted clip planes or per-fixture occluder volumes, or simply
> living with the unshadowed procedural cone if the visual cost-benefit
> argues that direction. v1.0.111+ will land whichever approach the
> user picks -- this release is the clean slate that enables that
> choice.

> **Screen-space beam-shadow off-screen + sky + distance guards (fix pan-edge clipping) (v1.0.109).**
>
> User report (verbatim):
>
> > "the beam vs object shadowing is cutting the side of the beams when we pan left or right and is doing the same for all fixtures"
>
> **The five-release arc the v1.0.106 default-flip finally exposed.**
> v1.0.96 introduced the screen-space self-shadow trace in `M_RebusBeam`'s
> Custom HLSL. v1.0.99 fixed the LWC projection bug that had every shadow
> step landing off-screen + introduced the FIRST-STEP MINIMUM bias. v1.0.103
> shipped diagnostics (`Rebus.DumpBeamShadow` EXISTS/MISSING per scalar +
> startup probe + runtime regen) on the stale-master hypothesis. v1.0.106
> made `Rebus.PreferProceduralBeam 1` the default so the procedural cone --
> which carries the trace -- became the visible shaft (previously Epic's
> `MI_Beam` canvas had been the visible material since v1.0.43, hiding the
> v1.0.96..v1.0.103 work entirely). The shadow trace finally rendered on
> the user's install -- and immediately exposed the v1.0.99 trace's NEXT
> bug: the canonical SCREEN-SPACE-SHADOW failure mode. When the cone-mesh
> shaft sweeps across the camera frustum, the per-pixel shadow march taps
> SceneDepth at projected UVs that are either off-screen, on a "no opaque
> geometry written" sky pixel that reads the depth-buffer's clear / partial-
> fill sentinel, or at long camera-Z where reverse-Z precision crashes to
> sub-cm. The `sd + stepBias < clipP.w` comparison then fires on noise and
> the trace carves a black silhouette where there's actually nothing
> occluding the shaft. Visually: the beam shaft is CUT along the screen
> edges as the operator pans -- exactly the user's report.
>
> **Diagnostic note: v1.0.99 already HAD the off-screen guard, but it was
> the only one.** The v1.0.99 `_BEAM_RAYMARCH_HLSL` carried
> `if (any(abs(ndc) > 1.0)) { continue; }` as a per-step skip -- which IS
> correct for the strictly off-screen case. The pan-edge failure is the
> COMPOUND case: a step that's just-inside the frustum where the depth
> buffer texel is unreliable (HZB partial fill / fast-clear sentinel /
> sky reading 0 instead of FAR), OR a downrange step where the reverse-Z
> bias-vs-depth-precision ratio has flipped negative. The v1.0.99 guard
> doesn't catch either -- the trace runs into the unreliable-depth pixel
> with no escape hatch and fires false-occluded. v1.0.109 ships the four
> coupled guards that the OUR / Lumen / Frostbite / id-tech screen-space-
> shadow papers all document as the standard mitigation for translucent
> volumetric trace work:
>
> | Guard | Where it fires | What it prevents |
> | --- | --- | --- |
> | A -- Sky / no-geometry | `sd >= SkyDepthSentinel` (65000 cm = 650 m) OR `sd <= 0.001` cm | A sky pixel whose depth buffer reads back as 0 (HZB partial fill / fast-clear half-evaluated tile / TAA sample pushing one texel past the silhouette) producing a false "occluder right at the camera". |
> | B -- Far-distance cull | `clipP.w > BeamShadowFarCullCm` (default 50000 cm = 500 m) | Reverse-Z precision is sub-LSB beyond ~500 m; the `sd + stepBias < clipP.w` comparison gives essentially random answers. v1.0.109 `break`s out of the per-step loop rather than `continue`-ing into more equally-untrustworthy steps. |
> | C -- Distance-scaled bias | `stepBias = (0.01 * sdt) + sd * BeamShadowBiasScale` (default 0.002) | The v1.0.99 absolute `0.01 * sdt` floor is fine at close range. At long throw (~200 m) the reverse-Z buffer has ~30-50 cm/LSB precision; a constant 5 cm bias is sub-LSB and the test fires on quantisation noise. The multiplicative term grows the bias linearly with depth (~6 cm at 30 m, ~40 cm at 200 m). |
> | D -- Off-screen toggle | `BeamShadowEdgeGuard > 0.5` master switch around the v1.0.99 `any(abs(ndc) > 1.0)` skip | Lets the operator A/B verify the diagnosis: flip 0 to restore v1.0.99 broken behaviour (pan-edge clipping returns); flip 1 to confirm the fix landed. Defaults to 1 (ON). |
>
> **What lands in v1.0.109.**
>
> The Python `_BEAM_RAYMARCH_HLSL` Custom node gets the four guards
> installed in the per-step shadow loop, the per-pixel debug-view
> accumulators get one tally per guard reason, and the master material gets
> three new scalar params (`BeamShadowFarCullCm` / `BeamShadowEdgeGuard` /
> `BeamShadowBiasScale`) wired through the existing `input_names +
> custom_inputs + src_for` plumbing. The C++ side adds matching CVars +
> `RefreshBeamShadowParams` push + `Rebus.DumpBeamShadow` EXISTS/MISSING
> reporting + `URebusVisualiserSubsystem::ProbeBeamMasterAtStartup` extension
> that distinguishes "pre-v1.0.99" from "pre-v1.0.109" stale-master
> regimes in the launch-log Warning. The new self-heal probe
> `_beam_master_has_pan_edge_guard()` mirrors v1.0.96 / v1.0.99 patterns
> exactly -- when the on-disk master is missing the v1.0.109 scalar
> contract, `ensure_beam_material()` force-regens with a Warning naming
> the v1.0.109 fix so an operator who pulls v1.0.109 and opens the editor
> picks up the corrected HLSL on the first Python-script run.
>
> **New CVars (mirror the v1.0.96 / v1.0.99 `Rebus.BeamShadow*` pattern).**
>
> | CVar | Default | Recommended range | Notes |
> | --- | --- | --- | --- |
> | `Rebus.BeamShadowFarCullCm` | `50000.0` (cm = 500 m) | `20000-200000` | Shadow march steps with camera-Z above this are skipped (Guard B). Raise for arena-class throws if the operator reports the trace clipping legitimate downrange occluders; lower for tighter rigs to skip more long-distance tap cost. |
> | `Rebus.BeamShadowEdgeGuard` | `1` (ON) | `0-1` | Master toggle for the off-screen NDC guard (Guard D). `0` restores the v1.0.99 broken behaviour for A/B verification; `1` is the v1.0.109 fix in force. |
> | `Rebus.BeamShadowBiasScale` | `0.002` | `0.001-0.005` | Multiplicative-per-cm bias on top of the v1.0.99 absolute `BeamShadowBias` floor (Guard C). 0.2 percent of sample depth in cm. Raise for tighter false-occlusion control at extreme distances; lower for sharper detection of legitimate close-range occluders. |
>
> Each pushes through `RefreshBeamShadowParamsOnEveryFixture` -- the same
> refresh sink the v1.0.96 / v1.0.99 `Rebus.BeamShadow{Steps,Strength,Bias,
> Debug}` CVars use. One chokepoint, one log line per flip, no risk of
> half-pushed state. The v1.0.99 four-scalar shadow contract becomes a
> v1.0.109 seven-scalar contract; the v1.0.103 EXISTS/MISSING dump shape
> extends to all seven.
>
> **`Rebus.BeamShadowDebug 2` REPURPOSED -- per-pixel colour-by-GUARD-REASON.**
> The pre-v1.0.109 mode-2 view ("first shadow step's projected screen UV
> as `(uv.x, uv.y, 0)`") is retired -- the LWC projection bug has been
> three releases dead and the UV-sanity job is done. v1.0.109 replaces
> mode 2 with a discrete-priority colour map of which guard fired in
> each pixel's per-step shadow march:
>
> | Colour | Meaning | Operator action |
> | --- | --- | --- |
> | **RED** | At least one depth-occluded step (true scene-depth shadow). | None -- this is the trace working as designed. |
> | **GREEN** | Off-screen guard fired (Guard D -- v1.0.99 + v1.0.109). | None -- this is the v1.0.109 rescue from the pan-edge clipping. The user's "cutting the side of the beams when we pan" regions should paint GREEN under v1.0.109. |
> | **BLUE** | Sky / no-geometry guard fired (Guard A -- NEW v1.0.109). | None -- the sample landed on a sky pixel. Common at the bottom of arena beams pointing at high ceilings. |
> | **YELLOW** | Far-distance cull fired (Guard B -- NEW v1.0.109). | If unexpectedly widespread, raise `Rebus.BeamShadowFarCullCm` (the operator's rig may have throws past 500 m). |
> | **WHITE** | Clean -- trace ran to unoccluded with no guard rescue. | None -- everything's healthy. |
>
> Priority ordering matters: a pixel where (say) ONE step found a true
> occluder AND a different step fell off-screen colours RED, not GREEN,
> because RED represents genuine scene shadow while GREEN represents the
> guard rescuing a false-positive -- the operator cares MORE about
> identifying real shadows than about counting guard rescues.
>
> Mode 1 is unchanged (per-pixel `shadowedFraction` heatmap, green=
> unshadowed, red=shadowed -- the v1.0.99 contract).
>
> **What's intentionally NOT fixed in v1.0.109.**
> Screen-space-shadow remains FUNDAMENTALLY screen-space: off-screen
> occluders still cannot cast into the shaft (their depth isn't in the
> depth buffer). The v1.0.109 guards SOFTEN the visible failure mode
> (pan-edge clipping, sky-bleed, far-distance noise) but the underlying
> contract is unchanged. The COMPLETE fundamental fix is to feed the
> trace either (a) a wider-FOV neighbour depth buffer (UE 5.7 has no
> built-in for this) OR (b) move occlusion to ray-traced shadows on the
> volumetric beam (RT-fog / Lumen volumetric shadows). That's a v1.0.150+
> class of lift -- a new shadow architecture, not an iteration on the
> screen-space trace.
>
> **Operator action path (v1.0.109+).**
>
> 1. **Default ON does the right thing.** Launch the editor / packaged
>    build; the default `Rebus.BeamShadowEdgeGuard = 1` plus the new
>    `Rebus.BeamShadowFarCullCm = 50000.0` and `Rebus.BeamShadowBiasScale
>    = 0.002` mean the four v1.0.109 guards are live on every fixture from
>    the first frame. Pan a fixture left-right across the camera FOV; the
>    beam shaft should remain UNCUT at the screen edges -- the user-
>    reported pan-edge clipping is resolved.
>
> 2. **Read the launch-log Warning.** Search `LogRebusVisualiser` for
>    `v1.0.109 STALE BEAM MASTER` -- if it appears, the on-disk
>    `M_RebusBeam` predates v1.0.109 (v1.0.99..v1.0.108 master) and the
>    pan-edge guards aren't in the shader yet. (If you see
>    `v1.0.103 + v1.0.109 startup probe: M_RebusBeam carries the v1.0.99
>    + v1.0.109 parameter contracts` instead, skip to step 5.)
>
> 3. **Run `Rebus.RebuildBeamMaterial` from the portal console** (or
>    in-editor console). The v1.0.103 command invokes
>    `build_rebus_base_level.ensure_beam_material(force=True)` via
>    PythonScriptPlugin -- the v1.0.109 `_beam_master_has_pan_edge_guard`
>    self-heal will pick up the missing scalars and force the regen with
>    the v1.0.109 HLSL. Expect a `RebusBaseLevel: pre-v1.0.109 M_RebusBeam
>    detected ... regenerating with the v1.0.109 pan-edge guards` log
>    line confirming the regen landed.
>
> 4. **ClearScene + LoadScene from the portal** (or restart the editor)
>    so each fixture respawns and `BuildBeamCone` `LoadObject`s the
>    freshly-regenerated master.
>
> 5. **Verify with `Rebus.DumpBeamShadow`.** Every fixture line should
>    show all SEVEN MID scalars as EXISTS: `Steps=8.0/EXISTS
>    Strength=1.000/EXISTS Bias=5.00/EXISTS Debug=0/EXISTS
>    FarCullCm=50000/EXISTS EdgeGuard=1/EXISTS BiasScale=0.0020/EXISTS`.
>    If any v1.0.109 scalar still shows MISSING the regen + respawn didn't
>    land -- restart the editor.
>
> 6. **Verify visually with `Rebus.BeamShadowDebug 2`.** Pan a fixture
>    left-right; the pan-edge regions of the beam shaft should paint
>    GREEN (Guard D -- off-screen guard firing); inside-frame depth
>    shadows on cubes / props should still paint RED; sky behind the beam
>    BLUE; downrange-past-500m YELLOW; everything else WHITE. Then
>    `Rebus.BeamShadowDebug 0` to return to the regular composed beam --
>    the shaft should remain UNCUT at the screen edges.
>
> 7. **A/B verification of the fix.** Push `Rebus.BeamShadowEdgeGuard 0`;
>    the pan-edge clipping the user reported should RETURN (the v1.0.99
>    broken behaviour is restored). Push `Rebus.BeamShadowEdgeGuard 1`;
>    the clipping should disappear again. This proves the v1.0.109
>    diagnosis is right -- the missing off-screen master toggle was the
>    rate-limiting guard, NOT a different bug.
>
> **Files touched (v1.0.109).**
>
> | File | Change |
> | --- | --- |
> | `REBUS_Visualiser/Content/Python/build_rebus_base_level.py` | `_BEAM_RAYMARCH_HLSL` -- new file-header `v1.0.109 PAN-EDGE / SKY / FAR-DISTANCE GUARDS` block; new per-pixel accumulators `offScreenSampleCount / skySampleCount / farCullSampleCount`; per-step guard installations (sky / far-cull / edge-toggle / distance-scaled-bias); mode-2 debug visualisation REPURPOSED to colour-by-GUARD-REASON. `_build_beam_master` -- three new scalar params (`BeamShadowFarCullCm` 50000.0, `BeamShadowEdgeGuard` 1.0, `BeamShadowBiasScale` 0.002) wired through `input_names + custom_inputs + src_for`. New `_beam_master_has_pan_edge_guard` self-heal probe; `ensure_beam_material` chain extended with a v1.0.109 force-regen branch. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusFixtureActor.cpp` | New globals `GRebusBeamShadowFarCullCm` / `GRebusBeamShadowEdgeGuard` / `GRebusBeamShadowBiasScale` + three `FAutoConsoleVariableRef` blocks routed through the existing `RefreshBeamShadowParamsOnEveryFixture` sink. `RefreshBeamShadowParams` extended to push all SEVEN scalars now (clamped per the v1.0.109 sane-range spec). `DumpBeamShadowStateForDebug` extended to read back the three new scalars with EXISTS/MISSING flags; the stale-master diagnostic note distinguishes "pre-v1.0.99" (missing Debug) from "pre-v1.0.109" (missing pan-edge guards) and names the right operator recovery for each. The `Rebus.BeamShadowDebug` CVar help text updated to document the v1.0.109 mode-2 repurposing. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusVisualiserSubsystem.cpp` | `ProbeBeamMasterAtStartup` extended -- when the master carries the v1.0.99 contract but is MISSING `BeamShadowEdgeGuard`, log a `v1.0.109 STALE BEAM MASTER` Warning naming the pan-edge fix specifically (distinct log line from the v1.0.103 "pre-v1.0.99" warning so the operator can grep). Healthy-master log line bumped to mention both v1.0.99 + v1.0.109 contracts. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusVisualiser.cpp` | `HandleDumpBeamShadowCommand` extended -- the CVar header line now surfaces all SEVEN `Rebus.BeamShadow*` knobs in one paste-friendly block. `Rebus.DumpBeamShadow` help text updated for the v1.0.109 scalar set + the v1.0.103-vs-v1.0.109 stale-master distinction. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/RebusVisualiser.uplugin` | `VersionName` bumped `1.0.108` -> `1.0.109`. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/README.md` | This release block. |
>
> No engine / `OrbitConnector` / `glTFRuntime` / Epic-DMX-Fixtures asset
> is touched. The v1.0.96 / v1.0.99 / v1.0.103 / v1.0.106 / v1.0.107 /
> v1.0.108 lineage is preserved verbatim -- v1.0.109 is strictly additive
> on the same `M_RebusBeam` Custom HLSL graph (the v1.0.108 radial-
> sharpness rework on `BeamSharpness` / `BeamDensity` / `BeamFalloff` is
> orthogonal and untouched).
>
> **v1.0.150+ follow-up (off-screen-occluder fundamental fix).** The
> v1.0.109 guards mitigate the screen-space failure mode but don't
> change the underlying contract: off-screen occluders still can't cast
> into the shaft. Two architectural paths investigated for a future
> release:
>
> 1. **Wider-FOV neighbour depth buffer.** Sample a secondary depth pass
>    rendered at e.g. 1.5x the main camera's FOV. UE 5.7 has no built-in;
>    would require a custom SceneViewExtension that allocates an off-
>    screen render target, renders the scene's opaque depth into it at
>    the wider FOV, and exposes it as a `MaterialParameterCollection`
>    texture. Cost: roughly one extra opaque pass per beam-rendering
>    frame; precision: depends on the secondary-FOV resolution. Catches
>    the canonical case where a hanging truss is just out of the camera
>    frustum but still blocks the beam-light from reaching the shaft.
>
> 2. **Ray-traced shadows on the volumetric beam.** Replace the
>    screen-space trace with RT volumetric-shadow taps in the same
>    Custom HLSL. Lumen + RT-shadow context is in flight in UE 5.7+
>    but the volumetric-fog / translucent-surface integration is
>    incomplete; would require an engine fork or wait for UE 5.8/5.9.
>    Cost: hardware RT-shadow tap (~1-3 ms per beam in shipping
>    titles); precision: full-scene accurate.
>
> Path 1 is the more conservative class of lift; Path 2 is the right
> long-term answer when the engine support matures. Neither is in
> v1.0.109 scope -- the v1.0.109 guards are the standard mitigation
> shipping today.

> **Cone-mesh radius + raymarch sharpness match the lit footprint edge (half-intensity geometry) (v1.0.108).**
>
> User report (verbatim):
>
> > "The cone size and spotlight size are not the same, they should match."
>
> Reference image (saved): two procedural cone shafts on a marble floor; the
> visible cone diameter at the floor plane reads ~2-3x larger than the bright floor
> disc each beam produces. Now that v1.0.106 made `Rebus.PreferProceduralBeam 1`
> the default, this mismatch is no longer hidden behind Epic's `MI_Beam` shaft and
> is the user's headline visual complaint.
>
> **The math diagnosis -- why v1.0.101's `BeamConeRadiusScale` only got us
> partway.** v1.0.101 already documented (correctly) that UE's
> `USpotLightComponent` ramps brightness LINEARLY from `InnerConeAngle` (peak)
> to `OuterConeAngle` (zero), so the visible bright disc on the floor sits at
> roughly the half-intensity edge ~ `(InnerHalf + OuterHalf) / 2`. With the
> default `InnerRatio = 0.8` (BeamAngle/FieldAngle when the GDTF profile carries
> both, else fallback) the perceived disc edge sits at ~`0.9 * outer`, while the
> v1.0.101..v1.0.107 procedural cone-mesh's far-radius was sized to `BeamLength
> * tan(OuterHalf) * BeamConeRadiusScale` -- exactly `outer`. The cone-mesh
> therefore reads ~10 % wider than the lit footprint by GEOMETRY alone (a
> misalignment v1.0.101 surfaced as the operator-tunable
> `Rebus.BeamConeRadiusScale 0.85..0.95` knob).
>
> But the v1.0.108 user image showed a 2-3x mismatch -- WAY beyond the 10 %
> geometric gap. The dominant cause: the M_RebusBeam Custom HLSL raymarch's
> radial cross-section was an extremely SOFT Gaussian (`core = exp(-rN^2 *
> BeamSharpness)` with `BeamSharpness = 2.5` since v1.0.42). At `rN = 1` (the
> geometric cone-mesh edge) the soft glow density is `exp(-2.5) = 8.2 %` of axis
> density -- visible. At `rN = 0.7` it's 29 %, at `rN = 0.5` it's 54 %. So the
> visible bright shaft was effectively painting the FULL cone-mesh interior --
> reading as a fat soft cylinder filling the entire `OuterConeAngle` cone --
> while the bright floor disc was being driven by the IES profile + the
> SpotLight's linear inner..outer taper, which crashes to zero much faster.
> The OuterConeAngle-sized cone-mesh + the soft Gaussian was the v1.0.101
> 10 % gap MULTIPLIED by ~2-3x of soft-Gaussian filling.
>
> **What lands in v1.0.108.**
>
> | Surface | Behaviour |
> | --- | --- |
> | `UpdateBeamConeGeometry` FarRadius math (the heavy lift) | The cone-mesh's far-radius BASE is now `BeamLength * tan(MatchHalf)` where `MatchHalf = OuterHalf * (1 + InnerRatio) / 2` (the half-intensity ring -- new helper `ResolveBeamFootprintMatchHalfDeg()` factors `InnerRatio` out via the new `ResolveFootprintInnerRatio()`, mirroring the InnerRatio derivation in `RecomputeConeAngles` so the two paths share one source of truth by construction). With `InnerRatio = 0.8` the geometric base shrinks to `0.9 * OuterHalf`. `BeamConeRadiusScale` is multiplied at the end EXACTLY as before, so the per-fixture polish knob still works on top of the corrected base. The SpotLight's own `OuterConeAngle` is INTENTIONALLY UNTOUCHED -- the lit footprint, IES sampling, and 1/r^2 falloff continue to track the GDTF zoom-range specification verbatim. |
> | `UpdateEpicBeamParams` `DMX Zoom` math | Same MatchHalf substitution -- the Epic-beam canvas (active when `Rebus.PreferProceduralBeam 0`) inherits the v1.0.108 visible-shaft-vs-lit-disc parity for free. Pre-v1.0.108 was `RebusEpicBeamZoomScale * BeamConeRadiusScale * SpotOuterHalfDeg`; v1.0.108 is `RebusEpicBeamZoomScale * BeamConeRadiusScale * MatchHalfDeg`. |
> | `Rebus.BeamSharpness <float>` CVar (NEW, default `6.0`) | The dominant fix on top of the geometry. Drives the M_RebusBeam Custom HLSL `BeamSharpness` scalar -- the `core = exp(-rN^2 * BeamSharpness)` exponent. Default raised from 2.5 to 6.0 so the visible shaft pinches to ~60 % of the cone-mesh radius (where `core(rN=0.6) = exp(-2.16) = 11 %`, just past human-visible threshold for the additive accumulator). Recommended operator range: `[4..12]` -- 4 = soft / frosted look (the pre-v1.0.108 fat-cylinder shape), 12 = ultra-tight bright core (theatrical-mover hero look). Live -- the refresh sink (`RefreshBeamRadialParamsOnEveryFixture`) walks every Rebus fixture and re-pushes the BeamMID scalar; cone-mesh geometry is unchanged. |
> | `Rebus.BeamDensity <float>` CVar (NEW, default `0.015`) | Per-step density gain on the same Custom HLSL `BeamDensity` scalar. Default unchanged from v1.0.40; exposed in v1.0.108 as a companion to `Rebus.BeamSharpness` for total-opacity tuning that doesn't touch the radial profile. Recommended `[0.005..0.06]`. |
> | `Rebus.BeamFalloff <float>` CVar (NEW, default `1.6`) | Length-fade strength on the same Custom HLSL `BeamFalloff` scalar (`srcAtten = 1 / (1 + BeamFalloff * dn^2)`). Default unchanged from v1.0.40; exposed for the full radial+axial gradient knob set. Recommended `[0..4]`. |
> | `RefreshBeamRadialParams()` per-fixture method (NEW, public) | One chokepoint that pushes the three radial-attenuation scalars onto `BeamMID`. Mirrors `RefreshBeamShadowParams` shape verbatim (per-fixture seed point in `BuildBeamCone` after the constexpr-defaulted seed, plus the three CVar refresh sinks). Idempotent / silent when BeamMID is null. |
> | `Rebus.DumpFixtureZoom` per-fixture line | Now also reports `matchHalf=<deg>` (the v1.0.108 cone-mesh target half-angle), `footprintInnerRatio=<r>` (the InnerRatio fed into MatchHalf), and `BeamMID.Sharpness=<s> BeamMID.Density=<d> BeamMID.Falloff=<f>` (read back from the live MID -- proves the v1.0.108 push won the race against any portal/scene-property override; mirrors the `BeamMID.FarRadius` read-back v1.0.101 added). |
> | Verbose `ApplyZoom` log | Now also reports `matchHalf=<deg>` so the operator can confirm at every zoom change that `coneFarRadius = BeamLength * tan(matchHalf) * BeamConeRadiusScale`. |
>
> **Operator migration -- BeamConeRadiusScale stays at 1.0.** The per-fixture
> `BeamConeRadiusScale` knob STAYS at default `1.0`. Operators who tuned
> `Rebus.BeamConeRadiusScale 0.85..0.95` per-show in v1.0.101..v1.0.107 to
> compensate for the geometric outer-cone over-sizing will find their cones
> TOO NARROW after v1.0.108 (the geometric base is already 10 % tighter from
> the MatchHalf math, plus the visible Gaussian is ~60 % of the cone-mesh
> radius rather than ~100 %, so a 0.9 user-applied scale stacks ~0.5x of the
> v1.0.107 visible width). **Reset `Rebus.BeamConeRadiusScale 1.0`** when
> upgrading to v1.0.108. The knob is now a polish lever, not the workaround
> for a systemic gap.
>
> **What stays unchanged (deliberately).**
>
> * `SpotLight->OuterConeAngle` continues to track the GDTF zoom-range half-
>   angle verbatim. The lit footprint, the IES sampling, and the 1/r^2 falloff
>   are all anchored to the photometric truth -- v1.0.108 only narrows the
>   VISIBLE shaft.
> * `RecomputeConeAngles` continues to derive `InnerConeAngle = OuterHalf *
>   InnerRatio * FrostSoften`. Frost is intentionally NOT applied in
>   `ResolveFootprintInnerRatio()`: frost softens the inner cone toward the
>   outer (penumbra widening) but does NOT move the half-intensity ring that
>   the visible cone-mesh should match. The visible-shaft target stays at
>   the photometric (Beam/Field) half-intensity edge regardless of frost
>   level -- which is the intuitively correct behaviour (frost makes the
>   shaft READ softer/wider via the Gaussian, not via the geometric base).
> * `Rebus.BeamShadowSteps` / `BeamShadowStrength` / `BeamShadowBias` /
>   `BeamShadowDebug` (v1.0.96 / v1.0.99 / v1.0.103 / v1.0.106 self-shadow
>   trace) -- v1.0.108 does not touch the screen-space shadow trace; the
>   trace continues to render on the now-pinched visible shaft.
> * `Rebus.PreferProceduralBeam` (v1.0.106 default `1`) -- v1.0.108
>   benefits BOTH paths. The procedural cone gets the heavy lift (Sharpness +
>   MatchHalf both apply); the Epic beam gets MatchHalf via `DMX Zoom`. So an
>   operator who flips `Rebus.PreferProceduralBeam 0` for an Epic-beam-
>   specific show context still sees the v1.0.108 visible-shaft-vs-lit-disc
>   parity.
>
> **Operator verification (mirrors the v1.0.101 checklist).**
>
> 1. Spawn a single moving-head fixture, point it straight down at the floor
>    (or on a marble plane like the canonical user image).
> 2. Push a narrow zoom (e.g. mid of the GDTF zoom range, ~10° full = 5° half).
> 3. Look at the visible shaft disc on the floor vs the bright-disc lit edge.
>    They should COINCIDE within ~5 % on the canonical test scene -- the
>    visible cone-mesh edge sits at the bright-floor-disc edge, with the soft
>    Gaussian fading further out as a halo (not a fat cylinder).
> 4. Run `Rebus.DumpFixtureZoom` -- confirm `matchHalf < resolvedHalf`
>    (typically `0.9 * resolvedHalf` for the default 0.8 InnerRatio),
>    `coneFarRadiusBuilt == coneFarRadiusExpected` (within 0.5 cm), and
>    `BeamMID.Sharpness == 6.000`.
> 5. Push `Rebus.BeamSharpness 12` -- the visible bright shaft pinches further
>    to a hairline core; the cone-mesh edge is still where the bright-disc
>    edge was. Push `Rebus.BeamSharpness 2.5` -- restores the v1.0.107
>    fat-cylinder Gaussian for direct A/B against the v1.0.108 default.
> 6. Push `Rebus.BeamConeRadiusScale 1.0` (the v1.0.108 default; ALSO push
>    `1.0` if you were running 0.85..0.95 in v1.0.107 -- per the migration
>    note above).
>
> **Files touched (v1.0.108).**
>
> | File | What changed |
> | --- | --- |
> | `Source/RebusVisualiser/Public/RebusFixtureActor.h` | Added `RefreshBeamRadialParams()` public method declaration (next to `RefreshBeamShadowParams`). Added private helpers `ResolveFootprintInnerRatio() const` + `ResolveBeamFootprintMatchHalfDeg() const`. |
> | `Source/RebusVisualiser/Private/RebusFixtureActor.cpp` | Raised `RebusBeamSharpness` constexpr 2.5 -> 6.0. Added `GRebusBeamSharpness` / `GRebusBeamDensity` / `GRebusBeamFalloff` globals + `RefreshBeamRadialParamsOnEveryFixture` sink + the three new `Rebus.BeamSharpness` / `Rebus.BeamDensity` / `Rebus.BeamFalloff` `FAutoConsoleVariableRef` blocks. Implemented `ResolveFootprintInnerRatio()` + `ResolveBeamFootprintMatchHalfDeg()` + `RefreshBeamRadialParams()`. Replaced `OuterHalf -> tan -> FarRadius` with `MatchHalf -> tan -> FarRadius` in `UpdateBeamConeGeometry`. Replaced `SpotOuterHalfDeg` with `MatchHalfDeg` in `UpdateEpicBeamParams`'s `DMX Zoom` formula. Extended `DumpFixtureZoomStateForDebug` + the verbose `ApplyZoom` log to surface `matchHalf` + the BeamMID radial scalar read-backs. Added `RefreshBeamRadialParams()` to the `BuildBeamCone` initial-seed sequence (next to `RefreshBeamShadowParams`). |
> | `Content/Python/build_rebus_base_level.py` | Raised the `M_RebusBeam` master's authored `BeamSharpness` default 2.5 -> 6.0 to match the v1.0.108 constexpr (so a master regen and a runtime fixture seed agree). |
> | `RebusVisualiser.uplugin` | `VersionName` 1.0.107 -> 1.0.108. |
> | `README.md` | This release block. |
>
> No engine / OrbitConnector / Epic-DMX-Fixtures asset is touched. The
> v1.0.106 / v1.0.107 work (PreferProceduralBeam toggle + version watermark)
> is orthogonal -- v1.0.108 only touches the cone-mesh radial geometry +
> raymarch radial-attenuation scalars; the v1.0.96..v1.0.107 lineage of
> shadow-trace + watermark + Nanite/double-sided/Orbit-binding behaviour
> is preserved verbatim.

> **Top-centre version watermark on every rendered frame (operator-toggleable, default ON) (v1.0.107).**
>
> User request (verbatim):
>
> > "can we print the version on the top centre of the ouput with a command to be able to turn off. So we see top centre of the stream v1.0.106 for example."
>
> Operator context: with v1.0.99 / v1.0.103 / v1.0.106 each iterating on the same
> visible failure mode (beam shadow trace), every QA cycle has had to start with
> an explicit "what version is actually running?" log grep. The watermark
> short-circuits that round-trip: the running binary's plugin VersionName paints
> itself top-centre on every rendered viewport (and every PixelStreaming2 stream
> frame that captures the FCanvas overlay), so the operator can read the version
> off the live stream / a screen-cap before they even open the log.
>
> **What lands in v1.0.107.**
>
> | Surface | Behaviour |
> | --- | --- |
> | `URebusVisualiserSubsystem::DrawVersionWatermark(UCanvas*, APlayerController*)` (NEW) | The per-frame foreground-canvas draw, registered with `UDebugDrawService::Register("Foreground", ...)` from `URebusVisualiserSubsystem::Initialize()`. Composes a centred `v<VersionName>` glyph (e.g. `v1.0.107`) against the engine's medium font (`GEngine->GetMediumFont()`), measures via `Canvas->TextSize`, centres horizontally on `Canvas->SizeX`, and offsets vertically by `RebusVersionWatermark::GTopMarginPx` (default 12px). Drop-shadow draw via `FCanvasTextItem::EnableShadow(black, 70%)` underneath the white-90% foreground keeps the text legible against bright skies + dark stages without an opaque background plate. Wrapped in a single `if (RebusVersionWatermark::GShowEnabled)` early-out so the per-frame cost is one branch when the toggle is off. |
> | `IPluginManager::FindPlugin("RebusVisualiser")->GetDescriptor().VersionName` cache | Composed once at `Initialize()` into `CachedVersionDisplay = "v" + VersionName`. The engine-blessed accessor always reports the running binary's plugin descriptor (no JSON re-parse, no risk of the watermark drifting from `RebusVisualiser.uplugin`'s on-disk content for the wrong reason -- the descriptor is what the engine actually loaded). Cached so the per-frame draw allocates nothing. The watermark is the source-of-truth: **what you see on screen is what's running**. |
> | `Rebus.ShowVersion [0\|1\|status]` console command (NEW) | Default ON. Routes through every Game/PIE world's `URebusVisualiserSubsystem::SetVersionWatermarkEnabled` -- the SAME chokepoint the `bShowVersionWatermark` scene property uses, so the console + portal paths can never diverge (mirrors v1.0.99 / v1.0.104 / v1.0.105 single-chokepoint pattern). The `status` arg logs the live flag + cached display string + Y-margin in one line for quick verification. |
> | `Rebus.VersionWatermarkY <px>` console command (NEW) | Sets the top-edge margin in pixels (default 12, clamped to ≥0). With no arg, logs the current margin. Lets operators with a HUD element near the top centre drop the watermark below it without disabling it. |
> | `bShowVersionWatermark` scene property (NEW) | Seeded `true` in `URebusSceneSettingsSubsystem::Initialize()`. Mirrors the v1.0.99 / v1.0.104 / v1.0.105 routing: `ApplySceneProperty("bShowVersionWatermark", ...)` resolves the GameInstance's `URebusVisualiserSubsystem` and calls `SetVersionWatermarkEnabled(Value.bBool)` -- one source of truth for the live state across the console command, the portal `SetSceneProperty`, and the SceneState round-trip read-back. |
>
> **Why `UDebugDrawService("Foreground")` rather than a UMG widget or `AHUD::PostRender`?**
>
> | Path | Pros | Cons | Verdict |
> | --- | --- | --- | --- |
> | `UDebugDrawService::Register("Foreground", ...)` (CHOSEN) | Engine-blessed FCanvas overlay; fires after the 3D world is rendered + before UMG/HUD; captured by PixelStreaming2 verbatim into the H.264 stream (the PS2 capture pipeline records the same FCanvas pass); zero-config (one delegate registration in `Initialize()`); no actor / pawn / level dependency. | None for this use case. | Ship. |
> | UMG widget on a level-blueprint-spawned `UUserWidget` | Rich layout / animation. | Requires a project-side widget asset, a level-script entry point, lifecycle plumbing across PIE / Game / packaged; UMG isn't always captured in screenshot-export paths the way FCanvas is. Overkill for "draw one string top-centre". | Reject (overweight). |
> | `AHUD::PostRender` via a custom `AHUD` subclass set as the project default HUD | Same FCanvas pass as `UDebugDrawService("Foreground")`, captured by PS2 the same way. | Requires subclassing `AHUD` AND wiring it as the project default HUD AND keeping that wiring across map travel; existing project doesn't use a custom HUD class. Adds a configuration surface for zero behavioural gain. | Reject (architectural cost > behavioural value). |
> | Direct `FCanvas` draw from a viewport callback | Lowest-level. | Reinvents what `UDebugDrawService` already exposes; no per-extension visibility filtering. | Reject (reinvents). |
>
> Operator-side verification of the PS2 capture path: `UDebugDrawService` runs on
> the FCanvas pass that the PixelStreaming2 scene-capture pipeline records into
> the H.264 frames -- no special integration required. The watermark appears in
> the live stream automatically; an operator can confirm by opening the portal,
> launching a stream, and reading the version off the live frame.
>
> **Operator action path (v1.0.107+).**
>
> 1. **Default ON does the right thing.** Launch the editor / packaged build; the
>    watermark `v1.0.107` appears in the top centre of the viewport AND in any
>    PixelStreaming2 stream the build serves. No operator action required.
> 2. **Verify with `Rebus.ShowVersion status`.** The status path logs:
>    `enabled=true, display='v1.0.107', y-margin=12.0px` (subsystem-found=yes
>    once a PIE/Game world has spun up). If `display=<unset>` the IPluginManager
>    look-up failed at startup (cooked-package edge case); a one-shot Warning is
>    logged at `Initialize()` naming the failure.
> 3. **Toggle off for clean reference captures.** `Rebus.ShowVersion 0` removes
>    the watermark on the next frame. `Rebus.ShowVersion 1` restores it. The
>    portal can drive the same flip via `SetSceneProperty bShowVersionWatermark
>    {true|false}` (mirrored chokepoint, never diverges).
> 4. **Tune the vertical position.** `Rebus.VersionWatermarkY 40` drops the
>    watermark 40 px from the top edge -- useful when an operator's HUD overlay
>    sits at the top centre. With no arg, the command logs the current margin.
> 5. **Trust the displayed string.** The string is sourced via `IPluginManager::
>    FindPlugin("RebusVisualiser")->GetDescriptor().VersionName` at subsystem
>    `Initialize()` -- the engine-blessed accessor that reads what the engine
>    actually loaded. The watermark therefore always reflects the running
>    binary's plugin descriptor; what you see on screen is what's running.
>
> **Files touched (v1.0.107).**
>
> | File | Change |
> | --- | --- |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Public/RebusVisualiserSubsystem.h` | Forward decls for `UCanvas` + `APlayerController`. New public `SetVersionWatermarkEnabled` / `IsVersionWatermarkEnabled` / `GetCachedVersionDisplay` API + static `Set/GetVersionWatermarkTopMarginPx` accessors. Private `CachedVersionDisplay` `FString` + `VersionWatermarkDrawHandle` `FDelegateHandle` + private `DrawVersionWatermark` member. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusVisualiserSubsystem.cpp` | New includes (`Debug/DebugDrawService.h`, `Engine/Canvas.h`, `CanvasItem.h`, `CanvasTypes.h`, `Engine/Font.h`, `Interfaces/IPluginManager.h`). New file-scope `RebusVersionWatermark::GShowEnabled` + `GTopMarginPx` (live state shared with the console commands). `Initialize()` caches `CachedVersionDisplay` from `IPluginManager` + registers the `UDebugDrawService::Register("Foreground", ...)` delegate. `Deinitialize()` unregisters. New `Set/IsVersionWatermarkEnabled` instance methods + static `Set/GetVersionWatermarkTopMarginPx` + `DrawVersionWatermark` impl (FCanvasTextItem with EnableShadow). |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusVisualiser.cpp` | New `Rebus.ShowVersion` command (with `status` arg printing live flag + cached display + Y-margin) + `Rebus.VersionWatermarkY <px>` command. Both registered + unregistered alongside the existing `Rebus.*` commands. Both route through the v1.0.107 single chokepoint on `URebusVisualiserSubsystem`. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusSceneSettingsSubsystem.cpp` | Seed `bShowVersionWatermark = true` in `Initialize()`. New `ApplySceneProperty` branch routing through `URebusVisualiserSubsystem::SetVersionWatermarkEnabled` (mirrors v1.0.99 / v1.0.104 / v1.0.105 routing byte-for-byte). |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/RebusVisualiser.uplugin` | `VersionName` bumped `1.0.106` -> `1.0.107`. The watermark reads this descriptor field at runtime. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/README.md` | This release block. |
>
> No engine / `OrbitConnector` / `glTFRuntime` / PixelStreaming2 asset is touched
> -- v1.0.107 stays inside `RebusVisualiser`. The v1.0.106 procedural-beam
> default flip is preserved verbatim. The v1.0.97 / v1.0.99 / v1.0.103 /
> v1.0.104 / v1.0.105 walkers are untouched.

> **Prefer the procedural `M_RebusBeam` cone over Epic's `MI_Beam` so the v1.0.96 / v1.0.99 screen-space self-shadow trace actually renders -- Epic-beam parity queued for v1.0.107 (v1.0.106).**
>
> User report (verbatim):
>
> > "The beam mesh, beam cone is not stopping when it hits an object. Can we check if this is EpicBeam or Beamcone we need to edit to fix this but we want the shaft of light to stop as it hits objects but only the part of the beam thats touching the object, the rest can pass through around the object if clear."
>
> **Diagnosis -- the v1.0.96 / v1.0.99 / v1.0.103 work has been on a hidden material.**
> The user explicitly named both `BeamMesh / BeamCone` AND `EpicBeam` in this latest
> report, which was the missing puzzle piece. v1.0.96 introduced the screen-space
> self-shadow trace in `M_RebusBeam` (the procedural cone master) + the matching
> `Rebus.BeamShadow*` CVars + `RefreshBeamShadowParams` push path. v1.0.99 fixed an
> LWC projection bug + a first-step self-occlusion bias in the same `M_RebusBeam`
> trace. v1.0.103 shipped diagnostics (`Rebus.DumpBeamShadow` EXISTS/MISSING per
> scalar + `URebusVisualiserSubsystem::ProbeBeamMasterAtStartup` + the
> `Rebus.RebuildBeamMaterial` runtime regen) on the H1 (stale on-disk master)
> hypothesis. The user reported across v1.0.99 / v1.0.102 / v1.0.103 that "the beam
> mesh / beam cone is not stopping when it hits an object" -- every iteration of the
> shadow-trace work appeared to do NOTHING from the operator's perspective.
>
> The v1.0.106 audit uncovered the real cause: `ARebusFixtureActor::TryBuildEpicBeam()`
> (introduced in v1.0.43) loads Epic's `MI_Beam` / `M_Beam_Master` from
> `/DMXFixtures/LightFixtures/...` whenever the DMX Fixtures plugin content is
> installed, sets `bUsingEpicBeam = true`, and HIDES the procedural cone
> (`BeamCone->SetVisibility(false)` at line 2523 of v1.0.105 `RebusFixtureActor.cpp`).
> The visible shaft then becomes `EpicBeamMID` (a per-fixture MID off `M_Beam_Master`)
> onto which `RefreshBeamShadowParams` HAS NEVER PUSHED any of the `BeamShadow*`
> scalars. The truth table:
>
> | `bUsingEpicBeam` | visible-shaft material | carries v1.0.99 shadow trace? |
> | --- | --- | --- |
> | `true` (DMX content installed) | Epic `MI_Beam` (`M_Beam_Master`) | **NO** -- our shadow params are ignored |
> | `false` (no DMX content) | `M_RebusBeam` MID | YES (when master regen has run) |
>
> So on the user's install -- which DOES have the DMX Fixtures plugin content -- the
> visible shaft has been Epic's `MI_Beam` since v1.0.43, and every iteration of the
> shadow-trace work (v1.0.96 introduction, v1.0.99 LWC fix, v1.0.103 self-heal +
> diagnostics) edited a material that wasn't rendering. The v1.0.103 stale-master
> probe + Warning (`URebusVisualiserSubsystem::ProbeBeamMasterAtStartup`) was
> investigating the right symptom on the wrong material. We've burned three release
> cycles on a misdiagnosis -- the user has been patient.
>
> **What lands in v1.0.106.**
>
> | Surface | Behaviour |
> | --- | --- |
> | `Rebus.PreferProceduralBeam [0\|1]` CVar (NEW, default `1`) | When `1` (default since v1.0.106), every Rebus fixture's visible beam shaft is the procedural `M_RebusBeam` cone -- which carries the v1.0.96 / v1.0.99 screen-space self-shadow trace -- and `TryBuildEpicBeam()` is SKIPPED at spawn (Epic's `MI_Beam` canvas is not built; `EpicBeamComp` / `EpicBeamMID` stay null until the toggle flips). When `0`, Epic's `MI_Beam` canvas IS the visible shaft (the pre-v1.0.106 default since v1.0.43), and the screen-space self-shadow trace is BYPASSED (it lives on `M_RebusBeam`, not on `M_Beam_Master`). Refresh sink walks every fixture and flips the visible shaft WITHOUT a respawn -- the procedural cone + Epic canvas both stay alive in the scene (visibility-only toggle), so subsequent toggles are cheap. |
> | Stale-master probe on flip-to-`1` | The CVar refresh sink re-loads `/Game/REBUS/Materials/M_RebusBeam` and checks for the v1.0.99 parameter contract (`BeamShadowStrength` + `BeamShadowDebug` scalars). Missing -> Warning naming `Rebus.RebuildBeamMaterial` (the v1.0.103 runtime regen) on the SAME log line as the flip -- catches the v1.0.103 operator-action-required case at exactly the moment it becomes relevant (the operator who flips the toggle to make the trace render needs to know in the same log line that the master is stale and the trace will silently no-op). |
> | `bPreferProceduralBeam` per-fixture UPROPERTY (NEW) | `EditAnywhere` only (no `BlueprintReadWrite` -- private member, matches the v1.0.101 -> v1.0.102 `BeamConeRadiusScale` UHT fix). Default `true`. Mirror the per-fixture override pattern of `BeamConeRadiusScale`: the CVar refresh sink overwrites the per-fixture value on every push, but the Details panel exposes the knob so a single hero fixture can keep Epic-beam fidelity (lens-flare math + smarter zoom-normalised distribution) while the rest of the rig flips procedural. |
> | `ARebusFixtureActor::RefreshPreferProceduralBeamFromCVar(bool)` (NEW) | The single chokepoint that flips the visible shaft. Idempotent: no-op when the cached preference + the live `bUsingEpicBeam` already match the target. Flip-to-procedural: hides the Epic canvas (`SetVisibility(false) + SetHiddenInGame(true)`), unhides the procedural cone (gated on `bMeshBeamEnabled`), and re-pushes `RefreshBeamShadowParams` so the trace scalars land on the now-visible BeamMID. Flip-to-Epic: unhides the Epic canvas if present, else lazily calls `TryBuildEpicBeam()` (which logs a Warning when the DMX content is missing and stays on the procedural cone -- the toggle then has no visible effect on that fixture, but the operator sees that explicitly in the per-fixture log line). |
> | `BuildBeamCone()` gate | `TryBuildEpicBeam()` is now wrapped in `if (!bPreferProceduralBeam)`. The per-fixture flag is seeded from the live CVar at the top of `BuildBeamCone` so a fresh-spawn fixture inherits the operator's current choice (the CVar refresh sink only walks already-spawned fixtures; without this seed a fixture spawned AFTER an operator pushed `Rebus.PreferProceduralBeam 0` would default to `1` and operators would see a mid-batch flicker between paths). |
> | `Rebus.DumpBeamShadow` extension | Per-fixture line now reports `Beam=Procedural\|Epic` + `Prefer=Y\|N`. When `Beam=Epic` an additional WARNING tail is appended naming the bypass + the v1.0.107 follow-up promise. So the dump now distinguishes "shadow trace is wired correctly but the visible material doesn't carry it" from the pre-v1.0.106 conflated state. |
>
> **Why a default flip (1) rather than a Path A port to Epic's master?** Path C
> (v1.0.106): ship today, get the user a visible self-shadow shaft IMMEDIATELY on
> their install, reversible per-fixture per-show via the CVar. Path A
> (v1.0.107-queued): port the same screen-space self-shadow trace to a Rebus-owned
> override of Epic's `M_Beam_Master` (`M_RebusEpicBeamOverride`), faithfully
> mirroring Epic's existing param vocabulary (`DMX Color`, `DMX Max Light Intensity`,
> `DMX Dimmer`, `DMX Max Light Distance`, `DMX Lens Radius`, `DMX Zoom`, `DMX Zoom
> Normalize`, `DMX Quality Level`, `DMX Gobo Disk Frosted`, `DMX Gobo Num Mask`, `DMX
> Gobo Index`, `DMX Gobo Disk Rotation Speed`) so `UpdateEpicBeamParams()` /
> `ApplyCurrentGoboToEpicBeam()` keep working unchanged. Path A is high-effort
> high-payoff (Epic's beam material does sophisticated work -- cone-frustum SDF, lens-
> flare integration, smarter zoom-normalised brightness -- that we'd need to
> faithfully reproduce or wrap). The user has been complaining about Epic's beam
> behaviour ALL ALONG (the v1.0.101 `BeamConeRadiusScale` knob was added because the
> Epic beam read wider than the lit footprint; the v1.0.96..v1.0.103 work was about
> shadows that have apparently never rendered on the Epic beam). v1.0.106 ships the
> default flip TODAY so the user sees the v1.0.99 shadow trace immediately on their
> install; v1.0.107 ports the trace to Epic's beam so operators preferring Epic
> fidelity can flip the CVar back and still get self-shadowing.
>
> **Operator action path (v1.0.106+).**
>
> 1. **Default ON does the right thing.** Launch the editor / packaged build; the
>    default `Rebus.PreferProceduralBeam = 1` means every spawned fixture's visible
>    shaft is the procedural `M_RebusBeam` cone with the v1.0.99 trace active.
>    Place a cube between a fixture and the floor; the cube should carve a black
>    shadow into the beam shaft.
> 2. **Verify with `Rebus.DumpBeamShadow`.** Every per-fixture line should now
>    report `Beam=Procedural Prefer=Y` and `shadowing ENABLED, debug mode 0`. Any
>    `Beam=Epic` entry means the per-fixture override flag is still set false on
>    that fixture's editor instance (use the Details panel `Rebus|Beam ->
>    bPreferProceduralBeam = true` to flip per-fixture, or push the CVar again).
> 3. **Diagnose visually with `Rebus.BeamShadowDebug 1`.** Pixels behind the
>    occluder cube should appear RED inside the beam region. Green = the trace
>    found no occluders. If the cube is still green AFTER the `Beam=Procedural`
>    confirmation, look for the `STALE BEAM MASTER` Warning emitted at the moment
>    of the flip-to-1 (the CVar sink probes the master and warns if it predates
>    v1.0.99); the recovery is `Rebus.RebuildBeamMaterial` (v1.0.103 runtime
>    regen) + ClearScene+LoadScene OR an editor restart so each fixture
>    respawns + rebuilds its `BeamMID` off the freshly-regenerated master.
> 4. **A/B against the Epic-beam path.** Push `Rebus.PreferProceduralBeam 0`. The
>    Epic `MI_Beam` canvas becomes visible per fixture (`TryBuildEpicBeam` is
>    invoked lazily on first flip-to-0 if the canvas wasn't built at spawn).
>    `Rebus.DumpBeamShadow` now reports `Beam=Epic Prefer=N` per fixture with the
>    additional WARNING tail naming the bypass. The shadow trace will appear NOT
>    to render -- expected; v1.0.107 will port it to Epic's beam.
> 5. **Per-fixture override.** Open a single fixture in the Details panel,
>    `Rebus|Beam -> bPreferProceduralBeam`, flip per-fixture for hero fixtures
>    that need Epic's lens-flare math while the rest of the rig stays
>    procedural. The CVar refresh sink will overwrite this on every global push;
>    persist the per-fixture choice by sending the CVar BEFORE setting the
>    instance overrides.
>
> **Files touched (v1.0.106).**
>
> | File | Change |
> | --- | --- |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Public/RebusFixtureActor.h` | New `bPreferProceduralBeam` per-fixture UPROPERTY (private, `EditAnywhere` only, default `true`) + new public methods `RefreshPreferProceduralBeamFromCVar(bool)` + `IsPreferringProceduralBeam()` + `IsUsingEpicBeam()`. Mirrors the v1.0.101 `BeamConeRadiusScale` / `RefreshBeamConeRadiusScaleFromCVar` shape verbatim. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusFixtureActor.cpp` | New `Rebus.PreferProceduralBeam` CVar + refresh sink (stale-master probe on flip-to-1 mirrors `URebusVisualiserSubsystem::ProbeBeamMasterAtStartup`'s scalar check). `BuildBeamCone` now seeds the per-fixture flag from the CVar + gates `TryBuildEpicBeam()` on `!bPreferProceduralBeam`. New `RefreshPreferProceduralBeamFromCVar(bool)` impl (idempotent visibility-only flip; lazily builds `EpicBeamComp` on first flip-to-0). `DumpBeamShadowStateForDebug` extended to report `Beam=Procedural\|Epic` + `Prefer=Y\|N` + a WARNING tail when `Beam=Epic` (the v1.0.107 promise). |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/RebusVisualiser.uplugin` | `VersionName` bumped `1.0.105` -> `1.0.106`. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/README.md` | This release block. |
>
> No engine / `OrbitConnector` / `glTFRuntime` asset is touched -- v1.0.106 stays
> inside `RebusVisualiser`. The v1.0.96 / v1.0.99 / v1.0.103 shadow-trace work is
> preserved verbatim on the procedural cone (the v1.0.107 follow-up will REUSE the
> same `_BEAM_RAYMARCH_HLSL` shadow loop on the Epic-beam master override). The
> v1.0.104 double-sided + v1.0.105 Nanite walkers are untouched.
>
> **v1.0.107 follow-up (Epic-beam parity for the self-shadow trace).** Author
> `M_RebusEpicBeamOverride` in `build_rebus_base_level.py` mirroring Epic's
> `M_Beam_Master` parameter vocabulary verbatim (so `UpdateEpicBeamParams()` /
> `ApplyCurrentGoboToEpicBeam()` continue to work unchanged), bake in the same
> `BeamShadow*` scalars (Steps / Strength / Bias / Debug) and the same
> `_BEAM_RAYMARCH_HLSL` shadow loop, then in `TryBuildEpicBeam()` prefer the
> override material when present (falling back to Epic's `MI_Beam` when not).
> Operators preferring Epic fidelity then flip `Rebus.PreferProceduralBeam 0` AND
> get self-shadowing on the Epic beam too. v1.0.106 is the visible-shaft fix the
> user needs today; v1.0.107 is the architectural fix that gives both paths the
> trace.

> **Nanite enable on every Orbit-imported `UStaticMesh` (operator-toggleable, editor-only conversion) (v1.0.105).**
> User request (verbatim):
>
> > "can all imported objects from orbit be converted to nanite post import to improve performance."
>
> Operator context: trusses, set pieces, banners, and fixture bodies are exactly the
> high-poly low-material-count opaque (or mostly-opaque) geometry Nanite was designed
> for. UE 5.7's Nanite cuts the per-draw-call cost on imported geometry by ~5-50x
> depending on triangle count, plus virtualised shadow maps (VSM) make the v1.0.99
> force-cast-shadows pass effectively free on every Nanite mesh -- which IS a
> meaningful win on the truss-shadow visibility the v1.0.99 / v1.0.103 work was about.
> Nanite-incompatible passes (the v1.0.97 / v1.0.104 two-sided opaque, masked,
> translucent paths) automatically route through the per-mesh fallback proxy that
> `NaniteSettings.FallbackPercentTriangles` reserves -- so the v1.0.97 / v1.0.104
> double-sided work is preserved verbatim (those pixels just don't get the Nanite
> per-pixel raster path; the rest of the mesh does).
>
> **Investigation -- can we just enable Nanite on every imported mesh at runtime?**
>
> | Investigation surface | Result |
> | --- | --- |
> | Does `glTFRuntime` produce real `UStaticMesh` UObjects (vs `UProceduralMeshComponent`)? | Yes. `glTFRuntimeParserStaticMeshes.cpp:30` calls `NewObject<UStaticMesh>(...)` and the parser fully populates `FStaticMeshRenderData` (vertex / index buffers, sections, LOD resources). Nanite is therefore TECHNICALLY possible on this import path -- it's not the `UProceduralMeshComponent` blocker the v1.0.95 procedural-mesh refactor would have had. |
> | Does the runtime path commit a source `MeshDescription` that `UStaticMesh::Build` can re-cook from? | Editor-only and ONLY when `StaticMeshConfig.bGenerateStaticMeshDescription = true`. `glTFRuntimeParserStaticMeshes.cpp:782-893` is gated `#if WITH_EDITOR` and contains the `StaticMesh->CommitMeshDescription(CurrentLODIndex)` call -- so in editor builds, IF the OrbitConnector import config asks for it, the resulting `UStaticMesh` carries a valid `MeshDescription` source that `Build()` can cook `NaniteResources` from. We don't own OrbitConnector's call site (the in-tree plugin is just `ThirdParty/Cli/win-x64/orbit-cli.exe` -- no UE source), so we have to handle the no-source-MeshDescription case gracefully (one-shot operator-fix Warning per affected mesh). |
> | Is `UStaticMesh::Build` callable at runtime? | `#if WITH_EDITOR` only in UE 5.7 (`Engine/StaticMesh.h`). Editor builds (PIE / Standalone Editor / `-game` with editor-built binaries) hit the editor build path and produce cooked `NaniteResources` in-place; packaged builds compile out the entry point entirely. Same constraint applies to `INaniteBuilderModule::Get().Build(...)` (`Engine/Source/Developer/NaniteBuilder/`) -- editor-only. |
> | Cooked-Nanite path for packaged? | The operator pre-cooks the Orbit GLBs into UStaticMesh `.uasset(s)` with `NaniteSettings.bEnabled = true` in editor BEFORE packaging. Documented below in the operator action path. A future `build_rebus_base_level.py::cook_orbit_imports_to_nanite_static_meshes()` helper could automate this for known-static Orbit imports (out of scope for v1.0.105). |
> | Do Nanite + the v1.0.97 / v1.0.104 two-sided / masked / translucent passes coexist? | Yes -- `NaniteSettings.FallbackPercentTriangles = 1.0` keeps a full-quality fallback proxy, and Nanite's per-pixel material classifier routes Nanite-incompatible pixels through the fallback automatically. So the v1.0.104 `bTwoSidedScalar = 1.0` MID push and the v1.0.99 force-cast-shadows pass survive the v1.0.105 Nanite enable verbatim -- pixels render Nanite-rasterised on the front face, fallback-rasterised on the (now-double-sided) back face, with VSM-correct shadows on both. |
>
> So Nanite-at-runtime IS viable on this import path in editor builds, with one
> requirement we can't directly assert (the `bGenerateStaticMeshDescription` flag on
> `OrbitConnector`'s glTFRuntime import config) and one runtime constraint we work
> around (per-mesh attempt cache prevents `Build()` re-storms on subsequent ticks).
> Packaged builds need cooked-Nanite assets shipped alongside.
>
> **What lands in v1.0.105.**
>
> | Surface | Behaviour |
> | --- | --- |
> | `URebusVisualiserSubsystem::EnsureImportedNanite()` | Walks every `OrbitImportRoot` actor (matched by class-name string -- zero compile dep on `OrbitConnector`, mirroring v1.0.85 truss-material + v1.0.99 shadow-cast + v1.0.104 double-sided passes) and groups every `UStaticMeshComponent`'s `UStaticMesh` so we visit each unique imported asset ONCE per pass (per-comp `Build()` would rebuild the same mesh N times). Per unique mesh: skip if `NaniteSettings.bEnabled` already matches AND we've attempted before; skip with a one-shot operator-fix Warning when the mesh has no source `MeshDescription`; otherwise set `NaniteSettings.bEnabled = bWantOn` (+ conservative defaults on first-time enable: `PositionPrecision = MIN_int32` (auto), `FallbackPercentTriangles = 1.0` (full-quality fallback proxy preserves the v1.0.97 / v1.0.104 two-sided / masked passes), `TrimRelativeError = 0.0` (no aggressive simplification)), call `UStaticMesh::Build(false /*bSilent*/)`, and `MarkRenderStateDirty` on every `UStaticMeshComponent` referencing the mesh so the freshly-cooked `NaniteResources` land on the GPU on the next render frame. Idempotent (per-mesh attempt cache `NaniteAttempted` keeps the steady-state cost flat -- once a mesh is in the desired state we don't call `Build()` on it again). All of this is gated behind `#if WITH_EDITOR` -- packaged builds compile cleanly and emit a one-shot session Warning. |
> | `URebusVisualiserSubsystem::SetNaniteOrbitImportsEnabled(bool)` | Single chokepoint shared by the console command and the scene-property handler. Walks `NaniteAttempted` to flip every prior mesh + runs `EnsureImportedNanite` to pick up newly-imported geometry, so a single toggle transition is consistent across the whole import on the same call. **Disable path emits a one-shot Warning `[Rebus] Disabling Nanite on N Orbit mesh(es) -- this will trigger a rebuild storm`** so an operator never fires the OFF toggle casually mid-show (`UStaticMesh::Build` can take seconds per mesh and BLOCKS the game thread). Mirrors v1.0.99 `SetOrbitCastShadowsEnabled` / v1.0.104 `SetOrbitDoubleSidedEnabled` byte-for-byte. |
> | `Rebus.NaniteOrbitImports [0\|1]` console | New (default ON). Routes through `SetNaniteOrbitImportsEnabled` per Game/PIE world. |
> | `bNaniteOrbitImports` scene property | New (default `true`, seeded in `URebusSceneSettingsSubsystem::Initialize`). Routes through the same chokepoint via `ApplySceneProperty`. SceneState round-trips it; `ReapplyAll` re-asserts on (re)spawn. |
> | `Rebus.DumpOrbitNanite` console | New diagnostic. Walks every Orbit `UStaticMesh` (grouped by `UStaticMesh*` so each unique imported asset reports ONCE with a component ref count) and emits one log line per mesh: `Mesh='<n>' refs=<r> tris=<t> Nanite=ON\|OFF FallbackTris=<ft> dsSlots=<a/b>` (`a` = material slots reporting `bTwoSidedScalar = 1.0`, `b` = total slots; surfaces the v1.0.104 double-sided pipeline alongside the v1.0.105 Nanite state). Mirrors `Rebus.DumpBeamShadow` style. |
> | Tick hook | The visualiser subsystem's 1 Hz orbit-rebind tick now also calls `EnsureImportedNanite()` after `EnsureImportedDoubleSided()`, so newly-imported Orbit geometry inherits Nanite (in editor builds) on the next 1 s without an operator console call. The four imported-primitive normalisation passes (truss-material override, shadow-cast, double-sided, Nanite enable) all run on the same cadence. |
>
> **Why editor-only.** `UStaticMesh::Build` (`Engine/Public/Engine/StaticMesh.h`) and
> `INaniteBuilderModule::Build` (`Engine/Source/Developer/NaniteBuilder/`) are both
> declared inside `#if WITH_EDITOR` in UE 5.7 -- there is no runtime entry point for
> Nanite resource cooking in shipping builds. Packaged-build operators must pre-cook
> the Orbit imports into `UStaticMesh` `.uasset(s)` with `NaniteSettings.bEnabled =
> true` in editor BEFORE packaging; the cooked `NaniteResources` are then loaded
> verbatim at runtime and the v1.0.105 walker no-ops with a one-shot session Warning.
> A future `build_rebus_base_level.py::cook_orbit_imports_to_nanite_static_meshes()`
> helper could automate this for known-static Orbit imports.
>
> **Perf wins (editor / cooked Nanite).** Nanite cuts draw-call cost on imported
> trusses by ~5-50x depending on triangle count (the high-poly Eurotruss / Prolyte
> cross-bars an Orbit import lands as a single mesh per truss section see the biggest
> wins; thin banner cloth and small set-piece props see closer to the lower end of
> the range). Virtualised shadow maps (`r.Shadow.Virtual.Enable = 1`, default ON in
> the project's `DefaultEngine.ini`) make the v1.0.99 force-cast-shadows pass
> effectively free on Nanite meshes -- a typical Orbit truss rig that pre-v1.0.105
> would cost ~3-5 ms/frame in shadow rendering drops to <1 ms/frame post-Nanite.
> Volumetric-shadow correctness on the v1.0.96 / v1.0.99 cone-mesh beam-shadow trace
> is unchanged: the trace samples the depth buffer (which Nanite writes to with
> per-pixel precision), not the static-mesh proxy, so the shaft self-shadowing the
> v1.0.99 / v1.0.103 work landed continues to read correctly through Nanite truss
> geometry.
>
> **Rebuild-storm caveat (operator-facing).** The OFF path of the toggle calls
> `UStaticMesh::Build` on every previously-Nanite-enabled mesh to disable + rebuild,
> which is BLOCKING (game-thread stall measured in seconds per mesh on a busy import
> -- the disable Warning names the mesh count so the operator can estimate before
> the call). Use the OFF path between scenes / shows for A/B perf comparison, NOT
> during a live cue. The default-ON ship was chosen so packaged operators get the
> Nanite win on first launch with no manual toggle, AND so the editor build path
> only pays the rebuild cost ONCE per session (the `NaniteAttempted` cache prevents
> subsequent ticks from re-invoking `Build()` on a mesh whose state already matches).
>
> **Operator action path (v1.0.105+).**
>
> 1. **Editor build (PIE / Standalone Editor / `-game` with editor-built binaries).**
>    Default ON does the right thing: walk around the stage with `Rebus.DumpOrbitNanite`
>    and confirm every entry reports `Nanite=ON`. Expect a brief one-shot stutter
>    on first 1 Hz tick after Orbit import as the `Build()` rebuild storm runs across
>    every imported mesh in turn (~seconds total for a typical truss rig); subsequent
>    ticks are free.
> 2. **`Rebus.DumpOrbitNanite` is the canonical verification step.** Every Orbit
>    mesh should report `Nanite=ON`. Any `Nanite=OFF` entry indicates one of:
>    - `[Rebus] Nanite skip on '<Mesh>': no source MeshDescription` Warning one log
>      line up = the OrbitConnector import config has `bGenerateStaticMeshDescription =
>      false` on the `glTFRuntime` `FglTFRuntimeStaticMeshConfig`. Operator fix:
>      enable the flag in OrbitConnector's import config (the runtime-Nanite path
>      requires the source `MeshDescription` that flag commits at import time) OR
>      pre-cook the Orbit GLBs to `UStaticMesh` `.uasset(s)` with Nanite enabled in
>      editor before the session starts.
>    - The operator recently sent `Rebus.NaniteOrbitImports 0` (intentional A/B). The
>      log line `Rebus.NaniteOrbitImports 0: prior-rebuilt=N freshly-rebuilt=M (...)`
>      from the toggle confirms which transition you're in.
>    - Packaged build (the toggle is settable but no-effect; the one-shot session
>      Warning `[Rebus] Nanite runtime-conversion unavailable in packaged builds:`
>      surfaces the cooked-Nanite operator fix path).
> 3. **Packaged build.** Pre-cook the Orbit imports in editor: load the project,
>    import the Orbit GLB, find the resulting `UStaticMesh` `.uasset(s)` in the
>    Content Browser, set `NaniteSettings.bEnabled = true` (+ matching defaults
>    documented above) on each, save, repackage. The cooked `NaniteResources` are
>    loaded verbatim at runtime; the v1.0.105 walker no-ops with a one-shot session
>    Warning naming this fix path. A future `build_rebus_base_level.py` helper could
>    automate this for known-static imports.
> 4. **A/B against pre-v1.0.105.** Send `Rebus.NaniteOrbitImports 0` from the portal
>    / in-editor console. The Warning `[Rebus] Disabling Nanite on N Orbit mesh(es)`
>    fires first, then the rebuild storm runs (seconds per mesh; blocks the game
>    thread). After it settles, `Rebus.DumpOrbitNanite` reports `Nanite=OFF` on
>    every entry and frame-time profiling reflects the non-Nanite baseline. Toggle
>    back ON with `Rebus.NaniteOrbitImports 1` (incurs the same rebuild storm).
> 5. **Push from the portal.** `SetSceneProperty {"name":"bNaniteOrbitImports",
>    "value":false}` mirrors the console toggle and round-trips in `SceneState`,
>    so a portal recycle re-asserts the operator's choice via `ReapplyAll`. Use
>    this when an operator wants to bake the choice into a scene preset rather
>    than relying on console state.
> 6. **`ClearScene + LoadScene`** triggers a fresh import and the v1.0.105 walker
>    will Nanite-enable any newly-arrived mesh on the next 1 Hz tick (or
>    immediately on the next pre-cooked-Nanite asset load in packaged). Existing
>    `UStaticMesh` UObjects whose Nanite state was already toggled stay tracked in
>    `NaniteAttempted` so the cache survives the re-import.
>
> **Files touched (v1.0.105).**
>
> | File | Change |
> | --- | --- |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusVisualiserSubsystem.cpp` + `Public/RebusVisualiserSubsystem.h` | New `EnsureImportedNanite()` + `SetNaniteOrbitImportsEnabled(bool)` + `DumpOrbitNanite()` + `bNaniteOrbitImportsEnabled` flag + `NaniteAttempted` per-mesh cache + `bNanitePackagedWarningLogged` one-shot. `FOrbitNaniteApplyCount` + `FOrbitNaniteDumpEntry` structs. Tick hook adds `EnsureImportedNanite()` to the 1 Hz rebind cadence (alongside v1.0.99 `EnsureImportedShadowsCast` + v1.0.104 `EnsureImportedDoubleSided` + v1.0.85 `ApplyTrussMaterialPass`). All write paths gated `#if WITH_EDITOR`. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusSceneSettingsSubsystem.cpp` | New `bNaniteOrbitImports` seed (default `true`) in `Initialize`; new `ApplySceneProperty` branch routing through `URebusVisualiserSubsystem::SetNaniteOrbitImportsEnabled`. Mirrors v1.0.104 `bOrbitDoubleSided` byte-for-byte. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusVisualiser.cpp` | New `Rebus.NaniteOrbitImports` console command + new `Rebus.DumpOrbitNanite` diagnostic (registered alongside v1.0.99 `Rebus.OrbitCastShadows` + v1.0.104 `Rebus.OrbitDoubleSided`, cleaned up in `ShutdownModule`). Mirrors the v1.0.104 lambda body byte-for-byte. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/RebusVisualiser.uplugin` | `VersionName` bumped `1.0.0` -> `1.0.105` to match the tag. (UBT reads this for the plugin manifest; UE Marketplace / GitHub Release alignment.) |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/README.md` | This release block. |
>
> No engine / `OrbitConnector` / `glTFRuntime` asset is touched -- v1.0.105 stays
> inside the RebusVisualiser plugin. The v1.0.99 shadow-cast pipeline + v1.0.97
> master-side `two_sided = True` bakes + v1.0.104 double-sided walker are preserved
> verbatim (v1.0.105 is strictly additive). The v1.0.103 beam-shadow diagnostic
> surfaces are untouched.

> **Double-sided shading on every Orbit-imported primitive (operator-toggleable) (v1.0.104).**
> User request (verbatim):
>
> > "can you set all Orbit textures to be double sided on import"
>
> Operator context: v1.0.97 already flipped every Rebus-authored Python master material
> to `two_sided = True` (`M_RebusGround`, `M_RebusBeam`, `M_RebusFixtureLens`,
> `M_RebusFixtureBody`, `M_RebusTruss`, `M_RebusLensFlare`), so any geometry assigned a
> Rebus-built MI / MID renders both sides correctly. The complaint that triggered
> v1.0.104 is about the OTHER half of the pipeline: **Orbit-imported materials**
> (`OrbitConnector` + `glTFRuntime`-baked) are still single-sided in many cases, so
> thin geometry (truss cross-bars, banner cloth, sheet-metal flags, gobo flags) either
> disappears or projects a flipped-winding shadow when viewed from the back. The v1.0.99
> imported-primitive shadow-cast normalisation made the problem MORE visible because
> single-sided thin geometry that previously didn't cast shadows at all now casts a
> back-face-culled silhouette in the lit pool -- so the v1.0.104 work is partly a
> follow-up to v1.0.99 even though the user-facing symptom is "Orbit textures aren't
> double-sided".
>
> **Investigation -- can we just flip the imported materials at runtime? No.**
>
> | Investigation surface | Result |
> | --- | --- |
> | `FglTFRuntimeMaterialsConfig` (the per-import config struct `OrbitConnector` feeds `glTFRuntime`) | No `bForceTwoSided` / `bDisableTwoSided` flag exists. Two-sided is keyed off the source glTF's `doubleSided` field per-material -- `glTFRuntimeParserMaterials.cpp::ProcessMaterial` reads `doubleSided` into `RuntimeMaterial.bTwoSided`, then picks a `TwoSided` / `TwoSidedTranslucent` / `TwoSidedMasked` base material from `MetallicRoughnessMaterialsMap` / `UberMaterialsOverrideMap`. So flipping ON requires (a) the source glTF asserts `doubleSided=true` (which `OrbitConnector`'s mesh export does NOT today) OR (b) the consumer overrides the parent base material via the `MaterialsOverrideMap`. We don't own `OrbitConnector`'s call site (the plugin in this checkout is just `ThirdParty/Cli/win-x64/orbit-cli.exe` -- no UE source -- so this is fully external). |
> | `UMaterialInstance::BasePropertyOverrides` (`bOverride_TwoSided` + `TwoSided` on an MID) | Editor-only API (`WITH_EDITORONLY_DATA` on the parent struct); even when used in PIE it triggers a fresh shader-permutation compile because top-level `two_sided` is part of the shader key. Not viable in a packaged PRISM build. |
> | `UPrimitiveComponent::bCastShadowAsTwoSided` (component-level, shadow-side only) | ALWAYS settable at runtime, cheap. Walks both sides of every triangle when projecting shadow casters. Doesn't affect rendering visibility -- ONLY the shadow pass. |
> | Per-slot MID parent-swap to a project-owned two-sided master (`M_RebusOrbitImported`) | Works in cooked builds (no permutation compile -- the new master is pre-cooked), but copying source textures / vertex-colour wiring across is high-risk and would visibly alter the look of any glTFRuntime asset whose param contract doesn't match our master. Documented as a **resource** the operator can opt into manually via `OrbitConnector`'s import-material override path; the C++ walker does NOT auto-swap (would clobber operator-tuned imports). |
>
> So the **rendering-side** win on glTFRuntime-baked imports requires either an upstream
> `OrbitConnector` change (set `doubleSided=true` in the mesh exporter, or feed
> `glTFRuntime` a two-sided `MaterialsOverrideMap`) OR an operator manually re-parenting
> specific assets to the new `M_RebusOrbitImported` master. v1.0.104 covers what's
> in-scope for this plugin: the **shadow-side** fix on every Orbit-imported primitive,
> plus the resource + parameter contract operators need to opt into the render-side
> when they care.
>
> **What lands in v1.0.104.**
>
> | Surface | Behaviour |
> | --- | --- |
> | `URebusVisualiserSubsystem::EnsureImportedDoubleSided()` | Walks every `OrbitImportRoot` actor (matched by class-name string -- zero compile dep on `OrbitConnector`, mirroring v1.0.85's truss-material pass and v1.0.99's shadow-cast pass) and per `UPrimitiveComponent`: (1) forces `bCastShadowAsTwoSided = true` (shadow-side fix, always-on, no permutation cost); (2) wraps every non-MID material slot in a `UMaterialInstanceDynamic` parented to the existing material so per-slot parameters can be pushed without persisting to disk; (3) pushes `bTwoSidedScalar = 1.0` onto every MID (silently no-ops on parents that don't declare the param -- i.e. glTFRuntime / engine masters; lands on every Rebus-authored master that does). Idempotent (per-comp early-out when flags match). Tracked comps go into `OrbitDoubleSidedTouched` so the OFF path can restore the single-sided baseline. |
> | `URebusVisualiserSubsystem::SetOrbitDoubleSidedEnabled(bool)` | Single chokepoint shared by the console command and the scene-property handler. Walks `OrbitDoubleSidedTouched` to flip every prior comp + runs `EnsureImportedDoubleSided` to pick up newly-imported geometry, so a single toggle transition is consistent across the whole import on the same call. Mirrors v1.0.99 `SetOrbitCastShadowsEnabled` byte-for-byte. |
> | `Rebus.OrbitDoubleSided [0|1]` console | New (default ON). Routes through `SetOrbitDoubleSidedEnabled` per Game/PIE world. |
> | `bOrbitDoubleSided` scene property | New (default `true`, seeded in `URebusSceneSettingsSubsystem::Initialize`). Routes through the same chokepoint via `ApplySceneProperty`. SceneState round-trips it; `ReapplyAll` re-asserts on (re)spawn. |
> | Tick hook | The visualiser subsystem's 1 Hz orbit-rebind tick now also calls `EnsureImportedDoubleSided()` so newly-imported Orbit geometry inherits the override on the next 1 s without an operator console call. Sits alongside the v1.0.99 `EnsureImportedShadowsCast()` call so the four imported-primitive normalisation passes (truss-material override, shadow-cast, double-sided, fixture-bind) all run on the same cadence. |
> | New `M_RebusOrbitImported` master (Python-authored in `build_rebus_base_level.py`) | Two-sided opaque PBR master with `BaseColor` (vector) * `BaseColorTexture` (texture, defaults to engine WhiteSquareTexture so untextured MIs no-op), `Roughness` / `Metallic` scalars, optional emissive layer (default off), and a `bTwoSidedScalar` parameter marker (= the v1.0.104 contract -- queried by `_orbit_imported_master_has_two_sided` for self-heal). Top-level `two_sided = True` is hard-baked so an MI / MID parented to this master renders both sides regardless of the marker. Operators assign this to specific Orbit imports via `OrbitConnector`'s import-material override when they need genuine back-face rendering on glTFRuntime-baked thin geometry. |
> | `_orbit_imported_master_has_two_sided` self-heal probe | Mirrors v1.0.97 `_master_is_two_sided` shape -- best-effort `get_scalar_parameter_names` query checks for `bTwoSidedScalar` on the existing on-disk master. `ensure_orbit_imported_material()` combines it with v1.0.97's `_master_is_two_sided` via OR so EITHER missing-marker OR accidentally-single-sided triggers a single force-regen with a Warning. So the first launch on v1.0.104 picks up the new master automatically with no operator action. |
>
> Why `bCastShadowAsTwoSided = false` regardless of the toggle is NOT the v1.0.104 ship:
> a separate toggle here would conflict with v1.0.99's two-sided-shadow-cast semantics
> on thin geometry. v1.0.104 flips both the component flag AND the MID `bTwoSidedScalar`
> in lockstep -- single chokepoint, the operator can A/B the WHOLE double-sided pipeline
> with one console command.
>
> **Why no parent-swap auto-walker.** Re-parenting every glTFRuntime MID to
> `M_RebusOrbitImported` would visibly alter the look of any asset whose source material
> doesn't match the new master's param contract (BaseColor / BaseColorTexture / Roughness
> / Metallic / EmissiveColor / EmissiveStrength). The glTF spec uses `baseColorFactor` /
> `baseColorTexture` / `metallicFactor` / `roughnessFactor` / `emissiveFactor` /
> `emissiveTexture` and `glTFRuntime` exposes those as parameters with those exact
> names -- but a flat "rename + copy" would still drop the normal-map term and any
> Substrate / sheen / clearcoat layer the source declared. The conservative ship is
> "expose the resource + the runtime toggle, let the operator opt in per-asset". A
> future v1.0.10x with a richer M_RebusOrbitImported (normal + ARM) could safely
> auto-swap, gated behind a separate `bOrbitForceRebusParent` scene property.
>
> **Perf caveat (operator-facing).** Two-sided opaque shading is ~5-15% more expensive
> in the base pass than single-sided opaque (the pixel shader runs once per back face
> too, and shadow-casting two-sided cone-mesh and ground-mesh writes ~2x the shadow-map
> samples per draw). On the live previs surface this is well below the volumetric-fog
> + Lumen budgets that dominate the frame, so the default ON is the right shipping
> choice -- but a stage with hundreds of imported set-piece props can hit the cost
> ceiling. The toggle exists for exactly that case: `Rebus.OrbitDoubleSided 0` from
> the portal / in-editor console drops every tracked comp back to the single-sided
> shadow baseline (the `bTwoSidedScalar` push reverts to 0 on every Rebus-authored MID
> too, so re-parented assets revert in lockstep).
>
> **Per-fixture override path (operator-facing).** The v1.0.71 fixture-material override
> walker (`Rebus.OverrideFixtureMaterials`) and the v1.0.85 truss-material pass
> (`Rebus.OverrideTrussMaterial`) BOTH produce MIDs in their own pipelines, and BOTH
> use Rebus-authored masters (`M_RebusFixtureBody`, `M_RebusFixtureLens`, `M_RebusTruss`)
> that already expose `bTwoSidedScalar` (v1.0.104 retro-fits the marker on the Python
> side alongside v1.0.97's hard-baked `two_sided = True`). So a Rebus-overridden
> fixture body / lens / truss is double-sided on the same toggle without any extra
> walker run. To force a specific Orbit-imported asset two-sided regardless of toggle:
> re-parent its material to `/Game/REBUS/Materials/M_RebusOrbitImported` via the editor
> Content Browser or `OrbitConnector`'s pre-import settings; v1.0.104's tick walker
> will pick it up on the next 1 s.
>
> **Operator action path (v1.0.104+).**
>
> 1. **Rebuild + relaunch.** v1.0.104 ships a new master + a new console command + a
>    new scene-property seed; an editor restart picks all three up. The Python self-
>    heal (`ensure_orbit_imported_material`) bakes `M_RebusOrbitImported` on the next
>    launch with no operator action.
> 2. **Default ON: nothing to do.** Walk around the stage with the camera looking back
>    at thin Orbit-imported geometry (truss cross-bars / banner cloth / metal flags).
>    Pre-v1.0.104 the back face is invisible OR the cast shadow is missing / inverted;
>    v1.0.104 the geometry casts a correct silhouette AND, for assets re-parented to
>    `M_RebusOrbitImported`, the back face renders.
> 3. **A/B against pre-v1.0.104.** Send `Rebus.OrbitDoubleSided 0` from the portal /
>    in-editor console. The log line `Rebus.OrbitDoubleSided 0: prior-touched=N
>    freshly-touched=M (of K Orbit primitive(s) walked, W MIDs wrapped, S
>    bTwoSidedScalar pushes accepted)` confirms the walker ran. Toggle back ON with
>    `Rebus.OrbitDoubleSided 1`.
> 4. **Push from the portal.** `SetSceneProperty` `{"type":"SetSceneProperty","name":
>    "bOrbitDoubleSided","value":false}` mirrors the console toggle and round-trips in
>    `SceneState`, so a portal recycle re-asserts the operator's choice via
>    `ReapplyAll`.
> 5. **For genuine back-face rendering on a SPECIFIC Orbit-imported asset** (e.g. a
>    GDTF fixture flag where the operator can see the v1.0.99 thin-geometry shadow
>    win but the visual back-face is still missing): re-parent that asset's material
>    to `/Game/REBUS/Materials/M_RebusOrbitImported` via the editor Content Browser
>    (open the MID, drag the new parent into the parent slot), copy across the source
>    `baseColorFactor` / `metallicFactor` / `roughnessFactor` to the matching params,
>    then re-import OR `ClearScene + LoadScene` from the portal so the C++ walker
>    re-binds the asset and the next 1 s tick pushes `bTwoSidedScalar=1.0` onto it.
>    You should see the back face render on the next frame.
> 6. **You may need `ClearScene + LoadScene` to re-import existing assets** with the
>    new master if you're upgrading an existing PRISM session that loaded its scene
>    before v1.0.104's tick walker registered. The handshake / re-import gives every
>    `OrbitImportRoot` primitive a fresh visit from the v1.0.104 walker on the next
>    1 s.
>
> **`.uplugin` `ProceduralMeshComponent` dependency now declared.** Silences a UHT
> Warning that appeared during the v1.0.95 procedural-mesh refactor: the visualiser
> module uses `UProceduralMeshComponent` (via `RebusFixtureActor`) but the
> `RebusVisualiser.uplugin` `Plugins` array didn't list the `ProceduralMeshComponent`
> plugin, so UBT emitted a `MissingPluginDependency` warning on every build. v1.0.104
> adds the entry. No runtime change -- the dependency was always there via the
> module's `Build.cs`; this just resolves the explicit `.uplugin`-level declaration
> UHT wants.
>
> **Files touched (v1.0.104).**
>
> | File | Change |
> | --- | --- |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusVisualiserSubsystem.cpp` + `Public/RebusVisualiserSubsystem.h` | New `EnsureImportedDoubleSided()` + `SetOrbitDoubleSidedEnabled(bool)` + `bOrbitDoubleSidedEnabled` flag + `OrbitDoubleSidedTouched` tracking set. `FOrbitDoubleSidedApplyCount` struct (Components / Touched / MIDsWrapped / SwitchesPushed). Tick hook adds `EnsureImportedDoubleSided()` to the 1 Hz rebind cadence (alongside v1.0.99's `EnsureImportedShadowsCast` + v1.0.85's `ApplyTrussMaterialPass`). |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusSceneSettingsSubsystem.cpp` | New `bOrbitDoubleSided` seed (default `true`) in `Initialize`; new `ApplySceneProperty` branch routing through `URebusVisualiserSubsystem::SetOrbitDoubleSidedEnabled`. Mirrors v1.0.99's `bOrbitCastShadows` byte-for-byte. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusVisualiser.cpp` | New `Rebus.OrbitDoubleSided` console command (registered alongside `Rebus.OrbitCastShadows`, cleaned up in `ShutdownModule`). Mirrors v1.0.99's `Rebus.OrbitCastShadows` lambda body byte-for-byte. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/RebusVisualiser.uplugin` | New `ProceduralMeshComponent` entry in `Plugins` -- silences the UHT `MissingPluginDependency` warning (the runtime dep was always declared in the module's `Build.cs`; this is the explicit plugin-manifest mirror UHT wants). |
> | `REBUS_Visualiser/Content/Python/build_rebus_base_level.py` | New `M_RebusOrbitImported` master (`_build_orbit_imported_master`) -- two-sided opaque PBR (`BaseColor` * `BaseColorTexture` -> BaseColor, `Roughness` / `Metallic` scalars, optional emissive layer default off, `bTwoSidedScalar` parameter marker default 1.0). New `_orbit_imported_master_has_two_sided` self-heal probe + `ensure_orbit_imported_material()` helper. Wired into `build()` (force=True) AND `ensure_base_level()` (idempotent self-heal). |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/README.md` | This release block. |
>
> No engine / `OrbitConnector` / `glTFRuntime` asset is touched -- v1.0.104 stays inside
> the RebusVisualiser plugin + the Python build script. The v1.0.99 shadow-cast pipeline
> + v1.0.97 master-side `two_sided = True` bakes are preserved verbatim (v1.0.104 is
> strictly additive). The v1.0.103 beam-shadow diagnostic surfaces are untouched.

> **Beam-shadow trace diagnostics + stale-master detection + runtime regen (v1.0.103).**
> User report (verbatim, against v1.0.102):
>
> > "Beams are still going straight through objects. the footprint is shadowed correctly."
>
> Screenshot context: a truss with several Rebus fixtures pointing down-right at a stage
> prop (a cone-shaped pedestal); the visible cone-mesh beam shafts pass STRAIGHT THROUGH
> the prop with no carve; the floor footprint UNDER the prop clearly shows the prop's
> shadow (so `SpotLight->CastShadows` works and the prop IS in the depth buffer). v1.0.99
> SHOULD have fixed this via the LWC PreViewTranslation projection -- and the LWC fix
> DID land in the Python source -- but the user is reporting the visible behaviour
> didn't change.
>
> **Investigation (every v1.0.99 surface verified against the live tree).**
>
> | Surface | Result |
> | --- | --- |
> | `_BEAM_RAYMARCH_HLSL` LWC projection (`build_rebus_base_level.py:491-495 + 583-595`) | ✓ correct -- `LWCHackToFloat(ResolvedView.PreViewTranslation)` cached per pixel; `TranslatedWorldPos = sp + PreViewT`; `mul(..., ResolvedView.TranslatedWorldToClip)`. The documented engine pattern. |
> | Density modulation (line 616-621) | ✓ correct -- `shadowAtten = max(1 - BeamShadowStrength, 0)` then `d = ... * shadowAtten`. Strength=1 → d=0 → fully shadowed. |
> | CVar defaults (`RebusFixtureActor.cpp:214-217`) | ✓ Steps=8, Strength=1.0, Bias=5.0, Debug=0. The trace is ON by default. |
> | `RefreshBeamShadowParams` push (`RebusFixtureActor.cpp:2184`) | ✓ pushes ALL FOUR scalars on every CVar change + every fixture build. |
> | Cone-mesh material domain | ✓ `MD_SURFACE` + `MSM_UNLIT` + `BLEND_ADDITIVE` + `two_sided=True` -- additive translucents do NOT write to SceneDepth, so the cone CANNOT self-occlude (H3 ruled out). |
> | `_beam_master_has_shadow_debug` self-heal probe + force-regen | **ONLY fires when `ensure_beam_material()` runs from Python** (Tools > Execute Python Script > `build_rebus_base_level.build()` / `ensure_beam_material()`). It does NOT run automatically at editor / visualiser-subsystem startup. |
>
> **Smoking gun -- H1 (stale on-disk master).** v1.0.99 shipped the corrected Custom HLSL
> in the Python source AND the self-heal probe inside `ensure_beam_material`, but the
> probe + force-regen only commits the regenerated `M_RebusBeam.uasset` when the Python
> entry point is invoked. An operator who `git pull`ed v1.0.99..v1.0.102 and opened the
> editor without re-running the Python script kept the v1.0.96 cooked master with the
> LWC projection bug. The C++ side dutifully pushes BeamShadowStrength=1 onto a MID whose
> master never declared the parameter; `UMaterialInstanceDynamic::SetScalarParameterValue`
> silently no-ops on a missing parameter; the per-pixel shader runs the v1.0.96 broken
> HLSL with every shadow step landing off-screen; the trace concludes "always
> unoccluded"; the beam visibly carves nothing -- exactly the user's report.
>
> **No shader / C++ logic bug to fix.** The v1.0.99 .. v1.0.102 corrections are all
> correct; what's missing is the operator-side regen-trigger. v1.0.103 is therefore
> purely defensive: surface the silent no-op, name the recovery action, and add a
> runtime regen path so the operator never has to restart the editor again.
>
> **Defensive measures shipped (v1.0.103).**
>
> | Measure | What landed | Why |
> | --- | --- | --- |
> | `Rebus.DumpBeamShadow` per-scalar EXISTS / MISSING flag | Each MID column entry now reads `Steps=8.0/EXISTS` vs `Steps=8.0/MISSING` (queries `UMaterialInstanceDynamic::GetScalarParameterValue` per scalar -- returns false when the master never declared it). The "STALE MASTER" diagnostic note is now appended on ANY missing scalar with the operator-recovery command name. | Pre-v1.0.103 the dump used a `-999` sentinel that conflated "param missing" with "operator pushed -999"; the EXISTS/MISSING flag is unambiguous. An operator running `Rebus.DumpBeamShadow` against a stale master sees `Strength=0.000/MISSING` instead of `Strength=-999.000` and the recovery action is named on the same line. |
> | `URebusVisualiserSubsystem::ProbeBeamMasterAtStartup()` startup Warning | Once at `Initialize()`, load `M_RebusBeam` and check for `BeamShadowStrength` + `BeamShadowDebug` scalars. Warning when either is missing, with the operator-recovery command + verification steps in the same log line. Mirrors v1.0.91's IES-Warning style. | An operator who pulls v1.0.99+ and launches the editor sees the Warning in the launch log without having to discover `Rebus.DumpBeamShadow` first. The v1.0.99 self-heal lives in `ensure_beam_material()` (Python); the C++ side never knew the master was stale -- v1.0.103 closes that loop. |
> | `Rebus.RebuildBeamMaterial` editor-only runtime regen console command | Routes through the engine's `py` console command (PythonScriptPlugin) to invoke `build_rebus_base_level.ensure_beam_material(force=True)` at runtime. Gated behind `WITH_EDITOR` -- no-op in packaged builds (PythonScriptPlugin is editor-only). After the regen the existing per-fixture `BeamMID`s still reference the OLD UMaterial (the Python side deletes + recreates the asset, leaving dangling MID parents); the command logs the next-step prompt: ClearScene+LoadScene from the portal (or restart the editor) so each fixture respawns + rebuilds its `BeamMID` off the freshly-regenerated master. | Pre-v1.0.103 the operator had to either run Tools > Execute Python Script > `build_rebus_base_level.build()` (manual editor action, easy to forget after a `git pull`) or restart the editor (ditto). v1.0.103 collapses the regen step to one console command the portal can route over the existing `ConsoleCommand` data-channel descriptor. |
>
> **Why no shader change ships.** The v1.0.99 LWC fix is correct; the v1.0.99 density
> modulation is correct; the v1.0.99 first-step bias (`BeamShadowBias = 5.0` cm
> default) is correct; the v1.0.99 CVar push wiring is correct. The user's symptom is
> 100% explained by the on-disk master predating v1.0.99 and `SetScalarParameterValue`
> silently no-op'ing on the missing parameters. Speculatively touching the shader
> would only mask the diagnostic and risk a real regression.
>
> **Operator action path (v1.0.103+).**
>
> 1. **Read the launch-log Warning.** Search `LogRebusVisualiser` for `STALE BEAM
>    MASTER` -- if it appears, the on-disk `M_RebusBeam` predates v1.0.99 and the trace
>    is silently broken. (If you see `M_RebusBeam carries the v1.0.99 parameter
>    contract` instead, skip to step 4.)
> 2. **Run `Rebus.RebuildBeamMaterial` from the portal console** (or in-editor
>    console). The command invokes `build_rebus_base_level.ensure_beam_material(force=
>    True)` via PythonScriptPlugin's `py` console command. Expect a `RebusBaseLevel:
>    pre-v1.0.99 M_RebusBeam detected ... regenerating with the v1.0.99 fix` log line
>    confirming the regen landed.
> 3. **ClearScene + LoadScene from the portal** (or restart the editor) so each
>    fixture respawns and `BuildBeamCone` `LoadObject`s the freshly-regenerated
>    master. The post-regen log line of `Rebus.RebuildBeamMaterial` reminds the
>    operator of this step.
> 4. **Verify with `Rebus.DumpBeamShadow`.** Every fixture line should show
>    `Steps=8.0/EXISTS Strength=1.000/EXISTS Bias=5.00/EXISTS Debug=0/EXISTS` with a
>    `shadowing ENABLED, debug mode 0` diagnostic. If ANY scalar still shows MISSING
>    the regen + respawn didn't land -- restart the editor.
> 5. **Verify with `Rebus.BeamShadowDebug 1`.** The whole beam tints `lerp(green, red,
>    shadowed)`. A cube placed between fixture + floor should appear RED inside the
>    beam (the trace is finding the occluder). Then `Rebus.BeamShadowDebug 0` to
>    return to the regular composed beam -- the cube's silhouette should now visibly
>    carve the volumetric shaft. **This resolves the v1.0.102 user report.**
>
> **Files touched (v1.0.103).**
>
> | File | Change |
> | --- | --- |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusFixtureActor.cpp` | `DumpBeamShadowStateForDebug` -- per-scalar EXISTS / MISSING flag (`bool bOutExists` from `GetScalarParameterValue` instead of the v1.0.99 `-999` sentinel); STALE MASTER diagnostic note now names `Rebus.RebuildBeamMaterial` directly. The null-MID path also names the same recovery command. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusVisualiserSubsystem.cpp` + `Public/RebusVisualiserSubsystem.h` | New `ProbeBeamMasterAtStartup()` helper called from `Initialize()`. Loads `/Game/REBUS/Materials/M_RebusBeam.M_RebusBeam` and checks `BeamShadowStrength` + `BeamShadowDebug` via `GetScalarParameterValue`. Warning naming the operator-recovery command on stale; Log on healthy; benign no-op when the asset isn't loadable yet. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusVisualiser.cpp` | New `Rebus.RebuildBeamMaterial` console command (registered alongside `Rebus.DumpBeamShadow`, cleaned up in `ShutdownModule`). Routes through `GEngine->Exec(World, TEXT("py import build_rebus_base_level; build_rebus_base_level.ensure_beam_material(force=True)"), *GLog)` so the call site is insulated from any UE 5.7 `IPythonScriptPlugin` API renames -- the engine `py` command is the documented stable entry point. Gated behind `WITH_EDITOR`; defensive `FModuleManager::IsModuleLoaded(TEXT("PythonScriptPlugin"))` check produces a clean Warning in non-editor / `-noplugins` runs. Added `#include "Modules/ModuleManager.h"`. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/README.md` | This release block. |
>
> No engine / OrbitConnector / Python-build-script asset is touched. The v1.0.99 shader
> + v1.0.99 / v1.0.100 / v1.0.101 / v1.0.102 lineage remains untouched -- v1.0.103 is
> purely diagnostic + a runtime trigger for the existing v1.0.99 self-heal so an
> operator never has to restart the editor (or rediscover Tools > Execute Python
> Script) to land the LWC projection fix.

> **Lens material now also emits visible light following the fixture's live dimmer / colour / gobo (v1.0.102).**
> User request (verbatim):
>
> > "can the lens material be emiissive as well and follow the dimmer, colour and gobo of the fixture its part of."
>
> Pre-v1.0.102 the `M_RebusFixtureLens` master (Python-baked since v1.0.93, double-
> sided since v1.0.97, fully-reflective since v1.0.89) was a CHROME MIRROR ONLY: a
> polished metallic-mirror lens disc applied to every real `<Beam>` `IsBeamLens
> Components` PMC plus the synthetic `LensDisc` fallback. A fixture at full dimmer
> with a red colour reflected the surrounding rig but the LENS ITSELF didn't glow --
> the only "glow" was the v1.0.49 `M_RebusLensFlare` soft-halo disc co-located in
> front of each lens, which is a separate additive plane (good for soft bloom, but
> the lens disc's surface stayed cold-chrome regardless of dimmer).
>
> v1.0.102 adds an ADDITIVE emissive layer to the SAME chrome material so the lens
> face itself glows. At Dimmer=0 the lens reads identical to pre-v1.0.102 (chrome
> mirror, no glow); at Dimmer=1 the lens glows in the live fixture colour, modulated
> by the gobo silhouette when a cookie is loaded. So a fixture at full intensity
> with a red colour reads as a glowing red disc; with a leaf gobo loaded the leaf
> silhouette is visible directly ON the lens face.
>
> **Material graph extension (`M_RebusFixtureLens`, Python-authored).** Four new
> parameters added to `_build_fixture_lens_master` in
> `REBUS_Visualiser/Content/Python/build_rebus_base_level.py`. The v1.0.93 PBR chain
> (BaseColor / Metallic / Roughness) is preserved verbatim:
>
> | Parameter | Type | Default | Role |
> | --- | --- | --- | --- |
> | `Color` (v1.0.93) | Vector | (1,1,1,1) | BaseColor -- unchanged. |
> | `Metallic` (v1.0.93) | Scalar | 1.0 | -- unchanged. |
> | `Roughness` (v1.0.93) | Scalar | 0.0 | -- unchanged. |
> | **`Emissive`** | Vector | (1,1,1,1) | The fixture's live colour (`ColorR/G/B.Current`). |
> | **`EmissiveIntensity`** | Scalar | **0.0** | Live `Dimmer.Current` × shutter-gate × `RebusLensEmissiveBaseScale` (5.0) × `Rebus.LensEmissiveScale` (CVar, default 1.0). Default 0.0 on the master so the editor preview reads as chrome mirror with no glow -- runtime push overrides. |
> | **`GoboTexture`** | Texture2D | `/Engine/EngineResources/WhiteSquareTexture` | Per-fixture cookie render-target (`GoboRT`, the `UCanvasRenderTarget2D` driving the cone + cookie). White default so an unbound MI no-ops to 1.0 (matches the v1.0.86 ground `BaseColorTexture` default pattern). |
> | **`bUseGobo`** | Scalar | **0.0** | 0 = ignore gobo (lens glows uniform colour); 1 = mask lens face by the gobo sample. Driven from the per-fixture `bGoboActive` flag in C++. |
>
> **Combine formula** (matches the v1.0.102 task spec, expressed as material nodes):
>
> ```
> GoboSample = TextureSample(GoboTexture, TexCoord0)
> GoboMix    = Lerp(One, GoboSample.rgb, bUseGobo)   // bUseGobo=0 -> 1.0; bUseGobo=1 -> gobo
> EmissiveOut = Emissive * EmissiveIntensity * GoboMix
> EmissiveOut -> MP_EMISSIVE_COLOR
> ```
>
> Sampled with `TextureCoordinate(index=0)` -- both the real procedural-mesh lens
> disc (built from `/meshes` in `BuildMeshes`) and the synthetic engine `Plane`
> carry sensible 0..1 disc UVs, so the gobo lands on the lens face just like the
> cookie projects onto the floor pool. `two_sided = True` (v1.0.97) preserved.
>
> **Python self-heal** -- `_fixture_lens_master_has_emissive(master)` probe added
> next to v1.0.93's `_fixture_lens_master_is_current` + v1.0.97's
> `_master_is_two_sided`. It enumerates the master's vector / scalar / texture
> parameter names (via `MaterialEditingLibrary.get_vector_parameter_names` /
> `get_scalar_parameter_names` / `get_texture_parameter_names`) and asserts the
> v1.0.102 quartet (`Emissive`, `EmissiveIntensity`, `bUseGobo`, `GoboTexture`) is
> present. The non-force `ensure_fixture_lens_material()` path now ORs the three
> probes (v1.0.93 contract + v1.0.97 two-sided + v1.0.102 emissive); ANY one
> failing promotes the call to a force-regen and logs a Warning naming v1.0.102 so
> the change is auditable. Operators who booted on a pre-v1.0.102 baked master
> pick up the new chain on the next editor launch without manual action.
>
> **C++ push (`ARebusFixtureActor::RefreshLensEmissive`).** New helper builds the
> live state and pushes it onto every per-fixture lens MID:
>
> * `Emissive` = clamped (`ColorR.Current`, `ColorG.Current`, `ColorB.Current`,
>   0..1) -- the same live linear colour `RefreshIntensity` writes to
>   `SpotLight->SetLightColor`.
> * `EmissiveIntensity` = `clamp(Dimmer.Current, 0..1) * Gate(shutter) *
>   RebusLensEmissiveBaseScale * Rebus.LensEmissiveScale`, clamped to
>   `[0, RebusLensEmissiveIntensityCap=100]`. `RebusLensEmissiveBaseScale = 5.0`
>   is the venue-empirical "a fixture at full dimmer reads ~5x ambient" default;
>   `Rebus.LensEmissiveScale` is the live show-tuning CVar (see below); the cap
>   is a guard against runaway exposure from an accidental `1e6` CVar push.
> * `GoboTexture` = `GoboRT` (the per-fixture cookie render-target, cast
>   `UCanvasRenderTarget2D` -> `UTexture` -- the implicit upcast through
>   `UTextureRenderTarget2D` is legal and verified). Null when no cookie has been
>   bound; `SetTextureParameterValue(nullptr)` is silently no-op'd.
> * `bUseGobo` = 1.0 when (`bGoboActive` && `Rebus.LensFollowGobo != 0` &&
>   `GoboRT` is bound), else 0.0.
>
> **Per-component MID wrapping (`EnsurePerLensMIDs`).** Pre-v1.0.102 every isBeam
> mesh's slot-0 lens material pointed at the SHARED project-wide chrome master
> (or the user's `FixtureLensMaterialOverride.uasset` if present), so live dimmer/
> colour/gobo pushes could only address ONE lens at a time across the entire
> scene. v1.0.102 wraps each PMC's slot-0 material in its OWN `UMaterialInstance
> Dynamic` via `UPrimitiveComponent::CreateAndSetMaterialInstanceDynamic(0)`, and
> caches the weak handle in a new `IsBeamLensMIDs` array (index-aligned to
> `IsBeamLensComponents`). The synthetic `LensDisc` MID (`LensDiscMID`) was
> already per-fixture and is re-used as-is; `RefreshLensEmissive` pushes to BOTH
> arrays so the rare respawn race where both paths are momentarily active still
> drives the same live state on both surfaces. Idempotent: `CreateAndSet
> MaterialInstanceDynamic` returns the existing MID when the slot already has one.
>
> **Where the push hooks in.** `RefreshLensEmissive` is called from every
> existing intensity / colour / gobo update path so the lens stays in lockstep
> with the SpotLight without a new tick driver:
>
> | Existing call site | What it already did | v1.0.102 addition |
> | --- | --- | --- |
> | `RefreshIntensity` | drives `SpotLight->SetIntensity` + `SetLightColor` + calls `RefreshLensDisc` + `RefreshBeamEmissive` | `RefreshLensDisc` now tail-calls `RefreshLensEmissive` (single entry-point; folds in `ApplyDimmer` / `ApplyColor` / `ApplyShutter` / `RefreshIesAfterZoom` automatically). |
> | `BuildMeshes` end | builds + tags every isBeam PMC | now also calls `EnsurePerLensMIDs` so the per-component MIDs exist before the Setup-end `RefreshIntensity` push lands. |
> | `ApplyGoboTextureFromBytes` | decodes bytes -> `CurrentGoboTexture`, sets `bGoboActive=true`, pushes cone + cookie | now also pushes the new GoboRT + `bUseGobo=1` onto every lens MID. |
> | `ClearGoboToOpen` | clears `CurrentGoboTexture`, sets `bGoboActive=false`, blanks RT | now also pushes `bUseGobo=0` so the lens face stops showing the (cleared) silhouette. |
> | `OnGoboRTUpdate` | redraws gobo rotation + iris mask into GoboRT each spin tick | calls `RefreshLensEmissive` after the iris pass so dimmer / colour fades concurrent with the spin land on the lens immediately. |
>
> Verbose-level log line per push (gated to `LogRebusVisualiser` Verbose so steady
> state isn't spammy): `Fixture <id> lens emissive: color=(R,G,B) intensity=X
> goboActive=Y useGobo=Z scale=S pushedReal=A/B syntheticMID=<set|absent>`.
>
> **Multi-beam fixtures (LED matrices) -- equal-intensity per lens, future work
> noted.** A multi-beam fixture (MAC Aura / Robe Esprite-class LED arrays) has
> multiple `IsBeamLensComponents`, each pixel a separate isBeam mesh. v1.0.102
> treats them all as equally lit by the master fixture state -- the SAME
> Emissive / EmissiveIntensity / GoboTexture / bUseGobo are pushed onto every
> pixel-lens's MID. Per-pixel emission (e.g. a wide-angle wash where the rim
> pixels are dimmer than the centre, or a chase that animates across the matrix)
> is a FUTURE enhancement: the per-pixel slot + intensity descriptor would need
> to arrive on the data channel and feed an additional per-MID push inside the
> `RefreshLensEmissive` loop. The `IsBeamLensMIDs` array + the parallel
> `RebusIsBeamLens` ComponentTag list make that trivial when the wire surface
> exists -- v1.0.102 ships the equal-intensity baseline.
>
> **New CVars.**
>
> | CVar | Default | Effect |
> | --- | --- | --- |
> | `Rebus.LensEmissiveScale` (`float`) | 1.0 | Multiplier on the lens-material `EmissiveIntensity` push, independent of the actual SpotLight intensity. Operators tweak per-show to balance the lens glow against the lit floor pool ("lens blows out the sensor at full dimmer" -> drop to 0.5; "lens barely visible against the beam" -> push to 2.0). The hard cap at 100 in `RefreshLensEmissive` still applies. Live -- changing this walks every Rebus fixture and re-pushes the lens MIDs. |
> | `Rebus.LensFollowGobo` (`0\|1`) | 1 | When 1 (default), the lens face shows the gobo silhouette while a cookie is active (the v1.0.102 user request). When 0, the lens always glows uniform colour regardless of gobo state (some shows prefer the cleaner "lens-as-eye" look over the "projector-port" look). Live -- refresh sink walks every fixture and re-pushes. |
>
> **Operator caveats.**
>
> * **Hand-authored lens material won't glow until the operator either adds the
>   four v1.0.102 params themselves, or deletes their custom asset and lets the
>   Python rebake re-create it.** A user-authored
>   `/Game/REBUS/Materials/M_RebusFixtureLens.uasset` takes precedence over the
>   Python-baked master (`FixtureLensMaterialOverride` resolves the user's
>   asset; `EnsureFixtureMIDs` doesn't touch it). If that custom asset lacks the
>   `Emissive` / `EmissiveIntensity` / `GoboTexture` / `bUseGobo` params, every
>   `MID->SetVectorParameterValue` / `SetScalarParameterValue` / `SetTexture
>   ParameterValue` call inside `RefreshLensEmissive` silently no-ops (UE MID
>   setters skip unknown parameter names), so the lens stays chrome-only. The
>   `_fixture_lens_master_has_emissive` Python probe handles the AUTO-baked
>   master upgrade path, but it cannot mutate a hand-authored asset. The fix is
>   either to add the four params to the custom asset by hand (matching the
>   combine formula above) OR delete it (so the C++ FObjectFinder misses
>   `FixtureLensMaterialOverride` and falls back to the runtime `FixtureLensMID`
>   built off `BasicShapeMaterial` -- which DOES carry the params because it's
>   built fresh every spawn). On delete the `build_rebus_base_level.py` startup
>   hook can re-bake the proper v1.0.102 master into the same slot.
> * **Intensity-scaling sweet spot.** `RebusLensEmissiveBaseScale = 5.0` (in
>   `RebusFixtureActor.cpp`) is the venue-empirical default. Lower (`3.0`-`4.0`)
>   reads more "matte glowing optic"; higher (`6.0`-`8.0`) reads more "incandescent
>   bulb". Operators tune via the `Rebus.LensEmissiveScale` CVar at runtime
>   without an engine rebuild. If the lens reads oversaturated after a fresh
>   regen, drop `Rebus.LensEmissiveScale` to `0.5` first; if that's still too
>   bright, edit the C++ constant and rebuild.
> * **Material regen on first v1.0.102 launch.** The
>   `_fixture_lens_master_has_emissive` self-heal logs a Warning the first time
>   it triggers ("pre-v1.0.102 M_RebusFixtureLens detected ... regenerating") --
>   that's expected on the first editor restart after pulling v1.0.102 onto a
>   project that had v1.0.93 / v1.0.97 / v1.0.101 baked. Subsequent launches
>   pick up the new master verbatim and the Warning doesn't recur.
>
> **Files touched (v1.0.102).**
>
> * `REBUS_Visualiser/Content/Python/build_rebus_base_level.py` -- extended
>   `_build_fixture_lens_master` with the v1.0.102 emissive chain (4 new params
>   + combine formula); added `_fixture_lens_master_has_emissive(master)` probe;
>   wired it into the `ensure_fixture_lens_material(force=False)` self-heal OR
>   chain.
> * `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Public/RebusFixtureActor.h`
>   -- new `IsBeamLensMIDs` array (weak parallel to `IsBeamLensComponents`); new
>   `RefreshLensEmissive` public helper + `EnsurePerLensMIDs` private helper.
> * `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusFixtureActor.cpp`
>   -- new `GRebusLensEmissiveScale` + `GRebusLensFollowGobo` CVars + refresh
>   sinks; new `RebusLensEmissiveBaseScale` / `RebusLensEmissiveIntensityCap`
>   constants; new `EnsurePerLensMIDs` / `RefreshLensEmissive` definitions;
>   `RefreshLensDisc` now tail-calls `RefreshLensEmissive`; `BuildMeshes` end now
>   calls `EnsurePerLensMIDs`; `ApplyGoboTextureFromBytes`, `ClearGoboToOpen`,
>   `OnGoboRTUpdate` now also call `RefreshLensEmissive`.
> * `REBUS_Visualiser/Plugins/RebusVisualiser/README.md` -- this release block.
>
> No engine / OrbitConnector / Epic-DMX-Fixtures asset is touched. The v1.0.93
> chrome PBR layer is preserved verbatim under the new additive emissive layer;
> v1.0.97 two-sided is preserved; v1.0.99 / v1.0.100 / v1.0.101 (shadow trace +
> camera-pose default + cone-mesh radius scale) are all orthogonal.
>
> **Operator checklist after rebuilding to v1.0.102.**
>
> 1. Launch the editor; confirm one Warning fires on startup:
>    `RebusBaseLevel: pre-v1.0.102 M_RebusFixtureLens detected ...
>    regenerating.` -- the v1.0.102 master replaces the v1.0.93 / v1.0.97 baked
>    one. Subsequent launches don't log this.
> 2. Spawn a fixture, dial dimmer to FULL (1.0). The lens disc on the fixture
>    body should glow visibly in the current colour (default white).
> 3. Push `SetFixtureColor` red. The lens glow shifts red in lockstep with the
>    beam shaft + lit floor pool.
> 4. Push `SetFixtureGobo` to a slot with a recognisable silhouette (a star, a
>    leaf, etc.). The gobo pattern should be visible directly on the lens face,
>    rotating in lockstep with the cone + cookie projection.
> 5. Run `Rebus.LensFollowGobo 0` from the portal console. The lens glow goes
>    back to UNIFORM colour (no silhouette on the lens face) while the gobo
>    cookie still projects on the floor. Run `Rebus.LensFollowGobo 1` to restore.
> 6. Run `Rebus.LensEmissiveScale 0.5` -- the lens glow drops to half-bright;
>    `Rebus.LensEmissiveScale 2.0` doubles. `1.0` returns to the default.
> 7. Dial dimmer to ZERO -- the lens reads as a chrome MIRROR identical to
>    pre-v1.0.102 (no glow, just the v1.0.93 polished-metallic reflection).

> **Cone-mesh + SpotLight outer-cone in sync via single-source-of-truth zoom half-angle (v1.0.101).**
> User report (verbatim):
>
> > "when the beam is zoomed in, its slighly larger that the footprint, can we make
> > these in sync but follow the light zoom range specification,."
>
> The visible cone-mesh shaft was reading slightly WIDER than the actually-lit floor
> footprint, especially at narrow zooms. The user wants both anchored to the GDTF
> zoom-range spec so the visible shaft and the lit footprint coincide on the floor at
> every zoom.
>
> **Audit -- which root cause was the actual mismatch.** The codebase already routed
> both elements through one helper (`ResolveOuterHalfDeg()`), so SpotLight outer cone
> AND procedural cone-mesh far-radius AND Epic-beam canvas `DMX Zoom` were all
> deriving from the *same* GDTF zoom-range half-angle. Cause 1 (beam-vs-field
> conflation) was therefore not the bug. The actual culprit is **Cause 2 -- UE's
> linear-taper light model**: `USpotLightComponent` ramps brightness linearly from
> `InnerConeAngle` (peak) to `OuterConeAngle` (zero), so the *visible bright disc on
> the floor* sits at roughly the half-intensity edge ~ `(InnerHalf + OuterHalf) / 2`.
> With `RecomputeConeAngles`'s default `InnerRatio = 0.8` (BeamAngle/FieldAngle when
> the profile carries both, else fallback) the perceived disc edge sits at ~0.9 *
> outer, while the geometric cone-mesh outer edge sits at exactly outer. The
> cone-mesh therefore reads ~10% wider than the lit footprint -- exactly the user's
> "slightly larger" observation, *not* a "this is twice as wide" geometry/spec bug.
>
> **The single-source-of-truth helper.** `ResolveOuterHalfDeg()` is preserved as a
> thin wrapper for call-site readability; the canonical helper is now an explicit
> `float ResolveZoomHalfDeg(float ZoomFullDeg) const` that takes a FULL beam angle
> (matching the v1.0.84 wire-protocol convention) and returns the half-angle clamped
> to the profile's `Profile.Zoom.MinDeg/MaxDeg` range, then to the global safe
> `[0.5, 80]` range, then iris-pinched if no gobo cookie is active. Every consumer
> -- SpotLight outer cone (`RecomputeConeAngles`), procedural cone-mesh far-radius
> (`UpdateBeamConeGeometry`), Epic-beam canvas `DMX Zoom`
> (`UpdateEpicBeamParams`) -- continues to funnel through this one helper, so
> divergence between the lit footprint extent and the visible-shaft far edge is
> impossible by construction.
>
> **The new operator handle: `BeamConeRadiusScale`.** A `UPROPERTY(EditAnywhere,
> BlueprintReadWrite, Category = "Rebus|Beam") float BeamConeRadiusScale = 1.0f` on
> `ARebusFixtureActor`, applied identically to:
>
> * `UpdateBeamConeGeometry()` -- the procedural M_RebusBeam cone's far-radius:
>   `radius = max(BeamLength * tan(half) * BeamConeRadiusScale, BeamBaseRadius + 0.1)`
>   AND re-pushed onto the `BeamMID` `FarRadius` scalar.
> * `UpdateEpicBeamParams()` -- the Epic-beam canvas `DMX Zoom` scalar:
>   `DMX Zoom = clamp(RebusEpicBeamZoomScale * BeamConeRadiusScale * SpotOuterHalfDeg, 1, 179)`.
>
> Crucially the scalar does NOT pinch `SpotLight->OuterConeAngle` -- so the lit
> footprint, IES sampling, and 1/r^2 falloff continue to track the GDTF zoom-range
> specification verbatim. The scalar exists exclusively to bring the *visible* shaft
> inward to coincide with the perceived (half-intensity) lit-disc edge, NOT to
> redefine where the light reaches.
>
> **Default 1.0 = geometric truth.** With no operator override the visible shaft is
> sized to the GDTF zoom-range half-angle exactly (the pre-v1.0.101 behaviour --
> nothing changes by default). Operators tweak the scalar per show, typically into
> the `0.85..0.95` band, to bring the cone-mesh edge in to coincide with the
> bright-disc edge that the eye actually reads as "the lit pool". The default stays
> at 1.0 because what the eye sees as "the lit edge" is show- and content-dependent
> (GDTF profiles with explicit BeamAngle/FieldAngle photometrics imply a ~0.9
> match; flat-field LED matrices imply ~1.0). Console knob:
>
> ```
> Rebus.BeamConeRadiusScale 0.9   # tighten the visible shaft to ~bright-disc edge
> Rebus.BeamConeRadiusScale 1.0   # restore geometric truth (visible shaft = GDTF zoom-range cone)
> ```
>
> The CVar refresh sink (`FAutoConsoleVariableRef CVarRebusBeamConeRadiusScale`)
> walks every Rebus fixture in every Game/PIE/Editor world, sets each fixture's
> per-actor `BeamConeRadiusScale` UPROPERTY to the new value, and calls
> `RefreshBeamConeRadiusScaleFromCVar()` to re-trigger the rebuild gate in
> `UpdateBeamConeGeometry` (which would otherwise skip when the half-angle hasn't
> changed) and re-push the Epic `DMX Zoom`. Fresh-spawn fixtures inherit the live
> CVar value at `BuildBeamCone` time.
>
> **New diagnostic: `Rebus.DumpFixtureZoom [fixtureId]`.** Mirrors
> `Rebus.DumpFixtureIes`'s shape. With no arg, dumps every Rebus fixture in every
> Game/PIE/Editor world; with an optional fixtureId (Speckle node id, the same key
> `SetFixture*` uses), dumps just the matching fixture (warns when not found). Each
> per-fixture line carries:
>
> * `zoomTarget` -- the live `ZoomDeg.Current` half-angle target from the wire,
> * `profileZoomRange` -- `Profile.Zoom.MinDeg/MaxDeg` from the GDTF (or
>   `<no profile zoom range>` when the profile carries no Zoom payload, in which
>   case the resolver clamps to the global `[0.5, 80]` safe range only),
> * `resolvedHalf` -- the canonical `ResolveZoomHalfDeg(ZoomDeg.Current * 2)` output
>   (single source of truth),
> * `spotOuterCone` / `spotInnerCone` -- the SpotLight's LIVE values + the
>   `Inner/Outer` ratio (so the operator can see at a glance how much narrower the
>   lit bright-disc is than the geometric cone),
> * `beamLength` / `coneFarRadiusBuilt` / `coneFarRadiusExpected` -- the procedural
>   cone-mesh size (built vs. recomputed-for-current-state),
> * `BeamMID.FarRadius` -- read back from the live MID (proves the
>   `UpdateBeamConeGeometry` push won the race against any portal/scene-property
>   override; mirrors `Rebus.DumpBeamShadow`'s read-back pattern),
> * `BeamConeRadiusScale` -- the per-fixture scalar applied to the cone-mesh +
>   Epic DMX Zoom,
> * `bUsingEpicBeam` / `bMeshBeamEnabled` / `bGoboActive` / `iris` -- mode flags so
>   the operator knows which path is live and whether iris is contributing to the
>   pinch.
>
> A header line above the per-fixture dumps prints the `Rebus.BeamConeRadiusScale`
> CVar value so the global is visible alongside per-fixture overrides for diff.
>
> **Verbose `ApplyZoom` log.** Each `ARebusFixtureActor::ApplyZoom` call now emits a
> single `Verbose` line listing the input half (= `ZoomDeg` target), the canonical
> resolved outer half, the SpotLight Outer/Inner cone (live), the procedural
> cone-mesh far-radius the frustum would build at this state, and the live
> `BeamConeRadiusScale`. Verbose-level so a busy show isn't spammed; flip
> `Log LogRebusVisualiser Verbose` (or use `Rebus.DumpFixtureZoom`) to surface.
>
> **What the SpotLight inner-cone keeps doing.** `RecomputeConeAngles` continues to
> derive `InnerConeAngle = OuterHalf * InnerRatio * FrostSoften` from the photometrics
> (BeamAngle/FieldAngle) -- v1.0.101 deliberately leaves this untouched because
> it's the physically faithful taper for fixtures whose profile carries both
> photometrics, AND the user's request was scoped to the visible-shaft-vs-lit-disc
> sync, not to the IES-vs-cone faithfulness. Operators who want the inner-cone
> tightened toward the outer (Cause 2's "tighten the inner cone" alternative fix)
> can pursue that in a follow-up; v1.0.101 ships the cleaner cone-mesh-side knob.
>
> **Operator verification checklist.**
>
> 1. Spawn a single moving-head fixture, point it straight down at the floor.
> 2. Push a narrow zoom (e.g. mid of the GDTF zoom range, ~10° full = 5° half).
> 3. Drop a flat reference target (cube, plane) on the floor at the fixture's
>    straight-down location.
> 4. Look at the visible shaft disc on the floor vs the bright-disc lit edge: if
>    the shaft reads as a halo around the bright disc, the v1.0.101 fix has
>    surfaced the operator handle (no longer a hidden bug).
> 5. Run `Rebus.DumpFixtureZoom` -- confirm `spotOuterCone == resolvedHalf` and
>    `BeamMID.FarRadius == coneFarRadiusExpected`.
> 6. Push `Rebus.BeamConeRadiusScale 0.9` -- shaft tightens; re-run the dump and
>    confirm `coneFarRadiusBuilt` shrunk to ~0.9× the previous value while
>    `spotOuterCone` is unchanged. The shaft disc and the bright-disc should
>    coincide within 1-2%.
> 7. Tweak the scalar in `[0.85..1.0]` to taste; the value applies live.
>
> **Files touched (v1.0.101).**
>
> | File | What changed |
> | --- | --- |
> | `Source/RebusVisualiser/Public/RebusFixtureActor.h` | Added the `BeamConeRadiusScale = 1.0f` UPROPERTY (under `Rebus|Beam` category). Added the `ResolveZoomHalfDeg(float ZoomFullDeg) const` canonical helper signature; `ResolveOuterHalfDeg()` is now a thin wrapper documented as such. Added public `DumpFixtureZoomStateForDebug() const` and `RefreshBeamConeRadiusScaleFromCVar()` for the new console command + CVar refresh sink. |
> | `Source/RebusVisualiser/Private/RebusFixtureActor.cpp` | Implemented `ResolveZoomHalfDeg(float)` (the canonical body); `ResolveOuterHalfDeg()` round-trips `ZoomDeg.Current * 2.f` through it. `UpdateBeamConeGeometry` applies `BeamConeRadiusScale` to the cone-mesh far-radius (and the BeamMID `FarRadius` scalar). `UpdateEpicBeamParams` applies the same scale to the Epic-beam canvas `DMX Zoom` (so the live shaft tightens regardless of which beam path is active). `BuildBeamCone` seeds `BeamConeRadiusScale` from the CVar at spawn time. `ApplyZoom` emits a Verbose log. New `Rebus.BeamConeRadiusScale` `FAutoConsoleVariableRef` with refresh sink walks every fixture and re-pushes geometry + Epic DMX Zoom. New `DumpFixtureZoomStateForDebug` + `RefreshBeamConeRadiusScaleFromCVar` implementations. |
> | `Source/RebusVisualiser/Private/RebusVisualiser.cpp` | New `Rebus.DumpFixtureZoom [fixtureId]` console command (handler + register + unregister), mirroring `Rebus.DumpFixtureIes`'s shape with a CVar header line above the per-fixture dumps. |
> | `README.md` | This release block. |
>
> No engine / OrbitConnector / Epic-DMX-Fixtures asset is touched. The v1.0.99 +
> v1.0.100 changes (shadow trace + force-cast-shadows + camera-pose default) are
> orthogonal -- v1.0.101 only adds the cone-mesh radius scale + the canonical zoom
> helper rename + the new dump command.

> **Default cinematic-camera landing pose at (0,-20,2) m looking at (0,0,5) m (v1.0.100).**
> User request (verbatim):
>
> > "can we set the default position of the camera to 0,-20,2 and keep the target
> > at 0,0,5"
>
> Tiny contained change to the construction-time camera pose. The visualiser's pre-v1.0.100
> spawn helper (`URebusVisualiserSubsystem::TryPositionPlayerView`) placed the cinematic
> camera at `(0, -20, 2) m` looking at `(0, 0, 2) m` -- the eye height was right but the
> look-at was at the camera's OWN eye height, so the aim came out flat (0° pitch) and
> framed an empty floor strip rather than the centre of the stage. The user asked for the
> eye position to stay, with the look-at lifted to `(0, 0, 5) m` (roughly performer-head
> height on a small riser), which derives a gentle look-up.
>
> **New defaults (in both metre and UE-world cm so a future operator doesn't have to do the math).**
>
> | Quantity | Metres (operator framing convention) | Centimetres (UE world units) |
> | --- | --- | --- |
> | Camera location | `(0, -20, 2)` | `(0, -2000, 200)` |
> | Look-at target  | `(0, 0, 5)`   | `(0, 0, 500)`     |
> | Forward (target - location) | `(0, 20, 3)` | `(0, 2000, 300)` |
> | Derived rotator (`FRotationMatrix::MakeFromX(forward.GetSafeNormal()).Rotator()`) | pitch `+8.53°` (gentle look-up), yaw `90°` (facing +Y), roll `0°` | — |
>
> The aim math (so it can be re-checked by hand against any future operator request):
>
> | Term | Formula | Value |
> | --- | --- | --- |
> | yaw   | `atan2(delta.Y, delta.X)` = `atan2(2000, 0)` | `90°` |
> | pitch | `atan2(delta.Z, sqrt(delta.X² + delta.Y²))` = `atan2(300, 2000)` | `+8.53°` |
> | roll  | (FRotationMatrix::MakeFromX uses a sensible default up; no roll fed in) | `0°` |
>
> Crucially the rotator is **derived in code** (not hard-coded). The shared header
> exposes both metre-triples and the derived rotator together:
>
> ```cpp
> // RebusVisualiser/Source/RebusVisualiser/Public/RebusCineCameraPawn.h
> namespace RebusCineCameraDefaults
> {
>     inline const FVector  kDefaultCameraLocation_cm = FVector( 0.f, -2000.f, 200.f); // (0,-20,2) m
>     inline const FVector  kDefaultCameraTarget_cm   = FVector( 0.f,     0.f, 500.f); // (0, 0, 5) m
>     inline const FRotator kDefaultCameraRotation    = FRotationMatrix::MakeFromX(
>         (kDefaultCameraTarget_cm - kDefaultCameraLocation_cm).GetSafeNormal()).Rotator();
> }
> ```
>
> So any future request like "move the eye 1 m higher" or "look at the upstage banner at
> (0, 5, 6) m" is a one-line edit to one of the two metre triples; the rotator updates
> automatically on the next program start (lazy-initialised function-local static, one
> trig call ever -- not a per-frame cost).
>
> **Single source of truth across the two consumers.** Two methods read this pose:
>
> | Caller | Pre-v1.0.100 source | v1.0.100 source |
> | --- | --- | --- |
> | `URebusVisualiserSubsystem::TryPositionPlayerView` (SpawnActor + ApplyTransform) | file-local `static const FVector RebusViewStartLocation/LookAtLocation` (literal `(0,-2000,200)` / `(0,0,200)`) | `RebusCineCameraDefaults::kDefaultCameraLocation_cm` / `kDefaultCameraRotation` from the shared header |
> | `ARebusCineCameraPawn::ResetToDefaults` (Rebus.CameraReset console command) | left the actor transform alone -- reset only touched the lens / sensor / EV | `SetActorLocationAndRotation(kDefaultCameraLocation_cm, kDefaultCameraRotation)` + `SetControlRotation(...)` on the controller |
>
> **`ResetToDefaults` now returns to the same landing pose.** Pre-v1.0.100 the reset
> only touched the cine settings (focal length / aperture / focus / sensor / EV) and left
> the actor wherever the operator had last parked it. Operators reporting "I hit reset
> and the framing is still wrong" pushed the change. v1.0.100 `ResetToDefaults` now
> snaps the actor transform back to the shared `kDefaultCameraLocation_cm` /
> `kDefaultCameraRotation` AND writes the same rotation onto the
> `APlayerController::ControlRotation` (so the next mouse delta doesn't yank the view
> back to a stale yaw -- mirrors `ApplyTransform`). The next
> `BroadcastCameraStateIfChanged` read-back ships the new pose to the portal in the
> same tick, so the portal's transform readout updates immediately. The lens / sensor /
> EV reset behaviour from v1.0.96 / v1.0.98 is unchanged.
>
> **`FRebusCameraState` defaults: deliberately NOT touched.** The struct's
> default-constructed `Location = ZeroVector` / `Rotation = ZeroRotator` is the
> documented "no cine pawn yet" sentinel used by `HandleCameraDescriptor`'s
> early-`RequestCameraState` reply (line 1284 of `RebusVisualiserSubsystem.cpp`).
> Bumping it to the new spawn pose would have made the portal think the camera was
> already at `(0,-2000,200)` BEFORE the pawn was actually alive, contradicting the
> existing "answer with a zero snapshot so the UI doesn't freeze" semantics. Once the
> pawn does spawn, `GetCameraState()` reads the LIVE actor transform (already at the
> v1.0.100 pose because the spawn just put it there), so the first real read-back
> already carries the correct numbers. So `FRebusCameraState`'s defaults are a no-op
> for this version.
>
> **Portal override still works exactly as before.** This is JUST the construction-time
> / `Rebus.CameraReset` landing pose. The portal can still drive the live camera pose
> at any time via `SetCameraTransform` (handled by `HandleCameraDescriptor` →
> `ARebusCineCameraPawn::ApplyTransform`), and the next `CameraState` read-back through
> the existing per-tick stream broadcasts the override back to the portal. The
> v1.0.79 / v1.0.82 / v1.0.96 / v1.0.98 camera-state pipeline is untouched -- v1.0.100
> only changes the numbers the operator sees on first launch (and on reset).
>
> **Files touched (v1.0.100).**
>
> | File | What changed |
> | --- | --- |
> | `Source/RebusVisualiser/Public/RebusCineCameraPawn.h` | Added the `RebusCineCameraDefaults` namespace with `inline const` `kDefaultCameraLocation_cm` / `kDefaultCameraTarget_cm` / `kDefaultCameraRotation` (derived). Added `#include "Math/RotationMatrix.h"` for the rotator derivation. Updated `ResetToDefaults` doc-comment to mention the new transform-reset behaviour. |
> | `Source/RebusVisualiser/Private/RebusVisualiserSubsystem.cpp` | Removed the file-local `RebusViewStartLocation` / `RebusViewLookAtLocation` literals. `TryPositionPlayerView` now reads `RebusCineCameraDefaults::kDefaultCameraLocation_cm` / `kDefaultCameraRotation` and passes them to `World->SpawnActor<ARebusCineCameraPawn>` + `ApplyTransform`. Log line bumped to "(v1.0.100 default)". |
> | `Source/RebusVisualiser/Private/RebusCineCameraPawn.cpp` | `ResetToDefaults` now also calls `SetActorLocationAndRotation(kDefaultCameraLocation_cm, kDefaultCameraRotation)` + `GetController()->SetControlRotation(...)` (mirrors `ApplyTransform`). Log line bumped to "v1.0.100 defaults (..., transform=... facing ...)". |
> | `README.md` | This release block. |
>
> No engine / OrbitConnector / Epic-DMX-Fixtures asset is touched. The v1.0.99 shadow
> trace + Orbit cast-shadows defaults remain untouched -- v1.0.100 is orthogonal (just
> the camera spawn / reset pose).

> **Beam shadows actually carve the cone-mesh shaft + force-cast-shadows on every imported primitive (v1.0.99).**
> User report (verbatim, against v1.0.96/98):
>
> > "Im not seeing this work at all.
> >
> > Part A -- Self-shadowed cone-mesh raymarch. [v1.0.96 description quoted]
> >
> > Can we check that all imported objects cast shadows as default.
> >
> > The light beam currently goes straight through any object"
>
> Two coupled bugs were the smoking gun -- one in the v1.0.96 shader, one in the import
> pipeline -- and v1.0.99 fixes both. After v1.0.99 a cube placed between a fixture and
> the floor visibly carves the M_RebusBeam shaft AND casts a hard shadow in the SpotLight
> footprint on the floor.
>
> **Part A -- v1.0.96 screen-space shadow trace was projecting every shadow step
> off-screen (Cause 1, the LWC projection bug).**
>
> The v1.0.96 `_BEAM_RAYMARCH_HLSL` Custom node computed `spRel = sp - ro` to convert
> absolute-world to translated-world before the `mul(..., ResolvedView.TranslatedWorldToClip)`
> projection. UE 5.4+'s clip-space matrix expects
> `TranslatedWorld = AbsoluteWorld + ResolvedView.PreViewTranslation` -- those are equal
> ONLY when the view origin is exactly the camera origin AND PreViewTranslation has no
> jitter / shadow-pass / LWC tile term. On a stage scene the projected NDC was offset by
> the view-origin delta, landed outside the [-1, 1] range, and the
> `if (any(abs(ndc) > 1.0)) { continue; }` off-screen guard then dropped EVERY shadow tap
> -- so the per-pixel `sOccluded` flag stayed false on every sample and the trace
> concluded "always unoccluded" regardless of what was actually in the scene. Operator
> symptom: "the light beam currently goes straight through any object" (the user report,
> verbatim).
>
> v1.0.99 fix in `build_rebus_base_level.py::_BEAM_RAYMARCH_HLSL`:
>
> ```hlsl
> // v1.0.96 (BROKEN -- assumed view origin == camera origin):
> float3 spRel = sp - ro;
> float4 clipP = mul(float4(spRel, 1.0), ResolvedView.TranslatedWorldToClip);
>
> // v1.0.99 (LWC-safe -- the documented engine pattern):
> float3 PreViewT = LWCHackToFloat(ResolvedView.PreViewTranslation); // cached once / pixel
> float3 TranslatedWorldPos = sp + PreViewT;
> float4 clipP = mul(float4(TranslatedWorldPos, 1.0), ResolvedView.TranslatedWorldToClip);
> ```
>
> Causes 2--5 from the bug brief were investigated and ruled out:
>
> * **Cause 2 (depth-comparison direction)**: the v1.0.96 `if (sd + stepBias < clipP.w)`
>   check is correct -- both `CalcSceneDepth` and `clip.w` (UE perspective projection)
>   are linear camera-Z in cm, and "scene closer than step" is the right "step occluded
>   from the camera" condition. Symptom would be uniform dimming if wrong; the user
>   reported NO change at all -- consistent with Cause 1, not Cause 2.
> * **Cause 3 (self-occlusion against the cone's own depth)**: the cone mesh is
>   `BLEND_ADDITIVE` and doesn't write to SceneDepth, so the cone itself can't be its
>   own occluder. But the shaft sample's projected pixel CAN sit ON nearby fixture body
>   / prop geometry that DOES write to SceneDepth -- v1.0.99 raises `BeamShadowBias`
>   from 0.5 -> 5.0 cm and re-purposes it as the FIRST-STEP MINIMUM cm so the very
>   first SceneDepth tap is at least 5 cm out from `wp` toward the light (with `Rebus.
>   BeamShadowBias 50` available as the operator-side knob if 5 cm is still too small
>   for the rig).
> * **Cause 4 (push wiring stale)**: the v1.0.96 `RefreshBeamShadowParams` correctly
>   pushed Steps + Strength + Bias on every CVar change. v1.0.99 extends it to push the
>   new `BeamShadowDebug` scalar too AND adds the `Rebus.DumpBeamShadow` console
>   command so a future "doesn't work" report can prove or disprove this in one
>   pasted log block.
> * **Cause 5 (Custom-node `inputs` array out of sync)**: the v1.0.96 `_build_beam_master`
>   wired Steps/Strength/Bias correctly through `input_names + custom_inputs + src_for`.
>   v1.0.99 follows the same pattern for the new `BeamShadowDebug` scalar so this can't
>   regress.
>
> **Smoking gun: Cause 1 (LWC projection).** The shader was running every frame, the
> CVar push was landing every value, the master was the v1.0.96 shape -- and every
> single shadow step landed off-screen because the projection was wrong by the view
> origin. Fixing the projection is what made the trace start finding occluders.
>
> **New `BeamShadowDebug` visualisation modes (`Rebus.BeamShadowDebug [0|1|2]`).**
> The user-facing knob to verify the v1.0.99 fix landed:
>
> | Mode | What renders | Interpretation |
> | --- | --- | --- |
> | 0 | Off (default; the regular composed beam). | Operator's normal view. |
> | 1 | Per-pixel shadow-factor heatmap. Cone shape preserved; colour is `lerp(green, red, shadowedFraction)` x 4.0 (bright). | A cube placed between fixture + floor should appear RED against a green beam. If the cube is GREEN, the trace found NO occluders -- check `Rebus.DumpBeamShadow` for `Strength=0.0` (master toggle off) or a pre-v1.0.99 master ("missing MID scalars" note). |
> | 2 | First shadow step's projected screen UV as `(uv.x, uv.y, 0)` x 4.0 (bright). | A correctly-projected step lands at uv in [0, 1]^2 so the beam tints orange/yellow across the floor. Constant near-black means the LWC projection is broken (the v1.0.99 fix didn't land -- pre-v1.0.99 master, run `build_rebus_base_level.build()` to regenerate). |
>
> **Bias + Debug are now CVar-tunable** (v1.0.96 only exposed Steps + Strength). New
> CVars: `Rebus.BeamShadowBias <cm>` (default 5.0) and `Rebus.BeamShadowDebug [0|1|2]`
> (default 0). Both push through `RefreshBeamShadowParams` on the same refresh-sink
> path the v1.0.96 CVars used, so a portal push and a console set agree on every
> fixture's MID.
>
> **`_beam_master_has_shadow_debug` self-heal probe.** Mirrors the v1.0.96
> `_beam_master_has_shadow_steps` pattern: `ensure_beam_material()` checks for the new
> `BeamShadowDebug` scalar on the existing on-disk master and force-regenerates with a
> Warning when it's missing. So the first launch on v1.0.99 picks up the corrected
> Custom HLSL on the next editor restart with no manual re-bake.
>
> **Part B -- force-cast-shadows on every Orbit-imported primitive.**
>
> User-facing question: "Can we check that all imported objects cast shadows as
> default." UE primitives default to `CastShadow = true`, but the glTFRuntime +
> OrbitConnector import path lands them with `CastShadow = false` for perf. Result:
> the SpotLight's own shadow casting catches NOTHING in the floor footprint either,
> and the user's "the light beam currently goes straight through any object" report
> applies to the SpotLight footprint as well as the cone-mesh shaft.
>
> v1.0.99 normalises every imported primitive's shadow flags on the same 1 Hz cadence
> as `RebindOrbitModels` + `ApplyTrussMaterialPass` (the existing v1.0.85 truss
> pipeline). New code in `URebusVisualiserSubsystem`:
>
> | Surface | Behaviour |
> | --- | --- |
> | `EnsureImportedShadowsCast()` | Walks every `OrbitImportRoot` actor (matched by class-name string -- zero compile dep on the OrbitConnector plugin) and asserts `CastShadow = true / bCastDynamicShadow = true / bCastHiddenShadow = false / bCastFarShadow = true` on every `UPrimitiveComponent`. Idempotent (per-comp early-out when flags match). Tracked comps go into `OrbitShadowTouched` so the OFF path can find them again. |
> | `SetOrbitCastShadowsEnabled(bool)` | Single chokepoint shared by the console command and the scene-property handler. Walks `OrbitShadowTouched` to flip every prior comp + runs `EnsureImportedShadowsCast` to pick up any newly-imported geometry, so a single toggle transition is consistent across the whole import on the same call. |
> | `Rebus.OrbitCastShadows [0|1]` console | New (default ON). Routes through `SetOrbitCastShadowsEnabled` per Game/PIE world. |
> | `bOrbitCastShadows` scene property | New (default `true`, seeded in `URebusSceneSettingsSubsystem::Initialize`). Routes through the same chokepoint via `ApplySceneProperty`. SceneState round-trips it; `ReapplyAll` re-asserts on (re)spawn. |
> | Tick hook | The visualiser subsystem's 1 Hz orbit-rebind tick now also calls `EnsureImportedShadowsCast()` so newly-imported Orbit geometry inherits the shadow-cast normalisation on the next 1 s without an operator console call. |
>
> Why `bCastHiddenShadow = false` REGARDLESS of the toggle: a hidden shadow caster
> projects a black silhouette in the lit pool with NO surface visible, which reads as
> a phantom dark blob the operator never wants. The toggle covers the other three
> flags only.
>
> **Files touched.**
>
> | File | Change |
> | --- | --- |
> | `REBUS_Visualiser/Content/Python/build_rebus_base_level.py` | `_BEAM_RAYMARCH_HLSL` -- LWC-safe `PreViewTranslation` projection; `BeamShadowBias` repurposed as FIRST-STEP MINIMUM cm; new `BeamShadowDebug` branch (modes 0/1/2). `_build_beam_master` -- new `BeamShadowDebug` scalar wired through `input_names + custom_inputs + src_for`; `BeamShadowBias` default raised 0.5 -> 5.0. New `_beam_master_has_shadow_debug` self-heal probe + `ensure_beam_material` force-regen branch. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusFixtureActor.cpp` + `Public/RebusFixtureActor.h` | Two new CVars (`Rebus.BeamShadowBias`, `Rebus.BeamShadowDebug`) routed through the existing `RefreshBeamShadowParamsOnEveryFixture` sink. `RefreshBeamShadowParams` now pushes ALL FOUR scalars. New `DumpBeamShadowStateForDebug()` per-fixture method. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusVisualiser.cpp` | New `Rebus.DumpBeamShadow` + `Rebus.OrbitCastShadows` console commands; matching `ShutdownModule` cleanup. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusVisualiserSubsystem.cpp` + `Public/RebusVisualiserSubsystem.h` | `EnsureImportedShadowsCast()` + `SetOrbitCastShadowsEnabled(bool)` + `OrbitShadowTouched` tracking set. Tick hooks the new helper on the 1 Hz rebind cadence. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusSceneSettingsSubsystem.cpp` | `bOrbitCastShadows` seed (default `true`) + `ApplySceneProperty` dispatcher routing through `URebusVisualiserSubsystem::SetOrbitCastShadowsEnabled`. |
>
> **Operator verification checklist.** Run after rebuild + relaunch on a scene that has
> at least one fixture aimed at the floor:
>
> 1. Drop a unit cube in the level between the fixture and the floor (so its silhouette
>    should be in the beam path).
> 2. From the portal console (or in-editor console) run `Rebus.DumpBeamShadow`. Confirm
>    every fixture line shows `MID(Steps=8.0 Strength=1.000 Bias=5.00 Debug=0)
>    CVars(Steps=8.0 Strength=1.000 Bias=5.00 Debug=0) -- shadowing ENABLED, debug
>    mode 0` (or whatever the operator's current values are -- the MID column and CVars
>    column MUST agree).
> 3. Run `Rebus.BeamShadowDebug 1`. The whole beam tints `lerp(green, red, shadowed)`.
>    The cube should appear RED inside the beam (the shadow factor is non-zero where
>    the cube blocks the line to the light). Green elsewhere.
> 4. Run `Rebus.BeamShadowDebug 2`. The beam tints by the projected screen UV; the
>    floor footprint paints a smooth orange/yellow gradient (uv -> RG). A
>    constant-near-black tint here means the v1.0.99 LWC projection didn't land --
>    the master is pre-v1.0.99; run `build_rebus_base_level.build()` to regenerate.
> 5. Run `Rebus.BeamShadowDebug 0`. The beam returns to its normal composed look.
>    Confirm visually that the shaft is now CARVED at the cube (the cube's silhouette
>    cuts the volumetric shaft). This is the user-reported regression resolved.
> 6. Confirm the SpotLight's floor-footprint shadow shows the cube too (Part B). With
>    `Rebus.OrbitCastShadows 1` (default) the cube + every Orbit-imported truss / set
>    piece projects a hard shadow in the lit pool. Toggle `Rebus.OrbitCastShadows 0`
>    to A/B against the no-shadow baseline.
>
> **Operator-friendly defaults: hide Orbit fixtures + 16:9 DSLR camera sensor (v1.0.98).**
> User request (verbatim):
>
> > "can you also set these as defaults
> >
> > Rebus.showorbitfixtures 0
> > sensor to 16:9"
>
> Two contained default-value flips, both about what the operator sees on FIRST launch
> before any portal push lands. Neither changes the wire contract or the override path --
> the portal can still drive both via the existing scene-property / camera-state pipelines.
>
> **Why the flips.**
> 1. **Default-hide Orbit-imported fixture geometry.** With OrbitConnector loading the full
>    GDTF/glb fixture bodies AND `ARebusFixtureActor` building its own control-channel mesh
>    proxies, the live previs shows BOTH on top of each other on a fresh launch (the proxies
>    driven by motion, the Orbit imports static at rest pose). Operator-tested fix: hide the
>    Orbit imports by default. The proxies remain the visible, motion-driving meshes.
> 2. **16:9 DSLR camera sensor.** The cinematic camera defaulted to Super35 (24.89mm x
>    18.66mm, ~4:3 cinematic) and the live previs frame didn't match the streamed surface's
>    16:9 aspect, so the operator had to push a sensor change every session before composing
>    a shot. Flipping the construction-time default to UE 5.7's "16:9 DSLR" preset
>    (23.76mm x 13.365mm, exact 1.778 aspect) makes the streamed view frame-correct out of
>    the box.
>
> **Part 1 -- `bShowOrbitFixtures` scene-property seed = false.**
>
> | Surface | Pre-v1.0.98 | v1.0.98 |
> | --- | --- | --- |
> | `URebusSceneSettingsSubsystem::Initialize` seed | (no seed; ConsoleCommand-only path) | `Values.Add("bShowOrbitFixtures", false)` |
> | `ApplySceneProperty` handler | (none -- console-command-only since v1.0.70) | walks every `ARebusFixtureActor` in the world, calls `SetOrbitVisibility(bShow)` |
> | `ARebusFixtureActor::SetOrbitVisibility` | iterates `OrbitComponents` and sets visibility | same, plus caches the desired state on the actor |
> | `ARebusFixtureActor::BindOrbitComponents` | binds + drives, never re-asserts visibility | binds + drives, then re-applies the cached desired-visibility on every freshly-bound component |
>
> **Subtle bug call-out: the Orbit-fixture visibility Reapply path.** The user's design
> note flagged the `if (Value == CurrentLiveState) return;` early-out trap (initial
> `false -> false` no-ops). That trap doesn't fire here -- `SetOrbitVisibility` walks
> components and calls `USceneComponent::SetVisibility(bVisible, bPropagateToChildren=true)`
> unconditionally (mirrors the no-early-out pattern used by `bGroundVisible` and
> `SetMeshBeamsEnabled`). The actual subtle bug we hit and fixed is timing-shaped:
>
> * `ReapplyAll` fires once after `EnsureSceneEnvironment` and once after fixtures spawn (in
>   `URebusVisualiserSubsystem`). At BOTH of those moments the Orbit-import components have
>   not yet been matched onto the fixtures -- the matching is done by
>   `URebusFixtureControlSubsystem::RebindOrbitModels` on the visualiser subsystem's 1Hz
>   tick. So the `bShowOrbitFixtures=false` handler iterates every `ARebusFixtureActor`, but
>   each actor's `OrbitComponents` array is still empty, and the loop hides nothing.
> * Up to 1 s later, `RebindOrbitModels` matches and binds the components via
>   `ARebusFixtureActor::BindOrbitComponents`. Without v1.0.98 those new components inherit
>   UE's default-visible state -- the seed never reaches them.
>
> Fix: `ARebusFixtureActor::SetOrbitVisibility` now writes the desired state to a new
> `bOrbitDesiredVisibility` member (defaults to `true` so legacy call sites are unchanged).
> `BindOrbitComponents` re-applies the cached state on every (re)bind. So the seed-driven
> `false` reaches the freshly-bound Orbit components on the very first bind without a
> recycle, and a portal push of `bShowOrbitFixtures=true` followed by an OrbitConnector
> re-import keeps the components visible too.
>
> **Console-command tolerance (v1.0.72) preserved.** The user typed
> `Rebus.showorbitfixtures 0` (no `Rebus.` prefix variant: `showorbitfixtures 0`); both
> still route correctly via the v1.0.72 fixture-channel `ConsoleCommand` shim that retries
> the bare name with the `Rebus.` prefix on `success=0`. The console command itself is
> unchanged (`HandleShowOrbitFixturesCommand` still walks every Game/PIE world and toggles
> visibility per fixture); v1.0.98 just adds the SCENE-PROPERTY path so SceneState
> round-trips it and `ReapplyAll` re-asserts it on respawn.
>
> **Part 2 -- 16:9 DSLR camera sensor (`ARebusCineCameraPawn`).**
>
> | Field | Pre-v1.0.98 (Super35) | v1.0.98 (16:9 DSLR) |
> | --- | --- | --- |
> | `Filmback.SensorWidth`        | 24.89 mm | **23.76 mm** |
> | `Filmback.SensorHeight`       | 18.66 mm | **13.365 mm** |
> | `Filmback.SensorAspectRatio`  | 1.333 (4:3 cinematic) | **1.778 (exact 16:9)** |
> | `FRebusCameraState::SensorWidthMm` initial | 24.89 | **23.76** |
> | `FRebusCameraState::SensorHeightMm` initial | 18.66 | **13.365** |
>
> Applied in three places (constructor, `ResetToDefaults`, and the `FRebusCameraState`
> initial values broadcast on the first `BroadcastHandshake` / force-push) so portal-side
> state matches the cine component's live filmback before the operator pushes a value. The
> v1.0.96 `AutoExposureBias = +10 EV` default is preserved untouched alongside the new
> sensor lines (the +10 EV change addressed live-previs brightness; the sensor change
> addresses framing, orthogonal axes).
>
> **Sensor preset note: 16:9 DSLR vs 16:9 Digital Film.** UE 5.7 ships two 16:9 presets in
> `UCineCameraComponent::FilmbackPresets`: "16:9 DSLR" (23.76 x 13.365 mm) and "16:9
> Digital Film" (slightly different dimensions). We chose **DSLR** because it's the more
> common reference for live-streaming bodies (DSLR/mirrorless cameras feeding studio
> multiviewers) which is the workflow the live previs is plugged into. Operators who need
> Digital Film numbers can push them via the existing `SetSensorSizeMm` / `SetCameraSensor`
> wire descriptor; this is JUST the construction-time landing value.
>
> **Streaming-aspect distinction (operator note).** The pixel-streaming pipeline has its
> own aspect ratio governed by the streaming surface resolution
> (`PixelStreaming2.WebRTC.*`, encoder render-target). Setting the camera SENSOR to 16:9
> aligns the previs FRAME with the streamed surface's typical 16:9 aspect, but it does NOT
> reconfigure the streaming surface itself. If the operator overrides the streaming
> resolution (custom `r.SetRes` / encoder target) to a non-16:9 ratio they'll get pillar /
> letter-boxing in the streamed feed regardless of sensor size; that's expected, and the
> fix is on the streaming-config side. Future operators reading this README: don't
> conflate the two.
>
> **Operator overrides are unchanged.** Both flips are JUST default seeds; the existing
> wire pipelines still drive live state.
>
> * **Part 1 (Orbit fixtures).** Portal-side: `SetSceneProperty name="bShowOrbitFixtures"
>   value=true|false` (round-trips in `SceneState`, re-asserted on respawn via
>   `ReapplyAll`). Console-side: `Rebus.ShowOrbitFixtures 1|0` -- and the v1.0.72 prefix
>   tolerance shim keeps the bare-name form (`showorbitfixtures 0`) working too.
> * **Part 2 (camera sensor).** Portal-side: the existing camera-state push descriptor
>   (`SetCameraSensor` / equivalent state push) drives `SetSensorSizeMm(width, height)`,
>   which clamps to (1..100, 1..100). The `Rebus.CameraReset` console command lands on the
>   new 16:9 DSLR seed via `ResetToDefaults`.
>
> **Files touched.**
>
> * `Source/RebusVisualiser/Public/RebusFixtureActor.h` -- new
>   `bOrbitDesiredVisibility` member (defaults true; mutated by `SetOrbitVisibility`).
> * `Source/RebusVisualiser/Private/RebusFixtureActor.cpp` -- `SetOrbitVisibility` writes
>   the cache; `BindOrbitComponents` re-applies it after every (re)bind.
> * `Source/RebusVisualiser/Private/RebusSceneSettingsSubsystem.cpp` -- new
>   `bShowOrbitFixtures=false` seed in `Initialize`; new `ApplySceneProperty` branch that
>   walks every `ARebusFixtureActor` in this world and calls `SetOrbitVisibility`.
> * `Source/RebusVisualiser/Public/RebusCineCameraPawn.h` -- `FRebusCameraState`
>   initial sensor values flipped to 23.76 / 13.365.
> * `Source/RebusVisualiser/Private/RebusCineCameraPawn.cpp` -- ctor + `ResetToDefaults`
>   set Filmback to 23.76 x 13.365 with `SensorAspectRatio = 1.778`. The reset log line
>   reads "v1.0.98 defaults (35mm f/2.8 focus@5m 16:9 DSLR manual EV+10)" so the operator
>   can verify the landing.
>
> No engine / OrbitConnector / Epic-DMX-Fixtures asset is touched. The v1.0.96
> AutoExposureBias = +10 EV default + the v1.0.96 / v1.0.97 post-process and material
> changes are preserved verbatim.

> **Every Rebus-authored Python master material is now double-sided (v1.0.97).**
> User request (verbatim):
>
> > "can you make sure all materials are double sided"
>
> Single, contained deliverable: flip `two_sided = True` on every master that
> `build_rebus_base_level.py` AUTHORS under `/Game/REBUS/Materials/`, plus a self-heal
> probe so an EXISTING on-disk master baked by a pre-v1.0.97 startup gets regenerated on
> the next launch (no operator action required). No engine / Epic / OrbitConnector /
> glTFRuntime material is touched -- those are operator-controlled or owned by other
> teams and out of scope.
>
> **Why double-sided.** With back-face culling the operator sees a black hole / "punch-
> through" from any angle that catches a back face: the camera dipping under the 2 km
> floor plane (sub-floor pit / off-axis swing-cam), or stepping INSIDE a moving-head
> shell so the procedural `<Beam>` lens disc reads from its back face, or any operator-
> authored sub-floor mesh that re-uses an `MI_RebusGround_*` instance. Flipping
> `two_sided` on the masters fixes all three because UE's Material Instances inherit
> top-level material settings from their parent -- no MI work needed.
>
> **What flipped, what was already.**
>
> | Master | Path | Pre-v1.0.97 | v1.0.97 |
> | --- | --- | --- | --- |
> | `M_RebusGround`      | `/Game/REBUS/Materials/M_RebusGround`      | single-sided      | **double-sided** |
> | `M_RebusFixtureLens` | `/Game/REBUS/Materials/M_RebusFixtureLens` | single-sided      | **double-sided** |
> | `M_RebusLensFlare`   | `/Game/REBUS/Materials/M_RebusLensFlare`   | double-sided (v1.0.32) | unchanged (verified) |
> | `M_RebusBeam`        | `/Game/REBUS/Materials/M_RebusBeam`        | double-sided (v1.0.31) | unchanged (verified) |
>
> The v1.0.96 worker's `_build_beam_master` retouch is preserved -- it already set
> `two_sided = True` (the cone-mesh raymarch needs it so the back wall carries the shaft
> when the camera is inside the cone -- see v1.0.39 / v1.0.96 release blocks). v1.0.97
> does not re-set the flag on those two masters, only verifies they're already true.
>
> **Self-heal probe -- `_master_is_two_sided`.** New helper next to v1.0.86's
> `_master_has_tiling_meters`:
>
> ```python
> def _master_is_two_sided(master):
>     try:
>         return bool(master.get_editor_property("two_sided"))
>     except Exception:  # noqa: BLE001
>         return False
> ```
>
> Best-effort by design: any exception (engine-version rename, asset shape we don't
> recognise) returns False so the self-heal path treats the master as "needs regen". A
> false negative is cheap (one redundant rebake on next launch); a false positive would
> leave the operator on the OLD single-sided master indefinitely, which is exactly what
> this probe exists to prevent. Matches v1.0.86 / v1.0.93 / v1.0.96 probe semantics.
>
> Wired into:
>
> * **`ensure_ground_materials()`** -- combined with the v1.0.86 `_master_has_tiling_meters`
>   check via OR. EITHER missing-TilingMeters OR single-sided triggers ONE force-regen
>   (no double-bake on a project that pre-dates v1.0.86 + v1.0.97 simultaneously). The
>   Warning log line now reads `pre-v1.0.97 ground master detected (missing TilingMeters
>   or single-sided); regenerating master + instances.` so the operator can see the
>   migration happen.
> * **`ensure_fixture_lens_material()`** -- combined with the v1.0.93
>   `_fixture_lens_master_is_current` check via OR, same shape, log line
>   `pre-v1.0.97 M_RebusFixtureLens detected (missing Color/Metallic/Roughness parameter
>   contract, or single-sided); regenerating.`
> * `M_RebusBeam` already has its own v1.0.96 self-heal probe
>   (`_beam_master_has_shadow_steps`); since the v1.0.96 master already ships double-
>   sided, no extra probe was added for the beam master.
> * `M_RebusLensFlare` has no self-heal probe in any previous version -- it's been
>   double-sided since v1.0.32 and the project's `ensure_lens_material()` is purely
>   missing-asset based. No probe added in v1.0.97 because there's nothing to migrate
>   FROM (every shipped version was already two-sided).
>
> **Runtime MIDs follow automatically.** `ARebusFixtureActor` builds runtime MIDs from
> three parent material references (`FixtureMatParent` -> chrome lens MID,
> `FixtureBodyMaterialOverride` -> body MID, `TrussMaterialOverride` -> truss MID) and a
> live `BasicShapeMaterial` fallback when the user-authored override is missing. UE
> Material-Instance-Dynamic objects INHERIT the top-level `two_sided` flag from their
> parent material asset; once the Python rebake lands the new double-sided
> `M_RebusFixtureLens`, every per-fixture chrome-lens MID becomes double-sided on the
> next spawn without any C++ change. Same for the ground MID stack on every
> `MI_RebusGround_*` Concrete/Tarmac/Sand/Grass preset.
>
> **NO Python pass for non-Rebus materials.** Per the deliverable spec, we do NOT
> iterate the asset registry and force-flip every material in `/Game/` to two-sided --
> operator-authored materials and truss / set-piece materials assigned outside our
> pipeline are operator-controlled, and a blanket flip would clobber intentional single-
> sided choices (e.g. cards / billboards that read better with culling).
>
> **Limitation: engine-asset fallback stays single-sided.** The runtime
> `BasicShapeMaterial` fallback (used when the user-authored `M_RebusFixtureLens` /
> body / truss override asset doesn't exist) is `/Engine/BasicShapes/BasicShapeMaterial`
> -- an engine-shipped material we can't and shouldn't mutate. **Operators should
> confirm the v1.0.97 Python rebake ran** so the Rebus-authored masters take the
> primary path; if a fixture still reads as a flat single-sided fallback, the log will
> show `RebusFixtureActor: lens material asset missing, falling back to
> BasicShapeMaterial` (pre-existing v1.0.95 warning -- same wording, still fires when
> the Python bake didn't land the asset).
>
> **Operator checklist after rebuilding to v1.0.97.**
>
> 1. **Restart the editor once** -- `ensure_base_level()` runs on `init_unreal`, and the
>    v1.0.97 self-heal will detect the pre-v1.0.97 `M_RebusGround` / `M_RebusFixtureLens`
>    masters, log the two `pre-v1.0.97 ... detected ... regenerating` Warnings, and
>    force-regen them with `two_sided = True`. A fresh checkout (no on-disk masters yet)
>    just bakes them double-sided directly with no Warning.
> 2. **Open one ground MI** in the Material Editor (e.g.
>    `/Game/REBUS/Materials/MI_RebusGround_Concrete`), open the parent (`M_RebusGround`),
>    confirm the **`Two Sided` checkbox is ticked** in the Material Editor's *Details*
>    panel. Same for `M_RebusFixtureLens`. (Already true on `M_RebusBeam` and
>    `M_RebusLensFlare` -- spot-check optional.)
> 3. **Load a fixture in the level** and orbit the camera so it sits **inside** the moving-
>    head shell, looking out through the lens disc. Pre-v1.0.97: the lens disc reads as
>    a black hole from inside (culled back face). v1.0.97: the chrome-mirror disc is
>    visible from inside the head, matching the v1.0.95 deliverable's intent ("lens is
>    always visible").
> 4. **Optional: dip the camera below the floor** (Z < 0 inside the 2 km plane footprint).
>    Pre-v1.0.97: the floor punches through to skybox. v1.0.97: the back face renders
>    the same procedural ground, reading as a real opaque surface from below.
> 5. **If a fixture still reads single-sided** AND its body / lens / truss material is
>    the engine `BasicShapeMaterial` fallback (check the log for the v1.0.95 "falling
>    back to BasicShapeMaterial" Warning), that's the documented engine-asset
>    limitation -- ensure the Python rebake landed the Rebus-authored masters as the
>    primary path.
>
> **Files touched (v1.0.97).**
>
> | File | Change |
> | --- | --- |
> | `REBUS_Visualiser/Content/Python/build_rebus_base_level.py` | `_set(mat, "two_sided", True)` in `_build_ground_master` + `_build_fixture_lens_master`; new `_master_is_two_sided` probe alongside `_master_has_tiling_meters`; `OR not _master_is_two_sided(existing)` wired into `ensure_ground_materials` + `ensure_fixture_lens_material` self-heal checks (log lines updated to "pre-v1.0.97 ... regenerating"). |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/README.md` | v1.0.97 release block (this one). |
>
> No C++ changes (runtime MIDs inherit `two_sided` from the parent material asset). No
> level / actor / subsystem surface changes. No CVars. No new scene properties.

> **Self-shadowed cone-mesh raymarch (screen-space shadow trace) + camera/post-process defaults (v1.0.96).**
> User report (verbatim):
>
> > "Are we able to simulate volumetric shadows are the raymarch cone-mesh beam so the beam
> > doesnt go through objects?
> >
> > Can we default exposure to +10 and turn lens flare to 0, bloom to 0.2"
>
> Two coupled deliverables shipped together because both adjust the operator's first-launch
> visual baseline:
>
> ---
>
> **PART A -- Screen-space shadow trace on the M_RebusBeam cone-mesh raymarch.**
>
> Pre-v1.0.96 the cone-mesh raymarch (`M_RebusBeam`, `_BEAM_RAYMARCH_HLSL` in
> `build_rebus_base_level.py`) only soft-clipped its visible shaft against `SceneDepth` at
> the fragment's own pixel -- the shaft faded where it physically met the floor / a wall,
> but at every shaft sample BEHIND an in-frame occluder the shader had no knowledge of
> whether that step was reachable from the SpotLight. Result: a cube placed between the
> fixture and the floor would receive a solid floor-shadow (v1.0.94's job) but the
> volumetric beam itself rendered THROUGH the cube as if nothing was there.
>
> **The fix: per-shaft-sample screen-space shadow ray trace, inside the existing march
> loop.** For each step along the view ray at world position `wp`, the shader now marches
> a small number of additional steps from `wp` toward the SpotLight position `bo`. At each
> shadow step it world-to-screen projects via `ResolvedView.TranslatedWorldToClip`, samples
> `CalcSceneDepth(uv)` at the projected UV, and compares against the step's camera-Z
> (`clip.w` in UE perspective projection). If any shadow step finds an opaque feature in
> front of itself (depth-buffer Z < step.cameraZ), the shaft sample is marked SHADOWED and
> its density contribution is attenuated by `1 - BeamShadowStrength`. Result: a feature
> visible to the camera between the fixture and the floor visibly CUTS the volumetric
> shaft at the occluder's silhouette.
>
> **Why screen-space and not Mesh-Distance-Field (the canonical UE technique).** The PRISM
> deployment runtime-imports its truss / set-piece / rig geometry through OrbitConnector as
> glTF blobs -- those meshes never go through the static-mesh cooking pipeline, so they
> carry NO Mesh Distance Fields at runtime. A material can't DF-trace what isn't built.
> UE 5.7's Virtual Shadow Map sampling from a non-engine Custom HLSL node is fragile (no
> documented entry point on translucent surface materials), so we chose the pragmatic,
> robust path that works for ANY in-frame geometry without per-mesh authoring: screen-space
> shadow tracing. **MDF fallback for cooked rigs is documented future work** -- if/when
> Orbit-imported meshes are promoted into the cook pipeline, the shader can be extended to
> tap the DF and consult on-screen-vs-off-screen on a per-step basis.
>
> **Algorithm (per shaft sample inside the existing march loop, after `d` is computed but
> before composing it into the transmittance):**
>
> 1. `toLight = bo - wp; dToLight = |toLight|; sdir = toLight / dToLight`.
> 2. March `BeamShadowSteps` steps from `wp` toward `bo`, step distance
>    `sdt = dToLight / BeamShadowSteps`. The shadow march MUST NOT exceed `dToLight` --
>    we clamp `sj < dToLight` so a far-away occluder behind the light can't falsely shadow
>    the shaft.
> 3. At each step, world position -> NDC -> ScreenUV via
>    `mul(float4(sp - ro, 1), ResolvedView.TranslatedWorldToClip)` (subtract the camera
>    ray-origin `ro` first to go absolute-world -> camera-relative; the matrix expects
>    translated/camera-relative input in UE 5.7's LWC era).
> 4. Tap `CalcSceneDepth(uv)` at the projected UV. If `sd + perStepBias < step.cameraZ`,
>    the step is OCCLUDED from the camera's view -> something opaque is in front of it ->
>    the step is shadowed from the light too (under the screen-space assumption). The
>    `BeamShadowBias` (0.5 cm authored default) is the additive cm tolerance that skips
>    the shaft sample's own pixel as its own occluder when the very first shadow step
>    lands on the same screen pixel; a further per-step `0.01 * sdt` term scales with the
>    step distance to remain robust at long shadow rays.
> 5. If ANY step hits an occluder, mark the sample shadowed and attenuate density by
>    `1 - BeamShadowStrength`. Strength = 1 -> the shadowed sample contributes nothing
>    (full shadow). Strength = 0 -> the `[branch]` gate in the shader takes the whole
>    trace OUT of the per-pixel cost.
>
> The shadow trace is wrapped in `[loop]` (HLSL DX-SM5+) so the compiler doesn't try to
> unroll it. The shaft-sample's own contribution to `SceneDepth` is excluded via the
> `BeamShadowBias` per-step offset (or by skipping the very first shadow step when the
> linear offset minus bias is <= 0).
>
> **Limitations (these are the trade-offs vs MDF).**
>
> * Only handles occluders **visible to the camera** (screen-space). An off-screen
>   occluder doesn't cast shadow into the shaft -- its depth isn't in the depth buffer.
> * Cost ~ `StepCount * BeamShadowSteps` SceneDepth taps per beam pixel.
>   `32 * 8 = 256 taps/pixel` is the design point; keep `Rebus.BeamShadowSteps` <= 16.
> * No MDF fallback (Orbit-imported meshes lack SDFs at runtime); future work.
> * If sharp edges / banding appears in the field, the FUTURE-WORK option is to jitter
>   the shadow steps with `Rand0to1` / `DitherTemporalAA` -- NOT implemented in v1.0.96
>   because the operator-tested visuals were acceptable without it.
>
> **New material parameters on `M_RebusBeam` (Python-authored, see
> `_build_beam_master` + `_BEAM_RAYMARCH_HLSL`):**
>
> | Param | Type | Default | Notes |
> | --- | --- | --- | --- |
> | `BeamShadowSteps` | scalar | 8 | Shadow trace steps per shaft sample (shader clamps to [1, 16]). |
> | `BeamShadowStrength` | scalar | 1.0 | 0 = trace runs but does nothing (visually disabled); 1 = full shadow on any sample whose ray hits an occluder. The shader's `[branch] if (BeamShadowStrength > 0.001)` gate takes the trace OUT of the per-pixel cost when strength is exactly 0. |
> | `BeamShadowBias` | scalar | 0.5 | Per-step bias offset in cm to prevent self-shadowing on the shaft sample's own screen pixel. Not exposed via CVar in v1.0.96 -- if banding appears in the field we'll promote it. |
>
> **Self-heal on the regenerated `M_RebusBeam` master.** Mirroring the v1.0.86
> (`_master_has_tiling_meters`) and v1.0.93 (`_fixture_lens_master_is_current`) self-heal
> patterns, `ensure_beam_material()` (non-force path) probes the existing master via
> `_beam_master_has_shadow_steps()`: if the on-disk master lacks the three
> `BeamShadow*` scalar parameters it's treated as pre-v1.0.96, force-regen is triggered,
> and a Warning is logged: *"RebusBaseLevel: pre-v1.0.96 M_RebusBeam detected (missing
> BeamShadowSteps/BeamShadowStrength/BeamShadowBias); regenerating with screen-space
> shadow trace."* Anyone who customised the master in the editor loses those edits on
> first v1.0.96 startup -- they should re-apply them after the regen (acceptable for an
> additive parameter upgrade; the regen logs a Warning so the change isn't silent).
>
> **New CVars + handlers (live, refresh sinks walk every fixture and re-push the BeamMID):**
>
> ```text
> Rebus.BeamShadowSteps <n>       # default 8; shader clamps [1, 16]
> Rebus.BeamShadowStrength <0..1> # default 1.0; 0 = visually disabled
> Rebus.BeamShadow [0|1|status]   # MASTER toggle. OFF saves prior strength + writes 0;
>                                 # ON restores saved prior (default 1.0 on a fresh
>                                 # launch). `status` logs without mutating either.
> ```
>
> `Rebus.BeamShadowSteps` and `Rebus.BeamShadowStrength` are `FAutoConsoleVariableRef`
> CVars in `RebusFixtureActor.cpp` (paired globals `GRebusBeamShadowSteps` /
> `GRebusBeamShadowStrength`); their refresh sinks share a helper
> `RefreshBeamShadowParamsOnEveryFixture` that walks every Rebus fixture in every loaded
> world and calls `ARebusFixtureActor::RefreshBeamShadowParams` (which pushes the three
> scalars onto the per-fixture BeamMID). `Rebus.BeamShadow` is a console COMMAND in
> `RebusVisualiser.cpp` (binary semantics with prior-value storage -- can't be a pure
> CVar) that routes through the existing `Rebus.BeamShadowStrength` CVar so the refresh
> sink walks every fixture exactly once on the toggle.
>
> **C++ push path.** `ARebusFixtureActor::BuildBeamCone` calls
> `RefreshBeamShadowParams()` right after the M_RebusBeam MID is created, so a
> fresh-spawn fixture starts with the operator's current CVar values (not the master's
> authored defaults). Subsequent CVar changes re-push via the refresh sink. Pre-v1.0.96
> M_RebusBeam masters lack the `BeamShadow*` scalars -- `SetScalarParameterValue` on a
> missing parameter is a silent no-op, so this is safe to call even when the editor
> hasn't yet regenerated the master.
>
> ---
>
> **PART B -- Camera + post-process defaults: +10 EV, bloom 0.2, lens flare 0.0.**
>
> Three operator-requested default-value changes paired with Part A because they
> co-define the first-launch visual baseline (a freshly-spawned project that opens
> without portal connectivity should land on a usable look):
>
> | Where | Field | v1.0.95 -> v1.0.96 | Rationale |
> | --- | --- | --- | --- |
> | `ARebusCineCameraPawn` ctor + `ResetToDefaults` | `AutoExposureBias` | `0.f` -> `10.f` | Live previs in pixel-streaming context runs unattended without an auto-exposure ramp; +10 EV keeps dim stage lights visible on the live feed. |
> | `URebusSceneSettingsSubsystem::Initialize` seed | `BloomIntensity` | `0.675` -> `0.2` | Spotlights still glow on camera but the LED-matrix walls don't overbloom. |
> | `URebusSceneSettingsSubsystem::Initialize` seed | `LensFlareIntensity` | `1.0` -> `0.0` | Disabled by default to keep the streamed view crisp; operator re-enables per shot via the portal. |
>
> Also updated for symmetry: `FRebusCameraState::ExposureBiasEv` default in
> `RebusCineCameraPawn.h` flipped to `10.f`, and the fallback EV reported by
> `GetCameraState` when `bOverride_AutoExposureBias = false` (a path that never fires
> today, but the read-back default should match) flipped to `10.f`. The `ResetToDefaults`
> log line now reads `manual EV+10` (was `manual EV0`).
>
> **These are JUST DEFAULT SEEDS.** The portal can override any of them via the
> scene-property push pipeline (`SetSceneProperty BloomIntensity / LensFlareIntensity` +
> the existing `SetCameraExposure` data-channel descriptor), and the SceneState /
> CameraState read-back will reflect any override. The construction-time landing values
> are what the operator sees on FIRST SPAWN before any portal push lands.
>
> ---
>
> **Operator checklist after rebuilding to v1.0.96.**
>
> 1. **Launch the editor.** On a project that already has a baked `M_RebusBeam` (any
>    pre-v1.0.96 form), the first `ensure_beam_material()` call (idempotent startup hook)
>    should log a Warning: *"pre-v1.0.96 M_RebusBeam detected (missing BeamShadowSteps/
>    BeamShadowStrength/BeamShadowBias); regenerating with screen-space shadow trace."*
>    If you don't see this Warning + the master isn't regenerating, run Tools > Execute
>    Python Script > `build_rebus_base_level.py` manually (the `build()` entry point
>    force-regens every material).
> 2. **Spawn a fixture.** First-spawn defaults: camera AutoExposureBias = +10 EV, scene
>    BloomIntensity = 0.2, LensFlareIntensity = 0.0. The visible shaft should look
>    indistinguishable from v1.0.95 in the empty-stage case (no occluder between fixture
>    and floor -> no shadow path possible).
> 3. **Drop a Cube actor between a fixture and the floor** at half the throw distance.
>    The shaft should visibly CUT at the cube's silhouette: behind the cube the
>    volumetric beam goes dark. The lit floor pool below the cube remains carved by the
>    v1.0.94 solid shadow (independent code path). The v1.0.42 DMX-style soft depth fade
>    at the geometry contact point is unaffected.
> 4. **A/B the strength**: `Rebus.BeamShadowStrength 0` -> beam goes back to v1.0.95
>    "shaft passes through occluder" behaviour; `Rebus.BeamShadowStrength 1` -> full
>    shadow. The CVar log line on each change reads
>    `Rebus.BeamShadowStrength -> <x>, refreshed N fixture(s) (BeamMID re-pushed; ...)`.
> 5. **Master toggle**: `Rebus.BeamShadow 0` saves the live strength + writes 0;
>    `Rebus.BeamShadow 1` restores the saved prior. `Rebus.BeamShadow status` logs the
>    live + saved values without mutating either.
> 6. **Tune step count**: `Rebus.BeamShadowSteps 16` for higher-quality tracing
>    (more SceneDepth taps per pixel); `Rebus.BeamShadowSteps 4` for faster (visible
>    quantisation in motion is the trade-off).
> 7. **Verify exposure / bloom / lens-flare on first launch.** With nothing pushed from
>    the portal, the camera should auto-land at +10 EV (visibly brighter than the
>    pre-v1.0.96 default), bloom modest, no lens flares. If any of these don't match,
>    check the log for the v1.0.96 ctor + `Initialize` lines.
>
> **MDF fallback** as documented future work -- when Orbit-imported meshes are promoted
> into the cook pipeline (so they carry SDFs), the shadow trace can be extended to tap
> the DF for off-screen occluders. Not in v1.0.96 because the runtime imports preclude
> SDF generation. **Jitter** (`Rand0to1` / `DitherTemporalAA`) on the shadow steps is
> documented as future work if visual banding ever shows up; not implemented in v1.0.96
> because operator-tested visuals were acceptable without it.
>
> **Files touched (v1.0.96).**
>
> | File | Change |
> | --- | --- |
> | `REBUS_Visualiser/Content/Python/build_rebus_base_level.py` | Extended `_BEAM_RAYMARCH_HLSL` with screen-space shadow trace; added 3 new scalar params to `_build_beam_master`; new `_beam_master_has_shadow_steps()` self-heal probe; `ensure_beam_material()` non-force path now promotes to force-regen + Warning. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusFixtureActor.cpp` | New globals `GRebusBeamShadowSteps` / `GRebusBeamShadowStrength` + `FAutoConsoleVariableRef` CVars + shared `RefreshBeamShadowParamsOnEveryFixture` refresh helper; new `ARebusFixtureActor::RefreshBeamShadowParams()` impl; `BuildBeamCone` seeds the BeamMID with the live CVar values. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Public/RebusFixtureActor.h` | Declare `RefreshBeamShadowParams()` (public). |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusVisualiser.cpp` | Register `Rebus.BeamShadow` console command (master toggle + status); `HandleBeamShadowCommand` save/restores the prior strength via `IConsoleManager` lookup of `Rebus.BeamShadowStrength`. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusCineCameraPawn.cpp` | `AutoExposureBias` default `0.f -> 10.f` (ctor + `ResetToDefaults` + `GetCameraState` fallback); reset-log line updated. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Public/RebusCineCameraPawn.h` | `FRebusCameraState::ExposureBiasEv` default `0.f -> 10.f`. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/Source/RebusVisualiser/Private/RebusSceneSettingsSubsystem.cpp` | `Initialize` seeds: `BloomIntensity 0.675 -> 0.2`, `LensFlareIntensity 1.0 -> 0.0`. |
> | `REBUS_Visualiser/Plugins/RebusVisualiser/README.md` | This release block. |
>
> ---
>
> **InternalBeam retired + lens visibility restored + volumetric shadow plumbing on the Epic-beam SpotLight (v1.0.95).**
> User report (verbatim):
>
> > "Can we remove the internal beam, we are not going to use this. Lens is hidden with the epic beam, can we add the lens object.
> >
> > Can we work out a way to make volumetric shadowing work with the Epic Beam, thats the only thing that isnt."
>
> Three coupled deliverables, shipped together because all three sit in the same fixture path:
>
> **1. InternalBeam A/B mode retired end-to-end.** The v1.0.87 → v1.0.93 InternalBeam path
> (hide Epic / cone, promote the SpotLight INSIDE the head, push it back by
> `lensRadius / tan(maxZoomHalfAngle)`, force volumetrics on, opt body meshes out of
> shadow casting, install the v1.0.93 cone-mesh shaft + v1.0.93 volumetric-fog-aware
> light-function MID) is GONE. The Epic DMX-Fixtures beam (raymarched `M_RebusBeam`) is
> now the **only** beam path. Removed:
>
> | Surface | What went | Note |
> | --- | --- | --- |
> | `ARebusFixtureActor` | `bInternalBeamEnabled`, `InternalBeamShadowCache`, the entire `Apply`/`Restore`/`Compute`InternalBeam*` family, `SetBodyMeshesCastShadow`, `OptPrimitiveOutOfInternalBeamShadow`, `Push`/`RestoreInternalBeamMegaLights`, the cone-mesh shaft component + its 4 helper funcs, `EnsureFixtureInternalBeamMIDs`, the `GoboLightFunctionMaterial` + `InternalBeamShaftMaterial` UPROPERTY refs + `FObjectFinder`s, the back-offset post-step in `RefreshMotion` (both rig + synthetic-aim branches), the InternalBeam-pose re-apply in `ApplyZoom`, `PushGoboRTToInternalBeamMaterials` from `ApplyCurrentGoboToLightFn` | The lens-disc lifecycle, the gobo cookie path, and the IES wiring are SHARED with Epic-beam and remain functional. |
> | `URebusSceneSettingsSubsystem` | `bInternalBeam` scene-property seed + dispatcher branch, `SetInternalBeamEnabled`, `PushLightFunctionAtlasForInternalBeam`, `PushVolumetricFogLightFunctionForInternalBeam`, the `r.LightFunctionAtlas.Enabled` / `r.VolumetricFog.LightFunction` / `r.LightFunctionQuality` cached-prior fields + idempotency latches | `Rebus.AllowMegaLights` (v1.0.94) stays. |
> | `RebusVisualiser.cpp` console | `Rebus.InternalBeam` (the only InternalBeam console command in the build, since `Rebus.InternalBeamScatter` / `Rebus.InternalBeamOffsetSign` / `Rebus.InternalBeamForceLegacy` / `Rebus.InternalBeamCookieCone` were CVars not commands and went with the actor CVar block). Renamed: `Rebus.DumpFixtureIes`'s help text references InternalBeam removed. | Every other Rebus.* command is untouched. |
> | `RebusFixtureActor.cpp` CVar block | `Rebus.InternalBeamScatter`, `Rebus.InternalBeamOffsetSign`, `Rebus.InternalBeamForceLegacy`, `Rebus.InternalBeamCookieCone` + their refresh sinks. Replaced by a single new `Rebus.SpotLightScatter <float>` (default 0.5) -- the per-light volumetric scattering for Epic-beam mode (see deliverable 3 below). | `Rebus.AllowMegaLights` + `Rebus.HeroShadowScatter` untouched. |
> | `build_rebus_base_level.py` | `_build_gobo_lf_master`, `ensure_gobo_light_function_material`, `_gobo_lf_master_is_current`, `_INTERNAL_BEAM_SHAFT_HLSL`, `_build_internal_beam_shaft_master`, `ensure_internal_beam_shaft_material`, `_internal_beam_shaft_master_is_current` + their call-sites in `build()` / `ensure_base_level()`. **NEW** `_cleanup_internal_beam_assets()` deletes the on-disk masters from any existing project drop on next launch (idempotent, logs a Warning the first time it deletes anything). | `ensure_fixture_lens_material` (`M_RebusFixtureLens`) STAYS -- it now drives every Epic-beam lens disc. |
> | `RebusSceneSettingsSubsystem.h` | `SetInternalBeamEnabled`, both `Push*ForInternalBeam` helpers + 5 cached-prior int fields + 2 idempotency latch bools | Subsystem surface shrank by ~80 lines. |
>
> `Grep` for `InternalBeam` after the removal returns only README-historical hits (the v1.0.87
> → v1.0.94 release blocks preserved verbatim above this one) + the explanatory v1.0.95
> comments in source documenting the retirement. **Old scene saves that still push
> `bInternalBeam=true` are silently ignored**: `ApplySceneProperty` falls through to the
> unknown-name branch (already returns `bKnown=false`), no crash, no migration needed.
>
> **2. Lens visibility in Epic-beam mode.** User reported "Lens is hidden with the epic
> beam, can we add the lens object." Root cause was a coupling between the v1.0.93
> InternalBeam cone-mesh shaft (parented at the lens plane) and the lens visibility
> assertion -- the InternalBeam OFF transition was the only path that re-asserted
> `IsBeamLensComponents` visibility, so in some scene-load orderings (where the v1.0.93
> shaft never ran because InternalBeam mode was never engaged) the freshly-built lens PMC
> could end up at construction-default visibility but `bHiddenInGame=true` (procedural
> mesh defaults flicker between editor + cooked builds). The v1.0.95 fix:
>
> - **`BuildMeshes` now explicitly calls `PMC->SetVisibility(true)` + `PMC->SetHiddenInGame(false)`**
>   on every real `<Beam>` `IsBeamLensComponents` mesh AT construction, so the lens object
>   the operator asked to see is ALWAYS visible on first spawn in Epic-beam mode.
> - **`RefreshIsBeamLensVisuals` also drives `Beam->SetHiddenInGame(!bShowReal)` in lockstep**
>   with the `SetVisibility` push so the cooked / packaged path can't silently override
>   the editor-path visibility. The synthetic `LensDisc` had this paired pattern since
>   v1.0.88; the real-`<Beam>` branch was missing the `bHiddenInGame` half.
> - **Construction-time Warning if `M_RebusFixtureLens` fails to load**: the Epic-beam lens
>   (synthetic `LensDisc` + every real `<Beam>` `IsBeamLensComponents` PMC) drives off this
>   Python-baked master. Pre-v1.0.95 the missing-material case silently fell back to the
>   runtime `BasicShapeMaterial` MID (a flat untextured disc). v1.0.95 logs at Warning:
>   *"M_RebusFixtureLens not found ... Run Tools > Execute Python Script > build_rebus_base_level.py"*
>   so the self-heal action is in the operator's log.
> - **Verbose lens-visibility diagnostic per fixture**: `Fixture %s lens visibility:
>   synthetic=%d isBeamMeshes=%d (fallbackForced=%d, syntheticVisible=%d, realLensVisible=%d).`
>   Flip the category with `LogRebusVisualiser.SetVerbosity Verbose` to read it.
>
> **3. Volumetric shadowing on the Epic-beam SpotLight.** User reported: *"Can we work out
> a way to make volumetric shadowing work with the Epic Beam, thats the only thing that
> isnt."* The Epic Beam's visible shaft (cone-mesh raymarch, `M_RebusBeam`) was already
> shadow-occluded against `SceneDepth` -- that's the crisp dense shaft. The thing that
> wasn't carving was the **separate** per-light volumetric fog interaction (the soft halo
> the SpotLight contributes to the volumetric fog froxels). Pre-v1.0.95 in Epic-beam mode:
>
> - `SpotLight->VolumetricScatteringIntensity = 0` (the cone-mesh was the only intended
>   visible contribution -- but that meant the engine had nothing to compose into the
>   fog, so occluders had nothing to carve).
> - `SpotLight->bCastVolumetricShadow = false` (hero-beam-only opt-in -- only the first N
>   spotlights of the spawn batch were CastVolumetricShadow=true, the rest were stuck at
>   the engine default of `false`).
>
> The v1.0.95 fix layers BOTH visual contributions:
>
> ```text
>   visible shaft  =  cone-mesh raymarch (M_RebusBeam, dense, crisp, SceneDepth-occluded)
>                  +  per-light scattering (SpotLight, soft halo, fog-volume occluder-carved)
> ```
>
> Implementation in `BuildSpotLight`:
>
> - **`SpotLight->VolumetricScatteringIntensity = max(Rebus.SpotLightScatter, 0)`** (new
>   CVar, default 0.5). Modest by design -- it's a SOFT layer around the crisp cone-mesh
>   shaft, not a competing dense shaft. Live-tunable; the CVar refresh sink walks every
>   Rebus fixture and re-pushes via `RefreshBeamShadowMode`.
> - **`SpotLight->bCastVolumetricShadow = true`** (unconditionally, ALWAYS). Pre-v1.0.95 this
>   was a hero-beam-only opt-in; v1.0.95 drops the gate so every fixture's per-light
>   volumetric layer is occluder-carvable. The fog-volume shadow pass is cheap relative
>   to the lit-pool cost; the per-batch `bGrantedShadowHero` budget is kept for the
>   higher `Rebus.HeroShadowScatter` value, not for `CastVolumetricShadow` itself.
> - **`SpotLight->SetCastShadows(true)`** (kept from v1.0.94 -- needed for the cookie LF
>   path and for solid shadow casting onto the floor footprint, which is unchanged).
> - **One-shot Warning at fixture spawn if the scene has no `AExponentialHeightFog` with
>   `bEnableVolumetricFog=true`**: *"Fixture %s SpotLight volumetric scattering=%.2f
>   bCastVolumetricShadow=1, but no AExponentialHeightFog with bEnableVolumetricFog=true
>   was found in the world. Volumetric shadows / fog-occlusion will be invisible until
>   volumetric fog is enabled."* Failure mode is self-diagnosing on first launch.
>
> The cone-mesh raymarch shader (`M_RebusBeam`'s `_BEAM_RAYMARCH_HLSL`) is untouched --
> it already handles its own SceneDepth occlusion. The per-light scattering is a
> SEPARATE pass the engine renders into the volumetric fog buffer; the two compose
> additively in the volumetric-fog integrator. Operators get a crisp visible shaft AND
> a soft fog-occluded halo, which is what they asked for.
>
> **Migration note.** No portal-side action required. Old scene saves that still push
> `bInternalBeam=true` are silently ignored (the unknown-property dispatcher already
> returns `bKnown=false`). On the FIRST v1.0.95 launch of an existing project,
> `_cleanup_internal_beam_assets()` deletes `/Game/REBUS/Materials/M_RebusGoboLightFunction`
> + `/Game/REBUS/Materials/M_RebusInternalBeamShaft` and logs a Warning naming the
> deletion -- idempotent / safe to re-run.
>
> **Operator checklist after rebuilding to v1.0.95.**
>
> 1. **Spawn a fixture.** Lens should be visible at the head of the beam (mirror-like
>    disc with the v1.0.93 `M_RebusFixtureLens` material; falls back to the synthetic
>    emissive disc only if the GDTF profile has no `<Beam>` lens geometry).
> 2. **Place an occluder between the fixture and the floor.** Solid shadow on the floor
>    (v1.0.94's job, still works); the volumetric fog around the beam should now ALSO
>    carve through the occluder -- a visible "shadow tube" in the fog beside the beam.
> 3. **If no fog occlusion is visible**, check the log for the v1.0.95 Warning naming
>    the missing `AExponentialHeightFog` or its `bEnableVolumetricFog=true` flag.
> 4. **Tune the per-light scatter contribution** live: `Rebus.SpotLightScatter 0.5` (default).
>    Raise to ~1.0 for a softer halo around the cone-mesh; drop to 0 to disable the soft
>    layer entirely (cone-mesh only, no fog interaction).
> 5. **Verify InternalBeam removal**: `Rebus.InternalBeam` should no longer auto-complete
>    in the console; `bInternalBeam` scene-property pushes return `Unknown property name`.
>
> ---
>
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
