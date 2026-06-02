// Copyright REBUS Industries.
#include "RebusVisualiserSubsystem.h"
#include "RebusRestClient.h"
#include "RebusDataChannel.h"
#include "RebusFixtureControlSubsystem.h"
#include "RebusSceneSettingsSubsystem.h"
#include "RebusFixtureActor.h"
#include "RebusJson.h"
#include "RebusVisualiserLog.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Misc/App.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"

static const TCHAR* RebusProjectVersion = TEXT("rebus-visualiser-1.0.0");

// Default streamed-view start pose. Authored in metres (0,-20,2) looking at (0,0,2); Unreal is
// centimetres, so x100. The look is horizontal along +Y (yaw 90). Tweak here if the stage moves.
static const FVector RebusViewStartLocation(0.f, -2000.f, 200.f);
static const FVector RebusViewLookAtLocation(0.f, 0.f, 200.f);

void URebusVisualiserSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Ensure our sibling subsystems exist.
	Collection.InitializeDependency(URebusFixtureControlSubsystem::StaticClass());

	ReadConfig();

	Rest = MakeShared<FRebusRestClient>();
	Rest->Configure(PortalUrl, ApiKey);

	Channel = MakeShared<FRebusDataChannel>();
	Channel->Initialize(StreamerId, GetControl(), GetSceneSettings());
	Channel->OnChannelReady.BindUObject(this, &URebusVisualiserSubsystem::OnChannelReady);
	Channel->OnViewerConnected.BindUObject(this, &URebusVisualiserSubsystem::OnViewerConnected);
	Channel->OnSceneDefinition.BindUObject(this, &URebusVisualiserSubsystem::HandleSceneDefinition);

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &URebusVisualiserSubsystem::Tick), 0.f);

	UE_LOG(LogRebusVisualiser, Log, TEXT("Visualiser subsystem initialised (streamer='%s', project='%s', model='%s')."),
		*StreamerId, *ProjectId, *ModelId);
}

void URebusVisualiserSubsystem::Deinitialize()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	Channel.Reset();
	Rest.Reset();
	Super::Deinitialize();
}

void URebusVisualiserSubsystem::ReadConfig()
{
	const TCHAR* Cmd = FCommandLine::Get();

	// Base URL + key: command line wins, then [RebusVisualiser] in DefaultGame.ini.
	if (!FParse::Value(Cmd, TEXT("PortalUrl="), PortalUrl) || PortalUrl.IsEmpty())
	{
		GConfig->GetString(TEXT("RebusVisualiser"), TEXT("PortalUrl"), PortalUrl, GGameIni);
	}
	if (!FParse::Value(Cmd, TEXT("RebusApiKey="), ApiKey) || ApiKey.IsEmpty())
	{
		GConfig->GetString(TEXT("RebusVisualiser"), TEXT("ApiKey"), ApiKey, GGameIni);
	}
	PortalUrl.TrimQuotesInline();

	// PRISM launch tokens (§10.2).
	FParse::Value(Cmd, TEXT("PixelStreamingID="), StreamerId);
	FParse::Value(Cmd, TEXT("OrbitProject="), ProjectId);
	FParse::Value(Cmd, TEXT("OrbitModel="), ModelId);
	FParse::Value(Cmd, TEXT("OrbitVersion="), VersionId);
	FParse::Value(Cmd, TEXT("OrbitServer="), OrbitServer);
	FParse::Value(Cmd, TEXT("OrbitTarget="), OrbitTarget);
}

UWorld* URebusVisualiserSubsystem::GetActiveWorld() const
{
	if (UGameInstance* GI = GetGameInstance())
	{
		return GI->GetWorld();
	}
	return nullptr;
}

URebusFixtureControlSubsystem* URebusVisualiserSubsystem::GetControl() const
{
	UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetSubsystem<URebusFixtureControlSubsystem>() : nullptr;
}

URebusSceneSettingsSubsystem* URebusVisualiserSubsystem::GetSceneSettings() const
{
	UWorld* World = GetActiveWorld();
	return World ? World->GetSubsystem<URebusSceneSettingsSubsystem>() : nullptr;
}

