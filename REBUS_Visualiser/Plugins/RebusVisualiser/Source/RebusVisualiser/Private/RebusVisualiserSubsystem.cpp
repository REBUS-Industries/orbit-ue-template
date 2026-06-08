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
// v1.0.113 -- on-disk M_RebusBeam.uasset md5 hash for the operator-facing "is your
// cooked master what we expect" diagnostic. FFileHelper loads the raw bytes,
// FMD5 computes the digest, FPackageName resolves /Game/... long names to the
// filesystem path, FCoreUObjectDelegates::PostLoadMapWithWorld re-fires the
// auto-purge on every level load (the v1.0.113 hardening for the v1.0.112 one-
// shot guard staying latched across PIE-stop / level-switch cycles).
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
// v1.0.121 -- IES pre-bake cache uses `UTextureLightProfile` (the engine's IES
// runtime asset). Header lives at `Engine/Source/Runtime/Engine/Classes/Engine/
// TextureLightProfile.h`, declared `ENGINE_API`; no separate Build.cs dependency.
#include "Engine/TextureLightProfile.h"
#include "UObject/UObjectGlobals.h" // FCoreUObjectDelegates::PostLoadMapWithWorld
#include "Engine/PostProcessVolume.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Texture.h"
#include "Modules/ModuleManager.h"
// v1.0.119 -- pulled in for `FAssetCompilingManager::Get().FinishAllCompilation()`
// in `RebuildAndVerifyBeamMaster`. `FAssetCompilingManager` ships INSIDE the
// `Engine` module (header at Engine/Source/Runtime/Engine/Public/Asset
// CompilingManager.h, declared `ENGINE_API`) -- no separate Build.cs dependency
// is needed (Engine is already a PublicDependencyModule). The include is gated
// behind `WITH_EDITOR` because the only consumer (`RebuildAndVerifyBeamMaster`'s
// post-regen flush) is itself editor-only -- packaged builds skip the regen
// entirely.
#if WITH_EDITOR
#include "AssetCompilingManager.h"
#endif

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
	//
	// v1.0.112 REINSTATEMENT (different shape, different intent) -- the v1.0.103 probe is
	// back, but as an AUTO-REGEN trigger rather than a passive Warning. User report against
	// post-v1.0.111: the screen-space pan-edge side-cutting artefact is STILL on screen,
	// even though the post-v1.0.111 `_BEAM_RAYMARCH_HLSL` source is provably clean. Root
	// cause is the stale on-disk `M_RebusBeam.uasset` -- the Python self-heal
	// (`_beam_master_has_shadow_mask` in `ensure_beam_material`) only fires when an
	// operator manually runs the script (it isn't in `[Python] +StartupScripts`), so on
	// the user's deployment the pre-v1.0.110 cooked HLSL is what's running. The C++ probe
	// below detects the staleness from the runtime side (presence of any obsolete
	// `BeamShadow*` scalar OR absence of the v1.0.111 `BeamShadowMaskRT` texture +
	// `BeamShadowMaskBiasCm` scalar contract) and AUTO-INVOKES `Rebus.RebuildBeamMaterial`
	// via the `py` console command -- the same Python regen path the manual operator
	// step would take, just driven without operator intervention. See the public
	// `EBeamMasterVersion` / `ProbeBeamMasterVersion` doc-comment in the header for the
	// full diagnosis (Cause A vs Cause B), the obsolete + required parameter contracts,
	// the editor-only Python regen path, and the packaged-build Warning fallback.
	ProbeAndAutoPurgeStaleBeamMaster();

	// v1.0.113 -- the v1.0.112 auto-purge is gated on a per-subsystem-instance
	// one-shot bool `bBeamMasterAutoPurgeRun`. `UGameInstanceSubsystem` instances
	// LIVE ACROSS LEVEL RELOADS inside one editor session (the game instance
	// outlives any individual UWorld), so the guard stayed latched true through a
	// `ClearScene + LoadScene` cycle, through a Play-In-Editor stop-and-restart, and
	// through any operator-initiated level reload. An operator who pulled a fresh
	// `M_RebusBeam.uasset` onto a running editor would never see the auto-regen
	// re-fire even though the on-disk state had changed underneath. Hook
	// `FCoreUObjectDelegates::PostLoadMapWithWorld` to reset the one-shot AND
	// re-fire the probe on every level load -- the v1.0.113 hardening so the
	// v1.0.112 chokepoint stays accurate for the actual lifetime of an editor
	// session. Mirrors the v1.0.107 `VersionWatermarkDrawHandle` shape (registered
	// here, unregistered in Deinitialize, never leaked).
	PostLoadMapAutoPurgeHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(
		this, &URebusVisualiserSubsystem::OnPostLoadMapAutoPurge);

	// v1.0.113 -- one-line operator-facing startup banner stitching the loaded
	// PLUGIN VersionName, the EBeamMasterVersion verdict on the live
	// `/Game/REBUS/Materials/M_RebusBeam.uasset`, AND the on-disk md5 hash of that
	// uasset (the ground-truth invariant -- if the operator pulls a fresh build but
	// doesn't rebuild C++ binaries OR doesn't restart the editor, the md5 surfaces
	// the mismatch immediately). Replaces the long string of guess-the-failure
	// theorising the v1.0.99..v1.0.112 brief loop went through; the operator can
	// now paste THIS one line and we can tell whether they're on a v1.0.113 binary,
	// against a v1.0.111+ master, with the cooked hash we expect. Mirrors the
	// v1.0.107 watermark banner shape.
	{
		const TSharedPtr<IPlugin> PluginForBanner = IPluginManager::Get().FindPlugin(TEXT("RebusVisualiser"));
		const FString PluginVer = PluginForBanner.IsValid()
			? PluginForBanner->GetDescriptor().VersionName
			: FString(TEXT("?.?.?"));
		const FBeamMasterVersionReport BannerReport = ProbeBeamMasterVersion();
		const TCHAR* BannerLabel = BeamMasterVersionLabel(BannerReport.Version);
		const FString BeamMd5 = ComputeBeamMasterUassetMd5();
		// v1.0.117 -- build timestamp from the __DATE__ / __TIME__ predefined macros.
		// Pinned at compile time so the banner reports WHEN the loaded DLL was built --
		// the operator can see "the .uplugin says v1.0.117 but the DLL was built at the
		// v1.0.116 commit timestamp" if they pulled but didn't rebuild. Wrapped in
		// PREPROCESSOR_TO_STRING so the macro expansion lands as a string literal in
		// the format string slot.
		// v1.0.119 -- banner extended with the new cache + regen telemetry
		// asked for in the v1.0.119 brief. `cachedBeamMaster` reports whether
		// `GetCachedBeamMaster()` has resolved a pointer this session (should
		// be the master's UObject name after this `Initialize()`-time probe;
		// `<null>` means even the probe couldn't load it -- which the probe
		// then classifies as `Missing`). `beamMasterLoadCount` is the session
		// counter for actual `LoadObject` calls against the master (should
		// READ 1 here -- the probe just fired it; will read 2 after a regen
		// fully completes; >2 = a per-tick load path the v1.0.119 audit
		// missed). `beamMasterRegen` reports the regen attempt counter + the
		// last attempt's result + the post-regen probe revision; surfaces the
		// v1.0.119 verify-after-regen plumbing the operator can see at a
		// glance instead of grepping for `STALE BEAM MASTER auto-purge`.
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("===== REBUS Visualiser v%s (binary built %s %s) -- beamMasterVerdict=%s "
				 "beamMasterRev=%d/%d beamMasterUassetMd5=%s cachedBeamMaster=%s "
				 "beamMasterLoadCount=%d beamMasterRegen={attempts=%d lastResult=%s "
				 "lastDetectedAfter=%d} =====%s"),
			*PluginVer,
			ANSI_TO_TCHAR(__DATE__), ANSI_TO_TCHAR(__TIME__),
			BannerLabel,
			BannerReport.DetectedRevision, BannerReport.ExpectedRevision,
			*BeamMd5,
			URebusVisualiserSubsystem::GetCachedBeamMasterDisplayName(),
			URebusVisualiserSubsystem::GetBeamMasterLoadCount(),
			BeamMasterRegenAttempts,
			*LastBeamMasterRegenResult,
			LastBeamMasterRegenDetectedAfter,
			TEXT(" -- Operator triage: if pluginVersion != v1.0.124 the C++ binary "
				 "is stale (`git pull` without rebuild); if beamMasterVerdict != "
				 "v1.0.121+ the on-disk M_RebusBeam.uasset is stale -- in a real "
				 "editor OR commandlet session the v1.0.112+ auto-purge SHOULD fire "
				 "one log line above to regen it (run `Rebus.DumpBeamMasterVersion` "
				 "to confirm), BUT in -game / -server mode the v1.0.121 stop-the-"
				 "bleeding gate aborts the regen (would otherwise crash, see v1.0.119 "
				 "regression) -- expect a `[Rebus] v1.0.121 SKIPPING beam-master "
				 "stale-probe auto-purge` warning instead, in which case the stale "
				 "master is used as-is (cone still renders, just without the v1.0.111 "
				 "depth-mask shadowing). To refresh: drive the v1.0.121 commandlet "
				 "bake from a shell: `UnrealEditor-Cmd.exe REBUS_Visualiser.uproject "
				 "-run=PythonScript -Script=\"import build_rebus_base_level as b; "
				 "b.ensure_beam_material(force=True); b.ensure_ies_profiles(force=True)\" "
				 "-unattended -nop4 -nosplash -stdout -FullStdOutLogOutput`, then "
				 "commit the regenerated Content/REBUS/Materials/M_RebusBeam.uasset + "
				 "Content/REBUS/IES/*.uasset (see v1.0.121 README release block). If "
				 "beamMasterLoadCount > 2 the v1.0.119 cache audit missed a per-tick "
				 "`LoadObject` site -- grep the codebase for `LoadObject` against "
				 "`M_RebusBeam` and route it through `GetCachedBeamMaster()`. Pair "
				 "with `Rebus.DumpBeamCulling [fixtureId]` for per-fixture verification, "
				 "and `Rebus.DumpBeamMaterialHealth` for per-fixture material binding "
				 "state."));
	}

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
	// v1.0.113 -- always unregister the PostLoadMapWithWorld delegate the
	// v1.0.113 auto-purge re-fire hook installed at `Initialize`. A dangling
	// delegate captured against `this` would crash on the next map load after
	// a hot-reload of the subsystem (the captured `this` having been
	// destructed). Mirrors the v1.0.107 watermark unregister above.
	if (PostLoadMapAutoPurgeHandle.IsValid())
	{
		FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapAutoPurgeHandle);
		PostLoadMapAutoPurgeHandle.Reset();
	}
	Channel.Reset();
	Rest.Reset();
	Super::Deinitialize();
}

