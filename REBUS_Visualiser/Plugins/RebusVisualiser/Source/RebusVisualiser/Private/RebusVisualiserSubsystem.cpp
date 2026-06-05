// Copyright REBUS Industries.
#include "RebusVisualiserSubsystem.h"
#include "RebusRestClient.h"
#include "RebusDataChannel.h"
#include "RebusFixtureControlSubsystem.h"
#include "RebusSceneSettingsSubsystem.h"
#include "RebusFixtureActor.h"
#include "RebusCineCameraPawn.h"
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
#include "Misc/Base64.h"
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
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/PrimitiveComponent.h"

// v1.0.107 -- watermark overlay support. UDebugDrawService is the engine's
// FCanvas-overlay registry; the "Foreground" extension fires after the world is
// rendered but before UMG/HUD, so the watermark sits over the 3D scene + under any
// operator UMG -- correct stacking for a non-intrusive overlay. Pixel Streaming 2
// captures the same FCanvas pass into the H.264 stream so the watermark appears
// in the live stream without PS2-side glue.
#include "Debug/DebugDrawService.h"
#include "Engine/Canvas.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Font.h"
#include "Interfaces/IPluginManager.h"

static const TCHAR* RebusProjectVersion = TEXT("rebus-visualiser-1.0.0");

// v1.0.107 -- version-watermark live state. File-scope (rather than member fields)
// so the per-frame foreground-canvas draw delegate + the `Rebus.ShowVersion` /
// `Rebus.VersionWatermarkY` console commands (registered in RebusVisualiser.cpp) +
// the SetSceneProperty handler all read the SAME bool/float without an extra
// subsystem lookup per frame. The flag default is ON: the watermark is the
// operator's at-a-glance confirmation that the running binary matches the
// expected release on every rendered frame (and every PixelStreaming2 stream
// frame that captures the FCanvas overlay), so it must be visible by default.
//
// Mirrors the existing Rebus.* IConsoleCommand pattern (Rebus.ShowOrbitFixtures /
// Rebus.OverrideTrussMaterial / Rebus.OrbitDoubleSided / Rebus.NaniteOrbitImports)
// rather than TAutoConsoleVariable: those existing commands all support multi-arg
// invocations (`status`, descriptive log lines, scene-property routing) that a
// raw CVar can't provide, and pairing the namespace with a CVar would conflict
// with the IConsoleCommand registration anyway.
namespace RebusVersionWatermark
{
	bool   GShowEnabled  = true;
	float  GTopMarginPx  = 12.f;
}

// v1.0.100 -- default streamed-view spawn pose now lives in RebusCineCameraDefaults
// (RebusCineCameraPawn.h) so URebusVisualiserSubsystem::TryPositionPlayerView (spawn) and
// ARebusCineCameraPawn::ResetToDefaults (Rebus.CameraReset) share one source of truth. The
// landing pose is location (0,-20,2) m looking at (0,0,5) m; aim is derived from
// `(target - location).GetSafeNormal()` via FRotationMatrix::MakeFromX so any future tweak
// to the metres triples re-derives the rotator cleanly. Today that resolves to pitch ~+8.53°
// (gentle look-up), yaw 90° (camera facing +Y), roll 0°. The portal can still override the
// live camera pose at any time via SetCameraTransform -- these constants are JUST the
// construction-time / reset landing pose.

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
	Channel->SetVisualiserSubsystem(this); // v1.0.79: route SetCamera* descriptors here
	Channel->OnChannelReady.BindUObject(this, &URebusVisualiserSubsystem::OnChannelReady);
	Channel->OnViewerConnected.BindUObject(this, &URebusVisualiserSubsystem::OnViewerConnected);
	Channel->OnSceneDefinition.BindUObject(this, &URebusVisualiserSubsystem::HandleSceneDefinition);

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &URebusVisualiserSubsystem::Tick), 0.f);

	// v1.0.103 startup beam-master probe REMOVED in v1.0.110 -- it specifically warned about
	// the v1.0.99 / v1.0.109 `BeamShadow*` parameter contract that the v1.0.110 rollback
	// just deleted. With the screen-space shadow trace gone there is no longer a stale-
	// master failure mode worth a startup Warning; the runtime `Rebus.RebuildBeamMaterial`
	// console command stays as the generic operator escape hatch for any future material
	// graph changes.

	// v1.0.107 -- compose the watermark display string ONCE from the plugin
	// descriptor's VersionName (the engine-blessed source-of-truth that always
	// reflects the running binary). Caching here makes the per-frame draw zero-
	// allocation. We fall back to the project-version literal if the plugin
	// somehow can't be located (cooked-package edge case); the watermark still
	// renders something useful instead of an empty string.
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("RebusVisualiser"));
		if (Plugin.IsValid())
		{
			CachedVersionDisplay = FString::Printf(TEXT("v%s"), *Plugin->GetDescriptor().VersionName);
		}
		else
		{
			CachedVersionDisplay = TEXT("v?.?.?");
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("v1.0.107 watermark: IPluginManager could not find 'RebusVisualiser' "
					 "-- falling back to placeholder version string. The watermark will "
					 "still render but it won't reflect the descriptor's VersionName."));
		}
	}

	// Register the foreground debug-draw delegate. UDebugDrawService dispatches the
	// "Foreground" extension after the 3D world is rendered + before UMG/HUD, so the
	// watermark sits on top of the scene + below any operator UMG (correct stacking
	// for a non-intrusive overlay). PixelStreaming2 captures the FCanvas overlay
	// pass verbatim into the H.264 stream frames -- no PS2-side integration needed.
	VersionWatermarkDrawHandle = UDebugDrawService::Register(
		TEXT("Foreground"),
		FDebugDrawDelegate::CreateUObject(this, &URebusVisualiserSubsystem::DrawVersionWatermark));

	UE_LOG(LogRebusVisualiser, Log,
		TEXT("v1.0.107 watermark registered: display='%s', toggle='Rebus.ShowVersion' (default ON), "
			 "y-margin='Rebus.VersionWatermarkY' (default 12px)."),
		*CachedVersionDisplay);

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
	// v1.0.107 -- always unregister the watermark draw delegate so a hot-reload of
	// the module / GameInstance respawn doesn't leak a dangling delegate into the
	// debug-draw service (would crash on the next "Foreground" dispatch when the
	// captured `this` is destructed).
	if (VersionWatermarkDrawHandle.IsValid())
	{
		UDebugDrawService::Unregister(VersionWatermarkDrawHandle);
		VersionWatermarkDrawHandle.Reset();
	}
	Channel.Reset();
	Rest.Reset();
	Super::Deinitialize();
}

// v1.0.107 -- single chokepoint for the version-watermark visibility toggle.
// Routes through the file-scope `RebusVersionWatermark::GShowEnabled` so the
// per-frame draw + the `Rebus.ShowVersion` console command + the
// `bShowVersionWatermark` scene property + a programmatic API call all share one
// source of truth for the live state. Mirrors the v1.0.99 / v1.0.104 / v1.0.105
// single-chokepoint pattern (SetOrbitCastShadowsEnabled,
// SetOrbitDoubleSidedEnabled, SetNaniteOrbitImportsEnabled).
void URebusVisualiserSubsystem::SetVersionWatermarkEnabled(bool bEnabled)
{
	RebusVersionWatermark::GShowEnabled = bEnabled;
}

bool URebusVisualiserSubsystem::IsVersionWatermarkEnabled() const
{
	return RebusVersionWatermark::GShowEnabled;
}

void URebusVisualiserSubsystem::SetVersionWatermarkTopMarginPx(float Px)
{
	RebusVersionWatermark::GTopMarginPx = FMath::Max(0.f, Px);
}

float URebusVisualiserSubsystem::GetVersionWatermarkTopMarginPx()
{
	return RebusVersionWatermark::GTopMarginPx;
}

