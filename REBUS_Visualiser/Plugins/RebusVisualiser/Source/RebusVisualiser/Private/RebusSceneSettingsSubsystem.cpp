// Copyright REBUS Industries.
#include "RebusSceneSettingsSubsystem.h"
#include "RebusJson.h"
#include "RebusVisualiserLog.h"
#include "RebusFixtureActor.h"
#include "RebusFixtureControlSubsystem.h"
#include "Engine/GameInstance.h"

#include "EngineUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/IConsoleManager.h"
#include "DrawDebugHelpers.h"

#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/StaticMeshActor.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"

void URebusSceneSettingsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The baked floor defaults to Concrete and is visible; seed these so SceneState
	// round-trips the ground controls before the portal pushes its first value.
	Values.Add(TEXT("GroundSurface"), FRebusPropertyValue::MakeString(TEXT("Concrete")));
	Values.Add(TEXT("bGroundVisible"), FRebusPropertyValue::MakeBool(true));
	// v1.0.86: floor textures tile at 1 m per repeat by default. The C++ runtime pushes this
	// scalar to the floor MID after the ground material is (re)applied; the v1.0.86 ground
	// master uses it to drive WorldPosition-derived UVs (UV = WorldPosition.xy / (TilingMeters
	// * 100)), so 1.0 -> 1 m/tex repeat regardless of the floor mesh's actor scale (currently
	// 2000x a 100 cm engine plane = 2 km square). Pre-v1.0.86 ground masters lack the
	// `TilingMeters` parameter -- the push is a silent no-op there until the master is
	// regenerated via build_rebus_base_level.build().
	Values.Add(TEXT("GroundTilingMeters"), FRebusPropertyValue::MakeNumber(1.0));
	Values.Add(TEXT("bShowOrigin"), FRebusPropertyValue::MakeBool(false));
	// Hybrid cone-mesh volumetric beam is the default beam mode (v1.0.31); seed it so SceneState
	// round-trips the control and a respawn re-asserts it via ReapplyAll.
	Values.Add(TEXT("bMeshBeams"), FRebusPropertyValue::MakeBool(true));
	// Drive Orbit-imported fixture models from each fixture's pan/tilt solve. v1.0.35 introduced
	// this as a Phase-1 A/B test (default OFF). v1.0.65 flipped the SceneState seed to TRUE so
	// the SceneState round-trip reports the new default consistently with the control subsystem's
	// own bDriveOrbitModels default (also now true) -- otherwise the portal would see the seed
	// as false on first SceneState query and could re-disable the feature.
	Values.Add(TEXT("bDriveOrbitModels"), FRebusPropertyValue::MakeBool(true));

	// Seed the lightest tier as the runtime default (live previs streaming). This overrides the
	// heavier [SystemSettings] baseline from DefaultEngine.ini for the live stream; the portal
	// can push "previs"/"final" via the RenderQuality scene property. SetRenderQuality stores
	// the canonical tier name so the first SceneState read-back hydrates the control.
	SetRenderQuality(TEXT("live"));
}

// ---- Actor lookups (cached) -----------------------------------------------------------

ADirectionalLight* URebusSceneSettingsSubsystem::GetSun()
{
	if (CachedSun.IsValid()) return CachedSun.Get();
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ADirectionalLight> It(World); It; ++It) { CachedSun = *It; break; }
	}
	return CachedSun.Get();
}

ASkyLight* URebusSceneSettingsSubsystem::GetSkyLight()
{
	if (CachedSky.IsValid()) return CachedSky.Get();
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ASkyLight> It(World); It; ++It) { CachedSky = *It; break; }
	}
	return CachedSky.Get();
}

AExponentialHeightFog* URebusSceneSettingsSubsystem::GetFog()
{
	if (CachedFog.IsValid()) return CachedFog.Get();
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AExponentialHeightFog> It(World); It; ++It) { CachedFog = *It; break; }
	}
	return CachedFog.Get();
}