bool URebusVisualiserSubsystem::Tick(float DeltaSeconds)
{
	// Keep trying to bind the data channel until a streamer exists, and attach the world-scoped
	// scene-settings subsystem once a world is live.
	if (Channel.IsValid())
	{
		if (URebusSceneSettingsSubsystem* SceneSettings = GetSceneSettings())
		{
			Channel->SetSceneSettings(SceneSettings);
		}
		Channel->TryBind();
	}

	// Ensure the local scene environment (fog + unbound post-process + ground plane) as soon as
	// a game world is live. This is a launch-time safety net and must NOT depend on the portal
	// API key / project / model -- otherwise an unconfigured session renders with no floor.
	if (!bEnvEnsured)
	{
		UWorld* World = GetActiveWorld();
		if (World && World->IsGameWorld() && World->HasBegunPlay())
		{
			bEnvEnsured = true;
			EnsureSceneEnvironment();
		}
	}

	// Place the streamed view at the configured start pose. The player controller / pawn may not
	// exist on the first ready tick, so retry until it succeeds (this is the only reposition).
	if (!bViewPositioned)
	{
		UWorld* World = GetActiveWorld();
		if (World && World->IsGameWorld() && World->HasBegunPlay())
		{
			bViewPositioned = TryPositionPlayerView();
		}
	}

	// Start the portal scene load once a world is live. The fixture fetch needs the API key +
	// project/model; when those are absent we still complete the handshake with zero fixtures so
	// the portal receives Ready and enables its scene/quality/ground controls (data-channel
	// control is otherwise gated behind a Ready that would never arrive).
	if (!bSceneRequested)
	{
		UWorld* World = GetActiveWorld();
		const bool bWorldReady = World && World->IsGameWorld() && World->HasBegunPlay();
		if (bWorldReady)
		{
			const bool bCanFetch = Rest.IsValid() && Rest->IsConfigured()
				&& !ProjectId.IsEmpty() && !ModelId.IsEmpty();
			bSceneRequested = true;
			if (bCanFetch)
			{
				BeginSceneLoad();
			}
			else
			{
				UE_LOG(LogRebusVisualiser, Warning,
					TEXT("Portal scene fetch skipped (api-key/project/model missing); reporting Ready with no fixtures so scene control still works."));
				bSceneLoaded = true; // nothing to fetch/spawn -> scene is "loaded"
				TrySendReady();
			}
		}
	}

	// Periodic FrameStats for the diagnostics strip (§6.6).
	if (bReadySent && Channel.IsValid())
	{
		FrameStatsTimer += DeltaSeconds;
		if (FrameStatsTimer >= 2.f)
		{
			FrameStatsTimer = 0.f;
			const float Dt = FApp::GetDeltaTime();
			const float Fps = Dt > 0.f ? (1.f / Dt) : 0.f;
			Channel->SendFrameStats(FMath::Clamp(Fps, 0.f, 240.f), 0.f, 0.f, 0.f);
		}
	}

	return true; // keep ticking
}

void URebusVisualiserSubsystem::BeginSceneLoad()
{
	UE_LOG(LogRebusVisualiser, Log, TEXT("Fetching scene (project='%s', model='%s', version='%s')."),
		*ProjectId, *ModelId, *VersionId);

	TWeakObjectPtr<URebusVisualiserSubsystem> WeakThis(this);
	Rest->FetchScene(ProjectId, ModelId, VersionId,
		FRebusSceneFetched::CreateLambda([WeakThis](bool bOk, const FRebusScene& Scene)
		{
			if (URebusVisualiserSubsystem* Self = WeakThis.Get())
			{
				Self->OnSceneFetched(bOk, Scene);
			}
		}));
}