void URebusVisualiserSubsystem::OnPostLoadMapAutoPurge(UWorld* /*LoadedWorld*/)
{
	// v1.0.113 -- re-fire the v1.0.112 stale-master probe after every level load.
	// The v1.0.112 `bBeamMasterAutoPurgeRun` one-shot bool was per-subsystem-
	// instance, but `UGameInstanceSubsystem` lives across level reloads inside one
	// editor session -- so the operator workflow `ClearScene + LoadScene` (or
	// Play-In-Editor stop + restart, or any operator-initiated level reload) would
	// NOT re-fire the auto-purge even after the operator manually flipped the on-
	// disk master to a known-stale state. Reset the bool and re-call the probe so
	// the v1.0.112 chokepoint stays accurate for the actual lifetime of an editor
	// session. The probe itself is idempotent / cheap when the master is current
	// (an `EBeamMasterVersion::V111Plus` verdict skips the `py` regen and just logs
	// a one-liner), so re-firing on every map load is safe.
	bBeamMasterAutoPurgeRun = false;
	ProbeAndAutoPurgeStaleBeamMaster();
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

		// v1.0.121 -- inline IES capture path. For every finalized profile, try to
		// persist the raw .ies bytes to `<ProjectSaved>/REBUS/IES_Inbox/<sanitized
		// profileId>.ies` so the next offline commandlet bake (which CAN call the
		// editor-only IESConverter via the engine's import factory) converts them
		// into `/Game/REBUS/IES/<sanitized profileId>.uasset` for the next -game
		// run. The helper is internally gated by `GIsEditor && !IsRunningGame()`
		// so this loop is a silent no-op under the standard PRISM Pixel Streaming
		// `-game` orchestrator launch -- the runtime inline-cache path
		// (`InlineIesCache` / `RebusIes::BuildLightProfile`) is still populated
		// above and drives whatever fallback the fixture actor's `SelectIesForZoom`
		// can muster (synthesized cone in -game where IESConverter is absent;
		// real conversion in editor / commandlet for the test path). One log
		// line per captured profile, written inside the helper.
		int32 CapturedToInbox = 0;
		for (const FRebusInlineIesProfile& Captured : Finalized.Profiles)
		{
			if (URebusVisualiserSubsystem::TryWriteInlineIesToInbox(
					Captured.ProfileId, Captured.Bytes))
			{
				++CapturedToInbox;
			}
		}

		InlineIesCache.Add(LibraryId, MoveTemp(Finalized));

		UE_LOG(LogRebusVisualiser, Log,
			TEXT("RegisterFixtureIes '%s' complete: %d inline IES profile(s), %d total bytes, "
				 "%d captured to v1.0.121 Saved/REBUS/IES_Inbox/ (0 in -game mode by design)."),
			*LibraryId, NumProfiles, TotalBytes, CapturedToInbox);

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

		const bool bAlreadyMatches = (Mesh->GetNaniteSettings().bEnabled == bWantOn);
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

		Mesh->GetNaniteSettings().bEnabled = bWantOn;
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
			Mesh->GetNaniteSettings().PositionPrecision = MIN_int32;
			Mesh->GetNaniteSettings().FallbackPercentTriangles = 1.0f;
			Mesh->GetNaniteSettings().TrimRelativeError = 0.0f;
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
			Mesh->GetNaniteSettings().FallbackPercentTriangles * 100.f);
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
		if (Mesh->GetNaniteSettings().bEnabled == bEnabled) continue;
		// Skip if the source MeshDescription is missing -- Build() would fail. The
		// EnsureImportedNanite walker already logged the operator-fix Warning when the
		// mesh first hit the no-source path.
		if (Mesh->GetNumSourceModels() == 0 || !Mesh->IsMeshDescriptionValid(0)) continue;

		Mesh->GetNaniteSettings().bEnabled = bEnabled;
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
				Entry.bNaniteEnabled = Mesh->GetNaniteSettings().bEnabled;
				Entry.FallbackTris = (int32)(Entry.Faces * Mesh->GetNaniteSettings().FallbackPercentTriangles);

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

// =========================================================================================
// v1.0.112 -- runtime stale `M_RebusBeam` master probe + auto-purge. See the public
// `EBeamMasterVersion` / `ProbeBeamMasterVersion` doc-comment in the header for the
// full diagnosis (this is the C++-driven replacement for the manually-triggered
// `_beam_master_has_shadow_mask` Python self-heal -- on the user's deployment that
// only fired when an operator manually ran `Tools > Execute Python Script`, which they
// hadn't, so the pre-v1.0.110 cooked HLSL kept producing the screen-space pan-edge
// side-cutting artefact even on a v1.0.111 binary).
// =========================================================================================

namespace
{
	// v1.0.112 -- the seven obsolete `BeamShadow*` scalars that the v1.0.96..v1.0.109
	// screen-space self-shadow trace pushed onto the BeamMID. The v1.0.110 rollback
	// deleted both the HLSL trace AND these parameter declarations from `_build_beam_
	// master`. If the loaded master still declares ANY of these, it was cooked by a
	// v1.0.96..v1.0.109 build of `build_rebus_base_level.py` -- i.e. the pre-v1.0.110
	// HLSL is what's compiled in. Order preserved for log-line predictability.
	const TCHAR* GObsoleteBeamShadowScalars[] = {
		TEXT("BeamShadowSteps"),
		TEXT("BeamShadowStrength"),
		TEXT("BeamShadowBias"),
		TEXT("BeamShadowDebug"),
		TEXT("BeamShadowFarCullCm"),
		TEXT("BeamShadowEdgeGuard"),
		TEXT("BeamShadowBiasScale"),
	};

	// v1.0.111 required parameter contract. The Python self-heal in
	// `ensure_beam_material::_beam_master_has_shadow_mask` checks the same set;
	// keep the lists in lockstep so the C++ + Python probes agree on the
	// definition of "v1.0.111+". Missing ANY entry => pre-v1.0.111 master.
	const TCHAR* GV111BeamMaskScalars[] = {
		TEXT("BeamShadowMaskEnabled"),
		TEXT("BeamShadowMaskBiasCm"),
		TEXT("BeamShadowMaskFadeCm"),
		TEXT("BeamShadowMaskFarCm"),
		TEXT("BeamShadowMaskTanHalfFov"),
		TEXT("BeamShadowMaskDebug"),
	};
	const TCHAR* GV111BeamMaskVectors[] = {
		TEXT("BeamLightFwd"),
		TEXT("BeamLightRight"),
		TEXT("BeamLightUp"),
	};
	const TCHAR* GV111BeamMaskTextures[] = {
		TEXT("BeamShadowMaskRT"),
	};