APostProcessVolume* URebusSceneSettingsSubsystem::GetPostProcess()
{
	if (CachedPostProcess.IsValid()) return CachedPostProcess.Get();
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<APostProcessVolume> It(World); It; ++It) { CachedPostProcess = *It; break; }
	}
	return CachedPostProcess.Get();
}

AStaticMeshActor* URebusSceneSettingsSubsystem::GetFloor()
{
	if (CachedFloor.IsValid()) return CachedFloor.Get();
	if (UWorld* World = GetWorld())
	{
		// The base level (and the EnsureSceneEnvironment backstop) tag the ground plane.
		for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
		{
			if (It->ActorHasTag(TEXT("RebusFloor"))) { CachedFloor = *It; break; }
		}
	}
	return CachedFloor.Get();
}

void URebusSceneSettingsSubsystem::SetGroundSurface(const FString& Preset)
{
	AStaticMeshActor* Floor = GetFloor();
	if (!Floor) return;

	UStaticMeshComponent* Comp = Floor->GetStaticMeshComponent();
	if (!Comp) return;

	// Only allow the known presets (matches GROUND_PRESETS in build_rebus_base_level.py),
	// so an arbitrary descriptor string can't trigger a load of an unintended asset.
	const FString Clean = Preset.TrimStartAndEnd();
	static const TSet<FString> Known = { TEXT("Concrete"), TEXT("Tarmac"), TEXT("Sand"), TEXT("Grass") };
	if (!Known.Contains(Clean))
	{
		UE_LOG(LogRebusVisualiser, Warning, TEXT("GroundSurface '%s' is not a known preset; ignoring."), *Clean);
		return;
	}

	const FString AssetPath = FString::Printf(
		TEXT("/Game/REBUS/Materials/MI_RebusGround_%s.MI_RebusGround_%s"), *Clean, *Clean);
	UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *AssetPath);
	if (!Mat)
	{
		UE_LOG(LogRebusVisualiser, Warning, TEXT("Ground material not found: %s"), *AssetPath);
		return;
	}

	// Apply the MI then immediately wrap in a fresh MID so per-session params (TilingMeters)
	// can be pushed without persisting to the .uasset. Discard any prior MID -- it wrapped the
	// previous MI and parameters on it would be ignored after this surface swap.
	Comp->SetMaterial(0, Mat);
	CachedFloorMID.Reset();
	if (UMaterialInstanceDynamic* MID = EnsureFloorMID())
	{
		// Re-assert the current scene's tiling so the swap doesn't reset it to the MI's default.
		if (const FRebusPropertyValue* TileVal = Values.Find(TEXT("GroundTilingMeters")))
		{
			MID->SetScalarParameterValue(TEXT("TilingMeters"), FMath::Max(TileVal->AsFloat(), 0.01f));
		}
	}
	UE_LOG(LogRebusVisualiser, Log, TEXT("Ground surface set to '%s' (TilingMeters reasserted on MID)."), *Clean);
}

UMaterialInstanceDynamic* URebusSceneSettingsSubsystem::EnsureFloorMID()
{
	if (UMaterialInstanceDynamic* MID = CachedFloorMID.Get())
	{
		return MID;
	}
	AStaticMeshActor* Floor = GetFloor();
	if (!Floor) return nullptr;
	UStaticMeshComponent* Comp = Floor->GetStaticMeshComponent();
	if (!Comp) return nullptr;
	// If the slot-0 material is already a MID (e.g. from a prior SetGroundSurface in the same
	// session) reuse it -- creating a second MID would orphan the parameters already pushed on
	// the first. Otherwise wrap the static MI in a fresh MID via the standard component helper
	// (sets it back on the component as slot 0 + returns the wrapper).
	if (UMaterialInstanceDynamic* Existing = Cast<UMaterialInstanceDynamic>(Comp->GetMaterial(0)))
	{
		CachedFloorMID = Existing;
		return Existing;
	}
	UMaterialInstanceDynamic* New = Comp->CreateAndSetMaterialInstanceDynamic(0);
	CachedFloorMID = New;
	return New;
}

