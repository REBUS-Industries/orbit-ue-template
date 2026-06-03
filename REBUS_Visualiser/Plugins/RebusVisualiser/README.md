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

// Inline base64 gobo wheel images (REST-free; no imageUrl fetch), keyed (libraryId, wheel, slot).
// Field aliases accepted: dataBase64|data, mime|contentType, imageUrl|url, wheelKind|kind|type.
{ "type": "RegisterFixtureGobos",
  "libraryId": "<libraryFixtureId>",
  "gobos": [
    { "wheel": "<wheel id/name>",          // groups slots into a wheel
      "wheelKind": "gobo",                 // wheelKind|kind|type; "gobo" wheel is the selectable one
      "slot": 0,                           // 0-based slot index; correlates to SetFixtureGobo.goboIndex
      "name": "...",
      "dataBase64": "<base64 image bytes>",// dataBase64|data; base64 of the png/jpeg
      "mime": "image/png",                 // mime|contentType (informational; decode auto-detects)
      "imageUrl": "<absolute signed GCS url>", // imageUrl|url; fallback when no inline bytes
      "part": 0,                           // optional, 0-based fragment index of THIS (wheel,slot) image
      "partCount": 1 }                     // optional, total fragments for THIS (wheel,slot)
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
    `(wheel, slot)`; when any entry has `partCount > 1`, its entries are sorted by `part` and
    their `dataBase64` is **concatenated BEFORE a single `FBase64::Decode`** (so a base64 blob
    split mid-string reassembles correctly). Otherwise the lone entry's `dataBase64` is decoded.
  - On finalize the plugin caches the decoded bytes per `(libraryId, wheel, slot)`. The
    `UTexture2D` is built lazily on selection via `FImageUtils::ImportBufferAsTexture2D` (engine
    image utils, auto-detects png/jpeg — no `ImageWrapper` dependency) and fed into the **same**
    light-function MID path the URL gobo fetch uses (texture swap only — no new render path). If
    the fixture is already spawned, the currently-selected gobo is re-applied so it appears
    without a reselect.
- **Gobo selection correlation** (`SetFixtureGobo`): the descriptor carries only `goboIndex`
  (0-based, no wheel field today). It resolves against the **first gobo-kind wheel** for the
  fixture's `libraryId`, picking the entry whose `slot == goboIndex`. A null/empty/out-of-range
  index clears the gobo. If a future `SetFixtureGobo` adds an optional `wheel` field, it is
  honored to disambiguate **multi-wheel fixtures**; until then, multi-gobo-wheel fixtures are
  ambiguous and the first gobo-kind wheel is assumed.
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
r.VolumetricFog.HistoryWeight=0.75
r.VolumetricFog.GridPixelSize=4
r.VolumetricFog.GridSizeZ=128
```

`r.MegaLights.Allow=1`, `r.MegaLights.DownsampleMode=1`, and `r.MegaLights.Volume=1` are
**baselined** (the same for every tier — the runtime tiers always re-assert `Allow`/`Volume`).

The engine **VolumetricFog froxel-grid** CVars (`r.VolumetricFog.*`) are **fixed defaults** and
are **NOT** controlled by the `RenderQuality` tiers — they stay put regardless of tier:

| CVar | Default | Engine default |
| --- | --- | --- |
| `r.VolumetricFog.HistoryWeight` | `0.75` | 0.9 |
| `r.VolumetricFog.GridPixelSize` | `4` | 16 |
| `r.VolumetricFog.GridSizeZ` | `128` | 64 |

> Note: the engine `r.VolumetricFog.*` froxel grid is **separate** from the MegaLights lighting
> volume grid (`r.MegaLights.Volume.*`). Only the latter is tier-controlled.

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

- `VolumetricScatteringIntensity = 2.5` — a beam-visible default for haze scatter.
- `bAllowMegaLights = true` — opts the light into MegaLights (5.7's per-light flag; defaults
  true, asserted so the project-level `r.MegaLights.Allow` governs the whole rig).
- **Hero-beam volumetric-shadow cap**: `SetCastVolumetricShadow(true)` for only the **first 8**
  spotlights created per spawn batch (`RebusMaxVolumetricShadowBeams`); the rest still scatter
  but skip the volumetric shadow pass. The session subsystem resets the budget
  (`ARebusFixtureActor::ResetVolumetricShadowBudget`) before every (re)spawn, so each fresh
  scene gets its own 8 hero beams. The portal can still override per fixture via
  `SetFixtureBeamVolumetrics` (`ApplyBeamVolumetrics`).

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

  "photometrics": { "luminousFlux":.., "beamAngle":.., "fieldAngle":.., "colorTemperature":.., "cri":.., "hasIesProfile":true },
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
- **Gobo projection** (`RebusFixtureActor::FetchAndAssignGobo`) decodes the wheel image to a
  `UTexture2D` and feeds a light-function MID; the actual light-function material
  (`M_RebusGobo` with a `GoboTexture` param + *Volumetric Fog Uses Light Function Atlas*) is a
  content asset to author. Without it, gobo fetch is a no-op (lights still work).
- **Mesh→axis bucketing** matches GDTF `affectedGeometryNames`; opaque MVR proxy names
  (`mvr-glb-<uuid>`) that match nothing fall to the static base. The guide's height-plane
  split (§7.6) is the more robust fallback to add if needed.
- **Volumetric beams under MegaLights** (§5.7) — beams scatter through the level's volumetric
  height fog with MegaLights + its lighting volume (`r.MegaLights.Volume=1`) enabled. See the
  `RenderQuality` tiers and the artefact caveat above if the fog volume shows noise/banding.

## Acceptance mapping (§12)

Fixtures spawn from `/scene` at placement · all `SetFixture*` keyed by node id, last-writer-
wins, optional `fadeMs` ease · selection highlight via custom-depth stencil (needs an outline
post-process material in content) · pan keeps base static (parent-first compose +
tilt-under-pan) · zoom reshapes cone + zoom-keyed IES with portal brightness authority ·
source radius → soft penumbrae · scene/quality applies, unknown names ignored · `Ready` →
`RequestSceneState` → `SceneState` read-back · streamer id from `-PixelStreamingID`.