	// v1.0.117 -- the on-disk M_RebusBeam master must declare a `BeamMaterialRevision`
	// scalar parameter whose DEFAULT value matches this constant. The Python
	// `_build_beam_master` (build_rebus_base_level.py) bakes the same revision into
	// the master; mismatch (or missing param) triggers an auto-regen via the v1.0.112
	// purge path. Bump this AND the Python mirror `REBUS_BEAM_MATERIAL_REVISION` in
	// lockstep when the master changes (the v1.0.117 release block in README documents
	// the bump cadence). MUST be >= 117 (the v1.0.117 floor; pre-v1.0.117 masters
	// have no revision sentinel at all and report `DetectedRevision = -1`).
	//
	// v1.0.119 -- bumped 117 -> 119. v1.0.117 / v1.0.118 shipped a Python module that
	// could not be imported by Python 3 (mixed TAB / SPACE indentation in `build()` and
	// `ensure_base_level()` => `TabError` at PARSE time). The C++ auto-purge issued
	// `py import build_rebus_base_level; build_rebus_base_level.ensure_beam_material(
	// force=True)` -- the import line failed, the `ensure_beam_material` call never
	// ran, the on-disk `M_RebusBeam.uasset` stayed at its pre-v1.0.117 shape, AND the
	// GEngine->Exec return value was logged as OK because Python's error class is not
	// surfaced through Exec -- so the operator's session reported "auto-purge ran
	// successfully" while leaving the master STALE. The user ran `Rebus.DumpBeamCulling`
	// on a v1.0.118 binary and saw `BeamMaterialRevision=MISSING`, proving the regen
	// never landed. v1.0.119 fixes the Python module AND bumps this sentinel so every
	// v1.0.117/v1.0.118-era machine is forced to re-regen on next launch (the existing
	// V117Plus verdict would otherwise stay STALE relative to the new V119 floor, and
	// the regen will now actually succeed because the Python import works).
	//
	//
	// v1.0.121 -- bumped 120 -> 121 as part of the commandlet-driven offline-bake
	// release. v1.0.120 left the master STALE on disk because the only valid bake
	// host was an interactive editor session and the user (rightly) refused to
	// bake by hand; v1.0.121 relaxes the C++ + Python editor-runtime gate to also
	// allow commandlets (`-run=PythonScript ...`), so the bake can be driven
	// unattended from a shell. The revision bump invalidates every pre-v1.0.121
	// cooked master so the first commandlet bake against an existing checkout
	// produces a fresh .uasset at the new sentinel. Master GRAPH unchanged in
	// v1.0.121; only the sentinel + the gate semantics. See README v1.0.121
	// release block for the commandlet invocation + the IES pre-bake companion.
	//
	// v1.0.120 -- bumped 119 -> 120 as part of the stop-the-bleeding release. v1.0.119
	// shipped an auto-regen path that CRASHED the editor binary when the project was
	// launched with `-game` (the standard PRISM Pixel Streaming orchestrator command
	// line: `UnrealEditor-Cmd.exe ... -game -PixelStreamingURL=...`), because the
	// Python regen calls `unreal.EditorAssetLibrary.*` / `unreal.MaterialEditing
	// Library.*` / `unreal.AssetToolsHelpers.get_asset_tools()` -- ALL of which live
	// in the editor-only `EditorScriptingUtilities` module. In `-game` mode `GIsEditor`
	// is false and the editor subsystems aren't initialised; the first editor-only
	// call inside `ensure_beam_material` (almost certainly the leading `unreal.Editor
	// AssetLibrary.does_asset_exist(BEAM_PATH)` / `load_asset(BEAM_PATH)` at lines
	// 1139..1141 of build_rebus_base_level.py) dereferences uninitialised editor
	// state and raises `EXCEPTION_ACCESS_VIOLATION reading address 0x...` on the
	// `EditorScriptingUtilities.dll` frame -- exactly the user-reported stack trace.
	// v1.0.120 GATES the regen behind a `GIsEditor && !IsRunningGame() && !IsRunning
	// Commandlet()` check at the C++ entry point (see `RebuildAndVerifyBeamMaster` +
	// `ProbeAndAutoPurgeStaleBeamMaster` below) AND adds a belt-and-braces guard in
	// `ensure_beam_material` Python that bails with `unreal.log_warning` if invoked
	// in any non-editor-runtime mode. The revision bump itself is so that when a
	// developer re-bakes the master in a real editor session post-v1.0.120 (the
	// operator workflow documented in the README v1.0.120 release block) the auto-
	// purge probe correctly recognises pre-v1.0.120 cooked masters as stale. The
	// master graph itself is UNCHANGED in v1.0.120 (no shader / parameter changes);
	// the bump exists purely to invalidate cached masters from the broken v1.0.119
	// window so the next editor-session bake re-stamps them at the new revision.
	constexpr int32 RebusExpectedBeamMaterialRevision = 121;
	const TCHAR* GBeamMaterialRevisionScalar = TEXT("BeamMaterialRevision");

	// v1.0.119 -- file-scope cache for the on-disk `M_RebusBeam` master pointer
	// + session counter. See the header doc-comment on `GetCachedBeamMaster()` for
	// the user-reported `LogStreaming: FlushAsyncLoading(...)` per-frame spam this
	// fixes. `GBeamMasterCache` is a weak ptr so a forced GC invalidates it
	// cleanly; `GBeamMasterLoadCount` counts ONLY hits that actually called
	// `LoadObject` (cache hits do not increment) so the operator can see the
	// load was one-shot per session via `Rebus.DumpBeamCulling` /
	// `Rebus.DumpBeamMasterVersion` / the startup banner. Initialised at file
	// scope (constant-init: zero-init on a TWeakObjectPtr + int32 is well-defined
	// for both types in C++17, no runtime construction race).
	TWeakObjectPtr<UMaterialInterface> GBeamMasterCache;
	int32 GBeamMasterLoadCount = 0;
	// Static buffer holding the cached display name -- the public accessor
	// returns a stable `const TCHAR*` (consumed verbatim by `FString::Printf("%s")`
	// slots in the v1.0.119 banner / dump lines), so we own the storage here.
	// "<null>" when the cache is invalid -- consistent with the rest of the
	// v1.0.117 dump fields. Mutex-free: writes only happen on the game thread
	// (LoadObject + the regen path are both game-thread-only), reads from any
	// thread that walks the banner / dump produce a stable pointer to one of
	// two static buffers, so a tearless read is fine.
	const TCHAR* GBeamMasterCachedDisplayName = TEXT("<null>");

	// v1.0.120 -- STOP-THE-BLEEDING editor-runtime gate. Returns true ONLY when the
	// running UE process is an editor-or-commandlet session capable of invoking
	// editor-only Python (EditorScriptingUtilities / EditorAssetLibrary /
	// MaterialEditingLibrary / AssetToolsHelpers). v1.0.119's auto-regen called these
	// APIs unconditionally from `URebusVisualiserSubsystem::ProbeAndAutoPurgeStale
	// BeamMaster` -> `RebuildAndVerifyBeamMaster` -> `GEngine->Exec("py import
	// build_rebus_base_level; build_rebus_base_level.ensure_beam_material(force=True)")`,
	// which in `-game` mode (the standard PRISM Pixel Streaming orchestrator launches
	// the editor binary with `-game -PixelStreamingURL=...`) dereferenced uninitialised
	// editor subsystem state and CRASHED with EXCEPTION_ACCESS_VIOLATION on the
	// `EditorScriptingUtilities.dll` frame.
	//
	// `GIsEditor` is the engine's authoritative global flag for "this process is the
	// editor and editor subsystems are initialised" (`Engine/Source/Runtime/Core/Public/
	// CoreGlobals.h`); `IsRunningGame()` returns true for `-game` mode launches even
	// of the editor binary (`Engine/Source/Runtime/Core/Public/Misc/CoreMiscDefines.h`
	// inline helper, reads `FApp::IsGame()`). Editor-only Python safely runs whenever
	// GIsEditor=true AND IsRunningGame=false -- which covers BOTH the interactive
	// editor AND headless commandlets (`-run=PythonScript ...`). Commandlets are
	// editor processes too (`GIsEditor=true` and the editor subsystems ARE
	// initialised), and they are the canonical way to drive an unattended bake from
	// CI / a shell -- exactly the v1.0.121 workflow.
	//
	// v1.0.121 -- the gate originally also blocked commandlets (`!IsRunningCommandlet()`)
	// because the v1.0.120 stop-the-bleeding pass only proved the editor-interactive
	// case worked. The v1.0.121 commandlet-driven offline bake REQUIRES commandlets
	// to be allowed (that is how we drive `ensure_beam_material(force=True)` +
	// `ensure_ies_profiles(force=True)` from a shell). Verified via test runs of
	// `UnrealEditor-Cmd.exe ... -run=PythonScript -Script=...` against the v1.0.121
	// builder. The `-game` block stays in place -- that is the path that crashed.
	//
	// Mirrors the Python-side guard in `build_rebus_base_level.py::_is_editor_runtime()`
	// (which checks `unreal.SystemLibrary.is_editor()`, true in both editor sessions
	// and commandlets) -- belt-and-braces so even if a future code path accidentally
	// calls into Python in `-game` mode the Python returns cleanly without crashing
	// on editor-only API access.
	bool CanRegenBeamMasterInProcess()
	{
#if WITH_EDITOR
		return GIsEditor && !IsRunningGame();
#else
		return false;
#endif
	}
}

UMaterialInterface* URebusVisualiserSubsystem::GetCachedBeamMaster()
{
	// v1.0.119 -- single chokepoint resolver for `M_RebusBeam`. See the public
	// header doc-comment for the per-frame `FlushAsyncLoading` root-cause that
	// motivated the cache.
	//
	// Fast path: the cache resolved on a previous call and the package is still
	// live. `TWeakObjectPtr::Get()` returns nullptr if the UObject has been GC'd
	// (which would happen if the package was force-evicted or the operator ran
	// `obj gc` -- both rare, but supported), in which case we fall through to a
	// fresh `LoadObject` round-trip. Cache hit does NOT bump `GBeamMasterLoadCount`
	// -- that counter exists specifically to surface "how many real loads
	// happened this session" so an operator can verify the cache is working.
	if (UMaterialInterface* Cached = GBeamMasterCache.Get())
	{
		return Cached;
	}

	UMaterialInterface* Loaded = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/REBUS/Materials/M_RebusBeam.M_RebusBeam"));
	GBeamMasterLoadCount++;
	if (Loaded)
	{
		GBeamMasterCache = Loaded;
		// Cache a stable display-name string for the operator-facing banner +
		// dumps. `UObject::GetName()` returns an FString temporary; we copy into
		// our static FString below and hand out its `*` pointer. Mutex-free
		// per the doc-comment.
		static FString DisplayNameStorage;
		DisplayNameStorage = Loaded->GetName();
		GBeamMasterCachedDisplayName = *DisplayNameStorage;
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("[Rebus] v1.0.119 BeamMaster CACHE LOAD #%d -- '%s' resolved + cached "
				 "(subsequent BuildBeamCone / probe / dump calls will hit the cache; if "
				 "this counter ever exceeds 2 for the session, the v1.0.119 audit missed "
				 "a per-tick LoadObject call site -- grep `BeamMasterLoadCount` in the "
				 "v1.0.119 README for the auditing recipe)."),
			GBeamMasterLoadCount, *Loaded->GetName());
	}
	else
	{
		GBeamMasterCachedDisplayName = TEXT("<null>");
		// One-line operator-facing diagnostic on load failure. Repeated load
		// attempts from BuildBeamCone (the most common caller) will re-fire
		// this branch every time -- which is what we want when the master is
		// genuinely missing (so the operator sees the failure surface every
		// fixture spawn, not just the first).
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("[Rebus] v1.0.119 BeamMaster CACHE LOAD #%d -- FAILED to load "
				 "/Game/REBUS/Materials/M_RebusBeam.M_RebusBeam. Either the asset is "
				 "not on disk (fresh checkout that never ran the Python build) or "
				 "/Game/REBUS is not cooked. Run `Rebus.ForceBeamMasterRegen` to "
				 "trigger the Python regen + fixture rebind."),
			GBeamMasterLoadCount);
	}
	return Loaded;
}