void URebusSceneSettingsSubsystem::SetGroundTilingMeters(float Metres)
{
	// Clamp away from zero -- zero would divide-by-zero in the WorldPosition / (TilingMeters*100)
	// UV calc inside the material and render as a single texel stretched across the whole plane.
	// 1 cm minimum still produces sensible (if heavily-tiled) output.
	const float Clamped = FMath::Max(Metres, 0.01f);
	UMaterialInstanceDynamic* MID = EnsureFloorMID();
	if (!MID)
	{
		UE_LOG(LogRebusVisualiser, Warning, TEXT("SetGroundTilingMeters %.3f: no floor MID (no RebusFloor in level?)."), Clamped);
		return;
	}
	MID->SetScalarParameterValue(TEXT("TilingMeters"), Clamped);
	UE_LOG(LogRebusVisualiser, Log, TEXT("Floor TilingMeters set to %.3f m (1 texture repeat per %.3f m world)."),
		Clamped, Clamped);
}

void URebusSceneSettingsSubsystem::SetOriginGizmo(bool bShow)
{
#if ENABLE_DRAW_DEBUG
	UWorld* World = GetWorld();
	if (!World) return;

	// Persistent debug draw renders into the game viewport (and so into the Pixel Streaming
	// frame) in Development builds. Clear first so repeated toggles don't stack.
	FlushPersistentDebugLines(World);
	FlushDebugStrings(World);
	if (!bShow) return;

	const float Len = 500.f;   // 5 m arms
	const float Thick = 6.f;
	const float Head = 40.f;
	DrawDebugDirectionalArrow(World, FVector::ZeroVector, FVector(Len, 0.f, 0.f), Head, FColor::Red,   true, -1.f, 0, Thick); // +X
	DrawDebugDirectionalArrow(World, FVector::ZeroVector, FVector(0.f, Len, 0.f), Head, FColor::Green, true, -1.f, 0, Thick); // +Y
	DrawDebugDirectionalArrow(World, FVector::ZeroVector, FVector(0.f, 0.f, Len), Head, FColor::Blue,  true, -1.f, 0, Thick); // +Z
	DrawDebugSphere(World, FVector::ZeroVector, 15.f, 12, FColor::White, true, -1.f, 0, 3.f);

	// Axis end labels so orientation is readable in-game (persistent, matching the arms above).
	const float LabelFontScale = 1.5f;
	DrawDebugString(World, FVector( Len, 0.f, 0.f), TEXT("X+"), nullptr, FColor::Red,   -1.f, false, LabelFontScale);
	DrawDebugString(World, FVector(-Len, 0.f, 0.f), TEXT("X-"), nullptr, FColor::Red,   -1.f, false, LabelFontScale);
	DrawDebugString(World, FVector(0.f,  Len, 0.f), TEXT("Y+"), nullptr, FColor::Green, -1.f, false, LabelFontScale);
	DrawDebugString(World, FVector(0.f, -Len, 0.f), TEXT("Y-"), nullptr, FColor::Green, -1.f, false, LabelFontScale);
	DrawDebugString(World, FVector(0.f, 0.f,  Len), TEXT("Z+"), nullptr, FColor::Blue,  -1.f, false, LabelFontScale);
	DrawDebugString(World, FVector(0.f, 0.f, -Len), TEXT("Z-"), nullptr, FColor::Blue,  -1.f, false, LabelFontScale);

	UE_LOG(LogRebusVisualiser, Log, TEXT("Origin gizmo enabled (X=red, Y=green, Z=blue)."));
#else
	UE_LOG(LogRebusVisualiser, Warning, TEXT("Origin gizmo unavailable: debug draw is compiled out in this build."));
#endif
}