// v1.0.107 -- the per-frame foreground-canvas draw that paints the watermark.
// Wrapped in a single early-out so the per-frame cost is zero when the toggle is
// off (the most common state in QA runs that don't want the overlay polluting
// reference captures). UDebugDrawService::Register passes us a per-viewport
// UCanvas + the local PlayerController; we measure the cached `v<VersionName>`
// string against the engine's medium font, centre it horizontally on the canvas
// width, and offset it down from the top edge by the configured margin.
//
// Drop-shadow: a black-70%-alpha shadow beneath the white-90% foreground glyph
// keeps the text readable against bright skies (set in PIE) AND dark stages
// (typical operator scene) without needing an opaque background plate. UE
// `FCanvasTextItem::EnableShadow` does both layers in one DrawItem call.
void URebusVisualiserSubsystem::DrawVersionWatermark(UCanvas* Canvas, APlayerController* /*PC*/)
{
	if (!Canvas || CachedVersionDisplay.IsEmpty())
	{
		return;
	}
	if (!RebusVersionWatermark::GShowEnabled)
	{
		return; // toggle is off -- per-frame cost collapses to one branch
	}

	UFont* Font = (GEngine != nullptr) ? GEngine->GetMediumFont() : nullptr;
	if (!Font)
	{
		return; // engine still bootstrapping; try again next frame
	}

	float TextW = 0.f, TextH = 0.f;
	Canvas->TextSize(Font, CachedVersionDisplay, TextW, TextH);

	const float CanvasW = static_cast<float>(Canvas->SizeX);
	const float MarginY = FMath::Max(0.f, RebusVersionWatermark::GTopMarginPx);
	const float DrawX = FMath::Max(0.f, (CanvasW - TextW) * 0.5f);
	const float DrawY = MarginY;

	// FCanvasTextItem with EnableShadow is the canonical UE way to draw a shadow
	// stroke + foreground glyph in one item. The foreground is white-90% and the
	// shadow is black-70%; works against bright + dark backgrounds without
	// needing an opaque background plate.
	FCanvasTextItem TextItem(FVector2D(DrawX, DrawY), FText::FromString(CachedVersionDisplay), Font, FLinearColor(1.f, 1.f, 1.f, 0.9f));
	TextItem.EnableShadow(FLinearColor(0.f, 0.f, 0.f, 0.7f));
	TextItem.bCentreX = false; // we computed the centred X explicitly above
	TextItem.bCentreY = false;
	Canvas->DrawItem(TextItem);
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
			UE_LOG(LogRebusVisualiser, Log, TEXT("Scene environment ensured."));

			// Now that the fog/post-process/floor (and sun/sky) actors exist, push every stored
			// scene-property value (seeded defaults + anything the portal pushed before the
			// actors were live) onto them so first-load state sticks without a recycle.
			if (URebusSceneSettingsSubsystem* Sce = GetSceneSettings())
			{
				Sce->ReapplyAll();
			}
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

	// Liveness: the Ready gates (channel open / scene loaded / environment ensured) can become
	// true on different ticks and in any order, so poll here -- TrySendReady is idempotent and
	// no-ops until every gate holds, then fires the handshake exactly once.
	if (!bReadySent)
	{
		TrySendReady();
	}

	// Periodic Orbit-model rebind: while driving is enabled, retry binding so a late
	// OrbitConnector import (or a re-import that replaced the components) binds to the
	// already-spawned fixtures without a manual re-toggle. Cheap no-op when driving is off or no
	// Orbit import is present. (RegisterFixture also kicks an immediate rebind for the
	// fixture-spawn-then-import-arrives case, so this 1 Hz timer mostly catches the inverse:
	// import refreshes mid-session and the existing fixtures need to re-bind to the new
	// components.)
	if (URebusFixtureControlSubsystem* Ctl = GetControl())
	{
		if (Ctl->IsDrivingOrbitModels())
		{
			OrbitRebindTimer += DeltaSeconds;
			if (OrbitRebindTimer >= 1.0f)
			{
				OrbitRebindTimer = 0.f;
				Ctl->RebindOrbitModels();
				// v1.0.85: piggyback the truss-material override on the rebind cadence so
				// newly-bound fixture components are correctly EXCLUDED (handled by the body/
				// lens override) and newly-imported orphan geometry inherits the powdercoat
				// pass without the operator having to re-run the console command. Cheap when
				// the cache is already up to date -- the per-pass diff returns Touched=0.
				if (bTrussMaterialOverrideEnabled)
				{
					ApplyTrussMaterialPass();
				}
				// v1.0.99: piggyback the imported-shadow-cast pass on the same cadence so
				// freshly-imported Orbit geometry inherits CastShadow=true (or =false, when
				// the operator flipped `Rebus.OrbitCastShadows 0` / SetSceneProperty
				// bOrbitCastShadows=false) on the next 1 Hz tick after import. Cheap when
				// stable (per-comp early-out -- Touched=0 when nothing changed). See the
				// EnsureImportedShadowsCast doc-comment in the header for the user report.
				EnsureImportedShadowsCast();
				// v1.0.104: piggyback the imported-double-sided pass on the same cadence
				// so freshly-imported Orbit geometry inherits bCastShadowAsTwoSided=true +
				// the `bTwoSided` MID switch on the next 1 Hz tick after import. Cheap
				// when stable; addresses the user report that thin Orbit geometry (truss
				// cross-bars, banner cloth, sheet-metal flags) disappears when viewed from
				// the back. See EnsureImportedDoubleSided doc-comment in the header for
				// the full algorithm + the perf caveat.
				EnsureImportedDoubleSided();
				// v1.0.105: piggyback the imported-Nanite enable pass on the same cadence
				// so freshly-imported Orbit static meshes inherit NaniteSettings.bEnabled=
				// true (and a Build() rebuild for the cooked NaniteResources) on the next
				// 1 Hz tick after import. Editor-only -- the WITH_EDITOR guard inside the
				// walker makes this a free no-op in packaged builds. Per-mesh cache
				// (NaniteAttempted) keeps the steady-state cost flat -- once a mesh is in
				// the desired state we don't call Build() on it again. See
				// EnsureImportedNanite doc-comment in the header for the full algorithm,
				// the editor-only constraint, and the cooked-Nanite path for packaged.
				EnsureImportedNanite();
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

	// v1.0.79: ~30Hz CameraState stream. Gated by Channel + a live cine pawn; the helper
	// itself dead-zone-rejects unchanged frames so a stationary camera never broadcasts.
	if (bReadySent && Channel.IsValid() && CineCameraPawn.Get())
	{
		CameraStateTimer += DeltaSeconds;
		if (CameraStateTimer >= (1.f / 30.f))
		{
			CameraStateTimer = 0.f;
			BroadcastCameraStateIfChanged(/*bForce*/ false);
		}
	}

	// v1.0.80: ~10Hz FixtureStates delta stream. Lower rate than the camera (one cine pawn vs
	// potentially 100+ fixtures) and per-fixture dead-zone-gated -- a static rig produces
	// zero traffic, an actively-fading rig produces one batched message every ~100ms with
	// only the fixtures whose values moved.
	if (bReadySent && Channel.IsValid())
	{
		FixtureStreamTimer += DeltaSeconds;
		if (FixtureStreamTimer >= (1.f / 10.f))
		{
			FixtureStreamTimer = 0.f;
			BroadcastFixtureStatesIfChanged(/*bForce*/ false);
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
	static const FRebusInlineIes EmptyInlineIes;
	static const FRebusInlineGobos EmptyInlineGobos;

	// Fresh "hero beam" volumetric-shadow budget per (re)spawn so the first N fixtures of this
	// scene get volumetric shadows (the cap is enforced in ARebusFixtureActor::BuildSpotLight).
	ARebusFixtureActor::ResetVolumetricShadowBudget();

	for (const FRebusSceneFixture& F : SceneData.Fixtures)
	{
		const FRebusFixtureProfile* Profile = F.LibraryFixtureId.IsEmpty() ? &EmptyProfile : ProfileCache.Find(F.LibraryFixtureId);
		if (!Profile) Profile = &EmptyProfile;
		const FRebusMeshBundle* Meshes = F.LibraryFixtureId.IsEmpty() ? &EmptyMeshes : MeshCache.Find(F.LibraryFixtureId);
		if (!Meshes) Meshes = &EmptyMeshes;
		const FRebusInlineIes* InlineIes = F.LibraryFixtureId.IsEmpty() ? &EmptyInlineIes : InlineIesCache.Find(F.LibraryFixtureId);
		if (!InlineIes) InlineIes = &EmptyInlineIes;
		const FRebusInlineGobos* InlineGobos = F.LibraryFixtureId.IsEmpty() ? &EmptyInlineGobos : InlineGoboCache.Find(F.LibraryFixtureId);
		if (!InlineGobos) InlineGobos = &EmptyInlineGobos;

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ARebusFixtureActor* Actor = World->SpawnActor<ARebusFixtureActor>(ARebusFixtureActor::StaticClass(), Params);
		if (!Actor) continue;

		Actor->SetRestClient(Rest);
		Actor->Setup(F, *Profile, *Meshes, *InlineIes, *InlineGobos);

		Ctl->RegisterFixture(F.Id, Actor); // register under the Speckle node id (§3)
		SpawnedFixtures.Add(Actor);
	}

	UE_LOG(LogRebusVisualiser, Log, TEXT("Spawned %d fixtures."), SpawnedFixtures.Num());

	// v1.0.47: one-shot batch summary of how many fixtures asked for / were granted volumetric
	// shadows, so the user can immediately diagnose missing truss-gap shafts (portal not sending
	// castVolumetricShadow=true, or the hero budget filtering beams out). Emitted always.
	ARebusFixtureActor::LogVolumetricShadowBudget(SpawnedFixtures.Num());

	// Reassert all stored scene-property values after a (re)spawn so the live state survives a
	// portal re-push / LoadScene rebuild (the env actors persist, but this keeps ground/quality/
	// fog authoritative and idempotent across rebuilds).
	if (URebusSceneSettingsSubsystem* Sce = GetSceneSettings())
	{
		Sce->ReapplyAll();
	}

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
	// v1.0.80: drop the per-fixture state-stream cache so the next scene's fixtures don't
	// get diffed against the previous scene's last-sent values (could suppress the first
	// state broadcast for an identical-id reload).
	LastSentFixtureStates.Reset();
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
		// Drop the inline-IES + inline-gobo caches and their accumulation scratch alongside the
		// fixtures, so a subsequent push starts clean (mirrors clearing the pushed scene).
		InlineIesCache.Empty();
		PendingIesProfiles.Empty();
		PendingIesChunksSeen.Empty();
		InlineGoboCache.Empty();
		PendingGoboEntries.Empty();
		PendingGoboChunksSeen.Empty();
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

	if (Type == TEXT("RegisterFixtureMeshes"))
	{
		// Additive, chunk-aware mesh delivery: a fixture whose full mesh bundle exceeds the
		// per-message budget is split across N messages, each carrying a SUBSET of meshes[].
		// We merge the chunks per libraryId and commit to MeshCache once all have arrived.
		// { libraryId, meshes:{ version, meshes:[...] }, chunkIndex?, chunkCount? }
		FString LibraryId;
		RebusJson::TryGetString(Msg, TEXT("libraryId"), LibraryId);
		const FString MeshJson = FieldToString(Msg, TEXT("meshes"));
		if (LibraryId.IsEmpty() || MeshJson.IsEmpty())
		{
			UE_LOG(LogRebusVisualiser, Warning, TEXT("RegisterFixtureMeshes missing libraryId/meshes; ignoring."));
			return;
		}

		FRebusMeshBundle Chunk;
		if (!RebusJson::ParseMeshBundle(MeshJson, Chunk))
		{
			UE_LOG(LogRebusVisualiser, Warning, TEXT("RegisterFixtureMeshes 'meshes' failed to parse (libraryId='%s'); ignoring."), *LibraryId);
			return;
		}

		double ChunkCountD = 1.0;
		double ChunkIndexD = 0.0;
		RebusJson::TryGetNumber(Msg, TEXT("chunkCount"), ChunkCountD);
		RebusJson::TryGetNumber(Msg, TEXT("chunkIndex"), ChunkIndexD);
		const int32 ChunkCount = FMath::Max(1, (int32)ChunkCountD);
		const int32 ChunkIndex = FMath::Max(0, (int32)ChunkIndexD);

		// Append this chunk's meshes into the per-libraryId accumulator.
		FRebusMeshBundle& Accum = PendingMeshChunks.FindOrAdd(LibraryId);
		if (Chunk.Version != 0) Accum.Version = Chunk.Version;
		const int32 MeshesInChunk = Chunk.Meshes.Num();
		Accum.Meshes.Append(MoveTemp(Chunk.Meshes));
		const int32 Seen = ++PendingMeshChunksSeen.FindOrAdd(LibraryId);

		UE_LOG(LogRebusVisualiser, Log,
			TEXT("RegisterFixtureMeshes '%s' chunk %d/%d (%d mesh(es) in chunk, %d accumulated)."),
			*LibraryId, ChunkIndex + 1, ChunkCount, MeshesInChunk, Accum.Meshes.Num());

		// Complete when we've seen all chunks (or this is a single non-chunked message).
		if (Seen < ChunkCount)
		{
			return;
		}

		FRebusMeshBundle Merged = MoveTemp(Accum);
		const int32 TotalMeshes = Merged.Meshes.Num();
		MeshCache.Add(LibraryId, MoveTemp(Merged));
		PendingMeshChunks.Remove(LibraryId);
		PendingMeshChunksSeen.Remove(LibraryId);

		UE_LOG(LogRebusVisualiser, Log,
			TEXT("RegisterFixtureMeshes '%s' complete: merged %d mesh(es) into the cache."),
			*LibraryId, TotalMeshes);

		// Re-apply to already-spawned fixtures so any beam/light-only fixtures of this libraryId
		// gain their geometry now that meshes arrived. Re-spawn only happens at load time and
		// SpawnAllFixtures re-broadcasts the handshake.
		if (bFixturesSpawned && SceneData.Fixtures.Num() > 0)
		{
			ClearSpawnedFixtures();
			SpawnAllFixtures();
		}
		return;
	}

	if (Type == TEXT("RegisterFixtureIes"))
	{
		// Inline raw IESNA LM-63 photometric *text* pushed over the data channel (REST-free
		// alternative to fetching a signed iesUrl). Two independent accumulation levels:
		//   * message-level (chunkIndex/chunkCount): profiles[] are appended per libraryId,
		//     order-independent, finalized once chunkCount messages arrive (like meshes);
		//   * per-profile fragmentation (part/partCount): within the accumulated entries we
		//     group by profileId and concatenate iesText fragments (sorted by part) when
		//     partCount > 1, otherwise the lone entry's iesText is the whole file.
		// { libraryId, profiles:[{ profileId, zoomDmx?, zoomAngleDeg?, beamAngleDeg?,
		//   fieldAngleDeg?, iesText, part?, partCount? }], chunkIndex?, chunkCount? }
		FString LibraryId;
		RebusJson::TryGetString(Msg, TEXT("libraryId"), LibraryId);
		const TArray<TSharedPtr<FJsonValue>>* ProfilesArr = nullptr;
		if (LibraryId.IsEmpty() || !Msg->TryGetArrayField(TEXT("profiles"), ProfilesArr) || !ProfilesArr)
		{
			UE_LOG(LogRebusVisualiser, Warning, TEXT("RegisterFixtureIes missing libraryId/profiles; ignoring."));
			return;
		}

		double ChunkCountD = 1.0;
		double ChunkIndexD = 0.0;
		RebusJson::TryGetNumber(Msg, TEXT("chunkCount"), ChunkCountD);
		RebusJson::TryGetNumber(Msg, TEXT("chunkIndex"), ChunkIndexD);
		const int32 ChunkCount = FMath::Max(1, (int32)ChunkCountD);
		const int32 ChunkIndex = FMath::Max(0, (int32)ChunkIndexD);

		// Append this message's profiles[] entries into the per-libraryId accumulator.
		TArray<FRebusInlineIesPending>& Accum = PendingIesProfiles.FindOrAdd(LibraryId);
		int32 EntriesInMsg = 0;
		for (const TSharedPtr<FJsonValue>& Val : *ProfilesArr)
		{
			const TSharedPtr<FJsonObject> PObj = Val.IsValid() ? Val->AsObject() : nullptr;
			if (!PObj.IsValid()) continue;

			FRebusInlineIesPending Entry;
			RebusJson::TryGetString(PObj, TEXT("profileId"), Entry.ProfileId);
			if (Entry.ProfileId.IsEmpty()) Entry.ProfileId = TEXT("default");

			double Num = 0.0;
			if (RebusJson::TryGetNumber(PObj, TEXT("zoomDmx"), Num)) Entry.ZoomDmx = (int32)Num;
			RebusJson::TryGetNumber(PObj, TEXT("zoomAngleDeg"), Entry.ZoomAngleDeg);
			RebusJson::TryGetNumber(PObj, TEXT("beamAngleDeg"), Entry.BeamAngleDeg);
			RebusJson::TryGetNumber(PObj, TEXT("fieldAngleDeg"), Entry.FieldAngleDeg);
			RebusJson::TryGetString(PObj, TEXT("iesText"), Entry.IesText);
			if (RebusJson::TryGetNumber(PObj, TEXT("part"), Num)) Entry.Part = (int32)Num;
			if (RebusJson::TryGetNumber(PObj, TEXT("partCount"), Num)) Entry.PartCount = FMath::Max(1, (int32)Num);

			Accum.Add(MoveTemp(Entry));
			++EntriesInMsg;
		}

		const int32 Seen = ++PendingIesChunksSeen.FindOrAdd(LibraryId);
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("RegisterFixtureIes '%s' chunk %d/%d (%d profile entr(ies) in msg, %d accumulated)."),
			*LibraryId, ChunkIndex + 1, ChunkCount, EntriesInMsg, Accum.Num());

		// Complete when we've seen all messages (or this is a single non-chunked message).
		if (Seen < ChunkCount)
		{
			return;
		}

		// Finalize: group by profileId, reassemble part fragments, build the per-profile cache.
		TArray<FRebusInlineIesPending> Pending = MoveTemp(Accum);
		PendingIesProfiles.Remove(LibraryId);
		PendingIesChunksSeen.Remove(LibraryId);

		// Group entries by profileId, preserving first-seen order.
		TArray<FString> Order;
		TMap<FString, TArray<FRebusInlineIesPending>> ByProfile;
		for (FRebusInlineIesPending& Entry : Pending)
		{
			TArray<FRebusInlineIesPending>& List = ByProfile.FindOrAdd(Entry.ProfileId);
			if (List.Num() == 0) Order.Add(Entry.ProfileId);
			List.Add(MoveTemp(Entry));
		}

		FRebusInlineIes Finalized;
		int32 TotalBytes = 0;
		for (const FString& Pid : Order)
		{
			TArray<FRebusInlineIesPending>& List = ByProfile[Pid];

			// If any fragment declares partCount > 1, this profile's iesText was split: sort by
			// part and concatenate to rebuild the full file. Otherwise a single entry holds it.
			bool bFragmented = false;
			for (const FRebusInlineIesPending& E : List) { if (E.PartCount > 1) { bFragmented = true; break; } }
			if (bFragmented)
			{
				List.Sort([](const FRebusInlineIesPending& A, const FRebusInlineIesPending& B)
				{
					return A.Part < B.Part;
				});
			}

			FRebusInlineIesProfile Out;
			Out.ProfileId = Pid;
			FString FullText;
			bool bMetaSet = false;
			for (const FRebusInlineIesPending& E : List)
			{
				FullText += E.IesText;
				if (!bMetaSet)
				{
					Out.ZoomDmx = E.ZoomDmx;
					Out.ZoomAngleDeg = E.ZoomAngleDeg;
					Out.BeamAngleDeg = E.BeamAngleDeg;
					Out.FieldAngleDeg = E.FieldAngleDeg;
					bMetaSet = true;
				}
			}
			if (FullText.IsEmpty())
			{
				continue; // nothing to build for this profileId
			}

			// .ies is ASCII text; carry it to the importer verbatim as UTF-8 bytes (same byte
			// buffer the URL-fetch path feeds to RebusIes::BuildLightProfile).
			const FTCHARToUTF8 Conv(*FullText);
			Out.Bytes.Append(reinterpret_cast<const uint8*>(Conv.Get()), Conv.Length());
			TotalBytes += Out.Bytes.Num();
			Finalized.Profiles.Add(MoveTemp(Out));
		}

		const int32 NumProfiles = Finalized.Profiles.Num();
		InlineIesCache.Add(LibraryId, MoveTemp(Finalized));

		UE_LOG(LogRebusVisualiser, Log,
			TEXT("RegisterFixtureIes '%s' complete: %d inline IES profile(s), %d total bytes."),
			*LibraryId, NumProfiles, TotalBytes);

		// Re-apply to already-spawned fixtures so the affected libraryId gets its true IES now
		// (consistent with the meshes path; SpawnAllFixtures re-broadcasts the handshake).
		if (bFixturesSpawned && SceneData.Fixtures.Num() > 0)
		{
			ClearSpawnedFixtures();
			SpawnAllFixtures();
		}
		return;
	}

	if (Type == TEXT("RegisterFixtureGobos"))
	{
		// Inline base64 gobo wheel images pushed over the data channel (REST-free alternative to
		// fetching a signed imageUrl). Same two-level accumulation as RegisterFixtureIes:
		//   * message-level (chunkIndex/chunkCount): gobos[] are appended per libraryId,
		//     order-independent, finalized once chunkCount messages arrive;
		//   * per-image fragmentation (part/partCount): within the accumulated entries we group
		//     by (wheel, slot) and concatenate dataBase64 fragments (sorted by part) BEFORE a
		//     single base64 decode, otherwise the lone entry's dataBase64 is the whole image.
		// { libraryId, gobos:[{ wheel, wheelKind|kind|type, slot, name, dataBase64|data,
		//   mime|contentType, imageUrl|url, part?, partCount? }], chunkIndex?, chunkCount? }
		//
		// Field-name aliases are accepted because the authoritative contract lives in the portal
		// docs; we log unknown variants by simply ignoring them (tolerant by design).
		auto FirstString = [](const TSharedPtr<FJsonObject>& Obj, std::initializer_list<const TCHAR*> Keys, FString& Out) -> bool
		{
			for (const TCHAR* Key : Keys)
			{
				if (RebusJson::TryGetString(Obj, Key, Out) && !Out.IsEmpty()) return true;
			}
			return false;
		};

		FString LibraryId;
		RebusJson::TryGetString(Msg, TEXT("libraryId"), LibraryId);
		const TArray<TSharedPtr<FJsonValue>>* GobosArr = nullptr;
		if (LibraryId.IsEmpty() || !Msg->TryGetArrayField(TEXT("gobos"), GobosArr) || !GobosArr)
		{
			UE_LOG(LogRebusVisualiser, Warning, TEXT("RegisterFixtureGobos missing libraryId/gobos; ignoring."));
			return;
		}

		double ChunkCountD = 1.0;
		double ChunkIndexD = 0.0;
		RebusJson::TryGetNumber(Msg, TEXT("chunkCount"), ChunkCountD);
		RebusJson::TryGetNumber(Msg, TEXT("chunkIndex"), ChunkIndexD);
		const int32 ChunkCount = FMath::Max(1, (int32)ChunkCountD);
		const int32 ChunkIndex = FMath::Max(0, (int32)ChunkIndexD);

		// Append this message's gobos[] entries into the per-libraryId accumulator.
		TArray<FRebusInlineGoboPending>& Accum = PendingGoboEntries.FindOrAdd(LibraryId);
		int32 EntriesInMsg = 0;
		for (const TSharedPtr<FJsonValue>& Val : *GobosArr)
		{
			const TSharedPtr<FJsonObject> GObj = Val.IsValid() ? Val->AsObject() : nullptr;
			if (!GObj.IsValid()) continue;

			FRebusInlineGoboPending Entry;
			RebusJson::TryGetString(GObj, TEXT("wheel"), Entry.Wheel);
			FirstString(GObj, { TEXT("wheelKind"), TEXT("kind"), TEXT("type") }, Entry.WheelKind);
			Entry.WheelKind.ToLowerInline();
			RebusJson::TryGetString(GObj, TEXT("name"), Entry.Name);
			FirstString(GObj, { TEXT("slotName"), TEXT("slot_name") }, Entry.SlotName);
			FirstString(GObj, { TEXT("mime"), TEXT("contentType") }, Entry.Mime);
			FirstString(GObj, { TEXT("imageUrl"), TEXT("url") }, Entry.ImageUrl);
			FirstString(GObj, { TEXT("dataBase64"), TEXT("data") }, Entry.DataBase64);

			double Num = 0.0;
			// wheelIndex is the PRIMARY key: 0-based into the full wheels[] (NOT gobo-kind only).
			if (RebusJson::TryGetNumber(GObj, TEXT("wheelIndex"), Num)) Entry.WheelIndex = (int32)Num;
			if (RebusJson::TryGetNumber(GObj, TEXT("slot"), Num)) Entry.Slot = (int32)Num;
			if (RebusJson::TryGetNumber(GObj, TEXT("part"), Num)) Entry.Part = (int32)Num;
			if (RebusJson::TryGetNumber(GObj, TEXT("partCount"), Num)) Entry.PartCount = FMath::Max(1, (int32)Num);

			Accum.Add(MoveTemp(Entry));
			++EntriesInMsg;
		}

		const int32 Seen = ++PendingGoboChunksSeen.FindOrAdd(LibraryId);
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("RegisterFixtureGobos '%s' chunk %d/%d (%d gobo entr(ies) in msg, %d accumulated)."),
			*LibraryId, ChunkIndex + 1, ChunkCount, EntriesInMsg, Accum.Num());

		// Complete when we've seen all messages (or this is a single non-chunked message).
		if (Seen < ChunkCount)
		{
			return;
		}

		// Finalize: group by (wheel, slot), reassemble part fragments, base64-decode once each.
		TArray<FRebusInlineGoboPending> Pending = MoveTemp(Accum);
		PendingGoboEntries.Remove(LibraryId);
		PendingGoboChunksSeen.Remove(LibraryId);

		// Group by the PRIMARY cache key (wheelIndex, slot), preserving first-seen order. The
		// unit-separator key avoids collisions (e.g. wheelIndex 1 slot 23 vs 12 slot 3). When an
		// entry carries no explicit wheelIndex (legacy push) we key by wheel NAME instead; the
		// 'i'/'n' prefix keeps the two namespaces from colliding (index 1 vs wheel named "1").
		TArray<FString> Order;
		TMap<FString, TArray<FRebusInlineGoboPending>> ByKey;
		for (FRebusInlineGoboPending& Entry : Pending)
		{
			const FString Key = (Entry.WheelIndex != INDEX_NONE)
				? FString::Printf(TEXT("i%d\x1f%d"), Entry.WheelIndex, Entry.Slot)
				: FString::Printf(TEXT("n%s\x1f%d"), *Entry.Wheel, Entry.Slot);
			TArray<FRebusInlineGoboPending>& List = ByKey.FindOrAdd(Key);
			if (List.Num() == 0) Order.Add(Key);
			List.Add(MoveTemp(Entry));
		}

		FRebusInlineGobos Finalized;
		int32 TotalBytes = 0;
		TSet<FString> WheelSet;
		for (const FString& Key : Order)
		{
			TArray<FRebusInlineGoboPending>& List = ByKey[Key];

			// If any fragment declares partCount > 1, this image's dataBase64 was split: sort by
			// part and concatenate to rebuild the full base64 string BEFORE decoding.
			bool bFragmented = false;
			for (const FRebusInlineGoboPending& E : List) { if (E.PartCount > 1) { bFragmented = true; break; } }
			if (bFragmented)
			{
				List.Sort([](const FRebusInlineGoboPending& A, const FRebusInlineGoboPending& B)
				{
					return A.Part < B.Part;
				});
			}

			FRebusInlineGobo Out;
			FString FullBase64;
			bool bMetaSet = false;
			for (const FRebusInlineGoboPending& E : List)
			{
				FullBase64 += E.DataBase64;
				if (!bMetaSet)
				{
					Out.WheelIndex = E.WheelIndex;
					Out.Wheel = E.Wheel;
					Out.WheelKind = E.WheelKind;
					Out.Slot = E.Slot;
					Out.Name = E.Name;
					Out.SlotName = E.SlotName;
					Out.Mime = E.Mime;
					Out.ImageUrl = E.ImageUrl;
					bMetaSet = true;
				}
				else if (Out.ImageUrl.IsEmpty() && !E.ImageUrl.IsEmpty())
				{
					Out.ImageUrl = E.ImageUrl; // keep a fallback url from any fragment
				}
			}

			if (!FullBase64.IsEmpty())
			{
				if (!FBase64::Decode(FullBase64, Out.Bytes))
				{
					UE_LOG(LogRebusVisualiser, Warning,
						TEXT("RegisterFixtureGobos '%s' wheel='%s' slot=%d: base64 decode failed; keeping url fallback."),
						*LibraryId, *Out.Wheel, Out.Slot);
					Out.Bytes.Reset();
				}
			}

			// v1.0.49: detect the OPEN slot (no-gobo position) from the slotName/Name so the runtime
			// can clear the gobo when the user selects it. Pre-v1.0.49 we DROPPED entries with no
			// payload, so Open vanished from the cache and the cone kept the last gobo. Now we KEEP
			// Open entries with bIsOpen=true (no bytes, no url), and only drop slots that have no
			// payload AND no Open marker (e.g. a wheel the portal hasn't pushed images for yet).
			const bool bIsOpen = ARebusFixtureActor::IsOpenSlotName(Out.SlotName)
				|| ARebusFixtureActor::IsOpenSlotName(Out.Name);
			Out.bIsOpen = bIsOpen;

			if (Out.Bytes.Num() == 0 && Out.ImageUrl.IsEmpty() && !bIsOpen)
			{
				continue;
			}
			// v1.0.48: per-slot finalize log so the user can see EXACTLY which (wheel, slot)
			// entries actually assembled, with their byte counts and url fallback presence.
			// v1.0.49: also reports the bIsOpen marker so the user can confirm Open-slot detection.
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("RegisterFixtureGobos '%s' finalized: wheelIndex=%d wheel='%s'(kind=%s) slot=%d slotName='%s' bytes=%d urlFallback=%d isOpen=%d"),
				*LibraryId, Out.WheelIndex, *Out.Wheel, *Out.WheelKind, Out.Slot, *Out.SlotName,
				Out.Bytes.Num(), Out.ImageUrl.IsEmpty() ? 0 : 1, bIsOpen ? 1 : 0);
			TotalBytes += Out.Bytes.Num();
			WheelSet.Add(Out.Wheel);
			Finalized.Gobos.Add(MoveTemp(Out));
		}

		const int32 NumGobos = Finalized.Gobos.Num();
		const int32 NumWheels = WheelSet.Num();
		InlineGoboCache.Add(LibraryId, MoveTemp(Finalized));

		UE_LOG(LogRebusVisualiser, Log,
			TEXT("RegisterFixtureGobos '%s' complete: %d gobo image(s) across %d wheel(s), %d total bytes."),
			*LibraryId, NumGobos, NumWheels, TotalBytes);

		// Targeted re-apply: push the new inline gobos into the already-spawned fixtures of this
		// libraryId and re-assign their currently-selected gobo, so it appears without a reselect
		// (no full re-spawn needed, which would otherwise drop the live gobo selection).
		if (bFixturesSpawned)
		{
			const FRebusInlineGobos& Cached = InlineGoboCache[LibraryId];
			for (ARebusFixtureActor* F : SpawnedFixtures)
			{
				if (F && F->GetLibraryFixtureId() == LibraryId)
				{
					F->SetInlineGobos(Cached);
				}
			}
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
	// Ready requires ALL of: channel open + scene loaded (so loadedModel counts are final) + the
	// scene environment ensured. The environment gate is the first-load fix: the portal starts
	// pushing SetSceneProperty/LoadScene as soon as it sees Ready, so the fog/post-process/floor
	// actors those pushes target MUST exist first -- otherwise early pushes hit missing actors,
	// are stored-but-not-applied, and only "take" after a recycle.
	if (bReadySent || !bChannelReady || !bSceneLoaded || !bEnvEnsured || !Channel.IsValid())
	{
		return;
	}
	bReadySent = true;
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Handshake: Ready (channel open, scene loaded, environment ensured) -> broadcasting."));
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
			F->GetDisplayName(), F->HasPanTilt(), F->HasGobo(),
			F->GetMotionAxisCount(), F->GetMeshComponentCount());
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

	// v1.0.79: push a fresh CameraState so the reconnecting viewer's portal UI paints the
	// current pose + lens immediately instead of waiting for the first dead-zone delta.
	BroadcastCameraStateIfChanged(/*bForce*/ true);

	// v1.0.80: same for the live fixture stream + selection. Forces a FULL snapshot (every
	// spawned fixture, full=true) and a SelectionState push so a late/reconnecting portal
	// paints the live rig immediately. Sent AFTER the per-fixture SendFixtureRegistered
	// loop above so the portal can index by id before the state arrives.
	BroadcastFixtureStatesIfChanged(/*bForce*/ true);
	if (URebusFixtureControlSubsystem* Ctl = GetControl())
	{
		Channel->SendSelectionState(Ctl->GetCurrentSelection(), Ctl->GetPrimarySelection());
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
				// Volumetric fog tuned for haze/beam visibility so per-fixture spotlights
				// scatter (§8.4). Mirrors the authoring-time defaults in
				// build_rebus_base_level.py so a fresh spawn matches the baked level.
				C->SetVolumetricFog(true);
				C->SetVolumetricFogDistance(35000.f);            // cm; far enough for stage beams
				C->SetVolumetricFogExtinctionScale(0.3f);        // subtle haze, not a wall of fog
				C->SetVolumetricFogScatteringDistribution(0.4f); // mild forward scatter
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

	// v1.0.100 -- spawn pose comes from the shared RebusCineCameraDefaults namespace
	// (RebusCineCameraPawn.h). Location/target are authored in metres (operator framing
	// convention) and converted to cm in the header. The rotator is DERIVED from
	// (target - location) via FRotationMatrix::MakeFromX so any future tweak to the metres
	// triples re-derives the aim cleanly (no manual yaw/pitch trig in this file). The
	// ApplyTransform call below mirrors what ARebusCineCameraPawn::ResetToDefaults does, so
	// the spawn-time pose and the Rebus.CameraReset pose are byte-for-byte identical.
	const FVector&  SpawnLocation = RebusCineCameraDefaults::kDefaultCameraLocation_cm;
	const FRotator& SpawnRotation = RebusCineCameraDefaults::kDefaultCameraRotation;

	// v1.0.79: ensure the player is possessing a cinematic camera pawn (replaces UE's stock
	// ADefaultPawn UCameraComponent with a UCineCameraComponent + manual exposure). If the PC
	// is currently possessing the engine default pawn, spawn ours, possess it, then destroy
	// the old pawn. Done here (rather than via a custom GameMode) so the existing
	// retry-until-PC-exists pattern of TryPositionPlayerView is preserved.
	if (!CineCameraPawn.Get() || CineCameraPawn->IsActorBeingDestroyed())
	{
		FActorSpawnParameters Spawn;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ARebusCineCameraPawn* NewPawn = World->SpawnActor<ARebusCineCameraPawn>(
			ARebusCineCameraPawn::StaticClass(),
			SpawnLocation,
			SpawnRotation,
			Spawn);
		if (!NewPawn) return false; // try again next tick (world may not be fully ready yet)

		APawn* OldPawn = PC->GetPawn();
		if (OldPawn != NewPawn)
		{
			PC->UnPossess();
			PC->Possess(NewPawn);
			if (OldPawn) OldPawn->Destroy();
		}
		CineCameraPawn = NewPawn;
		UE_LOG(LogRebusVisualiser, Log, TEXT("RebusCineCameraPawn spawned + possessed (replacing default pawn)."));

		// v1.0.82: TrySendReady doesn't gate on bViewPositioned, so the cine pawn can spawn
		// AFTER bReadySent flipped (handshake fired with a null pawn -> no initial CameraState
		// went out). Force-push now that the pawn is finally alive so the portal receives the
		// initial snapshot without having to send a RequestCameraState or wiggle the camera.
		if (bReadySent && Channel.IsValid())
		{
			UE_LOG(LogRebusVisualiser, Log, TEXT("RebusCineCameraPawn first-spawn after Ready -> forcing initial CameraState broadcast."));
			BroadcastCameraStateIfChanged(/*bForce*/ true);
		}
	}

	if (ARebusCineCameraPawn* Cam = CineCameraPawn.Get())
	{
		Cam->ApplyTransform(SpawnLocation, SpawnRotation);
	}

	UE_LOG(LogRebusVisualiser, Log, TEXT("Player view positioned at %s facing %s (v1.0.100 default)."),
		*SpawnLocation.ToString(), *SpawnRotation.ToString());
	return true;
}

bool URebusVisualiserSubsystem::HandleCameraDescriptor(const FString& Type, const TSharedPtr<FJsonObject>& Msg)
{
	if (!Msg.IsValid()) return false;

	// v1.0.82: trace EVERY camera descriptor + the pawn-availability state. Diagnoses the
	// "I sent SetCameraTransform but nothing happened / no CameraState came back" symptom by
	// proving (a) the descriptor reached the camera handler, (b) the cine pawn was alive at
	// the moment of dispatch, and (c) a force-push was scheduled. Cheap (one log line per
	// inbound command -- portals send these at <30Hz).
	const bool bIsCamera =
		Type == TEXT("RequestCameraState") || Type == TEXT("SetCameraTransform") ||
		Type == TEXT("SetCameraFocalLength") || Type == TEXT("SetCameraAperture") ||
		Type == TEXT("SetCameraFocusDistance") || Type == TEXT("SetCameraExposure") ||
		Type == TEXT("SetCameraSensor");
	if (bIsCamera)
	{
		ARebusCineCameraPawn* CamLog = CineCameraPawn.Get();
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("HandleCameraDescriptor: type='%s' pawn=%s readySent=%d channel=%d"),
			*Type,
			CamLog ? *CamLog->GetName() : TEXT("NULL"),
			(int32)bReadySent,
			(int32)Channel.IsValid());
	}

	// RequestCameraState is handled even if the pawn isn't ready yet -- we just answer with
	// a zero snapshot so the portal's UI doesn't freeze waiting for a response that depends
	// on a still-pending world spawn.
	if (Type == TEXT("RequestCameraState"))
	{
		if (Channel.IsValid())
		{
			ARebusCineCameraPawn* Cam = CineCameraPawn.Get();
			Channel->SendCameraState(Cam ? Cam->GetCameraState() : FRebusCameraState());
		}
		return true;
	}

	// Everything else needs a live pawn.
	ARebusCineCameraPawn* Cam = CineCameraPawn.Get();
	if (bIsCamera && !Cam)
	{
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("HandleCameraDescriptor '%s': cine pawn not spawned yet -- descriptor accepted but no effect. Pawn spawns in TryPositionPlayerView once the PlayerController + world exist; retry after stream start, or send RequestCameraState to confirm streaming is live."),
			*Type);
	}
	if (Type == TEXT("SetCameraTransform"))
	{
		const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
		FVector  Loc = Cam ? Cam->GetActorLocation() : FVector::ZeroVector;
		FRotator Rot = Cam ? Cam->GetActorRotation() : FRotator::ZeroRotator;
		if (Msg->TryGetArrayField(TEXT("loc"), LocArr) && LocArr && LocArr->Num() == 3)
		{
			Loc = FVector((*LocArr)[0]->AsNumber(), (*LocArr)[1]->AsNumber(), (*LocArr)[2]->AsNumber());
		}
		if (Msg->TryGetArrayField(TEXT("rot"), RotArr) && RotArr && RotArr->Num() == 3)
		{
			Rot = FRotator((*RotArr)[0]->AsNumber(), (*RotArr)[1]->AsNumber(), (*RotArr)[2]->AsNumber());
		}
		if (Cam)
		{
			Cam->ApplyTransform(Loc, Rot);
			BroadcastCameraStateIfChanged(/*bForce*/ true);
		}
		return true;
	}
	if (Type == TEXT("SetCameraFocalLength"))
	{
		double Mm = 35.0;
		if (RebusJson::TryGetNumber(Msg, TEXT("mm"), Mm) && Cam)
		{
			Cam->SetFocalLengthMm((float)Mm);
			BroadcastCameraStateIfChanged(true);
		}
		return true;
	}
	if (Type == TEXT("SetCameraAperture"))
	{
		double FStop = 2.8;
		if (RebusJson::TryGetNumber(Msg, TEXT("fStop"), FStop) && Cam)
		{
			Cam->SetAperture((float)FStop);
			BroadcastCameraStateIfChanged(true);
		}
		return true;
	}
	if (Type == TEXT("SetCameraFocusDistance"))
	{
		// Two shapes: { "cm": 500 } sets manual focus; { "auto": true } enables tracking-AF.
		bool bAuto = false;
		if (RebusJson::TryGetBool(Msg, TEXT("auto"), bAuto) && bAuto)
		{
			if (Cam) { Cam->SetManualFocus(false); BroadcastCameraStateIfChanged(true); }
			return true;
		}
		double Cm = 500.0;
		if (RebusJson::TryGetNumber(Msg, TEXT("cm"), Cm) && Cam)
		{
			Cam->SetManualFocus(true);
			Cam->SetFocusDistanceCm((float)Cm);
			BroadcastCameraStateIfChanged(true);
		}
		return true;
	}
	if (Type == TEXT("SetCameraExposure"))
	{
		double Ev = 0.0;
		if (RebusJson::TryGetNumber(Msg, TEXT("ev"), Ev) && Cam)
		{
			Cam->SetExposureBiasEv((float)Ev);
			BroadcastCameraStateIfChanged(true);
		}
		return true;
	}
	if (Type == TEXT("SetCameraSensor"))
	{
		double WMm = 24.89, HMm = 18.66;
		RebusJson::TryGetNumber(Msg, TEXT("widthMm"),  WMm);
		RebusJson::TryGetNumber(Msg, TEXT("heightMm"), HMm);
		if (Cam)
		{
			Cam->SetSensorSizeMm((float)WMm, (float)HMm);
			BroadcastCameraStateIfChanged(true);
		}
		return true;
	}

	return false;
}

bool URebusVisualiserSubsystem::HandleStateSyncDescriptor(const FString& Type, const TSharedPtr<FJsonObject>& Msg)
{
	if (Type == TEXT("RequestFixtureStates"))
	{
		BroadcastFixtureStatesIfChanged(/*bForce*/ true);
		return true;
	}
	if (Type == TEXT("RequestSelectionState"))
	{
		NotifySelectionChanged();
		return true;
	}
	return false;
}

void URebusVisualiserSubsystem::NotifyFixtureControlMutated()
{
	// The periodic tick (~10Hz) already catches fades as they progress -- this is a no-op for
	// fade-style commands. For instantaneous changes (shutter mode, gobo index) it still
	// arrives at most ~100ms later, which is below the perceptual threshold for "did my UI
	// update". Left as a stub so the data channel routing layer has a forward-compat hook;
	// if a future control type needs <100ms-latency state echo, this is where it lands.
}

void URebusVisualiserSubsystem::NotifySelectionChanged()
{
	if (!Channel.IsValid()) return;
	if (URebusFixtureControlSubsystem* Ctl = GetControl())
	{
		Channel->SendSelectionState(Ctl->GetCurrentSelection(), Ctl->GetPrimarySelection());
		// Selection is also a per-fixture state field; refresh the per-fixture cache so the
		// next periodic fixture-state tick doesn't push stale selected/primary flags.
		BroadcastFixtureStatesIfChanged(/*bForce*/ true);
	}
}

void URebusVisualiserSubsystem::NotifySceneSettingsChanged()
{
	if (!Channel.IsValid()) return;
	if (URebusSceneSettingsSubsystem* Sce = GetSceneSettings())
	{
		Channel->SendSceneState(Sce->GetSceneState());
	}
}

void URebusVisualiserSubsystem::BroadcastFixtureStatesIfChanged(bool bForce)
{
	if (!Channel.IsValid()) return;
	URebusFixtureControlSubsystem* Ctl = GetControl();
	if (!Ctl) return;

	const TArray<FString>& SelectedIds = Ctl->GetCurrentSelection();
	const FString& PrimaryId = Ctl->GetPrimarySelection();
	const TSet<FString> SelectedSet(SelectedIds);

	// Per-field dead zones. Tuned so a smooth fade tick (~0.01/frame) goes out, but tiny
	// numerical jitter from FInterp easing rounding does not.
	auto Approx = [](float A, float B, float Tol) { return FMath::Abs(A - B) <= Tol; };

	TArray<FRebusFixtureStateSnapshot> Batch;
	Batch.Reserve(SpawnedFixtures.Num());

	// Cull stale ids from the cache (fixtures destroyed since the last broadcast). Without
	// this the cache grows unbounded across ClearScene -> reload cycles.
	if (LastSentFixtureStates.Num() > 0)
	{
		TSet<FString> LiveIds;
		LiveIds.Reserve(SpawnedFixtures.Num());
		for (ARebusFixtureActor* F : SpawnedFixtures) if (F) LiveIds.Add(F->GetFixtureId());
		for (auto It = LastSentFixtureStates.CreateIterator(); It; ++It)
		{
			if (!LiveIds.Contains(It.Key())) It.RemoveCurrent();
		}
	}

	for (ARebusFixtureActor* F : SpawnedFixtures)
	{
		if (!F) continue;
		FRebusFixtureStateSnapshot S = F->GetFixtureStateSnapshot();
		S.bSelected = SelectedSet.Contains(S.FixtureId);
		S.bPrimarySelected = (PrimaryId == S.FixtureId);

		bool bChanged = bForce;
		if (!bChanged)
		{
			const FRebusFixtureStateSnapshot* Last = LastSentFixtureStates.Find(S.FixtureId);
			if (!Last)
			{
				bChanged = true; // first time seeing this fixture -- emit
			}
			else
			{
				// Single dead-zone block: any field over its threshold flags the whole
				// fixture for re-send. The full struct ships either way, so we don't gain
				// anything by tracking per-field changes.
				bChanged =
					!Approx(S.Dimmer,         Last->Dimmer,         0.002f) ||
					!Approx(S.PanDeg,         Last->PanDeg,         0.05f)  ||
					!Approx(S.TiltDeg,        Last->TiltDeg,        0.05f)  ||
					!Approx(S.ZoomDeg,        Last->ZoomDeg,        0.05f)  ||
					!Approx(S.Iris,           Last->Iris,           0.002f) ||
					!Approx(S.Frost,          Last->Frost,          0.002f) ||
					!Approx(S.Focus,          Last->Focus,          0.002f) ||
					!Approx(S.Color.R,        Last->Color.R,        0.002f) ||
					!Approx(S.Color.G,        Last->Color.G,        0.002f) ||
					!Approx(S.Color.B,        Last->Color.B,        0.002f) ||
					!Approx(S.ColorTempK,     Last->ColorTempK,     1.f)    ||
					!Approx(S.ShutterRateHz,  Last->ShutterRateHz,  0.01f)  ||
					!Approx(S.GoboRotSpeed,   Last->GoboRotSpeed,   0.002f) ||
					!Approx(S.AnimWheelSpeed, Last->AnimWheelSpeed, 0.002f) ||
					S.ShutterMode    != Last->ShutterMode    ||
					S.GoboIndex      != Last->GoboIndex      ||
					S.GoboWheelIndex != Last->GoboWheelIndex ||
					S.bSelected      != Last->bSelected      ||
					S.bPrimarySelected != Last->bPrimarySelected;
			}
		}

		if (bChanged)
		{
			Batch.Add(S);
			LastSentFixtureStates.Add(S.FixtureId, S);
		}
	}

	if (Batch.Num() == 0) return;
	Channel->SendFixtureStates(Batch, /*bIsFullSnapshot*/ bForce);
}

void URebusVisualiserSubsystem::BroadcastCameraStateIfChanged(bool bForce)
{
	if (!Channel.IsValid())
	{
		// Only log on bForce to avoid 30Hz spam when the streamer detaches mid-session.
		if (bForce) UE_LOG(LogRebusVisualiser, Warning, TEXT("BroadcastCameraStateIfChanged(force=1): Channel not valid -- nothing to send."));
		return;
	}
	ARebusCineCameraPawn* Cam = CineCameraPawn.Get();
	if (!Cam)
	{
		if (bForce) UE_LOG(LogRebusVisualiser, Warning, TEXT("BroadcastCameraStateIfChanged(force=1): cine pawn null -- portal will see zero state. Wait for TryPositionPlayerView to spawn it."));
		return;
	}

	const FRebusCameraState S = Cam->GetCameraState();

	// Dead zone (cm / deg / mm / f-stop / cm / EV) so a stationary camera doesn't push 30
	// identical messages/sec. Loose enough to avoid jitter spam but tight enough that any
	// user-perceptible change goes out within one broadcast slot.
	const bool bMoved =
		!S.Location.Equals(LastSentCameraState.Location, 0.1f) ||
		!S.Rotation.Equals(LastSentCameraState.Rotation, 0.05f) ||
		!FMath::IsNearlyEqual(S.FocalLengthMm,  LastSentCameraState.FocalMm,    0.1f) ||
		!FMath::IsNearlyEqual(S.Aperture,        LastSentCameraState.Aperture,   0.01f) ||
		!FMath::IsNearlyEqual(S.FocusDistanceCm, LastSentCameraState.FocusCm,    0.5f) ||
		!FMath::IsNearlyEqual(S.ExposureBiasEv,  LastSentCameraState.ExposureEv, 0.005f) ||
		(S.bManualFocus != LastSentCameraState.bManualFocus);

	if (!bForce && !bMoved) return;

	// v1.0.82: log every actual send (force OR delta). The data channel layer already logs
	// "Sending 'CameraState' (Response, N players)" with the connected-player count, so this
	// pair-of-logs lets the operator follow the chain end-to-end: source -> wire -> portal.
	UE_LOG(LogRebusVisualiser, Verbose,
		TEXT("BroadcastCameraStateIfChanged: force=%d moved=%d -> SendCameraState (loc=%s rot=%s focal=%.1fmm fStop=%.2f focus=%.0fcm ev=%.2f)"),
		(int32)bForce, (int32)bMoved,
		*S.Location.ToString(), *S.Rotation.ToString(),
		S.FocalLengthMm, S.Aperture, S.FocusDistanceCm, S.ExposureBiasEv);

	Channel->SendCameraState(S);
	LastSentCameraState.Location     = S.Location;
	LastSentCameraState.Rotation     = S.Rotation;
	LastSentCameraState.FocalMm      = S.FocalLengthMm;
	LastSentCameraState.Aperture     = S.Aperture;
	LastSentCameraState.FocusCm      = S.FocusDistanceCm;
	LastSentCameraState.ExposureEv   = S.ExposureBiasEv;
	LastSentCameraState.bManualFocus = S.bManualFocus;
}

// =========================================================================================
// v1.0.85 truss / set-piece material override
// =========================================================================================

void URebusVisualiserSubsystem::EnsureTrussMaterial()
{
	// Preferred path: a user-authored M_RebusTruss .uasset at /Game/REBUS/Materials/. If
	// present we use it verbatim (no parameter mangling). Mirrors the same convention as
	// ARebusFixtureActor::FixtureBodyMaterialOverride so an operator with one project setup
	// gets matching materials for fixture bodies, lenses, and truss.
	if (!TrussMaterialOverride)
	{
		TrussMaterialOverride = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/REBUS/Materials/M_RebusTruss.M_RebusTruss"));
	}
	if (TrussMaterialOverride) return; // user .uasset wins, no MID needed

	// Fallback: parametric MID built off BasicShapeMaterial. Same pattern the fixture-body
	// override uses, with PBR knobs tuned for a real powdercoat finish:
	//   * BaseColor #040404 -- slightly above pure black so the surface catches highlights
	//                          and reads as "matte black material" instead of "void". Pure
	//                          black crushes contrast and trusses end up looking 2D.
	//   * Roughness 0.55    -- powdercoat is microscopically textured but not as rough as raw
	//                          steel; ~0.55 lands between satin (0.4) and matte (0.7) and is
	//                          what real Prolyte / Eurotruss profiles meter at in a Macbeth
	//                          chart shoot.
	//   * Metallic 0.0      -- powdercoat is a polymer coating over aluminium; the lit surface
	//                          is dielectric, not metallic. Setting Metallic>0 would give the
	//                          truss an aluminium-mirror sheen that breaks the illusion.
	//   * Specular 0.5      -- BasicShapeMaterial respects this; default is 0.5 (water/plastic
	//                          F0) which is correct for polymer powdercoat.
	if (!TrussMatParent)
	{
		TrussMatParent = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	}
	if (TrussMatParent && !TrussMaterialMID)
	{
		TrussMaterialMID = UMaterialInstanceDynamic::Create(TrussMatParent, this);
		if (TrussMaterialMID)
		{
			TrussMaterialMID->SetVectorParameterValue(TEXT("Color"),     FLinearColor(0.015f, 0.015f, 0.015f, 1.f));
			TrussMaterialMID->SetScalarParameterValue(TEXT("Roughness"), 0.55f);
			TrussMaterialMID->SetScalarParameterValue(TEXT("Metallic"),  0.0f);
			TrussMaterialMID->SetScalarParameterValue(TEXT("Specular"),  0.5f);
		}
	}
}

UMaterialInterface* URebusVisualiserSubsystem::ResolveTrussMaterial()
{
	EnsureTrussMaterial();
	if (TrussMaterialOverride) return TrussMaterialOverride;
	if (TrussMaterialMID)      return TrussMaterialMID;
	return nullptr;
}

void URebusVisualiserSubsystem::BuildBoundOrbitComponentSet(TSet<UPrimitiveComponent*>& Out) const
{
	// Every ARebusFixtureActor in the active world publishes a list of Orbit components it
	// has bound (the body / yoke / head meshes under OrbitImportRoot that RebindOrbitModels
	// matched). Those keep the v1.0.71 fixture body+lens override and must NOT be touched by
	// the truss pass, otherwise the user would see the body material flip to truss material
	// the second the orbit rebind cycle runs. GetBoundOrbitPrimitives also walks descendants
	// so nested mesh trees under a transform-only Orbit node are correctly excluded.
	UWorld* World = GetActiveWorld();
	if (!World) return;
	for (TActorIterator<ARebusFixtureActor> It(World); It; ++It)
	{
		if (ARebusFixtureActor* F = *It)
		{
			F->GetBoundOrbitPrimitives(Out);
		}
	}
}

URebusVisualiserSubsystem::FTrussMaterialApplyCount URebusVisualiserSubsystem::ApplyTrussMaterialPass()
{
	FTrussMaterialApplyCount Count;
	if (!bTrussMaterialOverrideEnabled) return Count;

	UWorld* World = GetActiveWorld();
	if (!World) return Count;

	UMaterialInterface* Mat = ResolveTrussMaterial();
	if (!Mat) return Count; // material missing entirely (BasicShapeMaterial failed to load) -- bail

	TSet<UPrimitiveComponent*> Bound;
	BuildBoundOrbitComponentSet(Bound);

	// Drop dead entries from the cache (component or material got GC'd while we weren't
	// looking). Walk back-to-front so RemoveAtSwap doesn't shuffle the index we're iterating.
	for (int32 i = TrussMaterialCache.Num() - 1; i >= 0; --i)
	{
		if (!TrussMaterialCache[i].Comp.IsValid())
		{
			TrussMaterialCache.RemoveAtSwap(i, EAllowShrinking::No);
		}
	}

	// Build a quick lookup for "is this component already in the cache" so we know whether
	// the first-time snapshot capture path needs to run.
	TMap<UPrimitiveComponent*, int32> CacheIndex;
	CacheIndex.Reserve(TrussMaterialCache.Num());
	for (int32 i = 0; i < TrussMaterialCache.Num(); ++i)
	{
		if (UPrimitiveComponent* C = TrussMaterialCache[i].Comp.Get())
		{
			CacheIndex.Add(C, i);
		}
	}

	// Match RebindOrbitModels: find OrbitImportRoot actors by class-name so we keep zero
	// compile dependency on the separately-owned OrbitConnector plugin.
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor->GetClass()->GetName() != TEXT("OrbitImportRoot")) continue;

		TArray<UPrimitiveComponent*> Prims;
		Actor->GetComponents<UPrimitiveComponent>(Prims);
		for (UPrimitiveComponent* Prim : Prims)
		{
			if (!Prim) continue;
			++Count.Components;

			if (Bound.Contains(Prim))
			{
				++Count.SkippedBound;
				continue;
			}

			const int32 NumSlots = Prim->GetNumMaterials();
			if (NumSlots <= 0) continue;

			// First time we see this component, snapshot every original material slot so the
			// OFF path restores byte-exact. Existing-cache fast-path skips re-snapshotting
			// (otherwise a re-apply pass would overwrite the genuine original with the truss
			// material we just set last cycle).
			int32 EntryIdx = INDEX_NONE;
			if (int32* Existing = CacheIndex.Find(Prim))
			{
				EntryIdx = *Existing;
			}
			else
			{
				FTrussMaterialEntry E;
				E.Comp = Prim;
				E.OriginalMaterials.Reserve(NumSlots);
				for (int32 s = 0; s < NumSlots; ++s)
				{
					E.OriginalMaterials.Add(Prim->GetMaterial(s));
				}
				EntryIdx = TrussMaterialCache.Add(MoveTemp(E));
				CacheIndex.Add(Prim, EntryIdx);
			}
			(void)EntryIdx;

			// Apply to every slot. SetMaterial is a render-state-dirty no-op when the slot
			// already has Mat (UE's MaterialOverrides setter compares first), so this stays
			// cheap on the steady-state re-apply pass.
			bool bAnyApplied = false;
			for (int32 s = 0; s < NumSlots; ++s)
			{
				if (Prim->GetMaterial(s) != Mat)
				{
					Prim->SetMaterial(s, Mat);
					bAnyApplied = true;
				}
			}
			if (bAnyApplied) ++Count.Touched;
		}
	}
	return Count;
}