void URebusVisualiserSubsystem::InvalidateBeamMasterCache()
{
	// v1.0.119 -- called BEFORE any regen so the post-regen probe / fixture
	// rebind hits the fresh `LoadObject` path and reads the newly-baked asset.
	// Cheap (a single pointer assignment). The display name resets too so a
	// dump that runs DURING the regen window sees `<null>` rather than a
	// stale name pointing at a freed UObject.
	GBeamMasterCache.Reset();
	GBeamMasterCachedDisplayName = TEXT("<null>");
}

namespace
{
	// v1.0.121 -- pre-baked IES profile cache. Same architecture as the v1.0.119
	// beam-master cache (file-scope TMap, TWeakObjectPtr so a forced GC self-
	// invalidates, single LoadObject per name per session). The portal pushes
	// IES profile ids per fixture library at runtime; the bake commandlet
	// produces `/Game/REBUS/IES/<SanitizeIesProfileName(id)>` for every known id;
	// the runtime cache resolves the package path via LoadObject<UTextureLight
	// Profile> on first miss and stores the resolved pointer. Cache hits do
	// NOT bump `GIesProfileLoadCount` (same contract the beam-master cache
	// uses) so the counter cleanly reports how many LoadObject calls have
	// actually fired.
	TMap<FName, TWeakObjectPtr<UTextureLightProfile>> GIesProfileCache;
	int32 GIesProfileLoadCount = 0;
}

FString URebusVisualiserSubsystem::SanitizeIesProfileName(const FString& ProfileId)
{
	// v1.0.121 -- mirrors `build_rebus_base_level._sanitize_ies_profile_name`
	// EXACTLY so the bake-time asset path and the runtime LoadObject path
	// agree byte-for-byte on the asset name. Algorithm: replace every
	// character NOT in [A-Za-z0-9_-] with underscore, collapse consecutive
	// underscores, strip leading/trailing underscore/hyphen. Empty ->
	// "ies_profile" fallback.
	if (ProfileId.IsEmpty())
	{
		return TEXT("ies_profile");
	}

	FString Out;
	Out.Reserve(ProfileId.Len());
	for (TCHAR Ch : ProfileId)
	{
		const bool bIsAlpha = (Ch >= TEXT('A') && Ch <= TEXT('Z')) ||
			(Ch >= TEXT('a') && Ch <= TEXT('z'));
		const bool bIsDigit = (Ch >= TEXT('0') && Ch <= TEXT('9'));
		const bool bIsKept = bIsAlpha || bIsDigit || Ch == TEXT('_') || Ch == TEXT('-');
		Out.AppendChar(bIsKept ? Ch : TEXT('_'));
	}

	// Collapse consecutive underscores.
	FString Collapsed;
	Collapsed.Reserve(Out.Len());
	TCHAR Prev = 0;
	for (TCHAR Ch : Out)
	{
		if (Ch == TEXT('_') && Prev == TEXT('_'))
		{
			continue;
		}
		Collapsed.AppendChar(Ch);
		Prev = Ch;
	}

	// Strip leading + trailing underscore/hyphen.
	int32 Start = 0;
	while (Start < Collapsed.Len() &&
		(Collapsed[Start] == TEXT('_') || Collapsed[Start] == TEXT('-')))
	{
		++Start;
	}
	int32 End = Collapsed.Len();
	while (End > Start &&
		(Collapsed[End - 1] == TEXT('_') || Collapsed[End - 1] == TEXT('-')))
	{
		--End;
	}
	const FString Trimmed = Collapsed.Mid(Start, End - Start);
	return Trimmed.IsEmpty() ? FString(TEXT("ies_profile")) : Trimmed;
}

UTextureLightProfile* URebusVisualiserSubsystem::GetCachedIesProfile(FName ProfileName)
{
	// v1.0.121 -- single chokepoint resolver for `/Game/REBUS/IES/<name>`.
	// Mirrors `GetCachedBeamMaster`'s shape so the same audit story holds.
	// Cache hit: zero LoadObject traffic, returns the cached pointer.
	if (ProfileName.IsNone())
	{
		return nullptr;
	}
	if (TWeakObjectPtr<UTextureLightProfile>* Found = GIesProfileCache.Find(ProfileName))
	{
		if (UTextureLightProfile* Cached = Found->Get())
		{
			return Cached;
		}
		// Stale weak ptr (asset GC'd); fall through to a fresh load.
		GIesProfileCache.Remove(ProfileName);
	}

	// Compose the long package path. We DO NOT re-sanitize here -- the caller is
	// responsible for passing the already-sanitized name as the FName key (the
	// IES descriptor handler / the SelectIesForZoom site both pass through
	// `SanitizeIesProfileName(rawProfileId)` before keying). Doing the sanitize
	// here too would double-mutate names that contain valid underscores.
	const FString PackagePath = FString::Printf(
		TEXT("/Game/REBUS/IES/%s.%s"), *ProfileName.ToString(), *ProfileName.ToString());

	UTextureLightProfile* Loaded = LoadObject<UTextureLightProfile>(nullptr, *PackagePath);
	GIesProfileLoadCount++;
	if (Loaded)
	{
		GIesProfileCache.Add(ProfileName, Loaded);
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("[Rebus] v1.0.121 IesProfile CACHE LOAD #%d -- '%s' resolved + cached "
				 "(subsequent SelectIesForZoom / probe / dump calls will hit the cache; "
				 "if this counter ever exceeds 2 per pre-baked profile per session, the "
				 "v1.0.121 audit missed a per-tick LoadObject call site)."),
			GIesProfileLoadCount, *PackagePath);
	}
	else
	{
		// Not finding a pre-baked asset is EXPECTED in two scenarios: (1) the
		// portal pushed an IES id we never baked (operator needs to drop the
		// .ies into Content/REBUS/IES/Source/ + re-bake), (2) the bake never
		// ran on this checkout (fresh clone). Either way the caller falls
		// through to the runtime / synthesized-cone fallback path. Log Verbose
		// so the line is queryable but doesn't spam the default log level.
		UE_LOG(LogRebusVisualiser, Verbose,
			TEXT("[Rebus] v1.0.121 IesProfile CACHE LOAD #%d -- MISS for '%s' "
				 "(no pre-baked asset at that path). Caller falls back to runtime / "
				 "synthesized cone."),
			GIesProfileLoadCount, *PackagePath);
	}
	return Loaded;
}

void URebusVisualiserSubsystem::InvalidateIesProfileCache()
{
	// v1.0.121 -- called by the Rebus.ForceIesProfileBake console command (and
	// any future explicit operator path) so the next `GetCachedIesProfile`
	// call re-loads freshly-baked assets from disk instead of returning the
	// pre-bake pointer.
	GIesProfileCache.Reset();
}

int32 URebusVisualiserSubsystem::GetIesProfileLoadCount()
{
	return GIesProfileLoadCount;
}

bool URebusVisualiserSubsystem::TryWriteInlineIesToInbox(const FString& ProfileId,
	const TArray<uint8>& Bytes)
{
	// v1.0.121 -- inline IES capture path. The portal pushes raw .ies bytes
	// over the data channel via RegisterFixtureIes; in editor / commandlet
	// mode (the v1.0.121 gate), this helper persists the bytes to
	// `<ProjectSaved>/REBUS/IES_Inbox/<sanitized id>.ies` so the next bake
	// commandlet has source files to convert. Silent no-op in `-game` mode
	// (writing into Saved/ from the wrong runtime context risks racing the
	// orchestrator).
	if (ProfileId.IsEmpty() || Bytes.Num() == 0)
	{
		return false;
	}

#if WITH_EDITOR
	const bool bCanCaptureInProcess = GIsEditor && !IsRunningGame();
#else
	const bool bCanCaptureInProcess = false;
#endif
	if (!bCanCaptureInProcess)
	{
		return false;
	}

	const FString SafeName = SanitizeIesProfileName(ProfileId);
	const FString InboxDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("REBUS"), TEXT("IES_Inbox"));
	const FString InboxFile = FPaths::Combine(InboxDir, SafeName + TEXT(".ies"));

	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.DirectoryExists(*InboxDir))
	{
		if (!PF.CreateDirectoryTree(*InboxDir))
		{
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("[Rebus] v1.0.121 inline IES capture: could not create directory %s "
					 "-- profile '%s' will not be captured for offline bake."),
				*InboxDir, *ProfileId);
			return false;
		}
	}

	// Skip the write if the file already exists at byte-identical content so a
	// repeated portal handshake (same profile pushed across reconnect) doesn't
	// uselessly thrash disk. Read-and-compare is cheap (IES files are small,
	// tens of KB at most).
	TArray<uint8> Existing;
	if (PF.FileExists(*InboxFile) && FFileHelper::LoadFileToArray(Existing, *InboxFile))
	{
		if (Existing.Num() == Bytes.Num() &&
			FMemory::Memcmp(Existing.GetData(), Bytes.GetData(), Bytes.Num()) == 0)
		{
			// Already captured -- nothing to do.
			return true;
		}
	}

	const bool bOk = FFileHelper::SaveArrayToFile(Bytes, *InboxFile);
	if (bOk)
	{
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("[Rebus] v1.0.121 inline IES captured: profile '%s' -> %s (%d bytes). "
				 "Run `Rebus.ForceIesProfileBake` (or the commandlet `UnrealEditor-Cmd.exe "
				 "REBUS_Visualiser.uproject -run=PythonScript -Script=\"import "
				 "build_rebus_base_level as b; b.ensure_ies_profiles(force=True)\"`) to "
				 "convert the inbox into pre-baked /Game/REBUS/IES/*.uasset assets."),
			*ProfileId, *InboxFile, Bytes.Num());
	}
	else
	{
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("[Rebus] v1.0.121 inline IES capture FAILED: profile '%s' -> %s "
				 "(SaveArrayToFile returned false; check disk space / write permissions). "
				 "The runtime path will still try `RebusIes::BuildLightProfile` on this "
				 "fixture, which falls back to the synthesized cone in -game."),
			*ProfileId, *InboxFile);
	}
	return bOk;
}