void URebusSceneSettingsSubsystem::SetMeshBeamsEnabled(bool bEnabled)
{
	UWorld* World = GetWorld();
	if (!World) return;

	int32 Count = 0;
	for (TActorIterator<ARebusFixtureActor> It(World); It; ++It)
	{
		if (ARebusFixtureActor* Fixture = *It)
		{
			Fixture->SetMeshBeamEnabled(bEnabled);
			++Count;
		}
	}
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("bMeshBeams=%d applied to %d fixture(s) (%s)."),
		bEnabled ? 1 : 0, Count,
		bEnabled ? TEXT("cone-mesh beam") : TEXT("fog beam restored"));
}

void URebusSceneSettingsSubsystem::SetDriveOrbitModelsEnabled(bool bEnabled)
{
	// Delegate to the fixture control subsystem (GameInstance-scoped), which owns the fixture
	// registry + the Orbit bind/scan. World subsystem -> GameInstance subsystem hop.
	UWorld* World = GetWorld();
	if (!World) return;
	if (UGameInstance* GI = World->GetGameInstance())
	{
		if (URebusFixtureControlSubsystem* Ctl = GI->GetSubsystem<URebusFixtureControlSubsystem>())
		{
			Ctl->SetDriveOrbitModels(bEnabled);
		}
	}
}

void URebusSceneSettingsSubsystem::SetRenderQuality(const FString& Tier)
{
	// Resolve the tier (case-insensitive; anything unrecognised falls back to the lightest
	// "live" preset so a bad push can never make the stream heavier than the live baseline).
	const FString Clean = Tier.TrimStartAndEnd();
	// Per-tier values drive ONLY the MegaLights lighting volume grid + sample count (smaller
	// GridPixelSize = sharper/heavier). The engine VolumetricFog froxel grid (r.VolumetricFog.*)
	// is intentionally NOT touched here -- it stays at the fixed [SystemSettings] defaults so a
	// tier switch can never override the user's VolumetricFog grid/history settings.
	int32 Samples = 2, GridPixelSize = 8, GridSizeZ = 64;   // live (default)
	const TCHAR* Resolved = TEXT("live");
	if (Clean.Equals(TEXT("final"), ESearchCase::IgnoreCase))
	{
		Samples = 8; GridPixelSize = 2; GridSizeZ = 192;
		Resolved = TEXT("final");
	}
	else if (Clean.Equals(TEXT("previs"), ESearchCase::IgnoreCase))
	{
		Samples = 4; GridPixelSize = 4; GridSizeZ = 128;
		Resolved = TEXT("previs");
	}
	else if (!Clean.Equals(TEXT("live"), ESearchCase::IgnoreCase))
	{
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("RenderQuality '%s' unknown; falling back to 'live'."), *Clean);
	}

	// Always (re)assert MegaLights + its volume so a tier switch can never leave them off.
	SetCVarInt(TEXT("r.MegaLights.Allow"), 1);
	SetCVarInt(TEXT("r.MegaLights.Volume"), 1);
	SetCVarInt(TEXT("r.MegaLights.NumSamplesPerPixel"), Samples);
	SetCVarInt(TEXT("r.MegaLights.Volume.GridPixelSize"), GridPixelSize);
	SetCVarInt(TEXT("r.MegaLights.Volume.GridSizeZ"), GridSizeZ);

	// Store the canonical resolved name (overwriting whatever raw string was pushed) so the
	// SceneState read-back always reports a valid tier.
	Values.Add(TEXT("RenderQuality"), FRebusPropertyValue::MakeString(Resolved));

	UE_LOG(LogRebusVisualiser, Log,
		TEXT("RenderQuality -> '%s' (MegaLights NumSamplesPerPixel=%d Volume.GridPixelSize=%d Volume.GridSizeZ=%d; VolumetricFog grid untouched)."),
		Resolved, Samples, GridPixelSize, GridSizeZ);
}

void URebusSceneSettingsSubsystem::SetScalabilityBucket(const TCHAR* Group, int32 Bucket)
{
	if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Group))
	{
		CVar->Set(FMath::Clamp(Bucket, 0, 4), ECVF_SetByGameOverride);
	}
}
void URebusSceneSettingsSubsystem::SetCVarFloat(const TCHAR* Name, float Value)
{
	if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
	{
		CVar->Set(Value, ECVF_SetByGameOverride);
	}
}
void URebusSceneSettingsSubsystem::SetCVarInt(const TCHAR* Name, int32 Value)
{
	if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
	{
		CVar->Set(Value, ECVF_SetByGameOverride);
	}
}

