// Copyright REBUS Industries.
#include "RebusSceneSettingsSubsystem.h"
#include "RebusJson.h"
#include "RebusVisualiserLog.h"

#include "EngineUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/IConsoleManager.h"

#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"

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