int32 URebusVisualiserSubsystem::GetBeamMasterLoadCount()
{
	return GBeamMasterLoadCount;
}

const TCHAR* URebusVisualiserSubsystem::GetCachedBeamMasterDisplayName()
{
	return GBeamMasterCachedDisplayName;
}

const TCHAR* URebusVisualiserSubsystem::BeamMasterVersionLabel(EBeamMasterVersion V)
{
	switch (V)
	{
		case EBeamMasterVersion::Missing:        return TEXT("MISSING (M_RebusBeam.uasset not on disk)");
		case EBeamMasterVersion::PreV96:         return TEXT("pre-v1.0.96 (cone-only, no shadow contract)");
		case EBeamMasterVersion::V96ThroughV109: return TEXT("v1.0.96..v1.0.109 (HAS OBSOLETE -- screen-space trace cooked in)");
		case EBeamMasterVersion::V110:           return TEXT("v1.0.110 (clean slate, no shadow path)");
		case EBeamMasterVersion::V111Plus:       return TEXT("v1.0.111..v1.0.120 (PRE-v1.0.121 -- stale revision; the v1.0.121 commandlet-driven offline bake produces a current master in one shell command -- see v1.0.121 README release block)");
		case EBeamMasterVersion::V117Plus:       return TEXT("v1.0.121+ (current expected revision)");
		default:                                 return TEXT("UNKNOWN");
	}
}

URebusVisualiserSubsystem::FBeamMasterVersionReport URebusVisualiserSubsystem::ProbeBeamMasterVersion() const
{
	FBeamMasterVersionReport Report;

	// v1.0.119 -- route through the shared `GetCachedBeamMaster()` accessor so
	// the probe hits the cache after the first session-wide load (eliminates the
	// per-frame `FlushAsyncLoading` spam that v1.0.117 / v1.0.118 had whenever
	// any probe / refresh sink fired). The accessor returns the master as
	// `UMaterialInterface*` (the cache type); the obsolete-/v111-/revision-
	// scalar probes below require a concrete `UMaterial*` (parameter-default
	// reads). `Cast<UMaterial>` is safe because the on-disk asset IS a UMaterial
	// (the Python author side calls `MaterialFactoryNew`) -- if the cast ever
	// fails it means somebody substituted a UMaterialInstance, in which case we
	// fall through to the same Missing-verdict path.
	UMaterialInterface* CachedIface = URebusVisualiserSubsystem::GetCachedBeamMaster();
	UMaterial* Master = Cast<UMaterial>(CachedIface);
	if (!Master)
	{
		Report.Version = EBeamMasterVersion::Missing;
		return Report;
	}

	// Detect any obsolete v1.0.96..v1.0.109 `BeamShadow*` scalar. UMaterial's
	// `GetScalarParameterValue(FMaterialParameterInfo, OutValue)` returns true
	// only when the parameter is declared on the master -- false for missing
	// names (no exception, no log line). Exactly the semantics we want for a
	// probe; mirrors the v1.0.104 EnsureImportedDoubleSided / DumpOrbitNanite
	// reflection-probe pattern used elsewhere in this subsystem.
	for (const TCHAR* Name : GObsoleteBeamShadowScalars)
	{
		float V = 0.f;
		if (Master->GetScalarParameterValue(FMaterialParameterInfo(Name), V))
		{
			Report.DetectedObsoleteParams.Add(Name);
		}
	}

	// Probe the v1.0.111 parameter contract. The Python self-heal probe checks
	// the same set; keep them aligned so a stale master fails BOTH probes the
	// same way regardless of which one fires first.
	for (const TCHAR* Name : GV111BeamMaskScalars)
	{
		float V = 0.f;
		if (!Master->GetScalarParameterValue(FMaterialParameterInfo(Name), V))
		{
			Report.MissingV111Scalars.Add(Name);
		}
	}
	for (const TCHAR* Name : GV111BeamMaskVectors)
	{
		FLinearColor V = FLinearColor::Black;
		if (!Master->GetVectorParameterValue(FMaterialParameterInfo(Name), V))
		{
			Report.MissingV111Vectors.Add(Name);
		}
	}
	for (const TCHAR* Name : GV111BeamMaskTextures)
	{
		UTexture* T = nullptr;
		if (!Master->GetTextureParameterValue(FMaterialParameterInfo(Name), T))
		{
			Report.MissingV111Textures.Add(Name);
		}
	}

	// v1.0.117 -- read the `BeamMaterialRevision` default scalar value. The Python
	// `_build_beam_master` always bakes it as a scalar parameter (default 117 at
	// v1.0.117 ship time). A master that declares the parameter at the expected
	// revision is current; a master that has the v1.0.111 contract but NOT this
	// scalar (or reports a revision below the expected value) is pre-v1.0.117 and
	// auto-purged below.
	Report.ExpectedRevision = RebusExpectedBeamMaterialRevision;
	{
		float DetectedRevAsFloat = 0.f;
		const bool bHasRev = Master->GetScalarParameterValue(
			FMaterialParameterInfo(GBeamMaterialRevisionScalar), DetectedRevAsFloat);
		if (bHasRev)
		{
			// Round-trip via FMath::RoundToInt -- the Python sentinel is authored as
			// 117.0 but a future operator hand-edit to e.g. 117.5 must NOT pass as
			// "matches 117" (the rounded value drives the equality test).
			Report.DetectedRevision = FMath::RoundToInt(DetectedRevAsFloat);
		}
		else
		{
			Report.DetectedRevision = -1; // sentinel = "param absent" (pre-v1.0.117)
		}
	}

	// Classify. Order matters: obsolete-present is the loudest staleness mode
	// (cooked-in HLSL is producing the user-visible artefact) so it wins. Then
	// missing-v1.0.111-contract is the second staleness mode (clean v1.0.110
	// rollback state). Then v1.0.117 revision check classifies whether the
	// v1.0.111-contract master is current (V117Plus) or pre-v1.0.117 (V111Plus,
	// auto-purge target).
	const bool bHasObsolete = Report.DetectedObsoleteParams.Num() > 0;
	const bool bMissingAnyV111 =
		Report.MissingV111Scalars.Num() > 0 ||
		Report.MissingV111Vectors.Num() > 0 ||
		Report.MissingV111Textures.Num() > 0;

	if (bHasObsolete)
	{
		Report.Version = EBeamMasterVersion::V96ThroughV109;
	}
	else if (bMissingAnyV111)
	{
		// No obsolete AND missing v1.0.111 contract. Could be either pre-v1.0.96
		// (cone-only, never had a shadow contract) OR v1.0.110 (clean-slate
		// state, shadow contract not yet authored). Heuristic: if ALL three
		// v1.0.111 categories are missing entirely -- not a single mask scalar
		// or vector or texture -- treat as pre-v1.0.96 (the v1.0.110 state
		// would have had the v1.0.42-era cone params but never had any shadow
		// contract by design, same shape as the pre-v1.0.96 case from the
		// probe's perspective). Either way the auto-regen path is the same:
		// upgrade to v1.0.117+. We pick V110 for the label since v1.0.96 is
		// long dead -- it's the more recent of the two equally-stale shapes.
		Report.Version = EBeamMasterVersion::V110;
	}
	else if (Report.DetectedRevision >= Report.ExpectedRevision)
	{
		Report.Version = EBeamMasterVersion::V117Plus;
	}
	else
	{
		// v1.0.117 -- has full v1.0.111 contract but missing / older revision
		// sentinel. The cone is still writing to the depth pass (no
		// `disable_depth_test` on the material) and the user's clip symptom
		// persists. Same auto-regen path as the other stale states.
		Report.Version = EBeamMasterVersion::V111Plus;
	}
	return Report;
}

FString URebusVisualiserSubsystem::ComputeBeamMasterUassetMd5()
{
	// v1.0.113 -- the ground-truth invariant for "is the operator's cooked master
	// the one we expect": MD5 of the bytes on disk. We've spent v1.0.99/v1.0.103/
	// v1.0.110/v1.0.111/v1.0.112 chasing "the wrong shader is compiled in" -- the
	// hash exposes that condition the operator can't otherwise see (the loaded
	// UMaterial in memory is a deserialised representation; the on-disk .uasset
	// is the cook artefact the engine actually reads at boot). One log-line in,
	// one md5 out -- the operator pastes it in a bug report and we can tell at a
	// glance whether their workspace is the v1.0.113 cooked output.
	//
	// Resolve the engine's `/Game/REBUS/Materials/M_RebusBeam` long-package name
	// to a filesystem path via FPackageName::TryConvertLongPackageNameToFilename
	// (the same accessor `LoadObject` uses internally). FPackageName::GetAssetPackageExtension()
	// = ".uasset" for non-cooked / standard cooked content; the runtime + editor
	// both produce that extension.
	const FString LongPackageName = TEXT("/Game/REBUS/Materials/M_RebusBeam");
	FString FilesystemPath;
	if (!FPackageName::TryConvertLongPackageNameToFilename(
			LongPackageName, FilesystemPath, FPackageName::GetAssetPackageExtension()))
	{
		return FString(TEXT("<filesystem-path-unresolvable>"));
	}

	// `FFileHelper::LoadFileToArray` is the canonical "byte-for-byte read this
	// file" helper -- doesn't transform line endings (text-mode load would),
	// streams into a TArray<uint8>. Bail with a self-describing sentinel string
	// on any failure (file not on disk, IO error, permission denied) so the
	// operator gets a SOMETHING-IS-WRONG signal in the banner without crashing.
	TArray<uint8> RawBytes;
	if (!FFileHelper::LoadFileToArray(RawBytes, *FilesystemPath))
	{
		return FString::Printf(TEXT("<load-failed:%s>"), *FilesystemPath);
	}

	// FMD5::HashBytes signature in UE 5.7: `static FString HashBytes(const uint8*
	// Input, int64 InputSize)`. Returns 32-char lowercase hex (e.g.
	// "d41d8cd98f00b204e9800998ecf8427e" for empty input). MD5 is FINE for asset
	// staleness diagnostics -- it's not a cryptographic claim, just a stable
	// fingerprint of the cooked bytes; the existing engine uses FMD5 for the
	// same purpose in `UStaticMesh::ComputeRenderDataHash` etc.
	const FString Md5 = FMD5::HashBytes(RawBytes.GetData(), (int64)RawBytes.Num());
	return Md5;
}