// ---- Apply ----------------------------------------------------------------------------

bool URebusSceneSettingsSubsystem::ApplySceneProperty(const FString& Name, const FRebusPropertyValue& Value)
{
	if (Name.IsEmpty() || !Value.IsSet()) return false;

	// Record for SceneState regardless of whether we have a live binding for it yet.
	Values.Add(Name, Value);

	bool bKnown = true;

	// --- Studio Light: Key / Sun ---
	if (Name == TEXT("KeyLightIntensity"))
	{
		if (ADirectionalLight* Sun = GetSun()) Sun->GetComponent()->SetIntensity(Value.AsFloat());
	}
	else if (Name == TEXT("ColorTemperatureKelvin"))
	{
		if (ADirectionalLight* Sun = GetSun())
		{
			Sun->GetComponent()->SetUseTemperature(true);
			Sun->GetComponent()->SetTemperature(Value.AsFloat());
		}
	}
	else if (Name == TEXT("SunYaw") || Name == TEXT("SunPitch"))
	{
		if (ADirectionalLight* Sun = GetSun())
		{
			FRotator R = Sun->GetActorRotation();
			if (Name == TEXT("SunYaw")) R.Yaw = Value.AsFloat(); else R.Pitch = Value.AsFloat();
			Sun->SetActorRotation(R);
		}
	}
	else if (Name == TEXT("bCastShadows"))
	{
		if (ADirectionalLight* Sun = GetSun()) Sun->GetComponent()->SetCastShadows(Value.bBool);
	}
	else if (Name == TEXT("LightSourceAngle"))
	{
		if (ADirectionalLight* Sun = GetSun()) Sun->GetComponent()->LightSourceAngle = Value.AsFloat();
	}
	// --- Studio Light: Sky ---
	else if (Name == TEXT("bSkyLightEnabled"))
	{
		if (ASkyLight* Sky = GetSkyLight()) Sky->GetLightComponent()->SetVisibility(Value.bBool);
	}
	else if (Name == TEXT("SkyLightIntensity"))
	{
		if (ASkyLight* Sky = GetSkyLight()) Sky->GetLightComponent()->SetIntensity(Value.AsFloat());
	}
	else if (Name == TEXT("SkyLightColor"))
	{
		if (ASkyLight* Sky = GetSkyLight()) Sky->GetLightComponent()->SetLightColor(Value.Color);
	}
	else if (Name == TEXT("IndirectLightingIntensity"))
	{
		if (ASkyLight* Sky = GetSkyLight()) Sky->GetLightComponent()->SetIndirectLightingIntensity(Value.AsFloat());
	}
	else if (Name == TEXT("VolumetricScatteringIntensity"))
	{
		if (ASkyLight* Sky = GetSkyLight()) Sky->GetLightComponent()->SetVolumetricScatteringIntensity(Value.AsFloat());
	}
	// --- Height Fog ---
	else if (Name == TEXT("FogDensity"))
	{
		if (AExponentialHeightFog* Fog = GetFog()) Fog->GetComponent()->SetFogDensity(Value.AsFloat());
	}
	else if (Name == TEXT("StartDistance"))
	{
		if (AExponentialHeightFog* Fog = GetFog()) Fog->GetComponent()->SetStartDistance(Value.AsFloat());
	}
	else if (Name == TEXT("FogHeightFalloff"))
	{
		if (AExponentialHeightFog* Fog = GetFog()) Fog->GetComponent()->SetFogHeightFalloff(Value.AsFloat());
	}
	else if (Name == TEXT("InscatteringColor"))
	{
		if (AExponentialHeightFog* Fog = GetFog()) Fog->GetComponent()->SetFogInscatteringColor(Value.Color);
	}
	else if (Name == TEXT("bVolumetricFog"))
	{
		if (AExponentialHeightFog* Fog = GetFog()) Fog->GetComponent()->SetVolumetricFog(Value.bBool);
	}
	else if (Name == TEXT("VolumetricScatteringDistribution"))
	{
		if (AExponentialHeightFog* Fog = GetFog()) Fog->GetComponent()->SetVolumetricFogScatteringDistribution(Value.AsFloat());
	}
	else if (Name == TEXT("VolumetricExtinctionScale"))
	{
		if (AExponentialHeightFog* Fog = GetFog()) Fog->GetComponent()->SetVolumetricFogExtinctionScale(Value.AsFloat());
	}
	// --- Ground plane (portal-controllable surface + visibility) ---
	else if (Name == TEXT("GroundSurface"))
	{
		SetGroundSurface(Value.String);
	}
	else if (Name == TEXT("bGroundVisible"))
	{
		if (AStaticMeshActor* Floor = GetFloor())
		{
			const bool bHidden = !Value.bBool;
			Floor->SetActorHiddenInGame(bHidden);
#if WITH_EDITOR
			Floor->SetIsTemporarilyHiddenInEditor(bHidden);
#endif
		}
	}
	// v1.0.86 floor texture tiling at world scale -- 1 texture repeat per N metres regardless
	// of the floor mesh's actor scale. The push is a silent no-op for materials without the
	// `TilingMeters` parameter (e.g. a pre-v1.0.86 ground master that hasn't been regenerated).
	else if (Name == TEXT("GroundTilingMeters"))
	{
		SetGroundTilingMeters(Value.AsFloat());
	}
	// --- Debug: world-origin XYZ gizmo (orientation check) ---
	else if (Name == TEXT("bShowOrigin"))
	{
		SetOriginGizmo(Value.bBool);
	}
	// --- Hybrid beam mode: cone-mesh volumetric beam vs the old fog beam (§8.4a) ---
	else if (Name == TEXT("bMeshBeams"))
	{
		SetMeshBeamsEnabled(Value.bBool);
	}
	// --- Phase-1 sync test: drive Orbit-imported fixture models from fixture motion ---
	else if (Name == TEXT("bDriveOrbitModels"))
	{
		SetDriveOrbitModelsEnabled(Value.bBool);
	}
	// --- Stream Quality (Pixel Streaming encoder params) ---
	else if (Name == TEXT("StreamStartBitrateMbps"))
	{
		SetCVarInt(TEXT("PixelStreaming2.WebRTC.StartBitrate"), Value.AsInt() * 1000000);
	}
	else if (Name == TEXT("StreamMinBitrateMbps"))
	{
		SetCVarInt(TEXT("PixelStreaming2.WebRTC.MinBitrate"), Value.AsInt() * 1000000);
	}
	else if (Name == TEXT("StreamMaxBitrateMbps"))
	{
		SetCVarInt(TEXT("PixelStreaming2.WebRTC.MaxBitrate"), Value.AsInt() * 1000000);
	}
	else if (Name == TEXT("StreamFps"))
	{
		SetCVarInt(TEXT("PixelStreaming2.WebRTC.Fps"), Value.AsInt());
	}
	else if (Name == TEXT("StreamMinQP"))
	{
		SetCVarInt(TEXT("PixelStreaming2.Encoder.MaxQuality"), Value.AsInt());
	}
	else if (Name == TEXT("StreamMaxQP"))
	{
		SetCVarInt(TEXT("PixelStreaming2.Encoder.MinQuality"), Value.AsInt());
	}
	// --- Engine Quality ---
	else if (Name == TEXT("RenderQuality"))
	{
		// MegaLights + volumetric-fog-volume cost/quality tier (live/previs/final).
		SetRenderQuality(Value.String);
	}
	else if (Name == TEXT("ScreenPercentage"))
	{
		SetCVarFloat(TEXT("r.ScreenPercentage"), Value.AsFloat());
	}
	else if (Name == TEXT("ShadowQuality"))       { SetScalabilityBucket(TEXT("sg.ShadowQuality"), Value.AsInt()); }
	else if (Name == TEXT("PostProcessQuality"))  { SetScalabilityBucket(TEXT("sg.PostProcessQuality"), Value.AsInt()); }
	else if (Name == TEXT("EffectsQuality"))      { SetScalabilityBucket(TEXT("sg.EffectsQuality"), Value.AsInt()); }
	else if (Name == TEXT("TextureQuality"))      { SetScalabilityBucket(TEXT("sg.TextureQuality"), Value.AsInt()); }
	else if (Name == TEXT("ViewDistanceQuality")) { SetScalabilityBucket(TEXT("sg.ViewDistanceQuality"), Value.AsInt()); }
	else if (Name == TEXT("FoliageQuality"))      { SetScalabilityBucket(TEXT("sg.FoliageQuality"), Value.AsInt()); }
	else
	{
		// Ground (Surface), Cine Camera, post-fx etc. land here. We still store the value so
		// SceneState round-trips it; concrete binding is left to the scene actors in content.
		bKnown = false;
		UE_LOG(LogRebusVisualiser, Verbose, TEXT("SetSceneProperty '%s' stored without a live binding."), *Name);
	}

	if (bKnown)
	{
		UE_LOG(LogRebusVisualiser, Verbose, TEXT("Applied scene property '%s' to live actors."), *Name);
	}

	return bKnown;
}