URebusVisualiserSubsystem::FTrussMaterialApplyCount URebusVisualiserSubsystem::SetTrussMaterialOverrideEnabled(bool bEnabled)
{
	FTrussMaterialApplyCount Count;
	bTrussMaterialOverrideEnabled = bEnabled;

	if (bEnabled)
	{
		return ApplyTrussMaterialPass();
	}

	// Disable path: restore every cached original (slot-aligned) and clear the cache so a
	// later re-enable starts from a fresh snapshot of whatever the import currently has.
	for (FTrussMaterialEntry& E : TrussMaterialCache)
	{
		UPrimitiveComponent* P = E.Comp.Get();
		if (!P) continue;
		const int32 NumSlots = FMath::Min(P->GetNumMaterials(), E.OriginalMaterials.Num());
		bool bAnyRestored = false;
		for (int32 s = 0; s < NumSlots; ++s)
		{
			UMaterialInterface* Orig = E.OriginalMaterials[s].Get();
			P->SetMaterial(s, Orig); // null is fine -- restores to "no material" if the original was null
			bAnyRestored = true;
		}
		if (bAnyRestored) ++Count.Restored;
	}
	TrussMaterialCache.Reset();
	return Count;
}

// v1.0.99 imported-primitive shadow-cast normalisation. See the header doc-comment on
// `EnsureImportedShadowsCast` for the user report + design rationale. Walks every
// OrbitImportRoot actor (matched by class-name string for zero compile dependency on the
// separately-owned OrbitConnector plugin, mirroring the v1.0.85 truss-material pass) and
// asserts the four shadow-related flags on every UPrimitiveComponent so the SpotLight's own
// shadow casting catches them all. Tracked components are added to OrbitShadowTouched so the
// OFF path of the toggle can find them again.
URebusVisualiserSubsystem::FOrbitShadowApplyCount URebusVisualiserSubsystem::EnsureImportedShadowsCast()
{
	FOrbitShadowApplyCount Count;
	UWorld* World = GetActiveWorld();
	if (!World) return Count;

	const bool bWantOn = bOrbitCastShadowsEnabled;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor->GetClass()->GetName() != TEXT("OrbitImportRoot")) continue;

		TArray<UPrimitiveComponent*> Prims;
		Actor->GetComponents<UPrimitiveComponent>(Prims);
		for (UPrimitiveComponent* Prim : Prims)
		{
			if (!Prim) continue;
			++Count.Components;

			// Per-comp early-out: if every flag already matches the desired state we don't
			// touch the component (avoids MarkRenderStateDirty per tick on every comp). The
			// per-tick walk is therefore O(comps) cheap once the rig has stabilised.
			//   * CastShadow / bCastDynamicShadow / bCastFarShadow follow bWantOn.
			//   * bCastHiddenShadow is forced FALSE regardless: a hidden shadow caster
			//     produces a silhouette in the lit pool with NO surface, which reads as
			//     a phantom black blob -- never what the operator wants.
			const bool bMatches =
				(Prim->CastShadow == bWantOn)
				&& (Prim->bCastDynamicShadow == bWantOn)
				&& (Prim->bCastHiddenShadow == false)
				&& (Prim->bCastFarShadow == bWantOn);
			OrbitShadowTouched.Add(Prim);

			if (bMatches) continue;

			// SetCastShadow is the public setter that handles the render-state-dirty +
			// scene-proxy notify; the other three are USceneComponent / UPrimitiveComponent
			// public bools that need a manual MarkRenderStateDirty (handled below).
			Prim->SetCastShadow(bWantOn);
			Prim->bCastDynamicShadow = bWantOn;
			Prim->bCastHiddenShadow = false;
			Prim->bCastFarShadow = bWantOn;
			Prim->MarkRenderStateDirty();
			++Count.Touched;
		}
	}
	return Count;
}