void URebusVisualiserSubsystem::OnSceneFetched(bool bOk, const FRebusScene& Scene)
{
	if (!bOk)
	{
		// A failed fetch (e.g. 404 / bad key) must NOT dead-end the handshake: report Ready with
		// zero fixtures (non-fatal notice) so the portal still enables scene/quality/ground
		// control. Previously this returned early and Ready never fired.
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("Scene fetch failed; reporting Ready with no fixtures so scene control still works."));
		if (Channel.IsValid())
		{
			Channel->SendNotice(TEXT("fetch-failed"), TEXT("Could not fetch scene; fixtures unavailable."));
		}
		bSceneLoaded = true;
		TrySendReady();
		return;
	}
	SceneData = Scene;
	if (Scene.bTruncated && Channel.IsValid())
	{
		Channel->SendNotice(TEXT("truncated"), TEXT("Scene graph hit the 20k-object cap; fixture list may be partial."));
	}
	PrefetchProfiles();
}

void URebusVisualiserSubsystem::PrefetchProfiles()
{
	// Gather the unique library ids to fetch (profileIds, falling back to fixtures[].fixtureId).
	TSet<FString> Unique;
	for (const FString& Id : SceneData.ProfileIds) { if (!Id.IsEmpty()) Unique.Add(Id); }
	for (const FRebusSceneFixture& F : SceneData.Fixtures) { if (!F.LibraryFixtureId.IsEmpty()) Unique.Add(F.LibraryFixtureId); }

	if (Unique.Num() == 0)
	{
		// No library profiles at all -> spawn light-only/placeholder fixtures immediately.
		bSceneLoaded = true;
		TrySpawnFixtures();
		return;
	}

	PendingProfiles = Unique;
	PendingMeshes = Unique;

	TWeakObjectPtr<URebusVisualiserSubsystem> WeakThis(this);
	for (const FString& LibraryId : Unique)
	{
		Rest->FetchFixtureProfile(LibraryId,
			FRebusProfileFetched::CreateLambda([WeakThis, LibraryId](bool bOk, const FRebusFixtureProfile& Profile)
			{
				if (URebusVisualiserSubsystem* Self = WeakThis.Get())
				{
					Self->OnProfileFetched(LibraryId, bOk, Profile);
				}
			}));
	}
}

void URebusVisualiserSubsystem::OnProfileFetched(const FString& LibraryId, bool bOk, const FRebusFixtureProfile& Profile)
{
	PendingProfiles.Remove(LibraryId);

	if (bOk && Profile.bValid)
	{
		ProfileCache.Add(LibraryId, Profile);

		// Chain the mesh fetch off the profile's meshesUrl.
		if (!Profile.MeshesUrl.IsEmpty())
		{
			TWeakObjectPtr<URebusVisualiserSubsystem> WeakThis(this);
			Rest->FetchBytes(Profile.MeshesUrl,
				FRebusBytesFetched::CreateLambda([WeakThis, LibraryId](bool bMeshOk, const TArray<uint8>& Bytes)
				{
					FRebusMeshBundle Bundle;
					if (bMeshOk && Bytes.Num() > 0)
					{
						const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
						const FString Json(Conv.Length(), Conv.Get());
						RebusJson::ParseMeshBundle(Json, Bundle);
					}
					if (URebusVisualiserSubsystem* Self = WeakThis.Get())
					{
						Self->OnMeshesFetched(LibraryId, bMeshOk, Bundle);
					}
				}));
		}
		else
		{
			OnMeshesFetched(LibraryId, false, FRebusMeshBundle());
		}
	}
	else
	{
		// Profile failed: drop its mesh expectation too.
		PendingMeshes.Remove(LibraryId);
	}

	TrySpawnFixtures();
}

void URebusVisualiserSubsystem::OnMeshesFetched(const FString& LibraryId, bool bOk, const FRebusMeshBundle& Meshes)
{
	PendingMeshes.Remove(LibraryId);
	if (bOk)
	{
		MeshCache.Add(LibraryId, Meshes);
	}
	TrySpawnFixtures();
}

void URebusVisualiserSubsystem::TrySpawnFixtures()
{
	if (bFixturesSpawned) return;
	if (PendingProfiles.Num() > 0 || PendingMeshes.Num() > 0) return; // still loading

	bSceneLoaded = true;
	SpawnAllFixtures();
}