void URebusSceneSettingsSubsystem::ReapplyAll()
{
	if (Values.Num() == 0) return;

	// Snapshot first: ApplySceneProperty writes back into Values, so iterating it directly would
	// mutate the container mid-iteration. The snapshot is the authoritative state to reassert.
	TArray<TPair<FString, FRebusPropertyValue>> Snapshot;
	Snapshot.Reserve(Values.Num());
	for (const TPair<FString, FRebusPropertyValue>& Pair : Values)
	{
		Snapshot.Add(Pair);
	}

	// Drop the cached actor handles so each lookup re-resolves against the CURRENT world actors
	// (the environment may have just been (re)spawned, invalidating an earlier null/stale cache).
	CachedSun.Reset();
	CachedSky.Reset();
	CachedFog.Reset();
	CachedPostProcess.Reset();
	CachedFloor.Reset();
	CachedFloorMID.Reset(); // v1.0.86: invalidate so the next push re-wraps the (possibly re-spawned) floor's slot 0

	for (const TPair<FString, FRebusPropertyValue>& Pair : Snapshot)
	{
		ApplySceneProperty(Pair.Key, Pair.Value);
	}

	UE_LOG(LogRebusVisualiser, Log, TEXT("Re-applied %d scene property value(s) to live actors."), Snapshot.Num());
}

bool URebusSceneSettingsSubsystem::HandleSceneDescriptor(const FString& Type, const TSharedPtr<FJsonObject>& Msg)
{
	if (!Msg.IsValid()) return false;

	if (Type == TEXT("SetSceneProperty"))
	{
		FString Name;
		RebusJson::TryGetString(Msg, TEXT("name"), Name);
		const FRebusPropertyValue Value = FRebusPropertyValue::FromJson(Msg->TryGetField(TEXT("value")));
		ApplySceneProperty(Name, Value);
		return true;
	}
	if (Type == TEXT("SetSceneProperties"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Props = nullptr;
		if (Msg->TryGetArrayField(TEXT("properties"), Props) && Props)
		{
			for (const TSharedPtr<FJsonValue>& PV : *Props)
			{
				const TSharedPtr<FJsonObject>* PO = nullptr;
				if (!PV->TryGetObject(PO) || !PO) continue;
				FString Name;
				RebusJson::TryGetString(*PO, TEXT("name"), Name);
				const FRebusPropertyValue Value = FRebusPropertyValue::FromJson((*PO)->TryGetField(TEXT("value")));
				ApplySceneProperty(Name, Value);
			}
		}
		return true;
	}

	return false;
}
