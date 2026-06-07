// Copyright REBUS Industries.
//
// Top-level per-session orchestrator (ue-plugin-build-guide.md §2/§10):
//   * reads config (PortalUrl + x-api-key) and the PRISM -Orbit* / -PixelStreamingID launch
//     tokens,
//   * fetches GET /api/ue/scene, pre-fetches each unique fixture profile + meshes,
//   * spawns + registers one ARebusFixtureActor per fixture at its placement,
//   * binds the PS2 data channel and, once both the scene is loaded and the channel is open,
//     emits Ready + FixtureRegistered and re-applies the live selection,
//   * pushes periodic FrameStats.
//
// Model import: in a packaged PRISM build the ORBIT geometry is imported by OrbitConnector +
// glTFRuntime before the stream is interactable; this subsystem still owns the *fixture*
// lifecycle and drives the lights. Geometry proxies are built from /meshes so the plugin is
// self-sufficient even without OrbitConnector present.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "RebusSceneTypes.h"
#include "RebusFixtureActor.h" // FRebusFixtureStateSnapshot
#include "RebusVisualiserSubsystem.generated.h"

class FRebusRestClient;
class FRebusDataChannel;
class URebusFixtureControlSubsystem;
class URebusSceneSettingsSubsystem;
class ARebusFixtureActor;
class ARebusCineCameraPawn;
class FJsonObject;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UPrimitiveComponent;
class UCanvas;
class APlayerController;

UCLASS()
class REBUSVISUALISER_API URebusVisualiserSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	void ReadConfig();
	bool Tick(float DeltaSeconds);

	void BeginSceneLoad();
	void OnSceneFetched(bool bOk, const FRebusScene& Scene);
	void PrefetchProfiles();
	void OnProfileFetched(const FString& LibraryId, bool bOk, const FRebusFixtureProfile& Profile);
	void OnMeshesFetched(const FString& LibraryId, bool bOk, const FRebusMeshBundle& Meshes);
	void TrySpawnFixtures();
	void SpawnAllFixtures();

	// Destroy + unregister every fixture spawned so far (used before a re-load).
	void ClearSpawnedFixtures();

	// Portal-pushed scene/fixture definition over the data channel (alternative to the
	// /api/ue/scene REST contract when the portal can't be reached). Parses the same payload
	// shapes the REST endpoints serve, then (re)spawns fixtures and refreshes the handshake.
	void HandleSceneDefinition(const FString& Type, const TSharedPtr<FJsonObject>& Msg);

	void OnChannelReady();
	void TrySendReady();

	// Send Ready + FixtureRegistered + current SceneState + live selection to all viewers.
	// Idempotent on the portal side; safe to repeat for every (re)connecting viewer.
	void BroadcastHandshake();

	// A viewer's data track opened. If we've already completed the initial handshake, re-send it
	// so this (possibly late-joining) viewer becomes controllable instead of waiting forever.
	void OnViewerConnected();

	// Find-or-spawn the default scene environment (ExponentialHeightFog with volumetric fog +
	// an unbound PostProcessVolume) so the stream renders and is portal-controllable even if
	// the level was authored without them. No-ops when the level already provides them.
	void EnsureSceneEnvironment();

	// Place the streamed view at the configured start pose (the level has no PlayerStart, so the
	// default pawn would otherwise spawn at the origin). Returns false until the player
	// controller + pawn exist, so the caller can retry on a later tick.
	bool TryPositionPlayerView();