void URebusVisualiserSubsystem::SetOrbitCastShadowsEnabled(bool bEnabled)
{
	bOrbitCastShadowsEnabled = bEnabled;

	// First walk the previously-touched set so an OFF flips ALL prior comps off (and an ON
	// flip after that gets them back on). Then run the standard pass to pick up any newly-
	// imported geometry that hasn't been visited yet on this cadence -- so a single toggle
	// transition is fully consistent across the whole import on the same call.
	int32 Restored = 0;
	for (auto It = OrbitShadowTouched.CreateIterator(); It; ++It)
	{
		UPrimitiveComponent* Prim = It->Get();
		if (!Prim)
		{
			It.RemoveCurrent();
			continue;
		}
		Prim->SetCastShadow(bEnabled);
		Prim->bCastDynamicShadow = bEnabled;
		Prim->bCastHiddenShadow = false;
		Prim->bCastFarShadow = bEnabled;
		Prim->MarkRenderStateDirty();
		++Restored;
	}
	const FOrbitShadowApplyCount Count = EnsureImportedShadowsCast();
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Rebus.OrbitCastShadows %d: prior-touched=%d freshly-touched=%d (of %d Orbit primitive(s) walked)."),
		bEnabled ? 1 : 0, Restored, Count.Touched, Count.Components);
}