void URebusVisualiserSubsystem::RefreshAllSpawnedFixtureBeamMIDs()
{
	// SpawnedFixtures is normally EMPTY at Initialize() time -- the subsystem
	// orchestrator's SpawnAllFixtures path runs much later on the per-tick
	// scene-fetch chain -- so this is a no-op for the v1.0.112 first-launch
	// case. Kept as a belt-and-braces refresh helper in case the auto-purge
	// is ever re-fired from a tick-gated path where fixtures already exist
	// (e.g. a future hot-reload of the master mid-show). Newly-spawned
	// fixtures pick up the regenerated master directly via BuildBeamCone's
	// LoadObject<UMaterialInterface> call -- no per-fixture push needed for
	// them. Per-fixture body:
	//   * ClearParameterValues drops any operator-pinned scalar overrides so
	//     the MID reads the regenerated master's defaults instead of the
	//     stale-master values previously pushed on top.
	//   * The three Refresh* helpers re-push the live CVar values + world
	//     axes + light-space mask scalar/RT bindings against the new master's
	//     contract. Each helper silently no-ops when BeamMID is null (per
	//     their existing doc-comments in RebusFixtureActor.h), so a fixture
	//     that hadn't built its beam yet stays a clean no-op.
	int32 Refreshed = 0;
	for (ARebusFixtureActor* F : SpawnedFixtures)
	{
		if (!F) continue;
		// RefreshBeamMaterialBindings null-checks BeamMID internally and skips silently
		// for fixtures still in pre-BuildBeamCone state. Increment the counter
		// unconditionally for any live fixture so the log line reflects the WALK count;
		// the per-fixture body decides whether the work actually happened.
		F->RefreshBeamMaterialBindings();
		++Refreshed;
	}
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("v1.0.112 stale-master purge: refreshed %d already-spawned fixture(s) (of %d) "
			 "-- newly-spawned fixtures pick up the regenerated master via BuildBeamCone's "
			 "LoadObject and do not need this push."),
		Refreshed, SpawnedFixtures.Num());
}

void URebusVisualiserSubsystem::ProbeAndAutoPurgeStaleBeamMaster()
{
	// v1.0.119 -- the v1.0.113 once-per-session guard semantics are RETAINED for
	// the auto-driven path (the probe fires from `Initialize()` once + from
	// `PostLoadMapWithWorld` once per level reload via `OnPostLoadMapAutoPurge`'s
	// explicit `bBeamMasterAutoPurgeRun = false` reset). But the brief explicitly
	// asks v1.0.119 to ALSO reset the guard on a regen FAILURE so a subsequent
	// level-reload (or a manual `Rebus.ForceBeamMasterRegen`) gets a fresh
	// regen attempt instead of silently no-opping behind the latched bool.
	// `RebuildAndVerifyBeamMaster` (the new v1.0.119 chokepoint everything routes
	// through) sets the guard back to false in its failure paths and leaves it
	// true on success. See the doc-comment on that method for the full state
	// machine.
	if (bBeamMasterAutoPurgeRun) return;
	bBeamMasterAutoPurgeRun = true;

	// v1.0.120 STOP-THE-BLEEDING editor-runtime gate. The standard PRISM
	// orchestrator launches the editor binary with `-game -PixelStreamingURL=...`,
	// which sets `GIsEditor=false` + `IsRunningGame()=true`. v1.0.119's auto-
	// regen path (`RebuildAndVerifyBeamMaster` -> `GEngine->Exec("py ...")`)
	// invoked editor-only Python (`unreal.EditorAssetLibrary.*` /
	// `unreal.MaterialEditingLibrary.*` / `unreal.AssetToolsHelpers.get_asset_tools()`,
	// all hosted in `EditorScriptingUtilities.dll`) which dereferences un-
	// initialised editor subsystem state in `-game` mode and CRASHED with
	// EXCEPTION_ACCESS_VIOLATION on the `EditorScriptingUtilities.dll` frame.
	// User-reported stack trace pinned the failure at:
	//   UnrealEditor-Engine.dll
	//   UnrealEditor-EditorScriptingUtilities.dll  <-- editor-only API
	//   UnrealEditor-CoreUObject.dll
	//   UnrealEditor-PythonScriptPlugin.dll        <-- our py call
	// Gating here aborts the auto-purge cleanly + logs ONCE per session (the
	// static bool below) instead of every level reload. The stale on-disk
	// `M_RebusBeam.uasset` is used as-is for the session -- `BuildBeamCone`
	// still constructs a BeamMID off whatever master is on disk, so the cone
	// remains visible (just without the v1.0.111 depth-mask shadowing the
	// stale master may pre-date). See `CanRegenBeamMasterInProcess()` for the
	// gate semantics + the README v1.0.120 release block for the operator
	// offline-bake workflow.
	if (!CanRegenBeamMasterInProcess())
	{
		static bool bLoggedGameModeProbeSkip = false;
		if (!bLoggedGameModeProbeSkip)
		{
			bLoggedGameModeProbeSkip = true;
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("[Rebus] v1.0.121 SKIPPING beam-master stale-probe auto-purge: this "
					 "UE session is in -game / -server mode (GIsEditor=%d IsRunningGame=%d "
					 "IsRunningCommandlet=%d), so editor-only Python (EditorScriptingUtilities "
					 "/ EditorAssetLibrary / MaterialEditingLibrary / AssetToolsHelpers) "
					 "cannot run without crashing the process (v1.0.119 hit "
					 "EXCEPTION_ACCESS_VIOLATION here). The on-disk M_RebusBeam.uasset "
					 "will be used as-is for this session. To regenerate: drive the v1.0.121 "
					 "bake commandlet `UnrealEditor-Cmd.exe REBUS_Visualiser.uproject "
					 "-run=PythonScript -Script=\"import build_rebus_base_level as b; "
					 "b.ensure_beam_material(force=True); b.ensure_ies_profiles(force=True)\" "
					 "-unattended -nop4 -nosplash -stdout -FullStdOutLogOutput` and commit "
					 "the regenerated Content/REBUS/Materials/M_RebusBeam.uasset (+ .uexp) "
					 "to source control. Future -game sessions will then load the pre-baked "
					 "master without needing this rebuild. This warning is logged ONCE per "
					 "session (no spam)."),
				GIsEditor ? 1 : 0,
				IsRunningGame() ? 1 : 0,
				IsRunningCommandlet() ? 1 : 0);
		}
		return;
	}

	const FBeamMasterVersionReport Report = ProbeBeamMasterVersion();

	// Compose a one-line summary of the staleness shape for the log header. The
	// operator (and any future bug report) wants to see exactly which obsolete /
	// missing params triggered the auto-regen, not just "STALE".
	auto JoinNames = [](const TArray<FString>& Names) -> FString
	{
		if (Names.Num() == 0) return FString(TEXT("(none)"));
		return FString::Join(Names, TEXT(","));
	};

	const TCHAR* Label = BeamMasterVersionLabel(Report.Version);
	const bool bStale =
		Report.Version == EBeamMasterVersion::V96ThroughV109 ||
		Report.Version == EBeamMasterVersion::V110 ||
		Report.Version == EBeamMasterVersion::PreV96 ||
		// v1.0.117 -- pre-v1.0.117 masters (V111Plus, full mask contract but
		// `BeamMaterialRevision != ExpectedRevision`) are now also stale: they
		// lack the v1.0.117 `disable_depth_test = true` flag so the cone keeps
		// writing/reading depth and the user's clip symptom persists. Auto-
		// regen path is the same Python rebake the other stale states use.
		Report.Version == EBeamMasterVersion::V111Plus;

	if (Report.Version == EBeamMasterVersion::Missing)
	{
		// Asset isn't on disk -- this is the first-launch case (no Python regen
		// has ever run). BuildBeamCone's LoadObject + null-check covers this
		// case at fixture-spawn time with its own Warning + skip; the v1.0.112
		// probe just logs it here so a `Rebus.DumpBeamMasterVersion` operator
		// run is consistent. Don't try to regen -- the Python author path needs
		// an editor for ensure_beam_material to do create_asset, and a missing
		// asset typically means the operator hasn't run the project's build
		// step at all, which is a bigger problem the auto-regen can't fix.
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("[Rebus] BEAM MASTER %s -- the project's `build_rebus_base_level.py` has "
				 "never run against this content folder. Run `Tools > Execute Python Script "
				 "> build_rebus_base_level.build()` in the editor (or the headless "
				 "`-run=pythonscript build_rebus_base_level` invocation) to bake the master."),
			Label);
		return;
	}

	if (!bStale)
	{
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("[Rebus] BEAM MASTER version probe: %s -- no auto-purge needed."),
			Label);
		return;
	}

	// Stale. Compose the diagnostic line with the exact obsolete + missing params
	// that triggered the verdict so operators / bug reports can correlate it.
	// v1.0.117: also report the revision mismatch (got=N, want=117) when the
	// staleness is the new V111Plus-but-pre-v1.0.117 case.
	// v1.0.119: same call-site -- but now we delegate the actual `py` regen +
	// post-regen verify + per-fixture rebind to `RebuildAndVerifyBeamMaster`,
	// which is the SINGLE chokepoint v1.0.119 routes every regen call through
	// (manual `Rebus.RebuildBeamMaterial`, manual `Rebus.ForceBeamMasterRegen`,
	// the per-fixture self-heal trigger, and this auto-driven probe-then-purge
	// path). The chokepoint owns the cache-invalidate-before / post-regen
	// re-probe / LOUD-error-on-still-stale / regen-attempt-counter logic, so
	// every entry point gets the same hardening for free.
	UE_LOG(LogRebusVisualiser, Warning,
		TEXT("[Rebus] STALE BEAM MASTER detected -- %s. Obsolete v1.0.96..v1.0.109 scalars "
			 "present: [%s]. Missing v1.0.111 scalars: [%s]. Missing v1.0.111 vectors: [%s]. "
			 "Missing v1.0.111 textures: [%s]. BeamMaterialRevision detected=%d expected=%d. "
			 "This is the same on-disk staleness that bit us v1.0.99/v1.0.103/v1.0.110/"
			 "v1.0.111/v1.0.112/v1.0.117/v1.0.118: the cooked HLSL + material flags inside "
			 "`M_RebusBeam.uasset` are older than the running plugin binary. v1.0.119 "
			 "delegates to `RebuildAndVerifyBeamMaster` which (1) invalidates the v1.0.119 "
			 "cached master pointer, (2) invokes the Python `ensure_beam_material(force=True)` "
			 "regen, (3) RE-PROBES the on-disk master to verify the new revision actually "
			 "landed (the v1.0.117 / v1.0.118 silent-failure case the Python TabError hit), "
			 "(4) refreshes every spawned-fixture BeamMID against the freshly-baked master."),
		Label,
		*JoinNames(Report.DetectedObsoleteParams),
		*JoinNames(Report.MissingV111Scalars),
		*JoinNames(Report.MissingV111Vectors),
		*JoinNames(Report.MissingV111Textures),
		Report.DetectedRevision, Report.ExpectedRevision);

	const bool bRegenOk = RebuildAndVerifyBeamMaster(/*bForceEvenIfCurrent=*/ false);

	// v1.0.119 -- on FAILURE, release the once-per-session guard so the next
	// `PostLoadMapWithWorld` (or operator `Rebus.ForceBeamMasterRegen`) gets a
	// fresh attempt instead of silently no-opping behind the latched bool. The
	// `OnPostLoadMapAutoPurge` re-fire already resets the guard before re-entry
	// for the normal map-reload path; this branch covers the rarer case where
	// the probe runs from `Initialize()` against a stale master, fails to
	// regen (e.g. Python module broken), and the operator then triggers a
	// manual rescue without reloading the level.
	if (!bRegenOk)
	{
		bBeamMasterAutoPurgeRun = false;
	}
}

