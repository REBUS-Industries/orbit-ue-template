// Copyright REBUS Industries.
//
// One spawned actor per scene fixture (ue-plugin-build-guide.md §2/§7/§8). It owns:
//   * the fixture-root component placed from matrixZUpMeters (§7.4 step 7),
//   * procedural mesh proxies built from /meshes, bucketed onto motion axes (§7.6),
//   * a SpotLight placed/aimed from the GDTF <Beam> node, tracking the head (§7.7),
//   * all the per-fixture control state (dimmer/color/pan-tilt/zoom/gobo/...) with optional
//     fadeMs interpolation (§11),
//   * a selection highlight via custom-depth stencil (§5.3).
//
// The actor is registered under its Speckle node id in URebusFixtureControlSubsystem.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RebusSceneTypes.h"
#include "RebusFixtureActor.generated.h"

class USpotLightComponent;
class UStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class UProceduralMeshComponent;
class UTextureLightProfile;
class FRebusRestClient;

UENUM()
enum class ERebusShutterMode : uint8
{
	Open = 0,
	Closed = 1,
	Strobe = 2
};

// A single scalar channel that can either snap or ease-in-out over fadeMs.
struct FRebusScalarFade
{
	float Current = 0.f;
	float Start = 0.f;
	float Target = 0.f;
	float Elapsed = 0.f;
	float Duration = 0.f; // seconds; 0 = snapped

	void SetTarget(float InTarget, float FadeSeconds)
	{
		if (FadeSeconds <= 0.f)
		{
			Current = Target = InTarget;
			Duration = 0.f;
			return;
		}
		Start = Current;
		Target = InTarget;
		Elapsed = 0.f;
		Duration = FadeSeconds;
	}

	// Returns true while still animating.
	bool Tick(float Dt)
	{
		if (Duration <= 0.f) { Current = Target; return false; }
		Elapsed += Dt;
		const float Alpha = FMath::Clamp(Elapsed / Duration, 0.f, 1.f);
		Current = FMath::InterpEaseInOut(Start, Target, Alpha, 2.f);
		if (Alpha >= 1.f) { Duration = 0.f; return false; }
		return true;
	}
};

UCLASS()
class REBUSVISUALISER_API ARebusFixtureActor : public AActor
{
	GENERATED_BODY()

public:
	ARebusFixtureActor();

	virtual void Tick(float DeltaSeconds) override;

	// Build the actor from the scene placement + profile. Meshes may be empty (light-only).
	// InInlineIes carries any RegisterFixtureIes raw .ies profiles pushed for this libraryId
	// (preferred over a URL fetch in SelectIesForZoom); InInlineGobos carries any
	// RegisterFixtureGobos base64 images (preferred over a URL fetch in ApplyGobo). Both empty.
	void Setup(const FRebusSceneFixture& InSceneFixture,
		const FRebusFixtureProfile& InProfile,
		const FRebusMeshBundle& InMeshes,
		const FRebusInlineIes& InInlineIes,
		const FRebusInlineGobos& InInlineGobos);

	// Swap the inline gobo image set (RegisterFixtureGobos re-push) and re-apply the currently
	// selected gobo so it refreshes without a reselect. Empty set clears nothing already shown.
	void SetInlineGobos(const FRebusInlineGobos& InInlineGobos);

	// ---- Control surface (§5.2). Each takes an optional fade in seconds (<=0 = snap). ----
	void ApplyDimmer(float Intensity01, float FadeSeconds = 0.f);
	void ApplyColor(const FLinearColor& SrgbColor, float FadeSeconds = 0.f);
	void ApplyPanTilt(float PanDeg, float TiltDeg, float FadeSeconds = 0.f);
	void ApplyZoom(float ZoomHalfAngleDeg, float FadeSeconds = 0.f);
	// bHasIndex=false => clear. WheelIndex (0-based) selects the Nth gobo-kind wheel for
	// multi-wheel fixtures; Wheel is the legacy name hint. Selector precedence: WheelIndex >
	// Wheel name > first gobo-kind wheel. GoboIndex still selects the slot within the wheel.
	void ApplyGobo(int32 GoboIndex, bool bHasIndex, int32 WheelIndex = INDEX_NONE, const FString& Wheel = FString(), float FadeSeconds = 0.f);
	void ApplyIris(float Iris01, float FadeSeconds = 0.f);
	void ApplyFocus(float Focus01, float FadeSeconds = 0.f);
	void ApplyFrost(float Frost01, float FadeSeconds = 0.f);
	void ApplyColorTemp(float Kelvin);
	void ApplyShutter(ERebusShutterMode Mode, float RateHz);
	void ApplyGoboRotation(float Speed);
	void ApplyPrism(int32 Facets, float RotationDeg);
	void ApplyBeamVolumetrics(float Intensity, bool bCastVolumetricShadow);

	// Selection highlight (§5.3).
	void SetSelected(bool bSelected, bool bPrimary);