public:
	// Sibling-subsystem + world accessors. Public so the data channel + console commands +
	// the cine camera pawn glue can find what they need without friend-ing every caller.
	URebusFixtureControlSubsystem* GetControl() const;
	URebusSceneSettingsSubsystem* GetSceneSettings() const;
	UWorld* GetActiveWorld() const;

	// v1.0.79: live cine-camera pawn accessor for console commands + the DataChannel periodic
	// CameraState broadcast. May be null until TryPositionPlayerView spawns and possesses it.
	ARebusCineCameraPawn* GetCineCameraPawn() const { return CineCameraPawn.Get(); }

	// Apply one of the SetCamera* descriptors to the live cine pawn. Recognised types:
	//   SetCameraTransform   { "loc": [x,y,z], "rot": [pitch, yaw, roll] }
	//   SetCameraFocalLength { "mm": 35 }
	//   SetCameraAperture    { "fStop": 2.8 }
	//   SetCameraFocusDistance { "cm": 500 }    OR    { "auto": true }
	//   SetCameraExposure    { "ev": 0.0 }
	//   SetCameraSensor      { "widthMm": 24.89, "heightMm": 18.66 }
	//   RequestCameraState   { }                -- triggers a one-shot CameraState broadcast
	// Returns true if the descriptor matched (so the data channel knows to stop routing it).
	bool HandleCameraDescriptor(const FString& Type, const TSharedPtr<FJsonObject>& Msg);

	// v1.0.80: handle the live-state pull descriptors. Recognised types:
	//   RequestFixtureStates  { }   -- one-shot full broadcast of every spawned fixture
	//   RequestSelectionState { }   -- one-shot selection broadcast
	// Returns true if matched. Used by the data channel router so the portal can resync any
	// individual surface without round-tripping a full handshake.
	bool HandleStateSyncDescriptor(const FString& Type, const TSharedPtr<FJsonObject>& Msg);

	// v1.0.80: subsystems / control paths call these after they apply a mutation so the live
	// stream catches it on the next periodic tick (fixtures) or immediately (selection /
	// scene). The fixture path is intentionally batched -- a single descriptor like
	// SetFixtureDimmer triggers a long fade that only the periodic stream can report
	// frame-by-frame, so flagging here is harmless duplicate work and the dead zone rejects it.
	void NotifyFixtureControlMutated();
	void NotifySelectionChanged();
	void NotifySceneSettingsChanged();

	// v1.0.85: truss / set-piece material override. Walks every OrbitImportRoot's primitive
	// components in the active world, skips anything bound to a fixture (those keep the v1.0.71
	// fixture-body override behaviour), and replaces every remaining material slot with a
	// black-powdercoat material. Tries `/Game/REBUS/Materials/M_RebusTruss.M_RebusTruss` first;
	// if that .uasset isn't present in the project the subsystem builds a fallback MID from
	// BasicShapeMaterial with PBR params tuned for a matte powdercoat finish (#040404 base,
	// roughness ~0.55, fully dielectric). Per-component slot-aligned cache restores byte-exact
	// on disable. ON by default; the operator can flip with `Rebus.OverrideTrussMaterial 0`.
	struct FTrussMaterialApplyCount
	{
		int32 Components   = 0; // total Orbit primitive components considered this pass
		int32 Touched      = 0; // had at least one slot overridden this call
		int32 SkippedBound = 0; // skipped because the component is bound to a fixture
		int32 Restored     = 0; // had original materials restored this call
	};
	FTrussMaterialApplyCount SetTrussMaterialOverrideEnabled(bool bEnabled);
	bool IsTrussMaterialOverrideEnabled() const { return bTrussMaterialOverrideEnabled; }

	// v1.0.99 -- force every Orbit-imported primitive component to cast shadows by default.
	// User reported (against v1.0.96/98): "Can we check that all imported objects cast shadows
	// as default. The light beam currently goes straight through any object." UE primitives
	// default to CastShadow=true, but glTFRuntime + the OrbitConnector import pipeline can
	// land them with CastShadow=false (perf-driven import preset) -- so the SpotLight's own
	// shadow casting catches nothing and the floor footprint stays unshadowed.
	//
	// EnsureImportedShadowsCast walks every OrbitImportRoot actor in the active world (matched
	// by class-name string for zero compile dependency on the OrbitConnector plugin, mirroring
	// ApplyTrussMaterialPass) and asserts CastShadow / bCastDynamicShadow / bCastHiddenShadow=
	// false / bCastFarShadow on every UPrimitiveComponent. Idempotent (per-comp early-out when
	// flags already match). Driven from Tick on the same 1Hz cadence as RebindOrbitModels +
	// ApplyTrussMaterialPass, so newly-imported geometry inherits the override on the next
	// second after import without an operator-side console call.
	//
	// Operator overrides:
	//   * `Rebus.OrbitCastShadows [0|1]` flips bOrbitCastShadowsEnabled. ON (default) forces
	//     CastShadow=true on every tracked + newly-encountered comp. OFF walks the tracked
	//     set and forces CastShadow=false (so the operator can A/B against the no-shadow
	//     baseline). The tick walk continues to enforce the chosen state on newly-arrived
	//     geometry either way.
	//   * `bOrbitCastShadows` scene property mirrors the same flag through the portal /
	//     SetSceneProperty wire path -- routes via SetOrbitCastShadowsEnabled below.
	struct FOrbitShadowApplyCount
	{
		int32 Components = 0; // total Orbit primitive components considered this pass
		int32 Touched    = 0; // had a flag flipped this call
	};
	FOrbitShadowApplyCount EnsureImportedShadowsCast();
	void SetOrbitCastShadowsEnabled(bool bEnabled);
	bool IsOrbitCastShadowsEnabled() const { return bOrbitCastShadowsEnabled; }

	// v1.0.104 -- force every Orbit-imported primitive component (and its per-slot MIDs)
	// to render double-sided where the engine allows it at runtime. User report (verbatim
	// from the v1.0.104 brief): "Orbit-imported materials (glTFRuntime-baked + the
	// OrbitConnector plugin's own pipeline) are still single-sided in many cases, so thin
	// geometry (truss cross-bars, banner cloth, sheet-metal flags) disappears when viewed
	// from the back." Mirrors the v1.0.99 EnsureImportedShadowsCast shape byte-for-byte:
	// runs on the same 1Hz Tick cadence as RebindOrbitModels + ApplyTrussMaterialPass +
	// EnsureImportedShadowsCast so newly-imported geometry inherits the override on the
	// next second after import without an operator console call. Per-comp early-out keeps
	// the steady-state walk cheap (Touched=0 when nothing changed).
	//
	// What the walker does, per component:
	//   1. Force `bCastShadowAsTwoSided = true` on the UPrimitiveComponent. This is the
	//      cheap, always-effective shadow-side fix: a single-sided opaque material that
	//      casts a one-sided shadow flips its winding under a light catching the back
	//      face and projects a missing / inverted silhouette on the floor pool -- v1.0.99
	//      `bCastShadow = true` made the issue visible because thin Orbit geometry
	//      started casting shadows at all (it was off pre-v1.0.99). Two-sided shadow
	//      casting walks both sides of the triangle so the silhouette is geometrically
	//      correct from any light angle, regardless of whether the material itself
	//      renders two-sided.
	//   2. For every material slot whose live material is NOT already a
	//      UMaterialInstanceDynamic, wrap it in a MID parented to the existing material
	//      (preserves all textures + scalar / vector params; cheap). This lets the v1.0.97
	//      `bTwoSided` static switch on Rebus-authored masters (M_RebusGround,
	//      M_RebusFixtureLens, M_RebusOrbitImported, etc.) be flipped per-MID at runtime.
	//      v1.0.85's truss-material override + v1.0.71's fixture-material override
	//      already produce MIDs in their pipelines, so this is mostly a no-op on already-
	//      processed components.
	//   3. Push a `bTwoSided` static switch parameter onto every MID. Silently no-ops
	//      when the parent master doesn't expose the param (glTFRuntime masters don't --
	//      see the v1.0.104 README block for why we can't force-flip those at runtime).
	//
	// `bCastShadowAsTwoSided = false` REGARDLESS of the toggle is NOT what v1.0.104 ships
	// (a separate operator toggle would conflict with v1.0.99's two-sided-shadow-cast
	// semantics on thin geometry). The toggle flips both the component flag AND the MID
	// switch in lockstep -- single chokepoint.
	//
	// Operator overrides:
	//   * `Rebus.OrbitDoubleSided [0|1]` flips bOrbitDoubleSidedEnabled. ON (default)
	//     forces the two-sided state on every tracked + newly-encountered comp; OFF
	//     walks the tracked set and restores `bCastShadowAsTwoSided = false` + clears
	//     the bTwoSided switch (so the operator can A/B against the single-sided
	//     baseline). The tick walk continues to enforce the chosen state on newly-
	//     arrived geometry either way.
	//   * `bOrbitDoubleSided` scene property mirrors the same flag through the portal /
	//     SetSceneProperty wire path -- routes via SetOrbitDoubleSidedEnabled below.
	struct FOrbitDoubleSidedApplyCount
	{
		int32 Components = 0; // total Orbit primitive components considered this pass
		int32 Touched    = 0; // had a flag flipped this call (component OR any MID slot)
		int32 MIDsWrapped = 0; // material slots wrapped in a fresh MID this call
		int32 SwitchesPushed = 0; // MID slots that accepted the `bTwoSided` switch
	};
	FOrbitDoubleSidedApplyCount EnsureImportedDoubleSided();
	void SetOrbitDoubleSidedEnabled(bool bEnabled);
	bool IsOrbitDoubleSidedEnabled() const { return bOrbitDoubleSidedEnabled; }

	// v1.0.105 -- enable Nanite on every Orbit-imported UStaticMesh post-import (operator-
	// toggleable, default ON). User request (verbatim from the v1.0.105 brief): "can all
	// imported objects from orbit be converted to nanite post import to improve performance."
	//
	// Why Nanite matters here: trusses, set pieces, banners, and fixture bodies are exactly
	// the high-poly, low-material-count, opaque (or mostly-opaque) geometry that Nanite was
	// designed for. UE 5.7's Nanite cuts the per-draw-call cost on imported geometry by
	// ~5-50x depending on triangle count, plus virtualised shadow maps (VSM) make the
	// v1.0.99 force-cast-shadows pass effectively free on every Nanite mesh -- a meaningful
	// win on the truss-shadow visibility the v1.0.99/v1.0.103 work was about. Nanite-
	// incompatible passes (the v1.0.104 two-sided opaque, masked, translucent) automatically
	// route through the per-mesh fallback proxy that NaniteSettings.FallbackPercentTriangles
	// reserves -- so the v1.0.97/v1.0.104 double-sided work is preserved verbatim (those
	// pixels just don't get the Nanite per-pixel raster path; the rest of the mesh does).
	//
	// Why this is editor-only: Nanite resources are a cooked artefact -- they're built from
	// source MeshDescription via UStaticMesh::Build (editor-time) or the runtime
	// NaniteBuilder module (also editor-only in 5.7 -- no shipping-build entry point). The
	// glTFRuntime parser DOES instantiate a real UStaticMesh UObject (NewObject<UStaticMesh>
	// in glTFRuntimeParserStaticMeshes.cpp:30 -- not a UProceduralMeshComponent), and DOES
	// commit a MeshDescription source model when bGenerateStaticMeshDescription=true on the
	// import config (gated `#if WITH_EDITOR` in the same file at line 782). So in editor
	// builds (PIE / Standalone Editor / -game with editor-built binaries) we can flip
	// NaniteSettings.bEnabled and call Build() to (re)cook Nanite resources in-place.
	// Packaged builds need cooked-Nanite GLB assets (operator pre-cooks the Orbit imports
	// into UStaticMesh .uasset(s) with NaniteSettings.bEnabled=true in editor before
	// packaging) -- documented in the v1.0.105 README block.
	//
	// What the walker does, per UStaticMesh (NOT per UStaticMeshComponent -- meshes are
	// shared between components in the OrbitConnector import; a per-comp walker would
	// rebuild the same mesh N times):
	//   * Group every UStaticMeshComponent under OrbitImportRoot by its UStaticMesh.
	//   * Skip if NaniteSettings.bEnabled already matches the requested state AND we've
	//     attempted Build() on this mesh before this session (NaniteAttempted set entry).
	//   * Skip if the mesh has no SourceModels[0] / no MeshDescription -- Build() would
	//     crash on a missing source. Logs a one-shot Warning per mesh naming the operator
	//     fix (OrbitConnector's import config likely has bGenerateStaticMeshDescription=
	//     false; flipping it on rebuilds the import with editor-rebuildable meshes).
	//   * Otherwise: NaniteSettings.bEnabled = bWantOn (+ conservative defaults on first-
	//     time enable: PositionPrecision = MIN_int32 (auto bounds-derived precision),
	//     FallbackPercentTriangles = 1.0f (full-quality fallback proxy preserves the
	//     v1.0.97/v1.0.104 two-sided / masked passes), TrimRelativeError = 0.0f (no
	//     aggressive simplification)), then StaticMesh->Build(false /*bSilent*/), then
	//     MarkRenderStateDirty on every UStaticMeshComponent referencing this mesh so the
	//     freshly-cooked NaniteResources land on the GPU on the next render frame.
	//   * Per-mesh log line on success: `[Rebus] Nanite ENABLED on <Mesh> (LOD0 tris=N,
	//     fallback=100.0%)`.
	//
	// All of the above is gated behind `#if WITH_EDITOR` -- packaged builds compile cleanly
	// (UStaticMesh::Build is `#if WITH_EDITOR` in `Engine/StaticMesh.h`; NaniteSettings
	// itself is a runtime-readable USTRUCT but the cook-time path the toggle drives is
	// editor-only). In a packaged build the walker emits a one-shot Warning per session
	// ("Nanite runtime-conversion unavailable in packaged builds; pre-cook GLBs to
	// UStaticMesh assets with Nanite enabled in editor") and bails -- the toggle is still
	// settable so SceneState round-trips it, but the conversion is no-effect.
	//
	// Operator overrides:
	//   * `Rebus.NaniteOrbitImports [0|1]` flips bNaniteOrbitImportsEnabled. ON (default)
	//     enables Nanite on every tracked + newly-encountered mesh; OFF walks the tracked
	//     set and DISABLES Nanite + rebuilds. Disable path emits a single Warning
	//     "Disabling Nanite on N Orbit meshes -- this will trigger a rebuild storm
	//     (UStaticMesh::Build can take seconds per mesh, blocks the game thread)" so an
	//     operator never fires the toggle casually mid-show.
	//   * `bNaniteOrbitImports` scene property mirrors the same flag through the portal /
	//     SetSceneProperty wire path -- routes via SetNaniteOrbitImportsEnabled below so
	//     the console + portal paths can never diverge.
	//   * `Rebus.DumpOrbitNanite` diagnostic walks every Orbit StaticMesh and dumps one
	//     line per mesh: `Mesh='<n>' refs=<r> tris=<t> Nanite=ON|OFF FallbackTris=<ft>
	//     dsSlots=<a/b>` (a = slots reporting bTwoSidedScalar=1.0, b = total slots).
	//     Reports Nanite=ON on every entry once the v1.0.105 walker has run successfully
	//     -- the canonical operator verification step on the v1.0.105 ship.
	struct FOrbitNaniteApplyCount
	{
		int32 Components      = 0; // total Orbit static-mesh components considered this pass
		int32 Meshes          = 0; // unique UStaticMesh UObjects considered this pass
		int32 Touched         = 0; // had NaniteSettings.bEnabled flipped + Build() called
		int32 SkippedNoSource = 0; // mesh had no MeshDescription source -- Build() impossible
		int32 SkippedAlready  = 0; // state already matched + we'd visited it before
	};
	FOrbitNaniteApplyCount EnsureImportedNanite();
	void SetNaniteOrbitImportsEnabled(bool bEnabled);
	bool IsNaniteOrbitImportsEnabled() const { return bNaniteOrbitImportsEnabled; }

	// v1.0.107 -- always-on-top version watermark overlay. User request (verbatim):
	// "can we print the version on the top centre of the ouput with a command to be able
	// to turn off. So we see top centre of the stream v1.0.106 for example."
	//
	// Renders the cached `v<VersionName>` string (e.g. `v1.0.107`) at the top-centre of
	// every rendered viewport via `UDebugDrawService::Register("Foreground", ...)` -- the
	// "Foreground" debug-draw extension fires after the 3D world is rendered but before
	// UMG/HUD, so the watermark sits on top of the scene + below any operator-facing UMG
	// (correct stacking for a non-intrusive overlay). Pixel Streaming 2 captures the
	// FCanvas overlay verbatim into the H.264 stream frames, so the watermark appears in
	// the live stream without any PS2-side integration.
	//
	// Version source-of-truth: `IPluginManager::FindPlugin("RebusVisualiser")
	// ->GetDescriptor().VersionName` -- the engine-blessed accessor that always reports
	// the currently-running binary's plugin descriptor. Cached once at Initialize() so
	// the per-frame draw is zero JSON re-parse + zero string allocation.
	//
	// Operator overrides:
	//   * `Rebus.ShowVersion [0|1|status]` flips bShowVersionWatermark. Default ON
	//     (seeded true at Initialize()). Status prints the live flag + the cached
	//     version display string + the live Y-margin so an operator can verify in one
	//     log line that the watermark is wired and what it'll print.
	//   * `Rebus.VersionWatermarkY <px>` sets the top-edge margin in pixels (default 12).
	//     Lets operators drop the watermark below an overlapping HUD element if needed.
	//   * `bShowVersionWatermark` scene property mirrors the same flag through the
	//     portal / SetSceneProperty wire path -- routes via SetVersionWatermarkEnabled
	//     below so the console + portal paths can never diverge.
	void SetVersionWatermarkEnabled(bool bEnabled);
	bool IsVersionWatermarkEnabled() const;
	const FString& GetCachedVersionDisplay() const { return CachedVersionDisplay; }

	// Static accessors for the watermark Y-margin. Static (rather than instance) so the
	// `Rebus.VersionWatermarkY <px>` console command in RebusVisualiser.cpp can write
	// the live state without iterating GameInstance subsystems for the simple case
	// where there's only one number to push -- the Y-margin is a global pixel offset
	// with no per-world identity, so a single file-scope value is the right shape.
	static void  SetVersionWatermarkTopMarginPx(float Px);
	static float GetVersionWatermarkTopMarginPx();

	// v1.0.112 -- runtime stale-master probe + auto-purge for `M_RebusBeam`.
	//
	// User report against post-v1.0.111 (verbatim, the v1.0.112 brief): "we are
	// still seeing the origional attempt to shadow the cone/beam which cuts off
	// the sides. remove this." Translation: the screen-space pan-edge side-
	// cutting artefact that v1.0.110 ripped out + v1.0.111 replaced with the
	// light-space depth-mask IS STILL VISIBLE on the user's deployment. The
	// post-v1.0.111 `_BEAM_RAYMARCH_HLSL` source in `build_rebus_base_level.py`
	// has zero references to the obsolete `BeamShadow*` scalars, zero
	// `TranslatedWorldToClip` view-space projections, zero `SceneTextureSample
	// (SceneDepth, ...)` reads inside the raymarch loop, and the only
	// `abs(ndc.xy) < 1.0` guard is the LIGHT-space frustum bounds check using
	// the v1.0.111 `BeamLightFwd / Right / Up` axes -- not the v1.0.96..v1.0.109
	// screen-space NDC guard. The C++ side also has zero residual references to
	// any obsolete scalar. So this is unambiguously NOT residual source code
	// (Cause B was investigated and ruled out): the user's on-disk
	// `M_RebusBeam.uasset` is the OLD pre-v1.0.110 cooked master, with the
	// pre-v1.0.110 HLSL still embedded. The v1.0.111 Python self-heal
	// (`_beam_master_has_shadow_mask` probe in `ensure_beam_material`) only
	// fires when an operator manually runs `Tools > Execute Python Script >
	// build_rebus_base_level` -- because `build_rebus_base_level.py` is NOT
	// in `[Python] +StartupScripts` (verified: `REBUS_Visualiser/Config/
	// DefaultEngine.ini` has no such entry, and no other auto-import path).
	// On the user's deployment that operator step has not been reliably
	// performed across upgrades -- the same failure mode that bit us in
	// v1.0.103 (the v1.0.99 fix didn't take effect because the cooked master
	// stayed stale). We have burned at least three release cycles on this
	// (v1.0.99/v1.0.103/v1.0.110/v1.0.111). v1.0.112 makes it automatic.
	//
	// `ProbeAndAutoPurgeStaleBeamMaster` (called once from `Initialize` --
	// gated by `bBeamMasterAutoPurgeRun` so a re-entry never re-fires) does:
	//   1. `LoadObject<UMaterial>("/Game/REBUS/Materials/M_RebusBeam")` and
	//      probes for the presence of ANY of the seven v1.0.96..v1.0.109
	//      obsolete `BeamShadow*` scalars (`BeamShadowSteps / Strength / Bias
	//      / Debug / FarCullCm / EdgeGuard / BiasScale`) -- if ANY exists the
	//      master is pre-v1.0.110 (still carries the screen-space self-shadow
	//      param contract that v1.0.110 deleted).
	//   2. ALSO probes for the ABSENCE of the v1.0.111 light-space depth-mask
	//      texture param (`BeamShadowMaskRT`) AND the v1.0.111 marker scalar
	//      (`BeamShadowMaskBiasCm`) -- if EITHER is missing the master pre-
	//      dates v1.0.111 (was authored against v1.0.110's "no shadow at all"
	//      state).
	//   3. If EITHER staleness condition fires, in an editor build with
	//      `PythonScriptPlugin` loaded: invokes `py import build_rebus_base_
	//      level; build_rebus_base_level.ensure_beam_material(force=True)` via
	//      `GEngine->Exec(World, ..., *GLog)`, which deletes + regenerates
	//      `/Game/REBUS/Materials/M_RebusBeam.uasset` with the CURRENT
	//      `_BEAM_RAYMARCH_HLSL` source. Mirrors `Rebus.RebuildBeamMaterial`'s
	//      one-shot Python escape hatch byte-for-byte (same `WITH_EDITOR` +
	//      `FModuleManager::IsModuleLoaded` defence, same `py` exec entry).
	//      After the regen the helper logs the operator next-step ("ClearScene
	//      + LoadScene from the portal to rebuild per-fixture BeamMIDs off the
	//      regenerated master").
	//   4. Walks every already-spawned `ARebusFixtureActor` (typically empty
	//      at `Initialize` time -- subsystem init runs before any fixture
	//      spawn -- but a defensive belt-and-braces walk in case the probe
	//      ever re-fires on a per-tick path) and calls `BeamMID->Clear
	//      ParameterValues` + `RefreshBeamRadialParams` + `RefreshBeamSpatial
	//      Params` + `RefreshBeamShadowMaskParams` so the existing per-fixture
	//      MID is re-seeded against the freshly-regenerated master's
	//      parameter contract. Newly-spawned fixtures `LoadObject` the
	//      regenerated master cleanly via `BuildBeamCone` so they pick up the
	//      new HLSL without any of this dance.
	//   5. In a packaged build (no PythonScriptPlugin -- `WITH_EDITOR` is 0):
	//      logs a hard Warning naming the detected obsolete scalar AND the
	//      operator workflow ("Packaged build is shipping a pre-v1.0.110
	//      cooked master. Re-cook the project with a v1.0.110+ workspace to
	//      embed the new HLSL"). Doesn't attempt a no-op regen.
	//
	// The probe is paired with the public `Rebus.DumpBeamMasterVersion`
	// diagnostic (`ProbeBeamMasterVersion` below) so an operator can verify
	// the auto-purge ran. After v1.0.112 a fresh editor launch on a
	// pre-v1.0.110 cooked master logs `[Rebus] STALE BEAM MASTER detected
	// ... auto-running Rebus.RebuildBeamMaterial` exactly once, followed by
	// the Python regen's success line, followed by the operator next-step
	// nudge. A subsequent `Rebus.DumpBeamMasterVersion` reports `v1.0.111+`.
	enum class EBeamMasterVersion : uint8
	{
		// The asset doesn't exist on disk. First-launch case (no Python ever
		// ran). The runtime fallback in `BuildBeamCone` then logs a Warning
		// and the fixture's beam is skipped.
		Missing,
		// No obsolete `BeamShadow*` scalars AND no v1.0.111 `BeamShadowMask*`
		// params either -- the master pre-dates BOTH v1.0.96 and v1.0.111
		// (i.e. a v1.0.95-or-earlier cone-only master, never re-cooked).
		PreV96,
		// HAS at least one of the seven v1.0.96..v1.0.109 obsolete
		// `BeamShadow*` scalars. Stale. The screen-space self-shadow trace
		// HLSL is still cooked into this master and is what produces the
		// pan-edge side-cutting artefact.
		V96ThroughV109,
		// No obsolete params AND no v1.0.111 params either -- the master
		// represents the v1.0.110 "clean slate" state (rollback shipped, new
		// system not yet authored). Stale wrt v1.0.111+ but not visibly
		// broken -- the shaft just doesn't shadow.
		V110,
		// No obsolete params AND has the full v1.0.111 parameter contract
		// (texture `BeamShadowMaskRT` + the six `BeamShadowMask*` scalars +
		// the three `BeamLight*` vectors). Current.
		V111Plus,
	};

	struct FBeamMasterVersionReport
	{
		EBeamMasterVersion Version = EBeamMasterVersion::Missing;
		// Names of any of the seven v1.0.96..v1.0.109 `BeamShadow*` scalars
		// that the loaded master still declares -- present => stale.
		TArray<FString> DetectedObsoleteParams;
		// Names of any v1.0.111 required parameter that the loaded master is
		// missing -- present => stale relative to v1.0.111+.
		TArray<FString> MissingV111Scalars;
		TArray<FString> MissingV111Vectors;
		TArray<FString> MissingV111Textures;
	};

	// Diagnostic entry point for the `Rebus.DumpBeamMasterVersion` console
	// command. Mirrors `DumpOrbitNanite`'s shape: const, no side effects,
	// returns the structured report so the console command can format the
	// log line. Used to verify the v1.0.112 auto-purge actually ran.
	FBeamMasterVersionReport ProbeBeamMasterVersion() const;

	// Map the enum to a human-readable label for the operator-facing diagnostic
	// log line. Matches the labels the v1.0.112 README release block uses.
	static const TCHAR* BeamMasterVersionLabel(EBeamMasterVersion V);

	// v1.0.113 -- compute the lowercase-hex MD5 hash of the on-disk
	// `/Game/REBUS/Materials/M_RebusBeam.uasset` so the operator can prove from log
	// output alone whether their packaged / cooked binary is running the master we
	// expect. Resolves the long package name to a filesystem path via
	// `FPackageName::TryConvertLongPackageNameToFilename` (matches the editor's
	// `.uasset` extension); silently returns a self-describing sentinel string on
	// resolve / IO failure (e.g. asset not on disk -- the same case the v1.0.112
	// `EBeamMasterVersion::Missing` branch covers). Used by both the `Initialize`-
	// time startup banner AND the `Rebus.DumpBeamMasterVersion` console command,
	// so the two paths never disagree about what "current hash" means.
	//
	// v1.0.116 -- moved from `private:` to `public:` because
	// `RebusVisualiser.cpp::HandleDumpBeamMasterVersionCommand` calls it from
	// outside the class (`URebusVisualiserSubsystem::ComputeBeamMasterUassetMd5()`
	// at file-scope), which triggered C2248 on UE 5.7 builds. The function is a
	// pure-read static helper (file IO + MD5 of bytes; no instance state, no side
	// effects) -- safe to expose verbatim. See README v1.0.116 release block for
	// the cross-TU visibility audit that motivated the move.
	static FString ComputeBeamMasterUassetMd5();

	// Per-mesh dump entry surfaced by `Rebus.DumpOrbitNanite`. Grouped by UStaticMesh so
	// each unique imported asset reports ONCE with a count of components referencing it
	// (the OrbitConnector import shares trusses / set-piece geometry between many
	// component instances; per-comp dump would be O(comps) noisy).
	struct FOrbitNaniteDumpEntry
	{
		FString MeshName;
		int32 Faces           = 0; // sum of LOD0 triangles
		int32 FallbackTris    = 0; // expected fallback proxy triangle count (Faces * FallbackPercent)
		bool  bNaniteEnabled  = false;
		int32 ComponentRefs   = 0;       // how many StaticMeshComponents reference this mesh
		int32 SlotsTwoSided   = 0;       // material slots reporting bTwoSidedScalar >= 0.5
		int32 SlotsTotal      = 0;       // total material slots on the first-seen component
	};
	TArray<FOrbitNaniteDumpEntry> DumpOrbitNanite() const;