// v1.0.104 imported-primitive double-sided normalisation. Mirrors the v1.0.99
// EnsureImportedShadowsCast shape byte-for-byte: walks every OrbitImportRoot actor
// (matched by class-name string so we keep zero compile dependency on the OrbitConnector
// plugin, just like the v1.0.85 truss-material pass and the v1.0.99 shadow-cast pass) and
// asserts bCastShadowAsTwoSided + the `bTwoSided` static switch on every per-slot MID. See
// the EnsureImportedDoubleSided doc-comment in the header for the full algorithm + the
// user report + the perf caveat.
URebusVisualiserSubsystem::FOrbitDoubleSidedApplyCount URebusVisualiserSubsystem::EnsureImportedDoubleSided()
{
	FOrbitDoubleSidedApplyCount Count;
	UWorld* World = GetActiveWorld();
	if (!World) return Count;

	const bool bWantOn = bOrbitDoubleSidedEnabled;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor->GetClass()->GetName() != TEXT("OrbitImportRoot")) continue;

		TArray<UPrimitiveComponent*> Prims;
		Actor->GetComponents<UPrimitiveComponent>(Prims);
		for (UPrimitiveComponent* Prim : Prims)
		{
			if (!Prim) continue;
			++Count.Components;
			OrbitDoubleSidedTouched.Add(Prim);

			bool bTouched = false;

			// (1) Component-level shadow-side fix: bCastShadowAsTwoSided walks both sides
			// of every triangle when projecting shadow casters, so a single-sided opaque
			// material whose winding faces away from the light still throws the correct
			// silhouette in the lit pool. Cheap (one bool); no MarkRenderStateDirty
			// required (UE updates the shadow proxy from the cached value on the next
			// SceneRenderer draw). Per-comp early-out keeps the steady-state walk free.
			if (Prim->bCastShadowAsTwoSided != bWantOn)
			{
				Prim->bCastShadowAsTwoSided = bWantOn;
				bTouched = true;
			}

			// (2) Per-slot MID wrap + (3) `bTwoSided` static switch push. Wrapping
			// non-MID materials in a MID is cheap (one allocation per slot the first
			// time, byte-exact param inheritance on the parent) and lets the bTwoSided
			// switch be set per-instance without touching the on-disk asset. The static
			// switch silently no-ops on glTFRuntime / engine masters that don't expose it
			// (the SwitchesPushed counter only increments when the push was accepted) --
			// per the v1.0.104 README block, we cannot force-flip glTFRuntime's hard-baked
			// single-sided materials at runtime without re-cooking shader permutations,
			// so the operator-visible win on glTFRuntime imports is the (1) shadow-side
			// fix; the (2)+(3) win lands on every Rebus-authored master (M_RebusGround,
			// M_RebusFixtureLens, M_RebusOrbitImported, ...) that the operator has
			// re-parented Orbit assets to.
			const int32 NumSlots = Prim->GetNumMaterials();
			for (int32 s = 0; s < NumSlots; ++s)
			{
				UMaterialInterface* Mat = Prim->GetMaterial(s);
				if (!Mat) continue;

				UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Mat);
				if (!MID)
				{
					MID = UMaterialInstanceDynamic::Create(Mat, this);
					if (!MID) continue;
					Prim->SetMaterial(s, MID);
					++Count.MIDsWrapped;
					bTouched = true;
				}

				// SetStaticSwitchParameterValueEditorOnly is the editor-only path;
				// SetScalarParameterValue/SetVectorParameterValue are the runtime-safe
				// MID setters that silently no-op on a missing param. There is no
				// public runtime setter for static switches on MIDs (static switches
				// require shader permutations), so we route the toggle through a
				// SCALAR named `bTwoSidedScalar` that the v1.0.104 M_RebusOrbitImported
				// master + the existing v1.0.97 masters expose alongside their top-level
				// two_sided=true bake. Setting a scalar that doesn't exist on the parent
				// silently no-ops (UE's MaterialInstance setter compares the parameter
				// name against the parent's parameter set first), so on glTFRuntime
				// materials this is a free O(1) lookup.
				//
				// Naming note: the scalar is `bTwoSidedScalar` (not just `bTwoSided`)
				// so it can't ever collide with a future glTFRuntime / Substrate param
				// name; the leading `b` honours the v1.0.86 / v1.0.99 boolean-scalar
				// naming convention in our masters.
				const float Want = bWantOn ? 1.0f : 0.0f;
				float Live = 0.0f;
				const bool bHasParam = MID->GetScalarParameterValue(TEXT("bTwoSidedScalar"), Live);
				if (bHasParam)
				{
					if (!FMath::IsNearlyEqual(Live, Want))
					{
						MID->SetScalarParameterValue(TEXT("bTwoSidedScalar"), Want);
						bTouched = true;
					}
					++Count.SwitchesPushed;
				}
			}

			if (bTouched) ++Count.Touched;
		}
	}
	return Count;
}