	// REST client used to lazily fetch gobo wheel images / IES bytes.
	void SetRestClient(TSharedPtr<FRebusRestClient> InClient) { RestClient = InClient; }

	const FString& GetFixtureId() const { return FixtureId; }
	const FString& GetLibraryFixtureId() const { return LibraryFixtureId; }
	const FString& GetDisplayName() const { return DisplayName; }
	bool HasPanTilt() const { return bHasPanTilt; }
	bool HasGobo() const { return bHasGobo; }

	// Diagnostics: how many motion axes the rig has, and how many mesh proxies were built.
	int32 GetMotionAxisCount() const { return Profile.MotionRig.Axes.Num(); }
	int32 GetMeshComponentCount() const { return MeshComponents.Num(); }

	// Reset the per-batch "hero beam" volumetric-shadow budget. Called by the session subsystem
	// before each (re)spawn so the first N fixtures of every fresh scene get volumetric shadows
	// (rather than the budget being permanently consumed by the very first scene). See
	// BuildSpotLight + RebusMaxVolumetricShadowBeams.
	static void ResetVolumetricShadowBudget();

private:
	void BuildComponentHierarchy();
	void BuildMeshes(const FRebusMeshBundle& Meshes);
	void ResolveHeadAxisFromMeshes(); // refine HeadAxisIndex to the deepest axis driving a head mesh
	void BuildSpotLight();
	void BuildLensDisc();         // emissive "glowing lens" flare disc at the beam origin (§8.3a)
	void RefreshLensDisc();       // drive disc emissive from live dimmer x colour x shutter-gate
	// Resolve the lens-opening diameter (metres) shared by the lens disc + SpotLight SourceRadius,
	// so the glowing disc and the finite beam origin always line up. Precedence:
	// photometrics.lensDiameter -> source.radiusMeters*2 -> source.diameterMeters -> a clamped
	// fraction of the fixture dimensions (synthetic fallback so something always shows) -> -1
	// (nothing resolvable). OutSrc names the resolved source for diagnostics.
	double ResolveLensDiameterMeters(const TCHAR*& OutSrc) const;
	void RefreshMotion();         // re-solve pan/tilt and push transforms to groups + light
	void RefreshIntensity();      // fold dimmer * shutter-gate into the light intensity
	void RecomputeConeAngles();   // from zoom + photometrics + iris/frost
	void SelectIesForZoom();      // pick/assign the zoom-keyed IES profile (inline > URL > cone)
	// Pick the inline IES profile nearest the requested zoomDmx (null when none pushed).
	const FRebusInlineIesProfile* SelectInlineIes(int32 ZoomDmx) const;
	void FetchAndAssignIes(const FString& IesUrl);

	// Assign the gobo for the selected slot, preferring an inline base64 image (no fetch) over
	// the signed imageUrl fetch over nothing. WheelIndex/WheelName disambiguate multi-wheel
	// fixtures (see ResolveGoboWheel for the precedence).
	void AssignGobo(int32 GoboIndex, int32 WheelIndex, const FString& WheelName);
	// Resolve the target gobo WHEELINDEX (0-based into the full wheels[]) from the selectors.
	// An explicit WheelIndex is trusted as-is (the primary cache key). When absent, fall back to
	// the FIRST gobo-kind wheel = the smallest wheelIndex among inline entries tagged kind=="gobo"
	// (then any wheel's smallest index). INDEX_NONE when no inline entry carries a wheelIndex
	// (legacy push) -- callers then fall back to wheel-name / any-slot matching.
	int32 ResolveGoboWheelIndex(int32 WheelIndex, const FString& WheelName) const;
	// Pick the inline gobo image for the resolved (wheelIndex, slot); null when none pushed/match.
	const FRebusInlineGobo* SelectInlineGobo(int32 Slot, int32 WheelIndex, const FString& WheelName) const;
	// Build + assign a gobo UTexture2D from decoded image bytes via the existing light-function
	// MID path; returns true when a texture was applied.
	bool ApplyGoboTextureFromBytes(const TArray<uint8>& Bytes);
	void FetchAndAssignGobo(int32 GoboIndex);            // existing profile-wheel URL path
	void FetchAndAssignGoboFromUrl(const FString& ImageUrl);

	static int32 FindFirstGoboWheel(const FRebusFixtureProfile& Profile);

	// Count of spotlights granted volumetric shadows in the current spawn batch (hero-beam cap).
	static int32 VolumetricShadowBeamCount;

private:
	UPROPERTY() TObjectPtr<USceneComponent> FixtureRoot = nullptr;
	UPROPERTY() TObjectPtr<USpotLightComponent> SpotLight = nullptr;

	// One scene component per motion axis (index-aligned with Profile.MotionRig.Axes), plus
	// the mesh proxies parented under FixtureRoot (their relative transform is driven directly).
	UPROPERTY() TArray<TObjectPtr<UProceduralMeshComponent>> MeshComponents;

	// Index into MeshComponents -> axis index it tracks (INDEX_NONE = static base).
	TArray<int32> MeshAxisBucket;

