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
#include "RebusVisualiserSubsystem.generated.h"

class FRebusRestClient;
class FRebusDataChannel;
class URebusFixtureControlSubsystem;
class URebusSceneSettingsSubsystem;
class ARebusFixtureActor;

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

	URebusFixtureControlSubsystem* GetControl() const;
	URebusSceneSettingsSubsystem* GetSceneSettings() const;
	UWorld* GetActiveWorld() const;

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

	UPROPERTY() TArray<TObjectPtr<ARebusFixtureActor>> SpawnedFixtures;

	FTSTicker::FDelegateHandle TickHandle;

	bool bEnvEnsured = false;
	bool bViewPositioned = false;
	bool bSceneRequested = false;
	bool bSceneLoaded = false;
	bool bFixturesSpawned = false;
	bool bChannelReady = false;
	bool bReadySent = false;

	float FrameStatsTimer = 0.f;
};