bool URebusVisualiserSubsystem::RebuildAndVerifyBeamMaster(bool bForceEvenIfCurrent)
{
	// v1.0.119 -- single chokepoint for "regen the on-disk M_RebusBeam master,
	// then verify the regen actually landed". See the public-header doc-comment
	// for the architectural why; this function owns the actual sequence:
	//
	//   (1) Invalidate the v1.0.119 cached-master pointer FIRST so the post-
	//       regen probe / fixture rebind re-load freshly off disk.
	//   (2) Pre-regen probe -- detect whether the master is already current
	//       (early-out unless `bForceEvenIfCurrent` was set, which the manual
	//       `Rebus.ForceBeamMasterRegen` console command uses for operator
	//       rescue).
	//   (3) Invoke the Python `ensure_beam_material(force=True)` regen via
	//       GEngine->Exec("py ..."). The Exec return value is OK-ish but
	//       does NOT propagate Python exceptions (the v1.0.117 / v1.0.118
	//       silent-failure case), so step (4) is the real source of truth.
	//   (4) `FAssetCompilingManager::Get().FinishAllCompilation()` to flush
	//       any async shader compile the regen kicked off, so the post-
	//       regen probe reads a fully-compiled material rather than racing
	//       the compiler.
	//   (5) Re-invalidate + RE-PROBE the master. Compare the detected revision
	//       against the expected revision. Mismatch => LOUD `UE_LOG(Error)`
	//       with the operator-actionable failure diagnostic (Python module
	//       broken vs disk permissions vs cook-only path etc.).
	//   (6) On success: `RefreshAllSpawnedFixtureBeamMIDs` to rebind every
	//       existing fixture's BeamMID against the fresh master.
	//
	// Bumps the v1.0.119 regen-attempt counter unconditionally (operator wants
	// to see "we tried N times" in the banner regardless of outcome), records
	// the result string + post-regen detected revision into the banner-visible
	// instance state. Returns true iff the post-regen verify confirmed the
	// expected revision (or the early-out path proved we were already current).

	BeamMasterRegenAttempts++;

#if WITH_EDITOR
	// Editor build -- the only configuration where Python regen is possible.
	// Packaged builds don't ship PythonScriptPlugin (per v1.0.103 design).

	// v1.0.120 STOP-THE-BLEEDING editor-runtime gate. v1.0.119's auto-regen path
	// crashed the user's UE session at launch with EXCEPTION_ACCESS_VIOLATION on
	// the `EditorScriptingUtilities.dll` frame because the PRISM orchestrator
	// launches the editor binary with `-game -PixelStreamingURL=...` (so
	// `GIsEditor=false`, `IsRunningGame()=true`), and v1.0.119 unconditionally
	// invoked editor-only Python (`unreal.EditorAssetLibrary.*` /
	// `unreal.MaterialEditingLibrary.*` / `unreal.AssetToolsHelpers.*` -- all
	// hosted in `EditorScriptingUtilities.dll`) which dereferenced uninitialised
	// editor subsystem state. Gating at the C++ chokepoint stops the crash
	// regardless of which exact Python line is at fault. Pairs with the
	// belt-and-braces guard in `build_rebus_base_level.py::ensure_beam_material`
	// (returns False with a `log_warning` when `_is_editor_runtime()` is false)
	// so even a direct `py` invocation from a `-game` console can't crash. See
	// the doc-comment on `CanRegenBeamMasterInProcess()` (file scope above) for
	// the gate semantics and the v1.0.120 README release block for the operator
	// offline-bake workflow.
	if (!CanRegenBeamMasterInProcess())
	{
		LastBeamMasterRegenResult = TEXT("fail-non-editor-runtime");
		LastBeamMasterRegenDetectedAfter = -1;
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("[Rebus] v1.0.121 ABORTING beam-master auto-regen (RebuildAndVerify"
				 "BeamMaster): this UE session is in -game / -server mode (GIsEditor=%d "
				 "IsRunningGame=%d IsRunningCommandlet=%d bForceEvenIfCurrent=%d) and "
				 "editor-only Python (EditorScriptingUtilities / EditorAssetLibrary / "
				 "MaterialEditingLibrary / AssetToolsHelpers) would crash the process if "
				 "invoked here -- v1.0.119 hit EXCEPTION_ACCESS_VIOLATION on this exact "
				 "code path. The stale on-disk M_RebusBeam.uasset will be used as-is for "
				 "this session. To regenerate: drive the v1.0.121 bake commandlet "
				 "`UnrealEditor-Cmd.exe REBUS_Visualiser.uproject -run=PythonScript "
				 "-Script=\"import build_rebus_base_level as b; b.ensure_beam_material("
				 "force=True); b.ensure_ies_profiles(force=True)\" -unattended -nop4 "
				 "-nosplash -stdout -FullStdOutLogOutput` and commit the regenerated "
				 "Content/REBUS/Materials/M_RebusBeam.uasset (+ .uexp) to source control. "
				 "Future -game sessions will then load the pre-baked material without "
				 "needing this rebuild."),
			GIsEditor ? 1 : 0,
			IsRunningGame() ? 1 : 0,
			IsRunningCommandlet() ? 1 : 0,
			bForceEvenIfCurrent ? 1 : 0);
		return false;
	}

	if (!GEngine)
	{
		LastBeamMasterRegenResult = TEXT("fail-no-gengine");
		LastBeamMasterRegenDetectedAfter = -1;
		UE_LOG(LogRebusVisualiser, Error,
			TEXT("[Rebus] v1.0.119 RebuildAndVerifyBeamMaster ABORTED -- GEngine null. "
				 "Subsystem Initialize() is running BEFORE GEngine is ready; no `py` Exec "
				 "is possible from here. Operator fix: ignore (this is a one-off race at "
				 "very early module init; the next `PostLoadMapWithWorld` will re-fire the "
				 "probe with GEngine live)."));
		return false;
	}

	if (!FModuleManager::Get().IsModuleLoaded(TEXT("PythonScriptPlugin")))
	{
		LastBeamMasterRegenResult = TEXT("fail-no-python-module");
		LastBeamMasterRegenDetectedAfter = -1;
		UE_LOG(LogRebusVisualiser, Error,
			TEXT("[Rebus] v1.0.119 RebuildAndVerifyBeamMaster ABORTED -- PythonScriptPlugin "
				 "not loaded. The `py` console command is not registered in this run, so "
				 "`ensure_beam_material(force=True)` cannot be invoked. Operator fix: "
				 "enable the Python Editor Script Plugin in the project's Plugins panel + "
				 "restart the editor (or check the project's .uproject for `PythonScriptPlugin` "
				 "in Plugins[]). This should NEVER happen in the bundled REBUS editor build "
				 "because PythonScriptPlugin is a hard dependency."));
		return false;
	}

	// Step (1): invalidate the cached master pointer BEFORE the regen kicks off.
	// `LoadObject` after `ensure_beam_material(force=True)` returns the freshly-
	// recreated UMaterial (the asset registry tracks the package replacement),
	// but our cached weak ptr would otherwise still reference the old UObject
	// until GC ran. Invalidating up-front forces the post-regen re-probe through
	// a fresh `LoadObject` call.
	InvalidateBeamMasterCache();

	// Step (2): pre-regen probe so we can log before+after revisions in the
	// success / failure diagnostic. Also gives us the early-out for the
	// `bForceEvenIfCurrent == false` + already-current case (the auto-purge
	// path -- we only reach `RebuildAndVerifyBeamMaster` from the stale branch
	// there anyway, but the manual `Rebus.ForceBeamMasterRegen` path uses the
	// `bForceEvenIfCurrent == true` override to ALWAYS regen, which is the
	// operator rescue contract).
	const FBeamMasterVersionReport BeforeReport = ProbeBeamMasterVersion();
	const bool bAlreadyCurrent =
		(BeforeReport.Version == EBeamMasterVersion::V117Plus) &&
		(BeforeReport.DetectedRevision >= BeforeReport.ExpectedRevision);
	if (bAlreadyCurrent && !bForceEvenIfCurrent)
	{
		LastBeamMasterRegenResult = TEXT("skipped-already-current");
		LastBeamMasterRegenDetectedAfter = BeforeReport.DetectedRevision;
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("[Rebus] v1.0.119 RebuildAndVerifyBeamMaster SKIPPED -- the on-disk master "
				 "is already at revision %d (expected %d); regen not needed. Call with "
				 "`bForceEvenIfCurrent=true` (e.g. `Rebus.ForceBeamMasterRegen`) to override "
				 "this guard for operator rescue when the per-fixture MIDs look wrong."),
			BeforeReport.DetectedRevision, BeforeReport.ExpectedRevision);
		return true;
	}

	// Step (3): pick a world (Editor wins over Game/PIE, same precedence the
	// manual `Rebus.RebuildBeamMaterial` handler uses so all entry points
	// resolve to identical scopes) + invoke the Python regen.
	UWorld* World = nullptr;
	for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
	{
		UWorld* W = Ctx.World();
		if (!W) continue;
		if (Ctx.WorldType == EWorldType::Editor) { World = W; break; }
		if (Ctx.WorldType == EWorldType::Game || Ctx.WorldType == EWorldType::PIE)
		{
			if (!World) World = W;
		}
	}

	const TCHAR* PyCmd = TEXT("py import build_rebus_base_level; build_rebus_base_level.ensure_beam_material(force=True)");
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("[Rebus] v1.0.119 RebuildAndVerifyBeamMaster: invoking `%s` (pre-regen "
			 "revision=%d, expected=%d, force=%s). Expect a `RebusBaseLevel: ...` log "
			 "block on the next line(s) confirming the regen landed; this function will "
			 "then re-probe to verify."),
		PyCmd,
		BeforeReport.DetectedRevision, BeforeReport.ExpectedRevision,
		bForceEvenIfCurrent ? TEXT("true") : TEXT("false"));

	const bool bExecOk = GEngine->Exec(World, PyCmd, *GLog);

	// Step (4): drain any async asset / shader compile the regen kicked off so
	// the post-regen probe reads a stable / fully-compiled material rather than
	// racing the compiler. `FAssetCompilingManager::FinishAllCompilation()` is
	// the canonical UE 5.7 entry-point for "flush every outstanding async compile
	// before we read the asset state" (file: Engine/Source/Developer/AssetCompilingManager/
	// Public/AssetCompilingManager.h, signature: `void FinishAllCompilation()` --
	// game-thread only, blocking until the queue drains). Wrapping in editor-only
	// because the manager is editor-only (matches the WITH_EDITOR gate around
	// this whole block).
	FAssetCompilingManager::Get().FinishAllCompilation();

	// Step (5): re-invalidate + re-probe. The pre-regen probe populated the
	// cache; the regen recreated the .uasset; we need a fresh `LoadObject` to
	// see the new content.
	InvalidateBeamMasterCache();
	const FBeamMasterVersionReport AfterReport = ProbeBeamMasterVersion();
	LastBeamMasterRegenDetectedAfter = AfterReport.DetectedRevision;

	const bool bVerified =
		(AfterReport.Version == EBeamMasterVersion::V117Plus) &&
		(AfterReport.DetectedRevision >= AfterReport.ExpectedRevision);

	if (bVerified)
	{
		LastBeamMasterRegenResult = TEXT("success");
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("[Rebus] v1.0.119 RebuildAndVerifyBeamMaster SUCCESS -- py Exec returned %s, "
				 "post-regen probe reports revision %d (expected %d). Refreshing all spawned-"
				 "fixture BeamMIDs against the freshly-baked master."),
			bExecOk ? TEXT("OK") : TEXT("FAILED-but-verified-post"),
			AfterReport.DetectedRevision, AfterReport.ExpectedRevision);

		// Step (6): rebind every existing fixture's BeamMID against the fresh
		// master. Normally a no-op at `Initialize()` time (SpawnedFixtures is
		// empty -- the subsystem hasn't fetched the scene yet), but mandatory
		// for the post-spawn `Rebus.ForceBeamMasterRegen` operator rescue path.
		RefreshAllSpawnedFixtureBeamMIDs();
		return true;
	}

	// FAILURE -- LOUD operator-actionable error log. This is the v1.0.117 /
	// v1.0.118 silent-failure case made loud: the Python regen "succeeded"
	// from GEngine->Exec's perspective (no parse error in the C++ command
	// string) but the post-regen probe shows the on-disk master is STILL
	// stale. Almost always means the Python module itself errored at runtime
	// (the v1.0.117 / v1.0.118 `TabError` case) and the error was swallowed
	// by GEngine->Exec's Python-bridge layer.
	LastBeamMasterRegenResult = bExecOk ? TEXT("fail-post-verify") : TEXT("fail-exec");
	UE_LOG(LogRebusVisualiser, Error,
		TEXT("[Rebus] v1.0.119 RebuildAndVerifyBeamMaster FAILED -- py Exec returned %s but "
			 "post-regen probe STILL reports the master as stale (verdict=%s detectedRev=%d "
			 "expectedRev=%d). This is the silent-failure case that bit v1.0.117 / v1.0.118 "
			 "(the Python module had a TabError at import time and the regen never ran -- "
			 "v1.0.119 fixes that, so if you see this in a v1.0.119 binary something NEW is "
			 "broken). Operator fix: (1) check the OutputLog for a Python traceback above "
			 "this line -- the actual error class + line number lives there; (2) run "
			 "`Rebus.ForceBeamMasterRegen` manually to retry; (3) if it still fails, attach "
			 "Saved/Logs/REBUS_Visualiser.log to a v1.0.120+ bug report. The bBeamMaster"
			 "AutoPurgeRun guard is released on this failure path so the next level reload "
			 "(or PostLoadMapWithWorld fire) re-attempts the regen automatically."),
		bExecOk ? TEXT("OK") : TEXT("FAILED"),
		BeamMasterVersionLabel(AfterReport.Version),
		AfterReport.DetectedRevision, AfterReport.ExpectedRevision);
	return false;
