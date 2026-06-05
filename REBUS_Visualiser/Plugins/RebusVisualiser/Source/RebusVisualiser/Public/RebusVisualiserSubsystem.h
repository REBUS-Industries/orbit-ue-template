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
};
