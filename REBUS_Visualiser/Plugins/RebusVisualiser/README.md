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