#else
	// Packaged build -- PythonScriptPlugin is editor-only per the v1.0.103 design,
	// so the `py` console command isn't registered and `ensure_beam_material`
	// can't run. The cooked master is whatever shipped in the .pak; we cannot
	// mutate a cooked .uasset at runtime. Hard `Error` so the operator knows
	// EXACTLY what to do to ship a correct binary -- mirrors the v1.0.105
	// packaged-build Nanite-conversion warning's shape.
	LastBeamMasterRegenResult = TEXT("fail-packaged-build");
	LastBeamMasterRegenDetectedAfter = -1;
	UE_LOG(LogRebusVisualiser, Error,
		TEXT("[Rebus] v1.0.119 RebuildAndVerifyBeamMaster ABORTED -- packaged build cannot "
			 "regenerate cooked .uasset files at runtime (PythonScriptPlugin is editor-only "
			 "per v1.0.103 design; force=%d). Operator fix: open the project in editor, run "
			 "`build_rebus_base_level.ensure_beam_material(force=True)` (or `Rebus.Rebuild"
			 "BeamMaterial` / `Rebus.ForceBeamMasterRegen`), re-package, re-deploy."),
		bForceEvenIfCurrent ? 1 : 0);
	return false;
#endif
}

int32 URebusVisualiserSubsystem::DumpBeamMaterialHealthForAllFixtures() const
{
	// v1.0.119 -- per-fixture material-binding health dump asked for in the
	// v1.0.119 brief. For every spawned fixture in every world, reports:
	//   * MaterialSlot0 of the BeamCone proc-mesh: class name + asset name
	//     (the operator sees `UMaterialInstanceDynamic` / `MID_M_RebusBeam_*`
	//     when healthy, `UMaterial` / `WorldGridMaterial` or `DefaultMaterial`
	//     when the v1.0.117 fallback grey symptom appears),
	//   * MID parent material (class + name) so the operator can see whether
	//     the MID was parented to the regenerated master or a stale one,
	//   * detected `BeamMaterialRevision` scalar on the MID -- this is the
	//     v1.0.117 scalar that proves which generation of the master the MID
	//     was created from (MISSING means the MID is parented to a pre-v1.0.117
	//     master, indicating the regen never landed for this fixture),
	//   * cached-master pointer state (so the operator can confirm the
	//     v1.0.119 cache is populated).
	// Returns the number of fixtures dumped so the console command can report
	// the count in its operator-facing summary line.
	int32 Dumped = 0;
	const TCHAR* CachedName = URebusVisualiserSubsystem::GetCachedBeamMasterDisplayName();
	const int32 CachedLoadCount = URebusVisualiserSubsystem::GetBeamMasterLoadCount();

	for (const TWeakObjectPtr<ARebusFixtureActor>& WeakFixture : SpawnedFixtures)
	{
		ARebusFixtureActor* Fixture = WeakFixture.Get();
		if (!Fixture) continue;
		// v1.0.119 -- the actor owns the dump line so it can read its private
		// `BeamCone` / `BeamMID` UPROPERTYs without exposing accessors. The
		// subsystem only forwards the cross-cutting state (expected revision +
		// cache info) needed for the operator-facing one-liner.
		Fixture->DumpBeamMaterialHealthForDebug(
			RebusExpectedBeamMaterialRevision,
			CachedName,
			CachedLoadCount);
		Dumped++;
	}
	return Dumped;
}
