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

	// v1.0.103 -- one-shot probe at Initialize() that loads `/Game/REBUS/Materials/M_RebusBeam`
	// and checks for the v1.0.99 parameter contract (`BeamShadowStrength` + `BeamShadowDebug`
	// scalars). Logs a Warning naming the operator-recovery action (`Rebus.RebuildBeamMaterial`
	// + ClearScene/LoadScene OR editor restart) when the master predates v1.0.99 -- catches
	// the "operator pulled v1.0.99..v1.0.102 but never re-ran build_rebus_base_level.py" case
	// that the user's "beams still going straight through objects" report against v1.0.102
	// pinned down. Mirrors v1.0.91's IES-warning style; one-shot per session. See the
	// `ProbeBeamMasterAtStartup` doc-comment in the .cpp for the full diagnosis chain.
	void ProbeBeamMasterAtStartup();

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
	//     Mirrors the `Rebus.DumpBeamShadow` style. Reports Nanite=ON on every entry once
	//     the v1.0.105 walker has run successfully -- the canonical operator verification
	//     step on the v1.0.105 ship.
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
};