void URebusVisualiserSubsystem::SetOrbitDoubleSidedEnabled(bool bEnabled)
{
	bOrbitDoubleSidedEnabled = bEnabled;

	// First walk the previously-touched set so an OFF flips ALL prior comps off (and a
	// re-toggle ON walks them back on). Then run the standard pass to pick up any newly-
	// imported geometry that hasn't been visited yet on this cadence -- so a single
	// toggle transition is fully consistent across the whole import on the same call.
	// Mirrors v1.0.99 SetOrbitCastShadowsEnabled byte-for-byte.
	int32 Restored = 0;
	for (auto It = OrbitDoubleSidedTouched.CreateIterator(); It; ++It)
	{
		UPrimitiveComponent* Prim = It->Get();
		if (!Prim)
		{
			It.RemoveCurrent();
			continue;
		}
		Prim->bCastShadowAsTwoSided = bEnabled;

		// Walk every slot MID and re-push the desired state. Skip non-MID materials
		// (we never wrapped those into MIDs on the previous ON pass either; nothing to
		// restore). MID slots not exposing `bTwoSidedScalar` silently no-op.
		const int32 NumSlots = Prim->GetNumMaterials();
		const float Want = bEnabled ? 1.0f : 0.0f;
		for (int32 s = 0; s < NumSlots; ++s)
		{
			if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Prim->GetMaterial(s)))
			{
				float Live = 0.0f;
				if (MID->GetScalarParameterValue(TEXT("bTwoSidedScalar"), Live)
					&& !FMath::IsNearlyEqual(Live, Want))
				{
					MID->SetScalarParameterValue(TEXT("bTwoSidedScalar"), Want);
				}
			}
		}
		++Restored;
	}
	const FOrbitDoubleSidedApplyCount Count = EnsureImportedDoubleSided();
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Rebus.OrbitDoubleSided %d: prior-touched=%d freshly-touched=%d (of %d Orbit primitive(s) walked, %d MIDs wrapped, %d bTwoSidedScalar pushes accepted)."),
		bEnabled ? 1 : 0, Restored, Count.Touched, Count.Components, Count.MIDsWrapped, Count.SwitchesPushed);
}