void URebusVisualiserSubsystem::SpawnAllFixtures()
{
	if (bFixturesSpawned) return;
	bFixturesSpawned = true;

	UWorld* World = GetActiveWorld();
	URebusFixtureControlSubsystem* Ctl = GetControl();
	if (!World || !Ctl)
	{
		return;
	}

	static const FRebusFixtureProfile EmptyProfile;
	static const FRebusMeshBundle EmptyMeshes;

	for (const FRebusSceneFixture& F : SceneData.Fixtures)
	{
		const FRebusFixtureProfile* Profile = F.LibraryFixtureId.IsEmpty() ? &EmptyProfile : ProfileCache.Find(F.LibraryFixtureId);
		if (!Profile) Profile = &EmptyProfile;
		const FRebusMeshBundle* Meshes = F.LibraryFixtureId.IsEmpty() ? &EmptyMeshes : MeshCache.Find(F.LibraryFixtureId);
		if (!Meshes) Meshes = &EmptyMeshes;

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ARebusFixtureActor* Actor = World->SpawnActor<ARebusFixtureActor>(ARebusFixtureActor::StaticClass(), Params);
		if (!Actor) continue;

		Actor->SetRestClient(Rest);
		Actor->Setup(F, *Profile, *Meshes);

		Ctl->RegisterFixture(F.Id, Actor); // register under the Speckle node id (§3)
		SpawnedFixtures.Add(Actor);
	}

	UE_LOG(LogRebusVisualiser, Log, TEXT("Spawned %d fixtures."), SpawnedFixtures.Num());

	// First load completes the handshake; a re-load (e.g. portal re-push) re-broadcasts it so
	// the new FixtureRegistered set reaches viewers.
	if (bReadySent)
	{
		BroadcastHandshake();
	}
	else
	{
		TrySendReady();
	}
}

void URebusVisualiserSubsystem::ClearSpawnedFixtures()
{
	for (ARebusFixtureActor* F : SpawnedFixtures)
	{
		if (F) F->Destroy();
	}
	SpawnedFixtures.Reset();
	if (URebusFixtureControlSubsystem* Ctl = GetControl())
	{
		Ctl->Reset();
	}
	bFixturesSpawned = false;
}