private:
	// Config / launch tokens.
	FString PortalUrl;
	FString ApiKey;
	FString StreamerId;     // -PixelStreamingID
	FString ProjectId;      // -OrbitProject (portal doc id)
	FString ModelId;        // -OrbitModel
	FString VersionId;      // -OrbitVersion
	FString OrbitServer;
	FString OrbitTarget;

	TSharedPtr<FRebusRestClient> Rest;
	TSharedPtr<FRebusDataChannel> Channel;

	FRebusScene SceneData;

	// Per-library caches (multiple instances share one libraryFixtureId).
	TMap<FString, FRebusFixtureProfile> ProfileCache;
	TMap<FString, FRebusMeshBundle> MeshCache;
	TSet<FString> PendingProfiles;
	TSet<FString> PendingMeshes;

	// Chunked RegisterFixtureMeshes accumulation: the portal can split one fixture's meshes
	// across multiple data-channel messages (each capped ~60k chars). We append each chunk's
	// meshes per libraryId until chunkCount messages arrive, then commit the merged bundle into
	// MeshCache. PendingMeshChunksSeen counts received messages (not indices) so out-of-order or
	// duplicate chunkIndex values are tolerated.
	TMap<FString, FRebusMeshBundle> PendingMeshChunks;
	TMap<FString, int32> PendingMeshChunksSeen;

	// Inline IES (RegisterFixtureIes): raw IESNA LM-63 file *text* pushed over the data channel,
	// finalized per libraryId into a list of (profileId, zoomDmx) -> reassembled .ies bytes. The
	// fixture actor builds the UTextureLightProfile lazily and prefers these over a URL fetch.
	TMap<FString, FRebusInlineIes> InlineIesCache;

	// Two-level accumulation scratch for RegisterFixtureIes (mirrors the mesh-chunk members):
	//   * PendingIesProfiles   -- raw profiles[] entries appended per libraryId across messages,
	//   * PendingIesChunksSeen -- message count per libraryId (finalize once chunkCount arrive).
	// On finalize we group entries by profileId and concatenate part fragments (part/partCount)
	// to rebuild each profile's full .ies text before building the light profile.
	TMap<FString, TArray<FRebusInlineIesPending>> PendingIesProfiles;
	TMap<FString, int32> PendingIesChunksSeen;

	// Inline gobos (RegisterFixtureGobos): base64 wheel images pushed over the data channel,
	// finalized per libraryId into a list of (wheel, slot) -> decoded image bytes. The fixture
	// actor builds the UTexture2D lazily on selection and prefers these over a URL fetch.
	TMap<FString, FRebusInlineGobos> InlineGoboCache;

	// Two-level accumulation scratch for RegisterFixtureGobos (mirrors the IES/mesh members):
	//   * PendingGoboEntries    -- raw gobos[] entries appended per libraryId across messages,
	//   * PendingGoboChunksSeen -- message count per libraryId (finalize once chunkCount arrive).
	// On finalize we group entries by (wheel, slot) and concatenate part fragments (part/
	// partCount) of dataBase64 before a single decode.
	TMap<FString, TArray<FRebusInlineGoboPending>> PendingGoboEntries;
	TMap<FString, int32> PendingGoboChunksSeen;

	UPROPERTY() TArray<TObjectPtr<ARebusFixtureActor>> SpawnedFixtures;

	// v1.0.79: live cine-camera pawn (spawned + possessed in TryPositionPlayerView). The
	// PlayerController auto-uses the pawn's CineCameraComponent as the view target because
	// bFindCameraComponentWhenViewTarget defaults to true.
	UPROPERTY() TObjectPtr<ARebusCineCameraPawn> CineCameraPawn = nullptr;

	// Periodic CameraState outbound throttle (~30Hz) + dead-zone tracking. We only broadcast
	// when the camera moved beyond a small threshold OR a cine setting changed, so a
	// stationary camera doesn't flood the data channel with 30 identical messages/sec.
	float CameraStateTimer = 0.f;
	struct FCameraStateSnapshot
	{
		FVector  Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		float    FocalMm = -1.f;
		float    Aperture = -1.f;
		float    FocusCm = -1.f;
		float    ExposureEv = -1.f;
		bool     bManualFocus = true;
	};
	FCameraStateSnapshot LastSentCameraState;

	void BroadcastCameraStateIfChanged(bool bForce);

	// v1.0.80 fixture-state live stream. Per-fixture last-sent snapshot cache; the periodic
	// tick diffs against this and only sends fixtures whose values moved beyond a dead zone
	// (zero traffic from a static rig). bForce sends every spawned fixture with full=true so
	// a reconnecting portal paints the live state instead of waiting for the first change.
	float FixtureStreamTimer = 0.f;
	TMap<FString, FRebusFixtureStateSnapshot> LastSentFixtureStates;
	void BroadcastFixtureStatesIfChanged(bool bForce);

	FTSTicker::FDelegateHandle TickHandle;

	bool bEnvEnsured = false;
	bool bViewPositioned = false;
	bool bSceneRequested = false;
	bool bSceneLoaded = false;
	bool bFixturesSpawned = false;
	bool bChannelReady = false;
	bool bReadySent = false;

	float FrameStatsTimer = 0.f;

	// Throttle for the periodic Orbit-model rebind (1 Hz). While driving is enabled this re-scans
	// for a late/re-import and binds it to existing fixtures; cheap no-op otherwise. (Fresh fixture
	// registrations trigger their own immediate rebind in URebusFixtureControlSubsystem::Register
	// Fixture, so this timer mostly catches the import-arrived-after-fixtures-spawned case.)
	float OrbitRebindTimer = 0.f;

	// v1.0.85 truss-material override state. Lazy-loaded /Game/REBUS/Materials/M_RebusTruss.M_
	// RebusTruss with fallback to a runtime MID. Per-component slot-aligned snapshot so disable
	// restores the original Orbit-import materials byte-exact. ApplyTrussMaterialPass runs once
	// per OrbitRebindTimer firing (1 Hz) so newly-bound components inherit the override the next
	// second after Orbit binds them; cheap when nothing changed (TouchedThisPass==0).
	bool bTrussMaterialOverrideEnabled = true;
	UPROPERTY() TObjectPtr<UMaterialInterface> TrussMaterialOverride = nullptr;     // /Game asset if present
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> TrussMaterialMID = nullptr;    // runtime fallback (lazy)
	UPROPERTY() TObjectPtr<UMaterialInterface> TrussMatParent = nullptr;            // BasicShapeMaterial for the MID
	struct FTrussMaterialEntry
	{
		TWeakObjectPtr<UPrimitiveComponent> Comp;
		// One entry per material slot, captured the FIRST time we override the component.
		// Weak so a destroyed material asset doesn't keep the entry alive past the comp dying.
		TArray<TWeakObjectPtr<UMaterialInterface>> OriginalMaterials;
	};
	TArray<FTrussMaterialEntry> TrussMaterialCache;

	// Lazy material setup -- runs first time SetTrussMaterialOverrideEnabled(true) needs a
	// material. Sets TrussMaterialOverride from /Game (preferred) or builds the runtime MID
	// from BasicShapeMaterial with powdercoat PBR params.
	void EnsureTrussMaterial();
	// Resolve the active truss material (the /Game override wins over the MID fallback).
	UMaterialInterface* ResolveTrussMaterial();
	// Idempotent re-apply pass; called from Tick after RebindOrbitModels so freshly-imported
	// Orbit geometry inherits the override on the next 1 Hz cycle. Returns the same counts
	// the public SetTrussMaterialOverrideEnabled would return.
	FTrussMaterialApplyCount ApplyTrussMaterialPass();
	// Build the set of every Orbit component currently bound to a fixture (those keep the
	// fixture-material override and must be skipped). Called from ApplyTrussMaterialPass.
	void BuildBoundOrbitComponentSet(TSet<UPrimitiveComponent*>& Out) const;

	// v1.0.99 -- imported-primitive shadow-cast normalisation state. See the doc-comment on
	// `EnsureImportedShadowsCast` above for the rationale + algorithm.
	bool bOrbitCastShadowsEnabled = true;
	// Set of every primitive we've ever asserted shadow-cast state on, so the OFF path can
	// walk the same comps and force CastShadow=false (and a re-toggle ON walks them to
	// CastShadow=true). Weak so a destroyed comp on re-import is silently dropped on next
	// pass instead of crashing.
	TSet<TWeakObjectPtr<UPrimitiveComponent>> OrbitShadowTouched;

	// v1.0.104 -- imported-primitive double-sided normalisation state. See the doc-comment
	// on `EnsureImportedDoubleSided` above for the rationale + algorithm. Default ON
	// matches the scene-property seed in URebusSceneSettingsSubsystem::Initialize so a
	// fresh launch starts on the operator's reported failure mode resolved.
	bool bOrbitDoubleSidedEnabled = true;
	// Set of every primitive we've ever flipped to two-sided shadow casting; mirrors the
	// v1.0.99 OrbitShadowTouched contract (the OFF path uses this to restore the prior
	// single-sided baseline). Weak so a destroyed comp on re-import is silently dropped.
	TSet<TWeakObjectPtr<UPrimitiveComponent>> OrbitDoubleSidedTouched;

	// v1.0.105 -- imported-mesh Nanite enable state. See the doc-comment on
	// `EnsureImportedNanite` above for the rationale + algorithm. Default ON matches the
	// scene-property seed in URebusSceneSettingsSubsystem::Initialize. Editor-only effect
	// (the WITH_EDITOR-guarded walker is a no-op in packaged builds; the flag is still
	// stored so SceneState round-trips correctly).
	bool bNaniteOrbitImportsEnabled = true;
	// Set of every UStaticMesh we've already attempted to enable / disable Nanite on this
	// session. The per-tick walker reads this to skip the expensive Build() call once a
	// mesh is in the desired state -- without it, every 1 Hz tick would re-invoke Build()
	// on every mesh and stall the game thread for seconds. Weak so a destroyed mesh on a
	// re-import is silently dropped on the next pass.
	TSet<TWeakObjectPtr<UStaticMesh>> NaniteAttempted;
	// One-shot session warning that we've logged "packaged build -- Nanite runtime
	// conversion unavailable" so we don't spam every tick. Reset to false on a Set* call
	// so the operator gets a fresh log line if they fire the toggle in a packaged build.
	bool bNanitePackagedWarningLogged = false;

	// v1.0.107 -- top-centre version watermark state. See the doc-comment on
	// `SetVersionWatermarkEnabled` above for the rationale + algorithm. The display
	// string is cached at Initialize() (`v` + IPluginManager VersionName) so the per-
	// frame draw allocates nothing. UDebugDrawService("Foreground") is the single
	// integration point -- the registered delegate fires after every rendered viewport
	// and is captured by PixelStreaming2 verbatim. The toggle + Y-margin are runtime-
	// settable via `Rebus.ShowVersion` / `Rebus.VersionWatermarkY` (registered in
	// RebusVisualiser.cpp) and via the `bShowVersionWatermark` scene property.
	FString CachedVersionDisplay; // "v1.0.107" -- composed once at Initialize()
	FDelegateHandle VersionWatermarkDrawHandle; // UDebugDrawService::Register handle
	void DrawVersionWatermark(UCanvas* Canvas, APlayerController* PC);

	// v1.0.112 -- runtime stale-master probe + auto-purge for `M_RebusBeam`. See the
	// public `EBeamMasterVersion` / `ProbeBeamMasterVersion` doc-comment for the full
	// rationale + algorithm + user-report verbatim. Called once from `Initialize` AND
	// once from every `PostLoadMapWithWorld` (v1.0.113 hardening: the v1.0.112
	// `bBeamMasterAutoPurgeRun` one-shot guard was per-subsystem-instance, but
	// `UGameInstanceSubsystem` lives across level reloads inside one editor session --
	// so an operator who reloaded the level WITHOUT restarting the editor would not
	// see the auto-purge re-fire after they manually flipped a content path / un-
	// fixed a known-stale master, defeating the v1.0.112 chokepoint). v1.0.113
	// resets `bBeamMasterAutoPurgeRun = false` from the `PostLoadMapWithWorld`
	// delegate and re-fires here.
	void ProbeAndAutoPurgeStaleBeamMaster();
	// Belt-and-braces walker called from `ProbeAndAutoPurgeStaleBeamMaster` after a
	// successful Python regen. SpawnedFixtures is normally EMPTY at `Initialize`
	// time (subsystem init runs before any fixture spawn) so this is normally a
	// no-op; it exists so the probe stays correct even if a future change re-fires
	// it from a tick-gated path where fixtures already exist. Per-fixture action:
	//   * `BeamMID->ClearParameterValues()` to drop the override set so the MID
	//     reads the regenerated master's defaults instead of the stale-master
	//     scalar values the per-fixture push had pinned on top.
	//   * `RefreshBeamRadialParams()` + `RefreshBeamSpatialParams()` to re-seed
	//     the live `Rebus.Beam*` CVar values + world axes.
	//   * `RefreshBeamShadowMaskParams()` to re-push the v1.0.111 light-space
	//     depth-mask scalar / axis / RT bindings so the new master has its
	//     contract correctly seeded.
	// Silently no-ops on a null fixture or a null BeamMID.
	void RefreshAllSpawnedFixtureBeamMIDs();
	bool bBeamMasterAutoPurgeRun = false;
	// v1.0.113 -- delegate handle for the `FCoreUObjectDelegates::PostLoadMapWithWorld`
	// hook that re-fires `ProbeAndAutoPurgeStaleBeamMaster` after every level load.
	// Held so `Deinitialize` can unregister cleanly (a dangling delegate captured
	// against `this` would crash on the next map load after a hot-reload of the
	// subsystem). Mirrors the v1.0.107 `VersionWatermarkDrawHandle` shape.
	FDelegateHandle PostLoadMapAutoPurgeHandle;
	void OnPostLoadMapAutoPurge(UWorld* LoadedWorld);

	// (v1.0.116) `static FString ComputeBeamMasterUassetMd5()` was previously declared
	// here in the `private:` block. It was always called from outside the class
	// (`RebusVisualiser.cpp::HandleDumpBeamMasterVersionCommand` invokes it as
	// `URebusVisualiserSubsystem::ComputeBeamMasterUassetMd5()` at file scope), so
	// the declaration was moved into the `public:` block above (next to the related
	// `ProbeBeamMasterVersion` / `BeamMasterVersionLabel` accessors). No body
	// change -- only the access specifier moved. See README v1.0.116 release block.
};