// =========================================================================================
// v1.0.105 imported-mesh Nanite enable. Walks every UStaticMesh under OrbitImportRoot
// and (in editor builds only) flips NaniteSettings.bEnabled to the operator-chosen state
// + rebuilds. See the EnsureImportedNanite doc-comment in the header for the full
// rationale, the editor-only constraint (UStaticMesh::Build is `#if WITH_EDITOR` in
// `Engine/StaticMesh.h`), and the operator-facing performance characteristics. The walker
// runs on the same 1 Hz Tick cadence as RebindOrbitModels / EnsureImportedShadowsCast /
// EnsureImportedDoubleSided so newly-imported geometry inherits the override on the next
// second after import. Per-mesh cache (NaniteAttempted) keeps the steady-state cost flat.
// =========================================================================================
URebusVisualiserSubsystem::FOrbitNaniteApplyCount URebusVisualiserSubsystem::EnsureImportedNanite()
{
	FOrbitNaniteApplyCount Count;
	UWorld* World = GetActiveWorld();
	if (!World) return Count;

#if WITH_EDITOR
	const bool bWantOn = bNaniteOrbitImportsEnabled;

	// Walk OrbitImportRoot actors first; group their UStaticMeshComponents by the
	// underlying UStaticMesh so we visit each unique asset ONCE (Orbit imports share
	// truss / set-piece geometry across many component instances; per-comp Build()
	// would rebuild the same asset N times).
	TMap<UStaticMesh*, TArray<UStaticMeshComponent*>> CompsByMesh;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor->GetClass()->GetName() != TEXT("OrbitImportRoot")) continue;

		TArray<UStaticMeshComponent*> SMCs;
		Actor->GetComponents<UStaticMeshComponent>(SMCs);
		for (UStaticMeshComponent* SMC : SMCs)
		{
			if (!SMC) continue;
			UStaticMesh* Mesh = SMC->GetStaticMesh();
			if (!Mesh) continue; // component without a live mesh -- nothing to enable on
			++Count.Components;
			CompsByMesh.FindOrAdd(Mesh).Add(SMC);
		}
	}

	for (TPair<UStaticMesh*, TArray<UStaticMeshComponent*>>& Pair : CompsByMesh)
	{
		UStaticMesh* Mesh = Pair.Key;
		if (!Mesh) continue;
		++Count.Meshes;

		const bool bAlreadyMatches = (Mesh->NaniteSettings.bEnabled == bWantOn);
		const bool bAttemptedBefore = NaniteAttempted.Contains(Mesh);
		if (bAlreadyMatches && bAttemptedBefore)
		{
			++Count.SkippedAlready;
			continue;
		}

		// UStaticMesh::Build requires SourceModels[0] with a valid MeshDescription. The
		// glTFRuntime parser only commits one when StaticMeshConfig.bGenerateStaticMesh-
		// Description=true on the import config (see glTFRuntimeParserStaticMeshes.cpp:782
		// inside `#if WITH_EDITOR`); we don't own OrbitConnector's call site (the in-tree
		// plugin is just ThirdParty/Cli/win-x64/orbit-cli.exe -- no UE source), so we can
		// hit this path and must handle it gracefully. Log a one-shot Warning per mesh
		// naming the operator-recovery action so a future "why isn't Nanite enabled on
		// my Orbit imports?" report can be diagnosed in one log grep.
		if (Mesh->GetNumSourceModels() == 0 || !Mesh->IsMeshDescriptionValid(0))
		{
			if (!NaniteAttempted.Contains(Mesh))
			{
				UE_LOG(LogRebusVisualiser, Warning,
					TEXT("[Rebus] Nanite skip on '%s': no source MeshDescription -- glTFRuntime "
						 "import config likely has bGenerateStaticMeshDescription=false on "
						 "the OrbitConnector path (the parser only commits a MeshDescription "
						 "in the editor when that flag is true; without it, UStaticMesh::Build "
						 "has no source to cook NaniteResources from). Operator fix: enable "
						 "the flag in OrbitConnector's import config OR pre-cook the Orbit "
						 "GLBs into UStaticMesh .uasset(s) with NaniteSettings.bEnabled=true "
						 "in editor before packaging."),
					*Mesh->GetName());
				NaniteAttempted.Add(Mesh); // suppress repeat warnings on subsequent ticks
			}
			++Count.SkippedNoSource;
			continue;
		}

		Mesh->NaniteSettings.bEnabled = bWantOn;
		if (bWantOn)
		{
			// Conservative defaults for first-time enable. PositionPrecision = MIN_int32
			// is the engine's "auto" sentinel (Nanite picks precision based on bounds);
			// FallbackPercentTriangles = 1.0 keeps a full-quality fallback proxy for the
			// Nanite-incompatible passes the v1.0.97 / v1.0.104 work introduced (double-
			// sided / masked / translucent materials drop out of the Nanite per-pixel
			// raster path and use the fallback proxy instead -- so v1.0.104 is preserved
			// verbatim); TrimRelativeError = 0.0 disables aggressive simplification on
			// the Nanite cluster build (we want crisp geometry on imported set pieces,
			// not auto-LODed approximations).
			Mesh->NaniteSettings.PositionPrecision = MIN_int32;
			Mesh->NaniteSettings.FallbackPercentTriangles = 1.0f;
			Mesh->NaniteSettings.TrimRelativeError = 0.0f;
		}

		// Build() rebuilds RenderData (incl. NaniteResources). Real cost -- seconds per
		// mesh on a busy import -- but it's a ONE-SHOT per state transition (the
		// NaniteAttempted cache prevents subsequent ticks from re-invoking Build on a
		// mesh whose state already matches). The rebuild blocks the game thread; the
		// SetNaniteOrbitImportsEnabled rebuild-storm Warning warns the operator before
		// they fire the OFF path mid-show.
		Mesh->Build(/*bSilent*/ false);
		NaniteAttempted.Add(Mesh);

		// Force every component referencing this mesh to re-acquire its render proxy so
		// the new NaniteResources land on the GPU on the next render frame. We already
		// cached the per-comp set above so this is a free walk.
		for (UStaticMeshComponent* C : Pair.Value)
		{
			if (C) C->MarkRenderStateDirty();
		}

		const int32 Faces = (Mesh->GetRenderData() && Mesh->GetRenderData()->LODResources.Num() > 0)
			? Mesh->GetRenderData()->LODResources[0].GetNumTriangles() : 0;
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("[Rebus] Nanite %s on '%s' (LOD0 tris=%d, fallback=%.1f%%)"),
			bWantOn ? TEXT("ENABLED") : TEXT("DISABLED"),
			*Mesh->GetName(),
			Faces,
			Mesh->NaniteSettings.FallbackPercentTriangles * 100.f);
		++Count.Touched;
	}