void URebusVisualiserSubsystem::HandleSceneDefinition(const FString& Type, const TSharedPtr<FJsonObject>& Msg)
{
	if (!Msg.IsValid()) return;

	// Serialize a JSON object back to a string so we can reuse the REST payload parsers (the
	// portal sends the exact bodies /api/ue/scene + /api/ue/fixtures/{id} would return).
	auto ObjectToString = [](const TSharedPtr<FJsonObject>& Obj) -> FString
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	};
	auto FieldToString = [&ObjectToString](const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field) -> FString
	{
		const TSharedPtr<FJsonObject>* Sub = nullptr;
		if (!Obj->TryGetObjectField(Field, Sub) || !Sub) return FString();
		return ObjectToString(*Sub);
	};

	// Merge any inline profiles/meshes maps (keyed by libraryFixtureId) into the caches.
	auto IngestProfileMaps = [this, &ObjectToString](const TSharedPtr<FJsonObject>& Obj)
	{
		const TSharedPtr<FJsonObject>* ProfilesObj = nullptr;
		if (Obj->TryGetObjectField(TEXT("profiles"), ProfilesObj) && ProfilesObj)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ProfilesObj)->Values)
			{
				const TSharedPtr<FJsonObject> PObj = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
				if (!PObj.IsValid()) continue;
				FRebusFixtureProfile Profile;
				if (RebusJson::ParseFixtureProfile(ObjectToString(PObj), Profile) && Profile.bValid)
				{
					ProfileCache.Add(Pair.Key, Profile);
				}
			}
		}
		const TSharedPtr<FJsonObject>* MeshesObj = nullptr;
		if (Obj->TryGetObjectField(TEXT("meshes"), MeshesObj) && MeshesObj)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*MeshesObj)->Values)
			{
				const TSharedPtr<FJsonObject> MObj = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
				if (!MObj.IsValid()) continue;
				FRebusMeshBundle Bundle;
				if (RebusJson::ParseMeshBundle(ObjectToString(MObj), Bundle))
				{
					MeshCache.Add(Pair.Key, Bundle);
				}
			}
		}
	};

	if (Type == TEXT("ClearScene"))
	{
		ClearSpawnedFixtures();
		UE_LOG(LogRebusVisualiser, Log, TEXT("ClearScene: removed all pushed fixtures."));
		if (bReadySent) BroadcastHandshake();
		return;
	}

	if (Type == TEXT("RegisterFixtureProfile"))
	{
		// Single profile (+ optional meshes), for incremental pushes that stay under the
		// data-channel message size. { libraryId, profile, meshes? }
		FString LibraryId;
		RebusJson::TryGetString(Msg, TEXT("libraryId"), LibraryId);
		const FString ProfileJson = FieldToString(Msg, TEXT("profile"));
		if (LibraryId.IsEmpty() || ProfileJson.IsEmpty())
		{
			UE_LOG(LogRebusVisualiser, Warning, TEXT("RegisterFixtureProfile missing libraryId/profile; ignoring."));
			return;
		}
		FRebusFixtureProfile Profile;
		if (RebusJson::ParseFixtureProfile(ProfileJson, Profile) && Profile.bValid)
		{
			ProfileCache.Add(LibraryId, Profile);
			const FString MeshJson = FieldToString(Msg, TEXT("meshes"));
			if (!MeshJson.IsEmpty())
			{
				FRebusMeshBundle Bundle;
				if (RebusJson::ParseMeshBundle(MeshJson, Bundle)) MeshCache.Add(LibraryId, Bundle);
			}
			UE_LOG(LogRebusVisualiser, Log, TEXT("Cached pushed profile '%s'."), *LibraryId);
		}
		return;
	}

	// LoadScene: { scene, profiles?, meshes? } -> (re)spawn fixtures from the pushed scene.
	const FString SceneJson = FieldToString(Msg, TEXT("scene"));
	if (SceneJson.IsEmpty())
	{
		UE_LOG(LogRebusVisualiser, Warning, TEXT("LoadScene missing 'scene'; ignoring."));
		return;
	}
	FRebusScene Scene;
	if (!RebusJson::ParseScene(SceneJson, Scene))
	{
		UE_LOG(LogRebusVisualiser, Warning, TEXT("LoadScene 'scene' failed to parse; ignoring."));
		return;
	}

	IngestProfileMaps(Msg);

	ClearSpawnedFixtures();
	SceneData = Scene;
	bSceneLoaded = true;
	UE_LOG(LogRebusVisualiser, Log, TEXT("LoadScene: %d fixture(s) pushed over the data channel."),
		SceneData.Fixtures.Num());
	SpawnAllFixtures();
}

void URebusVisualiserSubsystem::OnChannelReady()
{
	bChannelReady = true;
	TrySendReady();
}

void URebusVisualiserSubsystem::TrySendReady()
{
	// Ready requires both: scene loaded (so loadedModel counts are final) + channel open.
	if (bReadySent || !bChannelReady || !bSceneLoaded || !Channel.IsValid())
	{
		return;
	}
	bReadySent = true;
	BroadcastHandshake();
}

void URebusVisualiserSubsystem::BroadcastHandshake()
{
	if (!Channel.IsValid())
	{
		return;
	}

	const TArray<FString> Capabilities = { TEXT("scene-state"), TEXT("gobos"), TEXT("truss-visibility") };
	const FString UeVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);

	Channel->SendReady(UeVersion, RebusProjectVersion, Capabilities,
		ProjectId, ModelId, VersionId, SpawnedFixtures.Num(), /*trussCount*/0);

	for (ARebusFixtureActor* F : SpawnedFixtures)
	{
		if (!F) continue;
		Channel->SendFixtureRegistered(F->GetFixtureId(), F->GetLibraryFixtureId(),
			F->GetDisplayName(), F->HasPanTilt(), F->HasGobo());
	}

	// Mirror current scene/ground/quality so a (re)connecting viewer paints the live state.
	if (URebusSceneSettingsSubsystem* Sce = GetSceneSettings())
	{
		Channel->SendSceneState(Sce->GetSceneState());
	}

	// Re-apply the live selection so a late/reconnecting stream paints it (§5.3).
	if (URebusFixtureControlSubsystem* Ctl = GetControl())
	{
		Ctl->SelectFixtures(Ctl->GetCurrentSelection(), Ctl->GetPrimarySelection());
	}
}