	UPROPERTY() TObjectPtr<UTextureLightProfile> ActiveIesProfile = nullptr;
	UPROPERTY() TObjectPtr<class UMaterialInstanceDynamic> GoboMID = nullptr;

	// Emissive "glowing lens" flare disc: a thin plane at the beam origin, normal along the beam
	// forward, parented under FixtureRoot and driven by BeamRest/head motion like the SpotLight so
	// it tracks pan/tilt. Null when no lensDiameter/source aperture resolved or the material is
	// missing. Its MID's EmissiveColor/EmissiveStrength follow the live fixture colour x dimmer.
	UPROPERTY() TObjectPtr<UStaticMeshComponent> LensDisc = nullptr;
	UPROPERTY() TObjectPtr<class UMaterialInstanceDynamic> LensDiscMID = nullptr;

	// Hard CDO references to the disc mesh + material, resolved in the constructor so the COOKER
	// packages them for -game/packaged builds. A runtime LoadObject-by-path is NOT a cook
	// dependency, which is why the disc silently failed to load in cooked builds (the material is
	// referenced by nothing in the level). May be null if the asset is absent; BuildLensDisc then
	// falls back to a runtime load and logs which asset failed.
	UPROPERTY() TObjectPtr<UStaticMesh> LensPlaneMesh = nullptr;
	UPROPERTY() TObjectPtr<UMaterialInterface> LensMaterial = nullptr;

	TSharedPtr<FRebusRestClient> RestClient;

	// Identity / capabilities.
	FString FixtureId;
	FString LibraryFixtureId;
	FString DisplayName;
	bool bHasPanTilt = false;
	bool bHasGobo = false;

	// Parsed profile (kept for motion solve, cone math, wheel lookup, IES selection).
	FRebusFixtureProfile Profile;

	// Inline IES profiles pushed for this fixture's libraryId via RegisterFixtureIes. Preferred
	// over the URL fetch in SelectIesForZoom; empty when none were pushed.
	FRebusInlineIes InlineIes;

	// Inline base64 gobo images pushed for this fixture's libraryId via RegisterFixtureGobos.
	// Preferred over the URL fetch in AssignGobo; empty when none were pushed.
	FRebusInlineGobos InlineGobos;

	// Beam rest transform in fixture-local Unreal space (light placement before motion).
	FTransform BeamRestTransform = FTransform::Identity;
	int32 HeadAxisIndex = INDEX_NONE;

	// Resolved beam emission forward/up in fixture-local Unreal space (pre-motion), shared by the
	// SpotLight and the lens disc so they aim identically. The disc rest transform (plane normal
	// along forward, co-located at the beam origin, scaled to the lens diameter) is composed with
	// the head motion (LensDiscRest * Head) exactly like the SpotLight.
	FVector BeamForwardLocal = FVector(0.f, 0.f, -1.f);
	FVector BeamUpLocal = FVector::UpVector;
	FTransform LensDiscRest = FTransform::Identity;

	// SpotLight emitter radius (UE cm) resolved from the lens opening (lensDiameter/2 ->
	// source.radius -> source.diameter/2) so the beam STARTS at the lens diameter (§8.3). Sentinel
	// < 0 = none known -> the engine-default SourceRadius is left untouched. Reused as the base for
	// frost penumbra scaling so the beam origin stays consistent with the lens-flare disc.
	float BaseSourceRadiusUnreal = -1.f;

	// True when BuildSpotLight() placed the beam from a GDTF <Beam> node; false on the
	// down-pointing fallback. Surfaced in the per-fixture diagnostics summary.
	bool bHasBeamNode = false;

	// ---- Live control state ----
	FRebusScalarFade Dimmer;       // 0..1
	FRebusScalarFade Iris;         // 0..1 (1 = open)
	FRebusScalarFade Frost;        // 0..1
	FRebusScalarFade Focus;        // 0..1
	FRebusScalarFade ZoomDeg;      // half-angle degrees
	FRebusScalarFade PanDeg;       // degrees
	FRebusScalarFade TiltDeg;      // degrees
	FRebusScalarFade ColorR, ColorG, ColorB; // linear

	float BaseCandela = 8.f;       // derived from photometrics flux (fallback to a default)
	ERebusShutterMode ShutterMode = ERebusShutterMode::Open;
	float ShutterRateHz = 0.f;
	float ShutterPhase = 0.f;      // accumulates for strobe gating
	float GoboRotationSpeed = 0.f; // -1..1
	float GoboAngle = 0.f;
	int32 CurrentGoboIndex = INDEX_NONE;
	int32 CurrentGoboWheelIndex = INDEX_NONE; // live wheelIndex selector (re-apply on re-push)
	FString CurrentGoboWheel;      // live wheel-name hint for the gobo selection (re-apply)
	int32 CurrentIesZoomDmx = -1;  // which IES entry (inline or URL) is loaded, by zoomDmx
	bool bActiveIesInline = false; // true when the loaded IES came from an inline iesText push

	bool bAnimating = false;
};