#else
	// Packaged build -- UStaticMesh::Build / INaniteBuilderModule are editor-only in
	// UE 5.7. The toggle remains settable so SceneState round-trips correctly, but the
	// conversion is no-effect here. One-shot per session so we don't spam the log; reset
	// to false on every Set* call so an operator A/Bing the toggle in a packaged build
	// gets a fresh log line.
	if (!bNanitePackagedWarningLogged)
	{
		bNanitePackagedWarningLogged = true;
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("[Rebus] Nanite runtime-conversion unavailable in packaged builds: "
				 "UStaticMesh::Build + INaniteBuilderModule are `#if WITH_EDITOR` in UE "
				 "5.7 (Engine/StaticMesh.h, Engine/Source/Developer/NaniteBuilder/). "
				 "Pre-cook the Orbit GLBs into /Game/REBUS/... UStaticMesh .uasset(s) "
				 "with NaniteSettings.bEnabled=true in editor before packaging -- the "
				 "RebusVisualiser README v1.0.105 release block documents the cooked "
				 "workflow. The bNaniteOrbitImports scene property + Rebus.NaniteOrbit"
				 "Imports console command remain settable for SceneState parity but do "
				 "not convert geometry in this build."));
	}
#endif

	return Count;
}

void URebusVisualiserSubsystem::SetNaniteOrbitImportsEnabled(bool bEnabled)
{
	bNaniteOrbitImportsEnabled = bEnabled;
	bNanitePackagedWarningLogged = false; // re-arm the packaged-build warning

#if WITH_EDITOR
	// Disable path is expensive: every previously-Nanite-enabled mesh gets Build() called
	// which can take seconds per mesh and blocks the game thread. Warn the operator BEFORE
	// the rebuild storm so a casual mid-show toggle isn't a multi-second hitch surprise.
	if (!bEnabled && NaniteAttempted.Num() > 0)
	{
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("[Rebus] Disabling Nanite on %d Orbit mesh(es) -- this will trigger a "
				 "rebuild storm (UStaticMesh::Build can take seconds per mesh, blocks the "
				 "game thread). Use carefully mid-show; preferred is to A/B the toggle "
				 "between scenes / shows, not during a live cue. The OFF path is provided "
				 "primarily so operators can A/B against the non-Nanite baseline for "
				 "perf comparison."),
			NaniteAttempted.Num());
	}

	// Walk the previously-touched set so an OFF flips ALL prior meshes off (and a re-
	// toggle ON flips them back). Then run the standard pass to pick up newly-imported
	// meshes that haven't been visited yet on this cadence -- so a single toggle
	// transition is fully consistent across the whole import on the same call. Mirrors
	// v1.0.99 SetOrbitCastShadowsEnabled / v1.0.104 SetOrbitDoubleSidedEnabled byte-for-
	// byte (only the per-mesh action body differs).
	int32 Restored = 0;
	for (auto It = NaniteAttempted.CreateIterator(); It; ++It)
	{
		UStaticMesh* Mesh = It->Get();
		if (!Mesh)
		{
			It.RemoveCurrent();
			continue;
		}
		if (Mesh->NaniteSettings.bEnabled == bEnabled) continue;
		// Skip if the source MeshDescription is missing -- Build() would fail. The
		// EnsureImportedNanite walker already logged the operator-fix Warning when the
		// mesh first hit the no-source path.
		if (Mesh->GetNumSourceModels() == 0 || !Mesh->IsMeshDescriptionValid(0)) continue;

		Mesh->NaniteSettings.bEnabled = bEnabled;
		Mesh->Build(/*bSilent*/ false);
		++Restored;

		// Re-acquire render proxies on every UStaticMeshComponent in the active world that
		// references this mesh -- TObjectIterator catches comps NOT under OrbitImportRoot
		// that happen to share the asset (e.g. fixture proxies that bound to the same
		// glTFRuntime mesh via RebindOrbitModels), so the new NaniteResources land
		// everywhere they're used.
		if (UWorld* World = GetActiveWorld())
		{
			for (TObjectIterator<UStaticMeshComponent> CIt; CIt; ++CIt)
			{
				UStaticMeshComponent* C = *CIt;
				if (C && C->GetWorld() == World && C->GetStaticMesh() == Mesh)
				{
					C->MarkRenderStateDirty();
				}
			}
		}
	}

	const FOrbitNaniteApplyCount Count = EnsureImportedNanite();
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Rebus.NaniteOrbitImports %d: prior-rebuilt=%d freshly-rebuilt=%d "
			 "(of %d Orbit primitive(s), %d unique mesh(es); skipped %d already-matching, %d no-source)."),
		bEnabled ? 1 : 0, Restored, Count.Touched, Count.Components, Count.Meshes,
		Count.SkippedAlready, Count.SkippedNoSource);
#else
	UE_LOG(LogRebusVisualiser, Warning,
		TEXT("Rebus.NaniteOrbitImports %d: state stored for SceneState parity, but runtime "
			 "Nanite conversion is unavailable in packaged builds (UStaticMesh::Build + the "
			 "NaniteBuilder module are `#if WITH_EDITOR` in UE 5.7). Pre-cook the Orbit GLBs "
			 "into UStaticMesh .uasset(s) with NaniteSettings.bEnabled=true in editor before "
			 "packaging."),
		bEnabled ? 1 : 0);
#endif
}

TArray<URebusVisualiserSubsystem::FOrbitNaniteDumpEntry> URebusVisualiserSubsystem::DumpOrbitNanite() const
{
	TArray<FOrbitNaniteDumpEntry> Out;
	UWorld* World = GetActiveWorld();
	if (!World) return Out;

	// Group by UStaticMesh so we report ONE line per unique mesh + its component-ref count
	// (the OrbitConnector import shares trusses / set pieces between many comp instances;
	// per-comp dump would be O(comps) noisy).
	TMap<UStaticMesh*, FOrbitNaniteDumpEntry> ByMesh;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor->GetClass()->GetName() != TEXT("OrbitImportRoot")) continue;

		TArray<UStaticMeshComponent*> SMCs;
		Actor->GetComponents<UStaticMeshComponent>(SMCs);
		for (UStaticMeshComponent* SMC : SMCs)
		{
			if (!SMC) continue;
			UStaticMesh* Mesh = SMC->GetStaticMesh();
			if (!Mesh) continue;

			FOrbitNaniteDumpEntry& Entry = ByMesh.FindOrAdd(Mesh);
			++Entry.ComponentRefs;
			if (Entry.MeshName.IsEmpty())
			{
				Entry.MeshName = Mesh->GetName();
				if (Mesh->GetRenderData() && Mesh->GetRenderData()->LODResources.Num() > 0)
				{
					Entry.Faces = Mesh->GetRenderData()->LODResources[0].GetNumTriangles();
				}
				Entry.bNaniteEnabled = Mesh->NaniteSettings.bEnabled;
				Entry.FallbackTris = (int32)(Entry.Faces * Mesh->NaniteSettings.FallbackPercentTriangles);

				// Per-slot bTwoSidedScalar probe (matches v1.0.104 EnsureImportedDoubleSided
				// param contract). Reports how many of the first-seen component's slots
				// carry the v1.0.104 marker -- gives the operator a single-glance view of
				// how much of this mesh actually renders two-sided.
				const int32 NumSlots = SMC->GetNumMaterials();
				Entry.SlotsTotal = NumSlots;
				for (int32 s = 0; s < NumSlots; ++s)
				{
					UMaterialInterface* Mat = SMC->GetMaterial(s);
					if (!Mat) continue;
					float V = 0.f;
					const bool bHas = Mat->GetScalarParameterValue(FMaterialParameterInfo(TEXT("bTwoSidedScalar")), V);
					if (bHas && V >= 0.5f) ++Entry.SlotsTwoSided;
				}
			}
		}
	}

	Out.Reserve(ByMesh.Num());
	for (TPair<UStaticMesh*, FOrbitNaniteDumpEntry>& P : ByMesh)
	{
		Out.Add(MoveTemp(P.Value));
	}
	return Out;
}