void URebusVisualiserSubsystem::OnViewerConnected()
{
	// The first viewer is covered by TrySendReady once the scene loads. But a viewer's data
	// track typically opens a beat AFTER the channel binds (and Ready was already broadcast to
	// nobody), and reconnects happen at any time -- so re-greet whenever we've a handshake to
	// send. If the scene hasn't loaded yet, the pending TrySendReady will reach this viewer.
	if (bReadySent)
	{
		BroadcastHandshake();
	}
}

void URebusVisualiserSubsystem::EnsureSceneEnvironment()
{
	UWorld* World = GetActiveWorld();
	if (!World) return;

	// --- Exponential height fog (global / full extent), with volumetric fog enabled ---
	bool bHasFog = false;
	for (TActorIterator<AExponentialHeightFog> It(World); It; ++It) { bHasFog = true; break; }
	if (!bHasFog)
	{
		AExponentialHeightFog* Fog = World->SpawnActor<AExponentialHeightFog>(
			AExponentialHeightFog::StaticClass(), FTransform(FVector(0.f, 0.f, 200.f)));
		if (Fog)
		{
			if (UExponentialHeightFogComponent* C = Fog->GetComponent())
			{
				C->SetFogDensity(0.02f);
				C->SetFogHeightFalloff(0.2f);
				C->SetVolumetricFog(true); // per-fixture beams scatter in this (§8.4)
			}
			UE_LOG(LogRebusVisualiser, Log, TEXT("Spawned default ExponentialHeightFog (volumetric)."));
		}
	}

	// --- Post-process volume (unbound = full extent) ---
	bool bHasPPV = false;
	for (TActorIterator<APostProcessVolume> It(World); It; ++It) { bHasPPV = true; break; }
	if (!bHasPPV)
	{
		APostProcessVolume* PPV = World->SpawnActor<APostProcessVolume>();
		if (PPV)
		{
			PPV->bUnbound = true;
			PPV->BlendWeight = 1.f;
			UE_LOG(LogRebusVisualiser, Log, TEXT("Spawned default unbound PostProcessVolume."));
		}
	}

	// --- Infinite-ish ground plane (tagged so scene-settings can swap surface / toggle) ---
	bool bHasFloor = false;
	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(TEXT("RebusFloor"))) { bHasFloor = true; break; }
	}
	if (!bHasFloor)
	{
		if (UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")))
		{
			if (AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>())
			{
				Floor->Tags.Add(TEXT("RebusFloor"));
				if (UStaticMeshComponent* C = Floor->GetStaticMeshComponent())
				{
					C->SetMobility(EComponentMobility::Movable);
					C->SetStaticMesh(Plane);
					Floor->SetActorScale3D(FVector(2000.f, 2000.f, 1.f));
					if (UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr,
						TEXT("/Game/REBUS/Materials/MI_RebusGround_Concrete.MI_RebusGround_Concrete")))
					{
						C->SetMaterial(0, Mat);
					}
				}
				UE_LOG(LogRebusVisualiser, Log, TEXT("Spawned default ground plane."));
			}
		}
	}
}

bool URebusVisualiserSubsystem::TryPositionPlayerView()
{
	UWorld* World = GetActiveWorld();
	if (!World) return false;

	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC) return false; // controller not spawned yet -> retry next tick

	// Forward (actor +X / control-rotation forward) points from the eye to the look-at target.
	const FRotator ViewRotation = (RebusViewLookAtLocation - RebusViewStartLocation).Rotation();

	if (APawn* Pawn = PC->GetPawn())
	{
		Pawn->SetActorLocationAndRotation(RebusViewStartLocation, ViewRotation);
	}
	else
	{
		return false; // pawn not spawned yet -> retry next tick
	}

	// DefaultPawn's camera follows the controller's control rotation; set it so the view faces
	// the target immediately (not just the pawn's actor rotation).
	PC->SetControlRotation(ViewRotation);

	UE_LOG(LogRebusVisualiser, Log, TEXT("Player view positioned at %s facing %s."),
		*RebusViewStartLocation.ToString(), *ViewRotation.ToString());
	return true;
}
