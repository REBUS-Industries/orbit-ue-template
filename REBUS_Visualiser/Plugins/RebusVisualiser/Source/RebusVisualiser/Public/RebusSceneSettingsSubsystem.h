// Copyright REBUS Industries.
//
// Applies SetSceneProperty / SetSceneProperties (ue-plugin-build-guide.md §5.4 / §9) by
// switching on the catalogue `name` and pushing `value` onto the matching scene-actor
// property, Pixel Streaming encoder param, or engine scalability command. Treats each as the
// new authoritative value (fire-and-forget; unknown names ignored). Owns the SceneState the
// plugin reports back on RequestSceneState (§6.3) so the portal hydrates from live values.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "RebusPropertyValue.h"
#include "RebusSceneSettingsSubsystem.generated.h"

class ADirectionalLight;
class ASkyLight;
class AExponentialHeightFog;
class APostProcessVolume;
class AStaticMeshActor;
class FJsonObject;

UCLASS()
class REBUSVISUALISER_API URebusSceneSettingsSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	// Seeds sensible defaults for the portal-controllable ground so the first SceneState
	// read-back hydrates the controls even before the portal pushes a value.
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	// Apply a single catalogue property; stores the value for SceneState read-back.
	// Returns false for an unknown name (the caller may surface a Notice).
	bool ApplySceneProperty(const FString& Name, const FRebusPropertyValue& Value);

	// Re-apply every stored scene-property value (seeded defaults + everything the portal has
	// pushed so far) to the live scene actors. Called by the session subsystem once the scene
	// environment is (re)ensured / fixtures (re)spawn, so portal state "sticks" on first load
	// without needing a recycle (values pushed before an actor existed, or actors rebuilt after
	// a value was applied, are reasserted here). Iterates a snapshot so it is re-entrant-safe.
	void ReapplyAll();

	// Route a SetSceneProperty / SetSceneProperties descriptor. Returns true if handled.
	bool HandleSceneDescriptor(const FString& Type, const TSharedPtr<FJsonObject>& Msg);

	// Snapshot every owned property for a SceneState reply.
	const TMap<FString, FRebusPropertyValue>& GetSceneState() const { return Values; }

private:
	ADirectionalLight* GetSun();
	ASkyLight* GetSkyLight();
	AExponentialHeightFog* GetFog();
	APostProcessVolume* GetPostProcess();
	AStaticMeshActor* GetFloor();

	// Swap the floor plane's material to the generated MI_RebusGround_<Preset> instance.
	void SetGroundSurface(const FString& Preset);

	// Draw (or clear) a persistent world-origin XYZ axis gizmo for orientation checks.
	// X=red, Y=green, Z=blue (Unreal convention). Toggled by the bShowOrigin scene property.
	void SetOriginGizmo(bool bShow);

	// Enable/disable the hybrid cone-mesh volumetric beam on every spawned fixture (bMeshBeams
	// scene property, default true). When disabled the cone is hidden and each fixture restores
	// its SpotLight fog VolumetricScatteringIntensity (the old froxel beam), for runtime A/B.
	void SetMeshBeamsEnabled(bool bEnabled);

	// Enable/disable driving Orbit-imported fixture models from fixture motion (bDriveOrbitModels
	// scene property, default false). Routes to the fixture control subsystem, which binds the
	// imported models to fixtures by object id and drives them with the same motion solve.
	void SetDriveOrbitModelsEnabled(bool bEnabled);

	// Apply a MegaLights + volumetric-fog-volume quality tier at runtime ("live"/"previs"/
	// "final", case-insensitive; unknown -> "live"). Re-tunes r.MegaLights.* via console
	// override and stores the canonical tier name so SceneState round-trips it.
	void SetRenderQuality(const FString& Tier);

	void SetScalabilityBucket(const TCHAR* Group, int32 Bucket);
	void SetCVarFloat(const TCHAR* CVar, float Value);
	void SetCVarInt(const TCHAR* CVar, int32 Value);

private:
	TMap<FString, FRebusPropertyValue> Values;

	TWeakObjectPtr<ADirectionalLight> CachedSun;
	TWeakObjectPtr<ASkyLight> CachedSky;
	TWeakObjectPtr<AExponentialHeightFog> CachedFog;
	TWeakObjectPtr<APostProcessVolume> CachedPostProcess;
	TWeakObjectPtr<AStaticMeshActor> CachedFloor;
};
