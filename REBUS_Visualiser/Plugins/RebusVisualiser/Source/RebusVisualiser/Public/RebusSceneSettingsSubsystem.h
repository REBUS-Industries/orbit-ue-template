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
class UMaterialInstanceDynamic;

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
	// v1.0.86: the loaded MI is now wrapped in a UMaterialInstanceDynamic so per-session
	// scalar parameters (specifically `TilingMeters`) can be pushed without persisting to the
	// .uasset; the cached MID is reused by SetGroundTilingMeters.
	void SetGroundSurface(const FString& Preset);

	// v1.0.86: set the floor texture's physical tile size. With the v1.0.86 ground master,
	// `TilingMeters = 1.0` means one texture repeat per 1 m of world space (so a 2 km floor
	// plane shows 2000 repeats per side instead of one stretched repeat). Lower => finer
	// tiling (0.5 m = 2 repeats/m); higher => coarser. Pushes the scalar to the cached floor
	// MID; safely no-ops if the underlying material has no `TilingMeters` parameter (e.g. a
	// pre-v1.0.86 master that hasn't been regenerated yet via the Python build script).
	void SetGroundTilingMeters(float Metres);

	// Lazily create-or-fetch the MID wrapping the floor's current slot-0 material so per-
	// session scalar parameters can be set without modifying the .uasset. Returns nullptr if
	// the floor or its material is missing.
	UMaterialInstanceDynamic* EnsureFloorMID();

	// Draw (or clear) a persistent world-origin XYZ axis gizmo for orientation checks.
	// X=red, Y=green, Z=blue (Unreal convention). Toggled by the bShowOrigin scene property.
	void SetOriginGizmo(bool bShow);

	// Enable/disable the hybrid cone-mesh volumetric beam on every spawned fixture (bMeshBeams
	// scene property, default true). When disabled the cone is hidden and each fixture restores
	// its SpotLight fog VolumetricScatteringIntensity (the old froxel beam), for runtime A/B.
	// Public so the v1.0.47 `Rebus.MeshBeams [0|1]` console command can drive the same path.
public:
	void SetMeshBeamsEnabled(bool bEnabled);

	// v1.0.87 InternalBeam A/B mode. When ON, every fixture hides its Epic / cone-mesh beam and
	// promotes its SpotLight to the visible volumetric shaft, applying the back-offset that places
	// the cone exit at the lens diameter and opting the body meshes out of self-shadowing. When OFF
	// the Epic beam returns intact + every per-fixture cached state restores byte-exact. Public so
	// the v1.0.87 `Rebus.InternalBeam [0|1]` console command can drive the same path the
	// SetSceneProperty bInternalBeam wire route uses.
	//
	// v1.0.89: the toggle ALSO drives a single global push of `r.LightFunctionAtlas.Enabled 1`
	// while the mode is ON, so each SpotLight's gobo light-function modulates the volumetric
	// shaft (UE 5.7 only routes light functions through the volumetric scattering integrator
	// when the atlas path is on). Cached prior value + restored byte-exact on the OFF transition;
	// see PushLightFunctionAtlasForInternalBeam in the .cpp.
	void SetInternalBeamEnabled(bool bEnabled);
private:
	// v1.0.89 InternalBeam <-> r.LightFunctionAtlas.Enabled push helper. bOn=true caches the
	// CVar's current int value into LightFunctionAtlasPriorValue and forces it to 1 (so the
	// gobo cookie modulates the volumetric shaft, not just the lit floor pool); bOn=false
	// restores the cached prior value byte-exact. Idempotent via bLightFunctionAtlasPushActive
	// so a double-true / double-false call is safe (the SetInternalBeamEnabled wire route is
	// hit on EVERY scene-property push, which can fire repeatedly on ReapplyAll).
	void PushLightFunctionAtlasForInternalBeam(bool bOn);

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
	// v1.0.86 MID wrapping the floor's slot-0 material so SetGroundTilingMeters can push
	// `TilingMeters` without persisting to disk. Lazily created in EnsureFloorMID, reset
	// when SetGroundSurface swaps the underlying material so the next push wraps the new MI.
	TWeakObjectPtr<UMaterialInstanceDynamic> CachedFloorMID;

	// v1.0.89: cached prior-value of `r.LightFunctionAtlas.Enabled` captured at the OFF -> ON
	// transition, restored on ON -> OFF. Sentinel int (negative implies "no snapshot taken")
	// guards against a double-restore landing the engine default 1 onto a deployment that
	// expected 0 (e.g. anti-ghost pack still in force). Idempotency latch below.
	int32 LightFunctionAtlasPriorValue = -1;
	bool bLightFunctionAtlasPushActive = false;
};
