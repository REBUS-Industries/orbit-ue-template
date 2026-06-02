// Copyright REBUS Industries.
#include "RebusSceneSettingsSubsystem.h"
#include "RebusJson.h"
#include "RebusVisualiserLog.h"

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

void URebusSceneSettingsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The baked floor defaults to Concrete and is visible; seed these so SceneState
	// round-trips the ground controls before the portal pushes its first value.
	Values.Add(TEXT("GroundSurface"), FRebusPropertyValue::MakeString(TEXT("Concrete")));
	Values.Add(TEXT("bGroundVisible"), FRebusPropertyValue::MakeBool(true));
	Values.Add(TEXT("bShowOrigin"), FRebusPropertyValue::MakeBool(false));
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
	if (UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *AssetPath))
	{
		Comp->SetMaterial(0, Mat);
		UE_LOG(LogRebusVisualiser, Log, TEXT("Ground surface set to '%s'."), *Clean);
	}
	else
	{
		UE_LOG(LogRebusVisualiser, Warning, TEXT("Ground material not found: %s"), *AssetPath);
	}
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
	// --- Debug: world-origin XYZ gizmo (orientation check) ---
	else if (Name == TEXT("bShowOrigin"))
	{
		SetOriginGizmo(Value.bBool);
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

	return bKnown;
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
