// Copyright REBUS Industries.
#include "RebusFixtureActor.h"
#include "RebusCoordinates.h"
#include "RebusMotionSolver.h"
#include "RebusIes.h"
#include "RebusRestClient.h"
#include "RebusVisualiserLog.h"

#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureLightProfile.h"
#include "Engine/Texture2D.h"
#include "Engine/Canvas.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ImageUtils.h"
#include "UObject/ConstructorHelpers.h"
#include "Misc/ConfigCacheIni.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

namespace
{
	// Per-channel sRGB -> linear (the wire sends sRGB 0..1; §5.2 SetFixtureColor).
	FORCEINLINE float SrgbToLinearChannel(float C)
	{
		C = FMath::Clamp(C, 0.f, 1.f);
		return (C <= 0.04045f) ? (C / 12.92f) : FMath::Pow((C + 0.055f) / 1.055f, 2.4f);
	}

	constexpr int32 RebusSelectionStencil = 252;        // primary
	constexpr int32 RebusSelectionStencilSecondary = 251;

	// Volumetric beam shadows are expensive, so only the first N spotlights of a spawn batch
	// cast them ("hero beams"); the rest still scatter (VolumetricScatteringIntensity) but skip
	// the per-light volumetric shadow pass (§8.4).
	constexpr int32 RebusMaxVolumetricShadowBeams = 8;

	// Emissive strength at FULL output (dimmer=1, shutter open) for the lens-flare disc; scaled
	// by dimmer x shutter-gate so it blooms bright at full and goes dark when fully dimmed (§8.3a).
	constexpr float RebusLensFlareMaxEmissive = 24.f;

	// Hybrid cone-mesh volumetric beam (§8.4a). Segment count of the procedural frustum (radial
	// resolution), and the additive beam intensity at FULL output (dimmer=1, gate open, user
	// multiplier 1) -- scaled live by dimmer x gate x SetFixtureBeamVolumetrics. BeamSharpness =
	// Fresnel exponent (edge softness), BeamFalloff = length-fade exponent; both seed the MID.
	constexpr int32 RebusBeamConeSegments = 24;
	// v1.0.40: brightened the shaft (was 3.0) so it reads as a clearly visible volumetric beam.
	constexpr float RebusMeshBeamMaxIntensity = 4.f;
	constexpr float RebusBeamSharpness = 2.5f;
	// v1.0.40: BeamFalloff is now the distance-from-SOURCE inverse-square STRENGTH in M_RebusBeam
	// (0 = flat along length, higher = dims faster downrange), NOT the old length-fade pow exponent.
	constexpr float RebusBeamFalloff = 1.6f;
	// v1.0.40: floor for the beam base (lens) radius so the shaft starts from a visible disc of the
	// lens diameter rather than a near-point when a fixture reports an unrealistically tiny lens.
	constexpr float RebusBeamLensRadiusFloorCm = 3.f;

	// Modest render-bounds margin for the beam cone (extent-only; origin unchanged so translucency
	// sort order is unaffected). The real "beam vanishes up close / inside" fix is the v1.0.39
	// raymarch entry/exit rework in M_RebusBeam -- bounds were NOT the cause -- so this is kept small
	// (just a little frustum-cull headroom for the elongated shaft), reduced from the v1.0.38 3x.
	constexpr float RebusBeamBoundsScale = 1.5f;

	// v1.0.44/45/46: Epic DMX beam (M_Beam_Master) conventions, verified by introspecting the
	// installed content (SM_Beam_RM + MI_Beam) plus runtime visual feedback:
	//  * SM_Beam_RM is a NORMALIZED unit tube whose geometry spans Z 0..-1, with bounds extended to
	//    +/-10000 so it's never culled. The material expands it into the actual cone via WORLD
	//    POSITION OFFSET from its params, so the canvas component MUST stay at scale (1,1,1) -- any
	//    component scale breaks the WPO cone (this was the v1.0.43 misalignment).
	//    ADMXFixtureActor::InitializeFixture forces WorldScale (1,1,1) for exactly this reason.
	//  * EMISSION AXIS is canvas-local +Z (v1.0.46 fix). v1.0.45 inferred -Z from the vertex extent
	//    (Z 0..-1) but had the sign inverted -- the beam emitted 180deg out the BACK of the fixture
	//    even though pan/tilt tracked the head correctly. M_Beam_Master raymarches along +Z (the
	//    pivot/apex is the Z=-1 end, the tube extends downstream toward +Z). Mapping +Z onto the
	//    spotlight's local +X via the constant relative rotation now points the beam through the lens.
	//  * Cone ANGLE comes from "DMX Zoom" (full beam angle in DEGREES; MI default 32.77), LENGTH from
	//    "DMX Max Light Distance" (cm, <= the ~10000 canvas length), start radius from "DMX Lens
	//    Radius", brightness from "DMX Max Light Intensity" (Epic scale ~1000) x "DMX Dimmer" (0..1).
	constexpr float RebusEpicBeamQuality = 1.0f;     // "DMX Quality Level" (1 == Epic High)
	const FVector RebusEpicBeamLocalEmission(0.f, 0.f, 1.f); // SM_Beam_RM emits along +Z (v1.0.46)

	// v1.0.52: gobo image rotation rate. The signed normalised gobo+animation speed in [-1, +1]
	// (each in [-1, 1] -> combined in [-2, +2]) is multiplied by this to drive the per-tick angle
	// step (deg). 360 deg/sec at speed=1.0 = one full revolution per second per wire, so a
	// combined speed of +1 (gobo only) or +2 (gobo+anim maxed) spins one or two revolutions per
	// second respectively. Tunable: lower if 1 rps feels too fast for typical fixture content.
	constexpr float RebusGoboMaxRotRateDegPerSec = 360.f;
	constexpr float RebusEpicBeamMaxDistanceCm = 10000.f;     // canvas length cap (mesh built length)
	// v1.0.45: "DMX Zoom" feed = this scale x the SpotLight's live OUTER cone HALF-angle (degrees).
	// The footprint is defined by the SpotLight's outer cone, so we drive the beam angle from the
	// exact same value (single source of truth) -- they can't diverge. Empirically M_Beam_Master's
	// "DMX Zoom" reads ~the half-angle (feeding the full 2x angle made the far end ~2x too wide), so
	// the default is 1.0 x the half-angle. Tunable: lower to hug the brighter IES core, raise to
	// widen toward the geometric field edge.
	constexpr float RebusEpicBeamZoomScale = 1.0f;
	// Beam brightness base for M_Beam_Master ("DMX Max Light Intensity"). Epic's scale is ~1000s of
	// candela (MI default 1000), NOT our M_RebusBeam 0..4 range -- a small value here is invisible.
	// Multiplied by the live SetFixtureBeamVolumetrics user scale; modulated by "DMX Dimmer".
	constexpr float RebusEpicBeamMaxIntensity = 2000.f;
	// Verified-on-disk object paths for Epic's UE 5.7 DMX Fixtures content (mount /DMXFixtures). A
	// config override ([RebusVisualiser] EpicDmxBeamMaterial/EpicDmxBeamMesh in DefaultGame.ini) lets
	// a differing install relocate them without a recompile.
	const TCHAR* RebusEpicBeamMaterialPath = TEXT("/DMXFixtures/LightFixtures/DMX_Materials/MI_Beam.MI_Beam");
	const TCHAR* RebusEpicBeamMeshPath = TEXT("/DMXFixtures/LightFixtures/Meshes/SM_Beam_RM.SM_Beam_RM");

	// Phase 2 (v1.0.33) raymarch tuning: view-ray march steps + per-step density seeded on the MID
	// (StepCount/BeamDensity in M_RebusBeam's Custom HLSL). 32 steps is a good live/final balance.
	constexpr float RebusBeamStepCount = 32.f;
	// v1.0.40: raised (was 0.0025) to pair with the width-normalized density model so the shaft is a
	// nice, clearly visible volumetric beam. Tunable live via the BeamDensity MID param.
	constexpr float RebusBeamDensity = 0.015f;

	// Phase 2 light-blocking volumetric shadows (the must-have) use the NATIVE VSM fog hybrid:
	// runtime-imported glTF trusses have NO mesh distance fields (glTFRuntime's import config has no
	// DF option, and DF are an editor/DDC build step), so a material raymarch can't trace them and
	// the Global Distance Field doesn't contain them. Virtual Shadow Maps DO shadow runtime meshes,
	// so hero shadow-casting beams re-enable a modest SpotLight VolumetricScatteringIntensity + Cast
	// Volumetric Shadow to carve real truss gaps in the fog, while the mesh cone provides the crisp
	// shaft. Gated by SetFixtureBeamVolumetrics(castVolumetricShadow) + this per-batch hero budget.
	constexpr int32 RebusMaxShadowFogBeams = 6;

	// Rotation that lays a plane (engine /BasicShapes/Plane, local +Z normal) perpendicular to the
	// beam: plane +Z -> Forward, plane +X -> Up. Guards a near-parallel up like MakeFromXZ does.
	FQuat LensDiscRotationFromForward(const FVector& Forward, const FVector& Up)
	{
		FVector F = Forward.GetSafeNormal();
		if (F.IsNearlyZero()) F = FVector(0.f, 0.f, -1.f);
		FVector U = Up;
		if (U.IsNearlyZero() || FMath::Abs(FVector::DotProduct(F, U)) > 0.999f)
		{
			U = (FMath::Abs(F.Z) < 0.9f) ? FVector::UpVector : FVector::ForwardVector;
		}
		return FRotationMatrix::MakeFromZX(F, U).ToQuat();
	}
}

int32 ARebusFixtureActor::VolumetricShadowBeamCount = 0;
int32 ARebusFixtureActor::ShadowFogBeamCount = 0;

// v1.0.47: hero SpotLight VolumetricScatteringIntensity used for the VSM-shadowed fog beam that
// produces visible truss-gap shafts INSIDE the Epic M_Beam_Master cone. The Epic cone is a very
// bright (~2000-candela) unshadowed additive raymarch, so the fog scattering has to be lifted from
// the v1.0.37 default of 0.8 to clearly read through it. Live-tunable via `Rebus.HeroShadowScatter
// <float>`; default 4.0 is paired with RebusEpicBeamMaxIntensity=2000 and our current fog tuning
// (r.VolumetricFog.GridPixelSize=4, r.VolumetricFog.HistoryWeight=0.95).
float GRebusHeroShadowScatter = 4.0f;
FAutoConsoleVariableRef CVarRebusHeroShadowScatter(
	TEXT("Rebus.HeroShadowScatter"),
	GRebusHeroShadowScatter,
	TEXT("Hero-beam SpotLight VolumetricScatteringIntensity for VSM-shadowed fog (truss gaps inside the Epic cone). Live."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
		// Refresh every live fixture so the new scatter is picked up immediately.
		if (!GEngine) return;
		int32 Refreshed = 0;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* W = Ctx.World();
			if (!W) continue;
			for (TActorIterator<ARebusFixtureActor> It(W); It; ++It)
			{
				if (ARebusFixtureActor* F = *It)
				{
					F->RefreshBeamShadowMode();
					++Refreshed;
				}
			}
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.HeroShadowScatter -> %.2f, refreshed %d fixture(s)."),
			CVar->GetFloat(), Refreshed);
	}),
	ECVF_Default);

void ARebusFixtureActor::ResetVolumetricShadowBudget()
{
	VolumetricShadowBeamCount = 0;
	ShadowFogBeamCount = 0;
}

void ARebusFixtureActor::LogVolumetricShadowBudget(int32 SpawnedTotal)
{
	// v1.0.47 diagnostic: per-spawn-batch summary so the user can tell at a glance whether the
	// portal is sending castVolumetricShadow=true and whether the hero budget is filtering anyone
	// out. Emitted from URebusVisualiserSubsystem after each (re)spawn.
	int32 WantShadow = 0;
	int32 Hero = 0;
	if (GEngine)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* W = Ctx.World();
			if (!W) continue;
			for (TActorIterator<ARebusFixtureActor> It(W); It; ++It)
			{
				if (const ARebusFixtureActor* F = *It)
				{
					if (F->bWantsVolumetricShadow) ++WantShadow;
					if (F->bGrantedShadowHero) ++Hero;
				}
			}
		}
	}
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Spawn batch shadow budget: spawned=%d wantsShadow=%d grantedHero=%d/%d (Rebus.HeroShadowScatter=%.2f). If wantsShadow=0 the portal isn't sending castVolumetricShadow=true; if grantedHero<wantsShadow the budget is filtering."),
		SpawnedTotal, WantShadow, Hero, RebusMaxShadowFogBeams, GRebusHeroShadowScatter);
}

void ARebusFixtureActor::DumpLightStateForDebug() const
{
	// v1.0.51 per-fixture light dump for the Rebus.DumpFixtureLights console command. Walks the
	// fixture's primary SpotLight + every other ULightComponent attached to this actor (looking
	// for duplicate / competing lights that would wash out the projected gobo cookie) + the bound
	// Orbit-imported components (to confirm whether the import path silently brought aux lights
	// via glTF KHR_lights_punctual). Every line is at Log level so the user can paste the output.
	const FVector Loc = GetActorLocation();
	if (!SpotLight)
	{
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("DumpFixtureLights '%s' (loc=(%.0f,%.0f,%.0f)): NO SpotLight component -- fixture not fully constructed."),
			*FixtureId, Loc.X, Loc.Y, Loc.Z);
		return;
	}

	const UMaterialInterface* LightFnMat = SpotLight->LightFunctionMaterial;
	const UTextureLightProfile* Ies = SpotLight->IESTexture;
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("DumpFixtureLights '%s' (loc=(%.0f,%.0f,%.0f)) SpotLight: visible=%d intensity=%.1f units=%d attenRadius=%.0f innerCone=%.1f outerCone=%.1f castShadows=%d castVolumetricShadow=%d bAllowMegaLights=%d LightFn=%s IES=%s mobility=%d"),
		*FixtureId, Loc.X, Loc.Y, Loc.Z,
		SpotLight->IsVisible() ? 1 : 0,
		SpotLight->Intensity, (int32)SpotLight->IntensityUnits,
		SpotLight->AttenuationRadius, SpotLight->InnerConeAngle, SpotLight->OuterConeAngle,
		SpotLight->CastShadows ? 1 : 0, SpotLight->bCastVolumetricShadow ? 1 : 0,
		SpotLight->bAllowMegaLights ? 1 : 0,
		LightFnMat ? *LightFnMat->GetPathName() : TEXT("nullptr"),
		Ies ? *Ies->GetName() : TEXT("nullptr"),
		(int32)SpotLight->Mobility);

	// Sibling light enumeration: anything else on this actor that's a ULightComponent is a
	// potential duplicate / wash-out source. Our pipeline only spawns ONE SpotLight per fixture
	// (BuildSpotLight, RebusFixtureActor.cpp:509), and OrbitImportSubsystem::SpawnNodeRecursive
	// only creates UStaticMeshComponents (no KHR_lights_punctual lights -- verified v1.0.51), so
	// any sibling light here would be a regression worth chasing.
	TArray<ULightComponent*> SiblingLights;
	GetComponents<ULightComponent>(SiblingLights);
	int32 SiblingCount = 0;
	for (ULightComponent* L : SiblingLights)
	{
		if (!L || L == SpotLight) continue;
		++SiblingCount;
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("DumpFixtureLights '%s' SIBLING LIGHT #%d: class=%s name=%s visible=%d intensity=%.1f castShadows=%d -- potential cookie wash-out source."),
			*FixtureId, SiblingCount, *L->GetClass()->GetName(), *L->GetName(),
			L->IsVisible() ? 1 : 0, L->Intensity, L->CastShadows ? 1 : 0);
	}
	if (SiblingCount == 0)
	{
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("DumpFixtureLights '%s' sibling lights: NONE (only the primary SpotLight is attached -- no duplication on the fixture actor)."),
			*FixtureId);
	}

	// Bound Orbit components: enumerate by class so the user can see whether any are lights.
	int32 OrbitLightCount = 0;
	int32 OrbitTotal = 0;
	for (const TWeakObjectPtr<USceneComponent>& W : OrbitComponents)
	{
		const USceneComponent* C = W.Get();
		if (!C) continue;
		++OrbitTotal;
		if (C->IsA(ULightComponent::StaticClass()))
		{
			++OrbitLightCount;
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("DumpFixtureLights '%s' BOUND ORBIT LIGHT #%d: class=%s name=%s owner=%s -- aux light from glTF KHR_lights_punctual; gobo cookie will be washed out by this."),
				*FixtureId, OrbitLightCount, *C->GetClass()->GetName(), *C->GetName(),
				C->GetOwner() ? *C->GetOwner()->GetName() : TEXT("(none)"));
		}
	}
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("DumpFixtureLights '%s' Orbit binding: bound=%d components (lights=%d) objectId='%s'."),
		*FixtureId, OrbitTotal, OrbitLightCount, *GetBoundOrbitObjectId());

	// Total component count under the actor (sanity check for unexpected attachments).
	TArray<USceneComponent*> AllScene;
	GetComponents<USceneComponent>(AllScene);
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("DumpFixtureLights '%s' totalSceneComponents=%d (this includes SpotLight + FixtureRoot + BeamCone + EpicBeamCanvas + LensDisc + any mesh proxies)."),
		*FixtureId, AllScene.Num());
}

ARebusFixtureActor::ARebusFixtureActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	FixtureRoot = CreateDefaultSubobject<USceneComponent>(TEXT("FixtureRoot"));
	RootComponent = FixtureRoot;
	FixtureRoot->SetMobility(EComponentMobility::Movable);

	// Hard-reference the lens-flare disc assets from the CDO so the cooker ALWAYS packages them
	// for -game/packaged builds. A runtime-only LoadObject-by-path is not a cook dependency, so
	// the emissive material (referenced by nothing in the level) was being stripped from cooked
	// builds -> the disc silently failed to load. Belt-and-suspenders with the
	// DirectoriesToAlwaysCook entries in DefaultGame.ini. FObjectFinder only resolves in-editor /
	// during cook; in a cooked runtime these UPROPERTYs are simply serialized in.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneFinder(TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (PlaneFinder.Succeeded()) LensPlaneMesh = PlaneFinder.Object;
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> LensMatFinder(TEXT("/Game/REBUS/Materials/M_RebusLensFlare.M_RebusLensFlare"));
	if (LensMatFinder.Succeeded()) LensMaterial = LensMatFinder.Object;

	// Same cook-safety hard-ref for the hybrid beam master (§8.4a): the cooker packages it because
	// the CDO references it; the per-fixture BuildBeamCone then makes a MID from this (or a runtime
	// LoadObject fallback). /Game/REBUS is also in DirectoriesToAlwaysCook (v1.0.30).
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BeamMatFinder(TEXT("/Game/REBUS/Materials/M_RebusBeam.M_RebusBeam"));
	if (BeamMatFinder.Succeeded()) BeamMaterial = BeamMatFinder.Object;

	// v1.0.43 cook-safe hard refs to Epic's REAL DMX beam assets (installed under /DMXFixtures). When
	// present, BuildBeamCone -> TryBuildEpicBeam uses Epic's SM_Beam_RM canvas + MI_Beam material as
	// the visible beam; absent, these stay null and we fall back to the procedural cone + M_RebusBeam.
	// FObjectFinder resolves in-editor/at-cook only; a missing path just leaves the ref null (a benign
	// "failed to find" note) and the runtime path also tries a config-overridable LoadObject.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> EpicBeamMatFinder(TEXT("/DMXFixtures/LightFixtures/DMX_Materials/MI_Beam.MI_Beam"));
	if (EpicBeamMatFinder.Succeeded()) EpicBeamMaterial = EpicBeamMatFinder.Object;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> EpicBeamMeshFinder(TEXT("/DMXFixtures/LightFixtures/Meshes/SM_Beam_RM.SM_Beam_RM"));
	if (EpicBeamMeshFinder.Succeeded()) EpicBeamMesh = EpicBeamMeshFinder.Object;
}

void ARebusFixtureActor::Setup(const FRebusSceneFixture& InSceneFixture,
	const FRebusFixtureProfile& InProfile, const FRebusMeshBundle& InMeshes,
	const FRebusInlineIes& InInlineIes, const FRebusInlineGobos& InInlineGobos)
{
	FixtureId = InSceneFixture.Id;
	LibraryFixtureId = InSceneFixture.LibraryFixtureId;
	DisplayName = InSceneFixture.Name;
	Profile = InProfile;
	InlineIes = InInlineIes;
	InlineGobos = InInlineGobos;

	bHasPanTilt = Profile.MotionRig.bValid && Profile.MotionRig.Axes.Num() > 0;
	// A fixture has gobos if its profile carries a gobo wheel OR the portal pushed inline gobo
	// images for it over the data channel (RegisterFixtureGobos).
	bHasGobo = FindFirstGoboWheel(Profile) != INDEX_NONE || InlineGobos.Gobos.Num() > 0;

	// Place the fixture root from the instance matrix (genuinely Z-up, §7.4 step 7).
	if (InSceneFixture.bHasMatrix)
	{
		const bool bRowMajor = (InSceneFixture.MatrixSource == ERebusMatrixSource::TransformRow);
		const FMatrix M = RebusCoords::MatrixToUnreal(InSceneFixture.Matrix, bRowMajor, /*bYUp*/false);
		SetActorTransform(FTransform(M));
	}

	// Derive a base candela from flux + field angle (portal's estimate, §8.1).
	if (Profile.Photometrics.LuminousFlux.IsSet() && Profile.Photometrics.FieldAngle.IsSet())
	{
		const double Flux = Profile.Photometrics.LuminousFlux.GetValue();
		const double HalfRad = FMath::DegreesToRadians(Profile.Photometrics.FieldAngle.GetValue() * 0.5);
		const double Denom = 2.0 * PI * (1.0 - FMath::Cos(HalfRad));
		if (Denom > KINDA_SMALL_NUMBER)
		{
			BaseCandela = (float)(Flux / Denom);
		}
	}

	// Default zoom half-angle from the field angle (FULL -> half), or zoom range midpoint.
	float DefaultZoomHalf = 20.f;
	if (Profile.Photometrics.FieldAngle.IsSet())
	{
		DefaultZoomHalf = (float)(Profile.Photometrics.FieldAngle.GetValue() * 0.5);
	}
	else if (Profile.Zoom.bValid)
	{
		DefaultZoomHalf = (float)((Profile.Zoom.MinDeg + Profile.Zoom.MaxDeg) * 0.25); // half of midpoint full
	}

	// Initial control state.
	Dimmer.SetTarget(0.f, 0.f);
	Iris.SetTarget(1.f, 0.f);
	Frost.SetTarget(0.f, 0.f);
	Focus.SetTarget(0.5f, 0.f);
	ZoomDeg.SetTarget(DefaultZoomHalf, 0.f);
	PanDeg.SetTarget(0.f, 0.f);
	TiltDeg.SetTarget(0.f, 0.f);
	ColorR.SetTarget(1.f, 0.f);
	ColorG.SetTarget(1.f, 0.f);
	ColorB.SetTarget(1.f, 0.f);

	BuildComponentHierarchy();
	BuildMeshes(InMeshes);
	// Tie the beam's tracked axis to the deepest axis that actually drives a head mesh proxy,
	// so the spotlight rides the exact same rig output as the moving head geometry (no separate
	// pan/tilt recompute that could drift). Falls back to the topological deepest axis for
	// light-only fixtures whose meshes matched nothing.
	ResolveHeadAxisFromMeshes();
	BuildSpotLight();
	BuildLensDisc();   // emissive lens-flare disc at the beam origin (reuses the beam transform)
	BuildBeamCone();   // hybrid cone-mesh volumetric beam (sized to IES + lens, rides the head)
	RefreshMotion();
	RecomputeConeAngles();
	RefreshIntensity();
	SelectIesForZoom();

	// Per-fixture diagnostics: lets the portal team verify the pushed motionRig + meshes landed
	// and wired up (rig axis count w/ pan/tilt breakdown, mesh-proxy count, beam source).
	int32 PanAxes = 0;
	int32 TiltAxes = 0;
	for (const FRebusMotionAxis& Axis : Profile.MotionRig.Axes)
	{
		if (Axis.Kind == ERebusAxisKind::Pan) ++PanAxes;
		else if (Axis.Kind == ERebusAxisKind::Tilt) ++TiltAxes;
	}
	// Beam tracking mode: rig-attached (the spotlight rides Cumulative[HeadAxisIndex], the same
	// solve output that drives the head meshes -> head-aligned, no drift) vs the synthetic
	// pan/tilt fallback used when there is no valid GDTF MotionRig.
	FString BeamAttach;
	if (bHasPanTilt && Profile.MotionRig.Axes.IsValidIndex(HeadAxisIndex))
	{
		const TCHAR* HeadKind = TEXT("Other");
		switch (Profile.MotionRig.Axes[HeadAxisIndex].Kind)
		{
		case ERebusAxisKind::Pan:  HeadKind = TEXT("Pan"); break;
		case ERebusAxisKind::Tilt: HeadKind = TEXT("Tilt"); break;
		default: break;
		}
		BeamAttach = FString::Printf(TEXT("rig-head axis %d (%s)"), HeadAxisIndex, HeadKind);
	}
	else
	{
		BeamAttach = TEXT("synthetic-pan-tilt fallback");
	}
	// Confirm the per-fixture spotlight is flagged as a MegaLight (folded into the one-per-fixture
	// summary so it stays a single log line).
	const bool bMegaLight = SpotLight && SpotLight->bAllowMegaLights;
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s (lib %s): hasPanTilt=%s axes=%d (pan=%d tilt=%d) meshProxies=%d beamSource=%s beamAttach=%s allowMegaLights=%d"),
		*FixtureId, *LibraryFixtureId,
		bHasPanTilt ? TEXT("true") : TEXT("false"),
		Profile.MotionRig.Axes.Num(), PanAxes, TiltAxes,
		MeshComponents.Num(),
		bHasBeamNode ? TEXT("gdtf-beam") : TEXT("default-down"),
		*BeamAttach,
		bMegaLight ? 1 : 0);
}

void ARebusFixtureActor::BuildComponentHierarchy()
{
	// Topological deepest axis: the head/beam default, and the fallback when no mesh proxy
	// bucketed onto a motion axis (e.g. a light-only fixture that still carries a rig).
	HeadAxisIndex = RebusMotion::DeepestAxisIndex(Profile.MotionRig);
}

void ARebusFixtureActor::ResolveHeadAxisFromMeshes()
{
	// Prefer the deepest axis that an actual mesh proxy is bucketed onto, so the beam tracks
	// the same rig output that visibly moves the head geometry. Keep the topological default
	// (set in BuildComponentHierarchy) when nothing matched, so light-only fixtures still aim.
	int32 BestAxis = INDEX_NONE;
	int32 BestDepth = -1;
	for (int32 Axis : MeshAxisBucket)
	{
		if (Axis == INDEX_NONE || !Profile.MotionRig.Axes.IsValidIndex(Axis))
		{
			continue;
		}
		int32 Depth = 0;
		int32 P = Profile.MotionRig.Axes[Axis].ParentAxisIndex;
		while (P != INDEX_NONE && Profile.MotionRig.Axes.IsValidIndex(P))
		{
			++Depth;
			P = Profile.MotionRig.Axes[P].ParentAxisIndex;
		}
		if (Depth > BestDepth)
		{
			BestDepth = Depth;
			BestAxis = Axis;
		}
	}
	if (BestAxis != INDEX_NONE)
	{
		HeadAxisIndex = BestAxis;
	}
}

void ARebusFixtureActor::BuildMeshes(const FRebusMeshBundle& Meshes)
{
	for (const FRebusMesh& Mesh : Meshes.Meshes)
	{
		if (Mesh.Vertices.Num() < 9 || Mesh.Faces.Num() == 0)
		{
			continue;
		}

		const int32 NumVerts = Mesh.Vertices.Num() / 3;
		TArray<FVector> Positions;
		Positions.Reserve(NumVerts);
		for (int32 v = 0; v < NumVerts; ++v)
		{
			const FVector P(Mesh.Vertices[v * 3 + 0], Mesh.Vertices[v * 3 + 1], Mesh.Vertices[v * 3 + 2]);
			Positions.Add(RebusCoords::PointYUpMetersToUnreal(P));
		}

		// Decode Speckle faces into a triangle index list (fan-triangulate polygons).
		TArray<int32> Triangles;
		for (int32 i = 0; i < Mesh.Faces.Num();)
		{
			const int32 Head = Mesh.Faces[i];
			int32 Count = (Head == 0) ? 3 : (Head == 1) ? 4 : Head; // 0=tri, 1=quad, else explicit
			++i;
			if (Count < 3 || i + Count > Mesh.Faces.Num()) break;

			const int32 I0 = Mesh.Faces[i];
			for (int32 k = 1; k + 1 < Count; ++k)
			{
				const int32 Ia = Mesh.Faces[i + k];
				const int32 Ib = Mesh.Faces[i + k + 1];
				if (Positions.IsValidIndex(I0) && Positions.IsValidIndex(Ia) && Positions.IsValidIndex(Ib))
				{
					Triangles.Add(I0);
					Triangles.Add(Ia);
					Triangles.Add(Ib);
				}
			}
			i += Count;
		}

		if (Triangles.Num() == 0)
		{
			continue;
		}

		// Smooth per-vertex normals from accumulated face normals.
		TArray<FVector> Normals; Normals.Init(FVector::ZeroVector, Positions.Num());
		for (int32 t = 0; t + 2 < Triangles.Num(); t += 3)
		{
			const int32 A = Triangles[t], B = Triangles[t + 1], C = Triangles[t + 2];
			const FVector FaceN = FVector::CrossProduct(Positions[B] - Positions[A], Positions[C] - Positions[A]);
			Normals[A] += FaceN; Normals[B] += FaceN; Normals[C] += FaceN;
		}
		for (FVector& N : Normals) { N = N.GetSafeNormal(); if (N.IsNearlyZero()) N = FVector::UpVector; }

		UProceduralMeshComponent* PMC = NewObject<UProceduralMeshComponent>(this);
		PMC->SetupAttachment(FixtureRoot);
		PMC->RegisterComponent();
		PMC->SetMobility(EComponentMobility::Movable);
		PMC->bUseComplexAsSimpleCollision = true;

		const TArray<FVector2D> EmptyUV;
		const TArray<FColor> EmptyColor;
		const TArray<FProcMeshTangent> EmptyTangents;
		PMC->CreateMeshSection(0, Positions, Triangles, Normals, EmptyUV, EmptyColor, EmptyTangents, /*bCreateCollision*/false);

		// Custom depth on for the selection outline; off until selected.
		PMC->SetRenderCustomDepth(false);

		// A fixture's own body must not cast a volumetric-fog shadow into its own beam (it sits at
		// the light source and otherwise mottles the beam base). Keeps contact/RT grounding.
		DisableSelfBeamVolumetricShadow(PMC);

		const int32 ComponentIndex = MeshComponents.Add(PMC);
		const int32 Axis = RebusMotion::ResolveAxisForMesh(Profile.MotionRig, Mesh.GeometryName, Mesh.ModelName);
		MeshAxisBucket.SetNum(MeshComponents.Num());
		MeshAxisBucket[ComponentIndex] = Axis;

		// Per-mesh diagnostics: which motion axis (if any) this proxy bucketed onto, so the
		// portal team can verify a pushed mesh actually attached to its pan/tilt group.
		FString AxisDesc;
		if (Axis == INDEX_NONE)
		{
			AxisDesc = TEXT("base");
		}
		else
		{
			const TCHAR* KindStr = TEXT("Other");
			if (Profile.MotionRig.Axes.IsValidIndex(Axis))
			{
				switch (Profile.MotionRig.Axes[Axis].Kind)
				{
				case ERebusAxisKind::Pan:  KindStr = TEXT("Pan"); break;
				case ERebusAxisKind::Tilt: KindStr = TEXT("Tilt"); break;
				default: break;
				}
			}
			AxisDesc = FString::Printf(TEXT("axis %d (%s)"), Axis, KindStr);
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Fixture %s: mesh[%d] geom='%s' name='%s' -> %s"),
			*FixtureId, ComponentIndex,
			*Mesh.GeometryName, *Mesh.Name, *AxisDesc);
	}

	UE_LOG(LogRebusVisualiser, Log, TEXT("Fixture %s: built %d mesh proxies."), *FixtureId, MeshComponents.Num());
}

void ARebusFixtureActor::BuildSpotLight()
{
	SpotLight = NewObject<USpotLightComponent>(this, TEXT("SpotLight"));
	SpotLight->SetupAttachment(FixtureRoot);
	SpotLight->RegisterComponent();
	SpotLight->SetMobility(EComponentMobility::Movable);
	SpotLight->SetIntensityUnits(ELightUnits::Candelas);
	SpotLight->SetAttenuationRadius(6000.f);

	// Hybrid mesh-beam (§8.4a): the visible shaft is the cone-mesh beam (BuildBeamCone), so suppress
	// THIS light's fog scattering by default to avoid a competing noisy froxel beam while keeping it
	// for surface lighting + IES + soft shadows. FogScatteringIntensity (2.5) is restored if the
	// portal toggles bMeshBeams=false (back to the fog beam). Phase 2: hero shadow beams later
	// re-enable a modest scattering + Cast Volumetric Shadow here via RefreshBeamShadowMode (the
	// native VSM path that produces light-blocking truss gaps on runtime meshes). Default => 0.
	SpotLight->SetVolumetricScatteringIntensity(bMeshBeamEnabled ? 0.f : FogScatteringIntensity);

	// Opt this fixture light into MegaLights. bAllowMegaLights is the public uint32:1 per-light
	// flag on ULightComponent (UE 5.7.4 has no Set* accessor for it) and defaults to true, but
	// we assert it explicitly so EVERY imported fixture light is a MegaLight regardless of any
	// future engine default change. The project-level r.MegaLights.Allow=1 ([SystemSettings])
	// then governs the whole rig. The spotlight is the only emissive light the plugin spawns.
	SpotLight->bAllowMegaLights = 1;

	// Hero-beam cap: volumetric shadows are costly, so only the first N spotlights of the spawn
	// batch cast them; the rest still scatter but skip the volumetric shadow pass. The session
	// subsystem resets the budget (ResetVolumetricShadowBudget) before each (re)spawn.
	const bool bHeroBeam = (VolumetricShadowBeamCount < RebusMaxVolumetricShadowBeams);
	SpotLight->SetCastVolumetricShadow(bHeroBeam);
	if (bHeroBeam)
	{
		++VolumetricShadowBeamCount;
	}
	SpotLight->MarkRenderStateDirty();

	// Source size: emit the beam from a finite disc the size of the lens opening so the beam (and
	// its volumetric scattering) STARTS at the lens diameter and gets soft-shadow penumbrae (§8.3).
	// The radius is HALF the SAME resolved lens diameter the lens-flare disc uses (so the glowing
	// disc and the beam origin always line up): lensDiameter/2 -> source.radius -> source.diameter/2
	// -> dimensions fallback. None resolvable -> leave the engine default untouched. Cached as
	// BaseSourceRadiusUnreal so the frost penumbra scaling (RecomputeConeAngles) stays consistent.
	// NOTE: SourceRadius gives soft penumbrae but does NOT visibly widen the volumetric beam base
	// in UE -- the emissive lens-flare disc (§8.3a) is the actual visual cue for the lens diameter.
	const TCHAR* SourceRadiusSrc = nullptr;
	const double LensDiamMeters = ResolveLensDiameterMeters(SourceRadiusSrc);
	if (LensDiamMeters > KINDA_SMALL_NUMBER)
	{
		BaseSourceRadiusUnreal = (float)(LensDiamMeters * 0.5 * RebusCoords::METERS_TO_UNREAL);
		SpotLight->SetSourceRadius(BaseSourceRadiusUnreal);
		SpotLight->SetSourceLength(0.f); // circular GDTF beam, no second axis
	}
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s source: SourceRadius=%s cm (src=%s)"),
		*FixtureId,
		BaseSourceRadiusUnreal >= 0.f ? *FString::Printf(TEXT("%.3f"), BaseSourceRadiusUnreal) : TEXT("engine-default"),
		SourceRadiusSrc ? SourceRadiusSrc : TEXT("engine-default"));

	if (Profile.Photometrics.ColorTemperature.IsSet())
	{
		SpotLight->SetUseTemperature(true);
		SpotLight->SetTemperature((float)Profile.Photometrics.ColorTemperature.GetValue());
	}

	// Beam rest transform from the GDTF <Beam> node (§7.7). A USpotLightComponent emits along
	// its local +X, so we map the beam's FORWARD (emission) direction onto +X and beamUp onto +Z
	// via MakeFromXZ. The emission axis is NEVER the matrix +X column -- that is the geometry's
	// SIDE axis, and using it fired the cone ~90deg off the lens ("out to the left"). When the
	// portal does not send an explicit beamDirectionWorld we take the beam node's +Y axis: that
	// is the lens-FRONT normal in this content (the -Y guess fired the cone out the REAR of the
	// head), so the cone exits the lens, not the side and not the back.
	bool bHaveBeam = false;
	TFunction<void(const FRebusFixturePart&)> Visit = [&](const FRebusFixturePart& Part)
	{
		if (!bHaveBeam && Part.bIsBeam && Part.bHasWorldMatrixMeters)
		{
			const FMatrix M = RebusCoords::MatrixToUnreal(Part.WorldMatrixMeters, /*bRowMajor*/false, /*bYUp*/true);
			const FVector Origin = M.GetOrigin();

			FVector Forward = Part.bHasBeamDirection
				? RebusCoords::DirectionYUpToUnreal(Part.BeamDirectionWorld)
				: M.GetUnitAxis(EAxis::Y).GetSafeNormal(); // GDTF emission = node's +Y (lens front)
			if (Forward.IsNearlyZero())
			{
				Forward = FVector(0.f, 0.f, -1.f);
			}

			FVector Up = Part.bHasBeamUp
				? RebusCoords::DirectionYUpToUnreal(Part.BeamUpWorld)
				: M.GetUnitAxis(EAxis::Z).GetSafeNormal();
			// Guard a beamUp that is (near) parallel to forward -- MakeFromXZ would degenerate.
			if (Up.IsNearlyZero() || FMath::Abs(FVector::DotProduct(Forward, Up)) > 0.999f)
			{
				Up = (FMath::Abs(Forward.Z) < 0.9f) ? FVector::UpVector : FVector::ForwardVector;
			}

			const FRotator Rot = FRotationMatrix::MakeFromXZ(Forward, Up).Rotator();
			BeamRestTransform = FTransform(Rot, Origin);
			BeamForwardLocal = Forward;   // shared with the lens disc so it aims identically
			BeamUpLocal = Up;
			bHaveBeam = true;
			bHasBeamNode = true;

			// Verifiable mapping: dump the resolved beam forward/up (UE world-ish) and the
			// spotlight component's resulting +X so a residual yaw is immediately obvious.
			const FVector CompFwd = Rot.RotateVector(FVector::ForwardVector);
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Fixture %s beam: src=%s fwdUE=(%.3f,%.3f,%.3f) upUE=(%.3f,%.3f,%.3f) compFwd(+X)=(%.3f,%.3f,%.3f)"),
				*FixtureId,
				Part.bHasBeamDirection ? TEXT("beamDirectionWorld") : TEXT("node +Y (lens front)"),
				Forward.X, Forward.Y, Forward.Z, Up.X, Up.Y, Up.Z, CompFwd.X, CompFwd.Y, CompFwd.Z);
		}
		for (const FRebusFixturePart& Child : Part.Children) Visit(Child);
	};
	for (const FRebusFixturePart& Part : Profile.Parts) Visit(Part);

	if (!bHaveBeam)
	{
		// Fallback: head pivot (if any), beam pointing straight DOWN (-Z). A spotlight emits
		// along its local +X, so a zero rotation would fire horizontally; moving-head fixtures
		// rest pointing down, which is also what a profile-less push expects. Pitch -90 aims +X
		// to -Z. (When a GDTF <Beam> node is present, the branch above wins and uses it.)
		FVector Origin = FVector::ZeroVector;
		if (Profile.MotionRig.Axes.IsValidIndex(HeadAxisIndex))
		{
			Origin = RebusCoords::PointYUpMetersToUnreal(Profile.MotionRig.Axes[HeadAxisIndex].Pivot);
		}
		BeamRestTransform = FTransform(FRotator(-90.f, 0.f, 0.f), Origin);
		const FVector CompFwd = BeamRestTransform.GetRotation().RotateVector(FVector::ForwardVector);
		BeamForwardLocal = CompFwd; // straight down at rest; shared with the lens disc
		BeamUpLocal = BeamRestTransform.GetRotation().RotateVector(FVector::UpVector);
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Fixture %s beam: no <Beam> node, resting straight down compFwd(+X)=(%.3f,%.3f,%.3f)."),
			*FixtureId, CompFwd.X, CompFwd.Y, CompFwd.Z);
	}
}

double ARebusFixtureActor::ResolveLensDiameterMeters(const TCHAR*& OutSrc) const
{
	if (Profile.Photometrics.LensDiameter >= 0.0)
	{
		OutSrc = TEXT("lensDiameter");
		return Profile.Photometrics.LensDiameter;
	}
	if (Profile.Source.RadiusMeters.IsSet())
	{
		OutSrc = TEXT("source.radius*2");
		return Profile.Source.RadiusMeters.GetValue() * 2.0;
	}
	if (Profile.Source.DiameterMeters.IsSet())
	{
		OutSrc = TEXT("source.diameter");
		return Profile.Source.DiameterMeters.GetValue();
	}
	// Synthetic fallback so a lens disc + finite beam origin still show when the portal sends no
	// lens/source size: a modest fraction of the fixture's smaller cross-section (width=Y /
	// height=Z), clamped to a plausible lens range. Logged as a fallback so it's clearly derived.
	if (Profile.bHasDimensions)
	{
		const double Cross = FMath::Min(FMath::Abs(Profile.DimensionsMeters.Y), FMath::Abs(Profile.DimensionsMeters.Z));
		if (Cross > KINDA_SMALL_NUMBER)
		{
			OutSrc = TEXT("dimensions-fallback");
			return FMath::Clamp(Cross * 0.4, 0.03, 0.5);
		}
	}
	OutSrc = TEXT("none");
	return -1.0;
}

void ARebusFixtureActor::BuildLensDisc()
{
	const TCHAR* DiamSrc = TEXT("none");
	const double DiamMeters = ResolveLensDiameterMeters(DiamSrc);

	// Always surface the parsed lensDiameter so the portal team can see exactly what arrived.
	const FString LensDiamStr = (Profile.Photometrics.LensDiameter >= 0.0)
		? FString::Printf(TEXT("%.4f"), Profile.Photometrics.LensDiameter) : FString(TEXT("null"));

	if (DiamMeters <= KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("Fixture %s lens disc: SKIP (no size) lensDiameter=%s source.radius=%s source.diameter=%s dims=%d"),
			*FixtureId, *LensDiamStr,
			Profile.Source.RadiusMeters.IsSet() ? TEXT("set") : TEXT("null"),
			Profile.Source.DiameterMeters.IsSet() ? TEXT("set") : TEXT("null"),
			Profile.bHasDimensions ? 1 : 0);
		return;
	}
	const float DiamUnreal = (float)(DiamMeters * RebusCoords::METERS_TO_UNREAL);

	// Cook-safe asset access: prefer the CDO hard refs (packaged by the cooker), fall back to a
	// runtime load by path. Log exactly which asset failed so a cooked-build miss is provable.
	UStaticMesh* Plane = LensPlaneMesh ? LensPlaneMesh.Get()
		: LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UMaterialInterface* LensMat = LensMaterial ? LensMaterial.Get()
		: LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/REBUS/Materials/M_RebusLensFlare.M_RebusLensFlare"));
	const bool bMeshOk = (Plane != nullptr);
	const bool bMatOk = (LensMat != nullptr);
	if (!bMeshOk || !bMatOk)
	{
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("Fixture %s lens disc: SKIP (asset load failed) meshOk=%d matOk=%d -- ensure /Engine/BasicShapes + /Game/REBUS are cooked (DirectoriesToAlwaysCook)."),
			*FixtureId, bMeshOk ? 1 : 0, bMatOk ? 1 : 0);
		return;
	}

	LensDisc = NewObject<UStaticMeshComponent>(this, TEXT("LensDisc"));
	LensDisc->SetupAttachment(FixtureRoot);
	LensDisc->RegisterComponent();
	LensDisc->SetMobility(EComponentMobility::Movable);
	LensDisc->SetStaticMesh(Plane);
	LensDisc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	LensDisc->SetCastShadow(false);            // purely an additive flare; never occludes the beam
	LensDisc->bCastDynamicShadow = false;
	LensDisc->SetVisibility(true);
	LensDisc->SetHiddenInGame(false);

	LensDiscMID = UMaterialInstanceDynamic::Create(LensMat, this);
	LensDisc->SetMaterial(0, LensDiscMID);

	// Disc rest: plane normal (+Z) along the beam forward, scaled so the 100 uu engine Plane spans
	// the lens diameter, pushed slightly PROUD of the lens plane along the aim so opaque head
	// geometry can't occlude/clip it. Composed with head motion in RefreshMotion.
	const float PlaneScale = DiamUnreal / 100.f;
	const float ForwardOffset = FMath::Max(DiamUnreal * 0.25f, 1.f); // cm, >= 1cm proud of the lens
	const FVector DiscOrigin = BeamRestTransform.GetLocation() + BeamForwardLocal.GetSafeNormal() * ForwardOffset;
	const FQuat DiscRot = LensDiscRotationFromForward(BeamForwardLocal, BeamUpLocal);
	LensDiscRest = FTransform(DiscRot, DiscOrigin, FVector(PlaneScale, PlaneScale, PlaneScale));
	LensDisc->SetRelativeTransform(LensDiscRest); // initial placement (RefreshMotion re-applies *Head)

	RefreshLensDisc(); // initial emissive from the current dimmer/colour

	// One consolidated, parseable diagnostics line so the next round is provable from logs.
	const float CurStrength = RebusLensFlareMaxEmissive * FMath::Clamp(Dimmer.Current, 0.f, 1.f);
	const FVector RelScale = LensDisc->GetRelativeScale3D();
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s lens disc: SPAWNED lensDiameter=%s diam=%.2fcm (src=%s) planeScale=%.4f relScale=(%.3f,%.3f,%.3f) meshOk=%d matOk=%d emissiveMax=%.1f curStrength=%.2f SourceRadius=%.3fcm"),
		*FixtureId, *LensDiamStr, DiamUnreal, DiamSrc, PlaneScale,
		RelScale.X, RelScale.Y, RelScale.Z, bMeshOk ? 1 : 0, bMatOk ? 1 : 0,
		RebusLensFlareMaxEmissive, CurStrength, BaseSourceRadiusUnreal);
}

void ARebusFixtureActor::RefreshLensDisc()
{
	if (!LensDiscMID) return;

	// Same shutter-gate the SpotLight uses, so the flare strobes/blacks-out in lockstep.
	float Gate = 1.f;
	switch (ShutterMode)
	{
	case ERebusShutterMode::Closed: Gate = 0.f; break;
	case ERebusShutterMode::Strobe: Gate = (ShutterPhase < 0.5f) ? 1.f : 0.f; break;
	default: break;
	}

	const FLinearColor Linear(
		FMath::Clamp(ColorR.Current, 0.f, 1.f),
		FMath::Clamp(ColorG.Current, 0.f, 1.f),
		FMath::Clamp(ColorB.Current, 0.f, 1.f), 1.f);
	LensDiscMID->SetVectorParameterValue(TEXT("EmissiveColor"), Linear);
	LensDiscMID->SetScalarParameterValue(TEXT("EmissiveStrength"),
		RebusLensFlareMaxEmissive * FMath::Clamp(Dimmer.Current, 0.f, 1.f) * Gate);
}

// ---- Hybrid cone-mesh volumetric beam (Phase 1, §8.4a) --------------------------------

float ARebusFixtureActor::ResolveOuterHalfDeg() const
{
	// The CURRENT outer (field) cone half-angle, matching the SpotLight's lit cone so the mesh
	// beam and the real light agree: the zoom half-angle clamped to the fixture's zoom range, then
	// pinched by the iris (iris 1 = open). Frost is intentionally NOT applied here (it softens the
	// inner cone + source radius, not the outer field extent).
	float OuterHalf = ZoomDeg.Current;
	if (Profile.Zoom.bValid)
	{
		OuterHalf = FMath::Clamp(OuterHalf, (float)(Profile.Zoom.MinDeg * 0.5), (float)(Profile.Zoom.MaxDeg * 0.5));
	}
	OuterHalf = FMath::Clamp(OuterHalf, 0.5f, 80.f);
	const float IrisScale = FMath::Lerp(0.4f, 1.f, FMath::Clamp(Iris.Current, 0.f, 1.f));
	return OuterHalf * IrisScale;
}

void ARebusFixtureActor::BuildBeamCone()
{
	// Base radius = the lens radius (same resolver as the SpotLight SourceRadius + lens disc so the
	// cone starts exactly at the lens). When no lens size is resolvable, fall back to a small
	// visible base so the shaft still originates from a finite disc rather than a mathematical apex.
	const TCHAR* DiamSrc = TEXT("none");
	const double DiamMeters = ResolveLensDiameterMeters(DiamSrc);
	BeamBaseRadiusUnreal = (DiamMeters > KINDA_SMALL_NUMBER)
		? FMath::Max((float)(DiamMeters * 0.5 * RebusCoords::METERS_TO_UNREAL), RebusBeamLensRadiusFloorCm)
		: RebusBeamLensRadiusFloorCm;

	// Length = the SpotLight throw (AttenuationRadius) so the shaft matches the light's reach.
	BeamLengthUnreal = SpotLight ? SpotLight->AttenuationRadius : 6000.f;

	// Prefer the cook-safe CDO hard-ref; fall back to a runtime load by path (logged on failure).
	UMaterialInterface* Mat = BeamMaterial
		? BeamMaterial.Get()
		: LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/REBUS/Materials/M_RebusBeam.M_RebusBeam"));
	if (!Mat)
	{
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("Fixture %s beam: SKIP (M_RebusBeam failed to load -- ensure /Game/REBUS is cooked)."),
			*FixtureId);
		return;
	}

	BeamCone = NewObject<UProceduralMeshComponent>(this, TEXT("BeamCone"));
	BeamCone->SetupAttachment(FixtureRoot);
	BeamCone->RegisterComponent();
	BeamCone->SetMobility(EComponentMobility::Movable);
	BeamCone->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BeamCone->SetCastShadow(false);
	BeamCone->bCastDynamicShadow = false;
	BeamCone->SetVisibility(bMeshBeamEnabled);
	// v1.0.38 culling fix: the cone is a long (~tens of metres), thin, additive-translucent mesh
	// that runs from the fixture down to the floor. Use the component's OWN section bounds (never
	// the small attach-parent bounds) and never let it act as an occluder.
	BeamCone->bUseAttachParentBound = false;
	BeamCone->bUseAsOccluder = false;

	BeamMID = UMaterialInstanceDynamic::Create(Mat, this);
	BeamMID->SetScalarParameterValue(TEXT("BeamSharpness"), RebusBeamSharpness);
	BeamMID->SetScalarParameterValue(TEXT("BeamFalloff"), RebusBeamFalloff);
	// Raymarch tuning (Phase 2): march step count + per-step density for the Custom HLSL body.
	BeamMID->SetScalarParameterValue(TEXT("StepCount"), RebusBeamStepCount);
	BeamMID->SetScalarParameterValue(TEXT("BeamDensity"), RebusBeamDensity);

	// Rest transform: the cone mesh is generated along its local +X (the SAME axis a
	// USpotLightComponent emits along), so it must use the SAME rotation basis as the SpotLight
	// (BeamRestTransform, built from MakeFromXZ(BeamForwardLocal, BeamUpLocal)) -- NOT the lens
	// disc's MakeFromZX basis, which pointed the cone 180deg the wrong way. Reusing the SpotLight's
	// rotation guarantees the cone's +X axis is identical to the spotlight's +X emission axis, so
	// the cone opens downrange along the v1.0.21 beam forward (base/lens at the origin, far/wide
	// end along +forward), exactly matching the lit cone. The cone is radially symmetric, so roll
	// is irrelevant -- only the forward axis matters.
	BeamConeRest = FTransform(BeamRestTransform.GetRotation(), BeamRestTransform.GetLocation());
	BeamCone->SetRelativeTransform(BeamConeRest);

	BeamConeLastFarRadius = -1.f; // force the first section build
	UpdateBeamConeGeometry();     // also seeds BeamLength/LensRadius/FarRadius on the MID
	BeamCone->SetMaterial(0, BeamMID);

	// v1.0.38 culling fix: conservatively inflate the render bounds so the beam is never wrongly
	// culled at certain camera angles. The CreateMeshSection bounds are geometrically correct, but a
	// very elongated translucent shaft whose screen projection falls mostly over the closer opaque
	// floor can be HZB occlusion-culled (and is borderline for frustum culling). A generous bounds
	// scale keeps enough of the volume poking past occluders so the additive beam stays drawn. Only
	// the extent is scaled (origin unchanged), so translucency sort order is unaffected.
	BeamCone->SetBoundsScale(RebusBeamBoundsScale);
	RefreshBeamEmissive();
	RefreshBeamSpatialParams();   // seed world BeamOrigin/BeamDir (RefreshMotion re-pushes per frame)

	const float OuterHalf = ResolveOuterHalfDeg();
	const float CurIntensity = RebusMeshBeamMaxIntensity * FMath::Clamp(Dimmer.Current, 0.f, 1.f) * MeshBeamUserScale;
	// Report the cone forward vs the spotlight forward so a residual flip is provable from logs:
	// both are the +X axis of their (now shared) rotation basis and must be identical.
	const FVector ConeFwd = BeamConeRest.GetRotation().RotateVector(FVector::ForwardVector);
	const FVector SpotFwd = BeamRestTransform.GetRotation().RotateVector(FVector::ForwardVector);
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s beam: SPAWNED matOk=1 baseRadius=%.2fcm farRadius=%.1fcm length=%.0fcm halfAngle=%.1fdeg BeamIntensity=%.2f occlusion=depthtest+depthfade meshBeams=%d (src=%s) coneFwd=(%.3f,%.3f,%.3f) spotFwd=(%.3f,%.3f,%.3f)"),
		*FixtureId, BeamBaseRadiusUnreal, BeamConeLastFarRadius, BeamLengthUnreal, OuterHalf,
		CurIntensity, bMeshBeamEnabled ? 1 : 0, DiamSrc,
		ConeFwd.X, ConeFwd.Y, ConeFwd.Z, SpotFwd.X, SpotFwd.Y, SpotFwd.Z);

	// v1.0.43: prefer Epic's REAL DMX beam (SM_Beam_RM + MI_Beam) when the DMX Fixtures content is
	// installed. On success the procedural cone above becomes the hidden fallback (it stays built so
	// the integration is fully reversible / robust to the content being removed). On failure we keep
	// the M_RebusBeam cone as the visible beam.
	bUsingEpicBeam = TryBuildEpicBeam();
	if (bUsingEpicBeam && BeamCone)
	{
		BeamCone->SetVisibility(false);
	}
}

void ARebusFixtureActor::UpdateBeamConeGeometry()
{
	if (!BeamCone) return;

	const float OuterHalf = ResolveOuterHalfDeg();
	const float TanHalf = FMath::Tan(FMath::DegreesToRadians(OuterHalf));
	const float FarRadius = FMath::Max(BeamLengthUnreal * TanHalf, BeamBaseRadiusUnreal + 0.1f);

	// Skip the rebuild when the far radius is essentially unchanged (zoom fades tick every frame).
	if (BeamConeLastFarRadius >= 0.f && FMath::Abs(FarRadius - BeamConeLastFarRadius) < 0.5f)
	{
		return;
	}
	BeamConeLastFarRadius = FarRadius;

	const int32 Segs = RebusBeamConeSegments;
	const float L = BeamLengthUnreal;
	const float RB = BeamBaseRadiusUnreal;
	const float RF = FarRadius;

	TArray<FVector> Positions;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	Positions.Reserve(Segs * 2 + 2);   // +2 cap centres
	Normals.Reserve(Segs * 2 + 2);
	UVs.Reserve(Segs * 2 + 2);
	Triangles.Reserve(Segs * 12);      // side walls + 2 cap fans

	// Generated along the local +X axis (the spotlight emission axis): base ring at x=0 (the lens)
	// and far ring at x=+L (downrange). With BeamConeRest reusing the SpotLight rotation, +X maps to
	// the beam forward, so the cone opens exactly along the spotlight aim. Rings lie in the YZ plane.
	for (int32 S = 0; S < Segs; ++S)
	{
		const float Angle = (2.f * PI * S) / Segs;
		const float C = FMath::Cos(Angle);
		const float Sn = FMath::Sin(Angle);
		Positions.Add(FVector(0.f, RB * C, RB * Sn)); // base ring (at the lens)
		Positions.Add(FVector(L,   RF * C, RF * Sn));  // far ring (at the throw)
		const FVector N = FVector(0.f, C, Sn).GetSafeNormal(); // outward radial (Fresnel rim)
		Normals.Add(N);
		Normals.Add(N);
		UVs.Add(FVector2D((float)S / Segs, 0.f)); // V=0 base
		UVs.Add(FVector2D((float)S / Segs, 1.f)); // V=1 far (drives the length fade)
	}
	for (int32 S = 0; S < Segs; ++S)
	{
		const int32 B0 = 2 * S;
		const int32 F0 = 2 * S + 1;
		const int32 B1 = 2 * ((S + 1) % Segs);
		const int32 F1 = 2 * ((S + 1) % Segs) + 1;
		Triangles.Add(B0); Triangles.Add(F0); Triangles.Add(F1);
		Triangles.Add(B0); Triangles.Add(F1); Triangles.Add(B1);
	}

	// End caps (v1.0.41): close the volume with a base disc at x=0 (the lens) and a far disc at x=+L
	// (the throw), each a triangle fan to a centre vertex on the axis. This gives the raymarch a
	// surface ALONG the axis, fixing the v1.0.39 down-axis thinning (looking straight down the cone
	// previously hit no lateral wall -> no fragment -> the shaft vanished). The material is two-sided
	// so the fan winding only needs to be self-consistent; normals are unused (unlit additive
	// raymarch) and the cap fragment behaves exactly like the side wall (EXIT = its own depth, so a
	// front cap self-cancels and the far cap carries the column -> no double-add). The v1.0.40
	// distance falloff already dims the far end, so the far cap reads as the column's end, not a hard
	// bright disc.
	const int32 BaseCenter = Positions.Num();
	Positions.Add(FVector(0.f, 0.f, 0.f));      // lens-end axis centre
	Normals.Add(FVector(-1.f, 0.f, 0.f));
	UVs.Add(FVector2D(0.5f, 0.f));
	const int32 FarCenter = Positions.Num();
	Positions.Add(FVector(L, 0.f, 0.f));        // throw-end axis centre
	Normals.Add(FVector(1.f, 0.f, 0.f));
	UVs.Add(FVector2D(0.5f, 1.f));
	for (int32 S = 0; S < Segs; ++S)
	{
		const int32 B0 = 2 * S;
		const int32 B1 = 2 * ((S + 1) % Segs);
		const int32 F0 = 2 * S + 1;
		const int32 F1 = 2 * ((S + 1) % Segs) + 1;
		// Base cap fan (faces -X back toward the fixture).
		Triangles.Add(BaseCenter); Triangles.Add(B0); Triangles.Add(B1);
		// Far cap fan (faces +X downrange; opposite winding to the base cap).
		Triangles.Add(FarCenter); Triangles.Add(F1); Triangles.Add(F0);
	}

	const TArray<FColor> NoColors;
	const TArray<FProcMeshTangent> NoTangents;
	BeamCone->ClearMeshSection(0);
	BeamCone->CreateMeshSection(0, Positions, Triangles, Normals, UVs, NoColors, NoTangents, /*bCreateCollision*/ false);

	// Feed the geometry sizes to the raymarch shader so the marched cone matches the mesh exactly
	// (length along +X, base = lens radius, far ring radius). World origin/dir come from the
	// component each RefreshMotion via RefreshBeamSpatialParams.
	if (BeamMID)
	{
		BeamMID->SetScalarParameterValue(TEXT("BeamLength"), L);
		BeamMID->SetScalarParameterValue(TEXT("LensRadius"), RB);
		BeamMID->SetScalarParameterValue(TEXT("FarRadius"), RF);
	}

	// v1.0.43: zoom/iris changed the far radius -> re-scale Epic's canvas + re-push its distance/lens
	// params so the Epic beam tracks the same cone the procedural mesh would have.
	if (bUsingEpicBeam)
	{
		DriveEpicBeamFromSpotLight();
	}
}

void ARebusFixtureActor::RefreshBeamEmissive()
{
	if (!BeamMID) return;

	// Same shutter-gate the SpotLight uses, so the beam strobes/blacks-out in lockstep.
	float Gate = 1.f;
	switch (ShutterMode)
	{
	case ERebusShutterMode::Closed: Gate = 0.f; break;
	case ERebusShutterMode::Strobe: Gate = (ShutterPhase < 0.5f) ? 1.f : 0.f; break;
	default: break;
	}

	const FLinearColor Linear(
		FMath::Clamp(ColorR.Current, 0.f, 1.f),
		FMath::Clamp(ColorG.Current, 0.f, 1.f),
		FMath::Clamp(ColorB.Current, 0.f, 1.f), 1.f);
	BeamMID->SetVectorParameterValue(TEXT("BeamColor"), Linear);
	BeamMID->SetScalarParameterValue(TEXT("BeamIntensity"),
		RebusMeshBeamMaxIntensity * FMath::Clamp(Dimmer.Current, 0.f, 1.f) * Gate * MeshBeamUserScale);

	// v1.0.43: when Epic's DMX beam is the live path, push the same colour/dimmer/gate onto it too.
	if (bUsingEpicBeam)
	{
		UpdateEpicBeamParams();
	}
}

void ARebusFixtureActor::RefreshBeamSpatialParams()
{
	if (!BeamMID || !BeamCone) return;

	// GROUND TRUTH (v1.0.34): the raymarched beam body MUST march along the direction the SpotLight
	// ACTUALLY lights the floor -- its live USpotLightComponent world forward (+X emission axis)
	// AFTER all rest/head composition -- not the cone component's own basis. The previous fix only
	// asserted "BeamConeRest's +X == emission" by construction, but the rendered shaft proved the
	// cone could still oppose the real emission; sampling the SpotLight directly removes any chance
	// of BeamDir disagreeing with where the light is cast. BeamOrigin = the lit origin (lens), at
	// the SpotLight component location, which the cone base is co-located with (DriveBeamConeFromSpotLight).
	const FVector O = SpotLight ? SpotLight->GetComponentLocation() : BeamCone->GetComponentLocation();
	const FVector D = (SpotLight ? SpotLight->GetForwardVector() : BeamCone->GetForwardVector()).GetSafeNormal();
	BeamMID->SetVectorParameterValue(TEXT("BeamOrigin"), FLinearColor((float)O.X, (float)O.Y, (float)O.Z, 0.f));
	BeamMID->SetVectorParameterValue(TEXT("BeamDir"), FLinearColor((float)D.X, (float)D.Y, (float)D.Z, 0.f));

	// Definitive alignment proof: the SpotLight world forward (where the floor is lit), the cone
	// mesh world forward (which way the frustum opens), and the material BeamDir feed must all be
	// the SAME vector -- dot ~= +1, never -1. Throttled to meaningful aim changes so a pan/tilt
	// sweep logs a verifiable trail without spamming every tick.
	if (SpotLight && FVector::DotProduct(D, LastLoggedBeamFwd) < 0.999f)
	{
		LastLoggedBeamFwd = D;
		const FVector ConeFwd = BeamCone->GetForwardVector().GetSafeNormal();
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Fixture %s beam align: spotFwd=(%.3f,%.3f,%.3f) coneFwd=(%.3f,%.3f,%.3f) beamDir=(%.3f,%.3f,%.3f) dot(spot,cone)=%.3f dot(spot,beamDir)=%.3f"),
			*FixtureId, D.X, D.Y, D.Z, ConeFwd.X, ConeFwd.Y, ConeFwd.Z, D.X, D.Y, D.Z,
			FVector::DotProduct(D, ConeFwd), FVector::DotProduct(D, D));
	}
}

void ARebusFixtureActor::DriveBeamConeFromSpotLight()
{
	if (!SpotLight || !BeamCone) return;

	// Orient the cone mesh so its local +X (the generated frustum opens base->far along +X, see
	// UpdateBeamConeGeometry) IS the SpotLight's live world emission forward, and co-locate its
	// base ring (local origin = the lens) with the SpotLight's world location. This replaces the
	// earlier BeamConeRest*Head reliance (which assumed that rest basis equalled the real emission)
	// with the single source of truth -- the same component whose +X lights the floor -- so the
	// mesh shaft can never render opposite the spotlight. The cone is radially symmetric, so the
	// arbitrary roll MakeFromX picks for the up axis is irrelevant.
	const FVector SpotFwd = SpotLight->GetForwardVector().GetSafeNormal();
	const FVector SpotLoc = SpotLight->GetComponentLocation();
	BeamCone->SetWorldLocationAndRotation(SpotLoc, FRotationMatrix::MakeFromX(SpotFwd).ToQuat());
	RefreshBeamSpatialParams(); // push the (now spotlight-aligned) world origin/dir to the raymarch MID

	// v1.0.43: ride the Epic DMX beam canvas off the same ground-truth spotlight transform.
	if (bUsingEpicBeam)
	{
		DriveEpicBeamFromSpotLight();
	}
}

bool ARebusFixtureActor::TryBuildEpicBeam()
{
	// Resolve Epic's official DMX beam assets: prefer the cook-safe CDO hard refs (constructor
	// FObjectFinder), else a runtime LoadObject by the verified /DMXFixtures path (config-overridable
	// for non-standard installs). If either the material or the canvas mesh is missing, the DMX
	// content isn't installed -> keep the M_RebusBeam fallback.
	FString MatPath = RebusEpicBeamMaterialPath;
	FString MeshPath = RebusEpicBeamMeshPath;
	GConfig->GetString(TEXT("RebusVisualiser"), TEXT("EpicDmxBeamMaterial"), MatPath, GGameIni);
	GConfig->GetString(TEXT("RebusVisualiser"), TEXT("EpicDmxBeamMesh"), MeshPath, GGameIni);

	UMaterialInterface* EpicMat = EpicBeamMaterial ? EpicBeamMaterial.Get()
		: LoadObject<UMaterialInterface>(nullptr, *MatPath);
	UStaticMesh* EpicMesh = EpicBeamMesh ? EpicBeamMesh.Get()
		: LoadObject<UStaticMesh>(nullptr, *MeshPath);
	if (!EpicMat || !EpicMesh)
	{
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("Fixture %s beam: Epic DMX content NOT found (MI_Beam=%s SM_Beam_RM=%s) -- using fallback beam (M_RebusBeam). Install the DMX Fixtures plugin content to enable Epic's M_LightBeam."),
			*FixtureId, EpicMat ? TEXT("ok") : *MatPath, EpicMesh ? TEXT("ok") : *MeshPath);
		return false;
	}

	EpicBeamComp = NewObject<UStaticMeshComponent>(this, TEXT("EpicBeamCanvas"));
	// v1.0.45 (Issue 1 fix): ride the SpotLight EXACTLY like ADMXFixtureActor's beam rides its Head --
	// parent the canvas to the SpotLight with a CONSTANT relative rotation, so ALL pan/tilt comes from
	// the single basis that actually creates the lit footprint (the SpotLight's own transform). The
	// v1.0.44 approach world-aimed the canvas every frame with FindBetweenNormals, whose roll varies
	// with the aim; because M_Beam_Master derives the cone from the object basis (not just one axis),
	// that varying roll mirrored the yaw. Inheriting the SpotLight basis can't mirror -- the beam and
	// the footprint are now driven by the same rotation.
	USceneComponent* BeamParent = SpotLight ? static_cast<USceneComponent*>(SpotLight) : static_cast<USceneComponent*>(FixtureRoot);
	EpicBeamComp->SetupAttachment(BeamParent);
	EpicBeamComp->RegisterComponent();
	EpicBeamComp->SetStaticMesh(EpicMesh);
	EpicBeamComp->SetMobility(EComponentMobility::Movable);
	EpicBeamComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	EpicBeamComp->SetCastShadow(false);
	EpicBeamComp->bCastDynamicShadow = false;
	// Parity with the procedural cone (v1.0.36): never occlude + no self-volumetric-shadow into its
	// own beam. The mesh already carries huge (+/-10000) bounds so the WPO cone is never culled --
	// we DON'T add a bounds scale (and crucially DON'T scale the component).
	EpicBeamComp->bUseAttachParentBound = false;
	EpicBeamComp->bUseAsOccluder = false;
	EpicBeamComp->SetVisibility(bMeshBeamEnabled);
	DisableSelfBeamVolumetricShadow(EpicBeamComp);

	EpicBeamMID = UMaterialInstanceDynamic::Create(EpicMat, this);
	EpicBeamComp->SetMaterial(0, EpicBeamMID);

	// v1.0.48: snapshot the MI parent's default "DMX Gobo Disk Frosted" so a "clear gobo"
	// (ApplyGobo with !bHasIndex) can restore Epic's default (T_GoboDisk_01_Frosted, the open
	// disc that lets the beam through unmasked) instead of leaving the last-selected gobo stuck.
	{
		UTexture* DefTex = nullptr;
		EpicMat->GetTextureParameterValue(FMaterialParameterInfo(TEXT("DMX Gobo Disk Frosted")), DefTex);
		EpicBeamDefaultGoboTex = DefTex;
	}

	// Fixed local transform relative to the SpotLight: apex/lens at the spotlight origin (relLoc 0),
	// canvas local emission (+Z, see comment block at top) mapped onto the spotlight's local +X
	// emission, scale 1 (the WPO cone is built in unit space; any component scale breaks it). When
	// there's no SpotLight to ride, fall back to FixtureRoot + a per-frame world aim.
	if (SpotLight)
	{
		const FQuat RelRot = FQuat::FindBetweenNormals(RebusEpicBeamLocalEmission, FVector::ForwardVector); // +Z -> +X
		EpicBeamComp->SetRelativeLocationAndRotation(FVector::ZeroVector, RelRot);
		EpicBeamComp->SetRelativeScale3D(FVector::OneVector);
	}

	UpdateEpicBeamParams();
	DriveEpicBeamFromSpotLight();

	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s beam: using Epic M_LightBeam (MI_Beam + SM_Beam_RM) attached=%s localEmission=(%.0f,%.0f,%.0f) scale=1 (WPO cone) src=%s."),
		*FixtureId, SpotLight ? TEXT("SpotLight") : TEXT("FixtureRoot"),
		RebusEpicBeamLocalEmission.X, RebusEpicBeamLocalEmission.Y, RebusEpicBeamLocalEmission.Z,
		EpicBeamMaterial ? TEXT("CDO") : TEXT("LoadObject"));
	return true;
}

void ARebusFixtureActor::UpdateEpicBeamParams()
{
	if (!EpicBeamMID) return;

	// Same shutter-gate the SpotLight/lens/cone use so the Epic beam strobes/blacks-out in lockstep.
	float Gate = 1.f;
	switch (ShutterMode)
	{
	case ERebusShutterMode::Closed: Gate = 0.f; break;
	case ERebusShutterMode::Strobe: Gate = (ShutterPhase < 0.5f) ? 1.f : 0.f; break;
	default: break;
	}

	const FLinearColor Col(
		FMath::Clamp(ColorR.Current, 0.f, 1.f),
		FMath::Clamp(ColorG.Current, 0.f, 1.f),
		FMath::Clamp(ColorB.Current, 0.f, 1.f), 1.f);
	// Epic separates brightness into a candela-scale "DMX Max Light Intensity" x a 0..1 "DMX Dimmer".
	const float Dim = FMath::Clamp(Dimmer.Current, 0.f, 1.f) * Gate;
	// Cone angle: drive "DMX Zoom" from the SpotLight's LIVE outer cone HALF-angle (the very angle
	// that defines the lit footprint -- single source of truth, so beam edge == pool edge and they
	// can't diverge). Empirically M_Beam_Master reads ~the half-angle (feeding 2x made it too wide),
	// hence RebusEpicBeamZoomScale defaults to 1.0. Length capped to the canvas mesh extent.
	const float SpotOuterHalfDeg = SpotLight ? SpotLight->OuterConeAngle : ResolveOuterHalfDeg();
	const float ZoomFullDeg = FMath::Clamp(RebusEpicBeamZoomScale * SpotOuterHalfDeg, 1.f, 179.f);
	const float DistCm = FMath::Clamp(BeamLengthUnreal, 1.f, RebusEpicBeamMaxDistanceCm);

	// Epic M_Beam_Master param vocabulary (mirrors ADMXFixtureActor::FeedFixtureData + the BP zoom
	// feed). Unknown params silently no-op, so this is safe across DMX content revisions.
	EpicBeamMID->SetVectorParameterValue(TEXT("DMX Color"), Col);
	EpicBeamMID->SetScalarParameterValue(TEXT("DMX Max Light Intensity"), RebusEpicBeamMaxIntensity * MeshBeamUserScale);
	EpicBeamMID->SetScalarParameterValue(TEXT("DMX Dimmer"), Dim);
	EpicBeamMID->SetScalarParameterValue(TEXT("DMX Max Light Distance"), DistCm);
	EpicBeamMID->SetScalarParameterValue(TEXT("DMX Lens Radius"), BeamBaseRadiusUnreal);
	EpicBeamMID->SetScalarParameterValue(TEXT("DMX Zoom"), ZoomFullDeg);
	EpicBeamMID->SetScalarParameterValue(TEXT("DMX Zoom Normalize"), 0.f); // DMX Zoom is in degrees
	EpicBeamMID->SetScalarParameterValue(TEXT("DMX Quality Level"), RebusEpicBeamQuality);

	// v1.0.48: also re-push the gobo state here so a beam (re)build or refresh doesn't drop the
	// live selection. ApplyCurrentGoboToEpicBeam picks the right texture + atlas indices.
	ApplyCurrentGoboToEpicBeam();
}

void ARebusFixtureActor::DriveEpicBeamFromSpotLight()
{
	if (!EpicBeamComp) return;

	// v1.0.45: the canvas is parented to the SpotLight with a fixed relative transform (see
	// TryBuildEpicBeam), so pan/tilt + apex-at-lens are inherited automatically from the same basis
	// that creates the footprint -- no per-frame world re-aim (that was the v1.0.44 mirror). We only
	// refresh the WPO/colour params here. Fallback: if there's no SpotLight to ride, world-aim once.
	if (!SpotLight)
	{
		const FVector Fwd = EpicBeamComp->GetForwardVector(); // best-effort with no spotlight
		EpicBeamComp->SetWorldRotation(FQuat::FindBetweenNormals(RebusEpicBeamLocalEmission, Fwd));
		UpdateEpicBeamParams();
		return;
	}

	UpdateEpicBeamParams();

	// REAL alignment proof (not tautological): read the canvas's ACTUAL world transform (from the
	// attachment) and compare its emission axis to the live spotlight forward -- dot must be ~+1 at
	// every pan/tilt, and the apex (canvas world origin) must sit on the lens (spotlight location).
	const FVector SpotFwd = SpotLight->GetForwardVector().GetSafeNormal();
	const FVector SpotLoc = SpotLight->GetComponentLocation();
	const FVector CanvasEmission = EpicBeamComp->GetComponentTransform().TransformVectorNoScale(RebusEpicBeamLocalEmission).GetSafeNormal();
	if (FVector::DotProduct(CanvasEmission, EpicLastLoggedFwd) < 0.999f)
	{
		EpicLastLoggedFwd = CanvasEmission;
		const FVector Apex = EpicBeamComp->GetComponentLocation();
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Fixture %s Epic beam align: spotFwd=(%.3f,%.3f,%.3f) canvasFwd=(%.3f,%.3f,%.3f) dot=%.3f apex=(%.0f,%.0f,%.0f) lens=(%.0f,%.0f,%.0f) |apex-lens|=%.2fcm zoomHalf=%.1fdeg"),
			*FixtureId, SpotFwd.X, SpotFwd.Y, SpotFwd.Z, CanvasEmission.X, CanvasEmission.Y, CanvasEmission.Z,
			FVector::DotProduct(CanvasEmission, SpotFwd),
			Apex.X, Apex.Y, Apex.Z, SpotLoc.X, SpotLoc.Y, SpotLoc.Z,
			(float)FVector::Dist(Apex, SpotLoc), SpotLight->OuterConeAngle);
	}
}

void ARebusFixtureActor::RefreshBeamShadowMode()
{
	if (!SpotLight) return;

	// A hero shadow beam is one that asked for volumetric shadows AND won a per-batch budget slot.
	const bool bShadowActive = bWantsVolumetricShadow && bGrantedShadowHero;

	if (bMeshBeamEnabled)
	{
		// Mesh cone is the crisp shaft. Hero shadow beams ALSO emit a modest native fog scattering
		// with Cast Volumetric Shadow so VSM (which works on runtime-imported glTF meshes that lack
		// distance fields) carves real truss gaps into the volume. Non-hero beams stay mesh-only
		// (scattering 0) so there's no competing froxel noise.
		SpotLight->SetVolumetricScatteringIntensity(bShadowActive ? GRebusHeroShadowScatter : 0.f);
		// Cast Volumetric Shadow only meaningfully carves the fog when the SpotLight is also casting
		// regular shadows -- VSM needs the per-light shadow data to derive the volumetric shadow.
		// v1.0.49: also enable CastShadows when a gobo is active -- a SpotLight LightFunctionMaterial
		// only projects when the light is also casting shadows (the function is sampled via the
		// shadow render target). Without this, the cookie wouldn't render on non-hero fixtures.
		SpotLight->SetCastShadows(bShadowActive || bGoboActive);
		SpotLight->SetCastVolumetricShadow(bShadowActive);
	}
	else
	{
		// Fog-beam A/B mode: restore the froxel beam; hero beams still cast volumetric shadow.
		SpotLight->SetVolumetricScatteringIntensity(FogScatteringIntensity);
		SpotLight->SetCastShadows(bShadowActive || bGoboActive);
		SpotLight->SetCastVolumetricShadow(bShadowActive);
	}
	SpotLight->MarkRenderStateDirty();
}

void ARebusFixtureActor::SetMeshBeamEnabled(bool bEnabled)
{
	bMeshBeamEnabled = bEnabled;
	if (BeamCone)
	{
		// When Epic's beam is live the procedural cone stays hidden (it's the fallback canvas);
		// otherwise it is the visible beam and follows the toggle.
		BeamCone->SetVisibility(bEnabled && !bUsingEpicBeam);
	}
	if (EpicBeamComp)
	{
		EpicBeamComp->SetVisibility(bEnabled);
	}
	// Re-resolve the SpotLight volumetric state (mesh-only vs hero VSM fog shadow vs restored fog).
	RefreshBeamShadowMode();
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s mesh beam %s (shadowHero=%d wantsShadow=%d fogScatter=%.2f)."),
		*FixtureId, bEnabled ? TEXT("ENABLED") : TEXT("DISABLED -> fog beam restored"),
		bGrantedShadowHero ? 1 : 0, bWantsVolumetricShadow ? 1 : 0,
		bEnabled ? (bWantsVolumetricShadow && bGrantedShadowHero ? GRebusHeroShadowScatter : 0.f) : FogScatteringIntensity);
}

void ARebusFixtureActor::RefreshMotion()
{
	if (!Profile.MotionRig.bValid || Profile.MotionRig.Axes.Num() == 0)
	{
		// No GDTF motion rig (e.g. a profile-less data-channel push). We can't tell the moving
		// head from the static base, so the mesh proxies stay put, but we still aim the BEAM
		// like a default moving head so pan/tilt are usable: rest straight down (-Z), tilt
		// raises it toward the +X front, pan orbits about local up (+Z). Tilt sign chosen so a
		// positive tilt lifts the beam off the floor; flip here if a fixture reads inverted.
		for (UProceduralMeshComponent* PMC : MeshComponents)
		{
			if (PMC) PMC->SetRelativeTransform(FTransform::Identity);
		}
		if (SpotLight)
		{
			FVector Dir(0.f, 0.f, -1.f); // straight down at rest
			Dir = FQuat(FVector::RightVector, FMath::DegreesToRadians(-TiltDeg.Current)).RotateVector(Dir);
			Dir = FQuat(FVector::UpVector, FMath::DegreesToRadians(PanDeg.Current)).RotateVector(Dir);

			FTransform T = BeamRestTransform; // keep the rest origin
			T.SetRotation(FRotationMatrix::MakeFromX(Dir).ToQuat());
			// v1.0.53: gobo image rotation is now done in the texture itself (per-fixture
			// UCanvasRenderTarget2D redrawn rotated by GoboAngle each tick), NOT by rolling the
			// SpotLight. v1.0.52 rolled the SpotLight around its local +X (emission axis) on the
			// assumption that the cookie / inherited canvas roll would rotate the gobo, but the
			// user reported "rotating around x instead of z" -- the desired axis is the cookie's
			// in-plane Z (perpendicular to the projected pattern), which a texture UV rotation
			// hits cleanly without disturbing the SpotLight transform. So the SpotLight's
			// relative rotation is restored to just the head-tracking rotation.
			SpotLight->SetRelativeTransform(T);

			// Lens disc tracks the same synthetic aim: plane normal along the beam Dir.
			if (LensDisc)
			{
				FTransform DiscT;
				DiscT.SetRotation(LensDiscRotationFromForward(Dir, BeamUpLocal));
				DiscT.SetLocation(LensDiscRest.GetLocation());
				DiscT.SetScale3D(LensDiscRest.GetScale3D());
				LensDisc->SetRelativeTransform(DiscT);
			}

			// Cone-mesh beam rides the SpotLight's live world emission (DriveBeamConeFromSpotLight
			// samples SpotLight->GetForwardVector() AFTER the SetRelativeTransform above), so the
			// shaft opens along EXACTLY the synthetic aim the spotlight lights, never 180deg out.
			DriveBeamConeFromSpotLight();
		}

		// No GDTF rig: the control-channel mesh proxies don't move, so drive the bound Orbit model
		// with an identity head -- it stays at its imported pose, in A/B lock-step with the (also
		// static) control meshes rather than diverging.
		DriveOrbitModel(FTransform::Identity);
		return;
	}

	TArray<FTransform> Cumulative;
	RebusMotion::Solve(Profile.MotionRig, PanDeg.Current, TiltDeg.Current, Cumulative);

	for (int32 i = 0; i < MeshComponents.Num(); ++i)
	{
		UProceduralMeshComponent* PMC = MeshComponents[i];
		if (!PMC) continue;
		const int32 Axis = MeshAxisBucket.IsValidIndex(i) ? MeshAxisBucket[i] : INDEX_NONE;
		const FTransform Rel = (Axis != INDEX_NONE && Cumulative.IsValidIndex(Axis)) ? Cumulative[Axis] : FTransform::Identity;
		PMC->SetRelativeTransform(Rel);
	}

	if (SpotLight)
	{
		// Rigidly ride the head: reuse the SAME cumulative axis transform that drives the head
		// mesh proxies above (Cumulative[HeadAxisIndex]) rather than recomputing pan/tilt for the
		// beam, so the beam can never drift from the geometry. Apply the beam rest as the light's
		// local-within-head transform, then the head motion: BeamRest * Head (§7.7).
		const FTransform Head = (HeadAxisIndex != INDEX_NONE && Cumulative.IsValidIndex(HeadAxisIndex))
			? Cumulative[HeadAxisIndex] : FTransform::Identity;
		// v1.0.53: gobo image rotation moved off the SpotLight transform (was a local +X roll in
		// v1.0.52) and into a per-fixture UCanvasRenderTarget2D that redraws the source gobo
		// texture rotated by GoboAngle each tick. The RT is bound as Epic's "DMX Gobo Disk
		// Frosted" texture param on EpicBeamMID + GoboLightFnMID, so the projected pattern spins
		// in plane (around the cookie's Z / out-of-screen axis) without rolling the SpotLight or
		// inheriting any motion via the parented EpicBeamComp. SpotLight relative transform is
		// just the head-tracking rotation again.
		SpotLight->SetRelativeTransform(BeamRestTransform * Head);

		// Lens disc rides the SAME head transform (LensDiscRest * Head), so it stays co-located
		// with the beam origin and perpendicular to the v1.0.21 beam direction through pan/tilt.
		if (LensDisc)
		{
			LensDisc->SetRelativeTransform(LensDiscRest * Head);
		}

		// Cone-mesh beam tracks the SpotLight's live world emission rather than re-deriving its own
		// BeamConeRest*Head basis: DriveBeamConeFromSpotLight reads SpotLight->GetForwardVector()
		// (set just above) so the shaft opens along exactly the direction the floor is lit, through
		// every pan/tilt, and can never invert relative to the spotlight.
		DriveBeamConeFromSpotLight();
	}

	// Drive the bound Orbit-imported model with the SAME head solve that moved the control-channel
	// head meshes above (Cumulative[HeadAxisIndex]), so the two render on top of each other and
	// pan/tilt together. No-op when not driving / unbound.
	const FTransform OrbitHead = (HeadAxisIndex != INDEX_NONE && Cumulative.IsValidIndex(HeadAxisIndex))
		? Cumulative[HeadAxisIndex] : FTransform::Identity;
	DriveOrbitModel(OrbitHead);
}

// ---- Orbit-imported model binding (Phase 1 A/B sync test) -----------------------------

FTransform ARebusFixtureActor::ComputeHeadLocal(float InPanDeg, float InTiltDeg) const
{
	// The head's fixture-local transform = the deepest head axis' cumulative solve. No rig -> the
	// control meshes don't move, so the head is identity (the Orbit model holds its imported pose).
	if (!Profile.MotionRig.bValid || Profile.MotionRig.Axes.Num() == 0)
	{
		return FTransform::Identity;
	}
	TArray<FTransform> Cumulative;
	RebusMotion::Solve(Profile.MotionRig, InPanDeg, InTiltDeg, Cumulative);
	return (HeadAxisIndex != INDEX_NONE && Cumulative.IsValidIndex(HeadAxisIndex))
		? Cumulative[HeadAxisIndex] : FTransform::Identity;
}

void ARebusFixtureActor::DisableSelfBeamVolumetricShadow(UPrimitiveComponent* Comp)
{
	if (!Comp) return;
	// Keep the primitive a shadow caster (CastShadow stays true -> contact/ray-traced grounding is
	// preserved) but drop its DYNAMIC shadow-map contribution. A movable spotlight's volumetric fog
	// inscattering is shadowed by that light's VSM/shadow depth, which only includes primitives with
	// CastShadow && bCastDynamicShadow -- so clearing bCastDynamicShadow removes the fixture body /
	// bound Orbit model from the fog occlusion that was mottling the base of its own beam, without a
	// (non-existent in UE5.7) per-primitive volumetric-fog toggle. Trade-off: the body no longer
	// casts a dynamic shadow into ANY beam (incl. neighbours) or onto the floor -- acceptable since
	// fixture bodies are small/airborne; the trusses/set (other actors) keep their dynamic shadows,
	// so the hybrid's wanted truss self-shadowing is unaffected.
	Comp->SetCastShadow(true);
	Comp->bCastDynamicShadow = false;
	Comp->MarkRenderStateDirty();
}

void ARebusFixtureActor::BindOrbitComponents(const TArray<USceneComponent*>& Components, const FString& MatchedObjectId)
{
	OrbitComponents.Reset();
	OrbitCompRestWorld.Reset();
	OrbitBindBase.Reset();
	BoundOrbitObjectId = MatchedObjectId;
	LastOrbitLogPanTilt = FVector2D(FLT_MAX, FLT_MAX);

	// The imported model corresponds to the fixture's REST pose (pan=tilt=0): cache the head world
	// transform there so DriveOrbitModel applies only the DELTA from rest as pan/tilt change. With
	// FTransform's child*parent convention, a component's world = childRel * parent, so the head
	// world (head mesh proxies are parented under the actor root) is HeadLocal * ActorWorld.
	const FTransform ActorWorld = GetActorTransform();
	OrbitHeadWorldRest = ComputeHeadLocal(0.f, 0.f) * ActorWorld;
	const FTransform HeadWorldRestInv = OrbitHeadWorldRest.Inverse();

	for (USceneComponent* Comp : Components)
	{
		if (!Comp) continue;
		const FTransform CompRest = Comp->GetComponentTransform();
		OrbitComponents.Add(Comp);
		OrbitCompRestWorld.Add(CompRest);
		// Driven world = CompRest * HeadWorldRest^-1 * HeadWorldNow; precompute the constant prefix.
		OrbitBindBase.Add(CompRest * HeadWorldRestInv);
		// The bound Orbit model sits right on top of this fixture's light source, so exclude it from
		// its own beam's volumetric-fog shadow (it would otherwise double-occlude with the body).
		DisableSelfBeamVolumetricShadow(Cast<UPrimitiveComponent>(Comp));
	}

	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s: BOUND %d Orbit-imported component(s) by objectId '%s' (drive=%s)."),
		*FixtureId, OrbitComponents.Num(), *MatchedObjectId,
		bDriveOrbitModel ? TEXT("ON") : TEXT("off"));

	// Snap the model to the fixture's current pose now, so a late bind doesn't pop on the next tick.
	if (bDriveOrbitModel)
	{
		DriveOrbitModel(ComputeHeadLocal(PanDeg.Current, TiltDeg.Current));
	}
}

void ARebusFixtureActor::ClearOrbitBinding()
{
	OrbitComponents.Reset();
	OrbitCompRestWorld.Reset();
	OrbitBindBase.Reset();
	BoundOrbitObjectId.Reset();
}

bool ARebusFixtureActor::HasOrbitBinding() const
{
	for (const TWeakObjectPtr<USceneComponent>& C : OrbitComponents)
	{
		if (C.IsValid()) return true;
	}
	return false;
}

void ARebusFixtureActor::SetDriveOrbitModel(bool bEnabled)
{
	bDriveOrbitModel = bEnabled;
	if (bEnabled)
	{
		// Push the model to the fixture's current pose immediately (if bound).
		if (HasOrbitBinding())
		{
			DriveOrbitModel(ComputeHeadLocal(PanDeg.Current, TiltDeg.Current));
		}
	}
	else
	{
		// Restore the Orbit components to their imported (rest) world transforms so they stop
		// tracking and sit exactly where the import placed them.
		int32 Restored = 0;
		for (int32 i = 0; i < OrbitComponents.Num(); ++i)
		{
			USceneComponent* Comp = OrbitComponents[i].Get();
			if (Comp && OrbitCompRestWorld.IsValidIndex(i))
			{
				Comp->SetWorldTransform(OrbitCompRestWorld[i]);
				++Restored;
			}
		}
		if (Restored > 0)
		{
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Fixture %s: drive Orbit model OFF; restored %d component(s) to imported pose."),
				*FixtureId, Restored);
		}
	}
}

void ARebusFixtureActor::DriveOrbitModel(const FTransform& HeadLocal)
{
	if (!bDriveOrbitModel || OrbitComponents.Num() == 0) return;

	const FTransform HeadWorldNow = HeadLocal * GetActorTransform();
	int32 Driven = 0;
	for (int32 i = 0; i < OrbitComponents.Num(); ++i)
	{
		USceneComponent* Comp = OrbitComponents[i].Get();
		if (!Comp || !OrbitBindBase.IsValidIndex(i)) continue;
		Comp->SetWorldTransform(OrbitBindBase[i] * HeadWorldNow);
		++Driven;
	}
	if (Driven == 0) return;

	// Per-update sync log (throttled to meaningful pan/tilt changes) so the Orbit model motion can
	// be compared against the control-channel head meshes for the A/B confirmation.
	const FVector2D PanTilt(PanDeg.Current, TiltDeg.Current);
	if (FMath::Abs(PanTilt.X - LastOrbitLogPanTilt.X) + FMath::Abs(PanTilt.Y - LastOrbitLogPanTilt.Y) > 0.5f)
	{
		LastOrbitLogPanTilt = PanTilt;
		const FRotator HeadRot = HeadLocal.Rotator();
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Fixture %s: drove Orbit model '%s' pan=%.1f tilt=%.1f headRot=(P=%.1f Y=%.1f R=%.1f) comps=%d"),
			*FixtureId, *BoundOrbitObjectId, PanTilt.X, PanTilt.Y,
			HeadRot.Pitch, HeadRot.Yaw, HeadRot.Roll, Driven);
	}
}

void ARebusFixtureActor::RecomputeConeAngles()
{
	if (!SpotLight) return;

	// Outer (field) half-angle: zoom clamped to the fixture's zoom range (§8.1) then pinched by the
	// iris. Shared with the cone-mesh beam (ResolveOuterHalfDeg) so the lit cone and the mesh shaft
	// always agree.
	const float OuterHalf = ResolveOuterHalfDeg();

	// Inner cone from the beam/field ratio when available, else 80% of outer. Frost softens
	// the inner cone toward the outer edge.
	float InnerRatio = 0.8f;
	if (Profile.Photometrics.BeamAngle.IsSet() && Profile.Photometrics.FieldAngle.IsSet()
		&& Profile.Photometrics.FieldAngle.GetValue() > KINDA_SMALL_NUMBER)
	{
		InnerRatio = (float)(Profile.Photometrics.BeamAngle.GetValue() / Profile.Photometrics.FieldAngle.GetValue());
		InnerRatio = FMath::Clamp(InnerRatio, 0.05f, 0.98f);
	}
	const float FrostSoften = FMath::Lerp(1.f, 0.2f, FMath::Clamp(Frost.Current, 0.f, 1.f));
	const float InnerHalf = OuterHalf * InnerRatio * FrostSoften;

	SpotLight->SetOuterConeAngle(OuterHalf);
	SpotLight->SetInnerConeAngle(FMath::Min(InnerHalf, OuterHalf));

	// Frost also enlarges the apparent source for softer penumbra. Scale the resolved base source
	// radius (the lens-opening disc set in BuildSpotLight) so the beam-origin diameter stays
	// consistent with the lens-flare disc; left untouched when no source size was known.
	if (BaseSourceRadiusUnreal >= 0.f)
	{
		SpotLight->SetSourceRadius(BaseSourceRadiusUnreal * FMath::Lerp(1.f, 4.f, FMath::Clamp(Frost.Current, 0.f, 1.f)));
	}

	// Keep the cone-mesh beam sized to the same field half-angle (regenerates the frustum far
	// radius only when zoom/iris actually changed it -- see the rebuild gate in UpdateBeamConeGeometry).
	UpdateBeamConeGeometry();
}

void ARebusFixtureActor::RefreshIntensity()
{
	if (!SpotLight) return;

	float Gate = 1.f;
	switch (ShutterMode)
	{
	case ERebusShutterMode::Closed: Gate = 0.f; break;
	case ERebusShutterMode::Strobe: Gate = (ShutterPhase < 0.5f) ? 1.f : 0.f; break;
	default: break;
	}

	SpotLight->SetIntensity(BaseCandela * FMath::Clamp(Dimmer.Current, 0.f, 1.f) * Gate);

	const FLinearColor Linear(
		FMath::Clamp(ColorR.Current, 0.f, 1.f),
		FMath::Clamp(ColorG.Current, 0.f, 1.f),
		FMath::Clamp(ColorB.Current, 0.f, 1.f), 1.f);
	SpotLight->SetLightColor(Linear);

	// Keep the emissive lens disc in lockstep with the live output (same dimmer/colour/shutter
	// path), so the glowing lens brightens/colours with the beam and darkens when dimmed (§8.3a).
	RefreshLensDisc();

	// Same for the cone-mesh beam: BeamColor follows the live colour, BeamIntensity follows
	// dimmer x shutter-gate x SetFixtureBeamVolumetrics, so it fades to nothing when dimmed/closed.
	RefreshBeamEmissive();
}

// ---- Control surface ------------------------------------------------------------------

void ARebusFixtureActor::ApplyDimmer(float Intensity01, float FadeSeconds)
{
	Dimmer.SetTarget(FMath::Clamp(Intensity01, 0.f, 1.f), FadeSeconds);
	bAnimating = true;
	if (FadeSeconds <= 0.f) RefreshIntensity();
}

void ARebusFixtureActor::ApplyColor(const FLinearColor& SrgbColor, float FadeSeconds)
{
	ColorR.SetTarget(SrgbToLinearChannel(SrgbColor.R), FadeSeconds);
	ColorG.SetTarget(SrgbToLinearChannel(SrgbColor.G), FadeSeconds);
	ColorB.SetTarget(SrgbToLinearChannel(SrgbColor.B), FadeSeconds);
	bAnimating = true;
	if (FadeSeconds <= 0.f) RefreshIntensity();
}

void ARebusFixtureActor::ApplyPanTilt(float InPanDeg, float InTiltDeg, float FadeSeconds)
{
	PanDeg.SetTarget(InPanDeg, FadeSeconds);
	TiltDeg.SetTarget(InTiltDeg, FadeSeconds);
	bAnimating = true;
	if (FadeSeconds <= 0.f) RefreshMotion();
}

void ARebusFixtureActor::ApplyZoom(float ZoomHalfAngleDeg, float FadeSeconds)
{
	ZoomDeg.SetTarget(ZoomHalfAngleDeg, FadeSeconds);
	bAnimating = true;
	if (FadeSeconds <= 0.f) { RecomputeConeAngles(); SelectIesForZoom(); }
}

void ARebusFixtureActor::ApplyIris(float Iris01, float FadeSeconds)
{
	Iris.SetTarget(FMath::Clamp(Iris01, 0.f, 1.f), FadeSeconds);
	bAnimating = true;
	if (FadeSeconds <= 0.f) RecomputeConeAngles();
}

void ARebusFixtureActor::ApplyFocus(float Focus01, float FadeSeconds)
{
	// Stored + interpolated; visual focus deferred on the reference plugin (§5.2).
	Focus.SetTarget(FMath::Clamp(Focus01, 0.f, 1.f), FadeSeconds);
	bAnimating = true;
}

void ARebusFixtureActor::ApplyFrost(float Frost01, float FadeSeconds)
{
	Frost.SetTarget(FMath::Clamp(Frost01, 0.f, 1.f), FadeSeconds);
	bAnimating = true;
	if (FadeSeconds <= 0.f) RecomputeConeAngles();
}

void ARebusFixtureActor::ApplyColorTemp(float Kelvin)
{
	if (!SpotLight) return;
	SpotLight->SetUseTemperature(true);
	SpotLight->SetTemperature(FMath::Clamp(Kelvin, 1000.f, 15000.f));
}

void ARebusFixtureActor::ApplyShutter(ERebusShutterMode Mode, float RateHz)
{
	ShutterMode = Mode;
	ShutterRateHz = FMath::Clamp(RateHz, 0.f, 30.f);
	ShutterPhase = 0.f;
	RefreshIntensity();
	if (Mode == ERebusShutterMode::Strobe) bAnimating = true;
}

void ARebusFixtureActor::ApplyGoboRotation(float Speed, int32 WheelIndex)
{
	GoboRotationSpeed = FMath::Clamp(Speed, -1.f, 1.f);
	CurrentGoboRotationSpeed = GoboRotationSpeed;
	// v1.0.52: the rotation no longer routes to a material param. Epic's M_Beam_Master,
	// MF_DMXGobo, and M_Light_Master expose NO image-rotation parameter (verified by enumerating
	// the uasset string tables: only DMX Gobo Disk Frosted / DMX Gobo Disk Rotation Speed / DMX
	// Gobo Index / DMX Gobo Num Mask exist, and "Disk Rotation Speed" is a U-axis scroll that
	// cycles through wheel slots -- exactly the bug the user reported in v1.0.50). Instead, the
	// per-tick combined speed (CurrentGoboRotationSpeed + CurrentAnimationWheelSpeed) is
	// integrated into GoboAngle (deg, modulo 360) in Tick(), and that angle is composed onto the
	// SpotLight's relative rotation as a roll around its local +X emission axis in RefreshMotion.
	// The Epic beam canvas is PARENTED UNDER the SpotLight (TryBuildEpicBeam:1195), so its local
	// frame inherits the roll for free -- which rolls its mesh-local GoboUV sampling and spins
	// the in-cone gobo image. The SpotLight roll also rotates the cookie projection on the lit
	// pool (cookie UV is computed in the light's local space). Material rotation param is pinned
	// to 0 in ApplyCurrentGoboToEpicBeam / ApplyCurrentGoboToLightFn so no U-scroll happens.
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s SetFixtureGoboRotation: wheelIndex=%d speed=%.3f (signed[-1,1]) -> per-tick TEXTURE rotation in GoboRT (gobo=%.3f anim=%.3f combined=%.3f max=%.0fdeg/sec at speed=1) (no material param: Epic's 'DMX Gobo Disk Rotation Speed' is a U-scroll) beamMID=%p lightFnMID=%p GoboRT=%p"),
		*FixtureId, WheelIndex, Speed,
		CurrentGoboRotationSpeed, CurrentAnimationWheelSpeed,
		CurrentGoboRotationSpeed + CurrentAnimationWheelSpeed,
		RebusGoboMaxRotRateDegPerSec,
		EpicBeamMID.Get(), GoboLightFnMID.Get(), GoboRT.Get());
	// Push 0 to the material rotation param so the wheel-scroll is silenced even if a prior
	// material-MID push left it non-zero. Cookie MID gets the same via the tail call.
	ApplyCurrentGoboToEpicBeam();
	if (!FMath::IsNearlyZero(GoboRotationSpeed)) bAnimating = true;
}

void ARebusFixtureActor::ApplyAnimationWheelRotation(float Speed)
{
	const float Prev = CurrentAnimationWheelSpeed;
	CurrentAnimationWheelSpeed = FMath::Clamp(Speed, -1.f, 1.f);
	// v1.0.52: same routing as gobo rotation -- composed into the SpotLight roll in Tick +
	// RefreshMotion. Epic's stock materials don't model a separate animation-wheel disc, so the
	// animation speed STILL folds into the same component roll (combined = gobo + anim), which
	// matches Epic's reference fixture behaviour where animation and gobo share the same disc
	// in M_Beam_Master. Logged as a Warning the first time we receive a non-zero animation speed
	// so the user knows the cone+cookie won't show a "stacked" two-disc effect with a separate
	// animation disc -- they spin together at the combined rate.
	if (!FMath::IsNearlyZero(CurrentAnimationWheelSpeed) && FMath::IsNearlyZero(Prev))
	{
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("Fixture %s SetFixtureAnimationRotation: speed=%.3f -- Epic M_Beam_Master has no animation-wheel disc param, folding into the same texture rotation as gobo (cone+cookie will spin at gobo+anim combined rate in the GoboRT)."),
			*FixtureId, CurrentAnimationWheelSpeed);
	}
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s SetFixtureAnimationRotation: speed=%.3f (signed[-1,1]) -> per-tick TEXTURE rotation in GoboRT (gobo=%.3f anim=%.3f combined=%.3f max=%.0fdeg/sec at speed=1) beamMID=%p lightFnMID=%p GoboRT=%p"),
		*FixtureId, Speed,
		CurrentGoboRotationSpeed, CurrentAnimationWheelSpeed,
		CurrentGoboRotationSpeed + CurrentAnimationWheelSpeed,
		RebusGoboMaxRotRateDegPerSec,
		EpicBeamMID.Get(), GoboLightFnMID.Get(), GoboRT.Get());
	ApplyCurrentGoboToEpicBeam();
	if (!FMath::IsNearlyZero(CurrentAnimationWheelSpeed)) bAnimating = true;
}

void ARebusFixtureActor::ApplyPrism(int32 Facets, float RotationDeg)
{
	// Stored + logged; visual deferred on the reference plugin (§5.2).
	UE_LOG(LogRebusVisualiser, Verbose, TEXT("Fixture %s prism facets=%d rot=%.1f"), *FixtureId, Facets, RotationDeg);
}

void ARebusFixtureActor::ApplyBeamVolumetrics(float Intensity, bool bCastVolumetricShadow)
{
	// §8.4a re-point: this tunes the MESH beam intensity (a multiplier on BeamIntensity), since the
	// cone-mesh beam is the visible shaft. The same value is stored as FogScatteringIntensity so a
	// bMeshBeams=false toggle restores an equivalent fog beam. castVolumetricShadow (Phase 2) opts
	// the fixture into the native VSM fog volumetric-shadow hybrid for light-blocking truss gaps.
	const float Clamped = FMath::Clamp(Intensity, 0.f, 10.f);
	MeshBeamUserScale = Clamped;
	FogScatteringIntensity = Clamped;
	bWantsVolumetricShadow = bCastVolumetricShadow;

	// Grant a hero volumetric-shadow slot once, under the per-batch budget (volumetric shadows are
	// costly). Runtime-imported glTF trusses have no distance fields, so the must-have light-blocking
	// shadows come from native VSM fog on these hero beams (see RefreshBeamShadowMode), not a
	// material raymarch. Latched in bGrantedShadowHero so re-toggling doesn't re-consume the budget.
	if (bCastVolumetricShadow && !bGrantedShadowHero && ShadowFogBeamCount < RebusMaxShadowFogBeams)
	{
		bGrantedShadowHero = true;
		++ShadowFogBeamCount;
	}

	RefreshBeamShadowMode();
	RefreshBeamEmissive();

	// v1.0.47 diagnostic: explicit per-call log so the user can see in real time whether the wire
	// flag is reaching us, whether the hero budget granted this fixture, and what fog scatter the
	// SpotLight is actually emitting (the source of the truss-gap shafts inside the Epic cone).
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s SetFixtureBeamVolumetrics: intensity=%.2f castVolumetricShadow=%d -> bWantsVolumetricShadow=%d bGrantedShadowHero=%d (heroBudget=%d/%d, activeFogScatter=%.2f, Rebus.HeroShadowScatter=%.2f)"),
		*FixtureId, Clamped, bCastVolumetricShadow ? 1 : 0,
		bWantsVolumetricShadow ? 1 : 0, bGrantedShadowHero ? 1 : 0,
		ShadowFogBeamCount, RebusMaxShadowFogBeams,
		(bMeshBeamEnabled && bWantsVolumetricShadow && bGrantedShadowHero) ? GRebusHeroShadowScatter : 0.f,
		GRebusHeroShadowScatter);
}

void ARebusFixtureActor::ApplyGobo(int32 GoboIndex, bool bHasIndex, int32 WheelIndex, const FString& Wheel, float /*FadeSeconds*/)
{
	// v1.0.48: always-on log so the user can see SetFixtureGobo arrivals + which path they took.
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s SetFixtureGobo: bHasIndex=%d goboIndex=%d wheelIndex=%d wheelName='%s' inlineGobos=%d epicBeamMID=%s lightFnMID=%s"),
		*FixtureId, bHasIndex ? 1 : 0, GoboIndex, WheelIndex, *Wheel,
		InlineGobos.Gobos.Num(),
		EpicBeamMID ? TEXT("set") : TEXT("absent"),
		GoboLightFnMID ? TEXT("set") : TEXT("lazy"));

	// Discrete: switch the slot immediately (a wheel slot can't be half-selected, §11).
	if (!bHasIndex)
	{
		CurrentGoboIndex = INDEX_NONE;
		CurrentGoboWheelIndex = WheelIndex;
		CurrentGoboWheel = Wheel;
		// v1.0.49: route through ClearGoboToOpen so both the cone AND the cookie revert in lockstep.
		ClearGoboToOpen(TEXT("SetFixtureGobo(!bHasIndex)"));
		return;
	}
	// Remember the full selection so a RegisterFixtureGobos re-push can re-apply it.
	CurrentGoboIndex = GoboIndex;
	CurrentGoboWheelIndex = WheelIndex;
	CurrentGoboWheel = Wheel;
	AssignGobo(GoboIndex, WheelIndex, Wheel);
}

void ARebusFixtureActor::SetInlineGobos(const FRebusInlineGobos& InInlineGobos)
{
	InlineGobos = InInlineGobos;
	if (InlineGobos.Gobos.Num() > 0) bHasGobo = true;
	// Refresh the live selection so the newly-pushed image appears without a reselect.
	if (CurrentGoboIndex != INDEX_NONE)
	{
		AssignGobo(CurrentGoboIndex, CurrentGoboWheelIndex, CurrentGoboWheel);
	}
}

// ---- IES / gobo fetch -----------------------------------------------------------------

const FRebusInlineIesProfile* ARebusFixtureActor::SelectInlineIes(int32 ZoomDmx) const
{
	// Pick the inline profile nearest the requested zoomDmx. A single "default" profile (or any
	// lone entry) is therefore always selected; a per-zoom set picks the closest step. This
	// mirrors the URL iesProfiles[] zoom selection so both paths behave the same.
	if (InlineIes.Profiles.Num() == 0) return nullptr;
	const FRebusInlineIesProfile* Best = &InlineIes.Profiles[0];
	for (const FRebusInlineIesProfile& P : InlineIes.Profiles)
	{
		if (FMath::Abs(P.ZoomDmx - ZoomDmx) < FMath::Abs(Best->ZoomDmx - ZoomDmx)) Best = &P;
	}
	return Best;
}

void ARebusFixtureActor::SelectIesForZoom()
{
	if (!SpotLight) return;

	// Map the current zoom half-angle back to a 0..255 DMX-ish key for zoom selection. We
	// approximate by linearly mapping the zoom range onto 0..255; this same key drives both the
	// inline iesText profiles and the URL iesProfiles[] lookup.
	int32 ZoomDmx = 128;
	if (Profile.Zoom.bValid && Profile.Zoom.MaxDeg > Profile.Zoom.MinDeg)
	{
		const double FullAngle = ZoomDeg.Current * 2.0;
		const double T = (FullAngle - Profile.Zoom.MinDeg) / (Profile.Zoom.MaxDeg - Profile.Zoom.MinDeg);
		ZoomDmx = FMath::Clamp((int32)FMath::RoundToInt(T * 255.0), 0, 255);
	}

	// 1) Prefer an inline iesText profile pushed via RegisterFixtureIes (no REST fetch). Build
	//    the UTextureLightProfile straight from the cached .ies bytes (same RebusIes path the
	//    URL fetch uses). On a build failure we fall through to the URL path below.
	if (const FRebusInlineIesProfile* Inline = SelectInlineIes(ZoomDmx))
	{
		if (bActiveIesInline && CurrentIesZoomDmx == Inline->ZoomDmx && ActiveIesProfile)
		{
			return; // this inline entry is already loaded
		}
		if (UTextureLightProfile* Prof = RebusIes::BuildLightProfile(this, Inline->Bytes))
		{
			ActiveIesProfile = Prof;
			SpotLight->SetIESTexture(Prof);
			// Keep the portal's brightness authority (§8.2 step 4).
			SpotLight->bUseIESBrightness = false;
			SpotLight->IESBrightnessScale = 1.f;
			SpotLight->MarkRenderStateDirty();
			bActiveIesInline = true;
			CurrentIesZoomDmx = Inline->ZoomDmx;
			return;
		}
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("Fixture %s: inline IES profile '%s' failed to build; falling back to URL/cone."),
			*FixtureId, *Inline->ProfileId);
	}

	// 2) Fall back to a signed iesUrl/iesProfileUrl fetch (REST relative redirect or absolute
	//    signed GCS URL -- opaque to us). Nearest entry by zoomDmx (§8.2 zoom selection).
	auto PickProfileUrl = [&](FString& OutUrl, int32& OutKey) -> bool
	{
		if (Profile.IesProfiles.Num() == 0)
		{
			if (!Profile.IesProfileUrl.IsEmpty()) { OutUrl = Profile.IesProfileUrl; OutKey = -1; return true; }
			return false;
		}
		const FRebusIesProfileRef* Best = &Profile.IesProfiles[0];
		for (const FRebusIesProfileRef& Ref : Profile.IesProfiles)
		{
			if (FMath::Abs(Ref.ZoomDmx - ZoomDmx) < FMath::Abs(Best->ZoomDmx - ZoomDmx)) Best = &Ref;
		}
		OutUrl = Best->IesUrl;
		OutKey = Best->ZoomDmx;
		return !OutUrl.IsEmpty();
	};

	FString Url; int32 Key = -1;
	if (!PickProfileUrl(Url, Key))
	{
		// 3) No IES at all -> clear and keep the synthesized cone (§8.2 step 5).
		SpotLight->SetIESTexture(nullptr);
		ActiveIesProfile = nullptr;
		CurrentIesZoomDmx = -1;
		bActiveIesInline = false;
		return;
	}
	if (!bActiveIesInline && Key == CurrentIesZoomDmx && ActiveIesProfile)
	{
		return; // already loaded this URL entry
	}
	CurrentIesZoomDmx = Key;
	bActiveIesInline = false;
	FetchAndAssignIes(Url);
}

void ARebusFixtureActor::FetchAndAssignIes(const FString& IesUrl)
{
	if (!RestClient.IsValid() || IesUrl.IsEmpty()) return;

	TWeakObjectPtr<ARebusFixtureActor> WeakThis(this);
	RestClient->FetchBytes(IesUrl, FRebusBytesFetched::CreateLambda(
		[WeakThis](bool bOk, const TArray<uint8>& Bytes)
		{
			ARebusFixtureActor* Self = WeakThis.Get();
			if (!Self || !bOk || !Self->SpotLight) return;
			UTextureLightProfile* Prof = RebusIes::BuildLightProfile(Self, Bytes);
			if (!Prof) return;
			Self->ActiveIesProfile = Prof;
			Self->SpotLight->SetIESTexture(Prof);
			// Keep the portal's brightness authority (§8.2 step 4).
			Self->SpotLight->bUseIESBrightness = false;
			Self->SpotLight->IESBrightnessScale = 1.f;
			Self->SpotLight->MarkRenderStateDirty();
		}));
}

int32 ARebusFixtureActor::ResolveGoboWheelIndex(int32 WheelIndex, const FString& WheelName) const
{
	if (InlineGobos.Gobos.Num() == 0) return INDEX_NONE;

	// An explicit wheelIndex is the contract's primary key (0-based into the full wheels[]): trust
	// it directly. SelectInlineGobo will warn + fall through if no (wheelIndex, slot) entry exists.
	if (WheelIndex != INDEX_NONE) return WheelIndex;

	// Absent wheelIndex -> the FIRST gobo-kind wheel = smallest wheelIndex among inline entries
	// tagged kind=="gobo" (NOT insertion order, so a colour/effect wheel preceding the gobo wheel
	// can't mis-resolve). Falls back to the smallest wheelIndex of any entry, else INDEX_NONE.
	int32 Best = INDEX_NONE;
	for (const FRebusInlineGobo& G : InlineGobos.Gobos)
	{
		if (G.WheelIndex == INDEX_NONE) continue;
		if (G.WheelKind.Equals(TEXT("gobo"), ESearchCase::IgnoreCase))
		{
			Best = (Best == INDEX_NONE) ? G.WheelIndex : FMath::Min(Best, G.WheelIndex);
		}
	}
	if (Best != INDEX_NONE) return Best;

	for (const FRebusInlineGobo& G : InlineGobos.Gobos)
	{
		if (G.WheelIndex == INDEX_NONE) continue;
		Best = (Best == INDEX_NONE) ? G.WheelIndex : FMath::Min(Best, G.WheelIndex);
	}
	return Best; // INDEX_NONE when no entry carries an explicit wheelIndex (legacy push)
}

const FRebusInlineGobo* ARebusFixtureActor::SelectInlineGobo(int32 Slot, int32 WheelIndex, const FString& WheelName) const
{
	if (InlineGobos.Gobos.Num() == 0) return nullptr;

	// 1) Primary key (wheelIndex, slot): the contract's direct lookup.
	const int32 TargetWheelIndex = ResolveGoboWheelIndex(WheelIndex, WheelName);
	if (TargetWheelIndex != INDEX_NONE)
	{
		for (const FRebusInlineGobo& G : InlineGobos.Gobos)
		{
			if (G.Slot == Slot && G.WheelIndex == TargetWheelIndex) return &G;
		}
		if (WheelIndex != INDEX_NONE)
		{
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("Fixture %s gobo: no inline image for (wheelIndex=%d, slot=%d); trying name/any-slot."),
				*FixtureId, WheelIndex, Slot);
		}
	}

	// 2) Secondary: match by wheel NAME (back-compat for legacy entries without a wheelIndex).
	if (!WheelName.IsEmpty())
	{
		for (const FRebusInlineGobo& G : InlineGobos.Gobos)
		{
			if (G.Slot == Slot && G.Wheel.Equals(WheelName, ESearchCase::IgnoreCase)) return &G;
		}
	}

	// 3) No usable selector: accept any wheel's matching slot.
	if (WheelIndex == INDEX_NONE && WheelName.IsEmpty())
	{
		for (const FRebusInlineGobo& G : InlineGobos.Gobos)
		{
			if (G.Slot == Slot) return &G;
		}
	}
	return nullptr;
}

bool ARebusFixtureActor::ApplyGoboTextureFromBytes(const TArray<uint8>& Bytes)
{
	if (Bytes.Num() == 0)
	{
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("Fixture %s gobo decode: 0 bytes supplied; nothing to apply."), *FixtureId);
		return false;
	}

	// Decode the bytes to a transient UTexture2D (auto-detects PNG/JPEG/etc). v1.0.48: the result
	// is the user-facing gobo image -- we route it INTO Epic's M_Beam_Master MID's "DMX Gobo Disk
	// Frosted" (visible in the cone) rather than the legacy SpotLight light-function path
	// (GoboMID was declared but never instantiated, so that path silently no-oped from day one).
	UTexture2D* Tex = FImageUtils::ImportBufferAsTexture2D(Bytes);
	if (!Tex)
	{
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("Fixture %s gobo decode FAILED: %d bytes did not parse as a known image (PNG/JPEG/etc)."),
			*FixtureId, Bytes.Num());
		return false;
	}

	CurrentGoboTexture = Tex;
	bGoboActive = true; // v1.0.49: forces SpotLight->CastShadows on so the cookie projects.
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s gobo decode OK: %d bytes -> texture(%dx%d) -> epicBeamMID=%s lightFnMID=%s"),
		*FixtureId, Bytes.Num(), Tex->GetSizeX(), Tex->GetSizeY(),
		EpicBeamMID ? TEXT("set") : TEXT("absent"),
		GoboLightFnMID ? TEXT("set") : TEXT("lazy"));

	ApplyCurrentGoboToEpicBeam(); // tail-calls ApplyCurrentGoboToLightFn (cone + cookie).
	RefreshBeamShadowMode();      // enables CastShadows now that bGoboActive is true.
	return true;
}

void ARebusFixtureActor::EnsureGoboRT()
{
	// v1.0.53: lazy per-fixture RT used to redraw the source gobo texture rotated by GoboAngle.
	// 512x512 is a balance: large enough to look crisp through Epic's MF_DMXGobo sampling, small
	// enough to UpdateResource each tick without measurable cost. ClearColor = transparent so a
	// rotated quad doesn't smear into the previous frame (UCanvasRenderTarget2D::UpdateResource
	// clears to ClearColor BEFORE firing OnCanvasRenderTargetUpdate when
	// bShouldClearRenderTargetOnReceiveUpdate is true, which is the default). Bind our UFUNCTION
	// to the OnCanvasRenderTargetUpdate dynamic delegate; UE marshals the canvas-draw to the
	// render thread internally, so the bound function runs on the game thread with a UCanvas
	// proxy and writes are safe. Immediately UpdateResource() once so the first material param
	// push (in the caller) doesn't bind a blank RT.
	if (GoboRT) return;

	GoboRT = UCanvasRenderTarget2D::CreateCanvasRenderTarget2D(this, UCanvasRenderTarget2D::StaticClass(), 512, 512);
	if (!GoboRT)
	{
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("Fixture %s gobo RT: CreateCanvasRenderTarget2D returned null; falling back to direct CurrentGoboTexture push (no in-plane rotation)."),
			*FixtureId);
		return;
	}
	GoboRT->ClearColor = FLinearColor::Transparent;
	GoboRT->OnCanvasRenderTargetUpdate.AddDynamic(this, &ARebusFixtureActor::OnGoboRTUpdate);
	GoboRT->UpdateResource(); // first redraw so the param push isn't blank
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s gobo RT allocated %dx%d at %p (src=%s GoboAngle=%.1fdeg)"),
		*FixtureId, GoboRT->SizeX, GoboRT->SizeY, GoboRT.Get(),
		CurrentGoboTexture ? *CurrentGoboTexture->GetName() : TEXT("<null>"),
		GoboAngle);
}

void ARebusFixtureActor::OnGoboRTUpdate(UCanvas* Canvas, int32 Width, int32 Height)
{
	// v1.0.53: bound to GoboRT->OnCanvasRenderTargetUpdate. The RT has already been cleared to
	// transparent by UpdateResource (bShouldClearRenderTargetOnReceiveUpdate default true), so
	// we just draw the source gobo texture once, centered and square (largest square that fits
	// the RT), rotated by GoboAngle around its centre. The cookie / cone sample the resulting
	// translucent RT, so the projected pattern spins in plane.
	//
	// Sign convention: portal sends +speed = clockwise looking DOWN the beam (audience-side CCW
	// because the perspectives are mirrored across the projection plane). UCanvas::K2_DrawTexture
	// applies the Rotation as a screen-space yaw via FRotator(0, Rotation, 0); in UE's screen
	// space (X right, Y down), a +yaw rotates the quad clockwise on screen. The cookie projects
	// "with" the texture orientation onto the floor, so the floor pattern rotates clockwise as
	// the texture rotates clockwise (looking down the beam). That matches the portal contract.
	// If a future test shows the pattern rotating opposite, negate `Rotation` below -- the rest
	// of the pipeline (GoboAngle integration, wire-signed speed) stays identical.
	if (!Canvas || !CurrentGoboTexture) return;
	const float Side = (float)FMath::Min(Width, Height);
	if (Side <= 0.f) return;
	const FVector2D ScreenPos(((float)Width - Side) * 0.5f, ((float)Height - Side) * 0.5f);
	const FVector2D ScreenSize(Side, Side);
	const FVector2D CoordPos(0.f, 0.f);
	const FVector2D CoordSize(1.f, 1.f);
	Canvas->K2_DrawTexture(
		CurrentGoboTexture.Get(),
		ScreenPos, ScreenSize,
		CoordPos, CoordSize,
		FLinearColor::White,
		BLEND_Translucent,
		GoboAngle,
		FVector2D(0.5f, 0.5f));
}

void ARebusFixtureActor::ApplyCurrentGoboToEpicBeam()
{
	if (EpicBeamMID)
	{
		// v1.0.53: when a real gobo is loaded, push the per-fixture RENDER TARGET (drawn rotated
		// every tick in OnGoboRTUpdate) instead of CurrentGoboTexture directly. The RT IS-A
		// UTexture (UTextureRenderTarget2D base), so Epic's "DMX Gobo Disk Frosted" param accepts
		// it. EnsureGoboRT lazily allocates + binds the OnCanvasRenderTargetUpdate callback and
		// does an immediate redraw so the first param push isn't blank. On "clear"
		// (CurrentGoboTexture == null) revert to Epic's MI default (T_GoboDisk_01_Frosted, the
		// open disc that lets the beam through unmasked) cached at TryBuildEpicBeam time.
		UTexture* TexToPush = nullptr;
		if (CurrentGoboTexture)
		{
			EnsureGoboRT();
			// v1.0.53: even when the RT was already allocated from a previous gobo, the source
			// texture may have just changed (new SetFixtureGobo); kick a redraw so the RT
			// contains the NEW gobo BEFORE we push it to the material -- otherwise the cone +
			// cookie would project the previous gobo for one frame before the Tick spin block
			// would refresh it.
			if (GoboRT) GoboRT->UpdateResource();
			TexToPush = GoboRT ? static_cast<UTexture*>(GoboRT.Get()) : static_cast<UTexture*>(CurrentGoboTexture.Get());
		}
		else
		{
			TexToPush = EpicBeamDefaultGoboTex.Get();
		}
		if (TexToPush)
		{
			EpicBeamMID->SetTextureParameterValue(TEXT("DMX Gobo Disk Frosted"), TexToPush);
		}
		UE_LOG(LogRebusVisualiser, Verbose,
			TEXT("Fixture %s gobo TEX param: beamMID=%s lightFnMID=lazy src=%s push=%s (RT=%s)"),
			*FixtureId, EpicBeamMID ? TEXT("set") : TEXT("absent"),
			CurrentGoboTexture ? *CurrentGoboTexture->GetName() : TEXT("<default>"),
			TexToPush ? *TexToPush->GetName() : TEXT("<none>"),
			GoboRT ? TEXT("ready") : TEXT("none"));
		EpicBeamMID->SetScalarParameterValue(TEXT("DMX Gobo Num Mask"), 1.f);
		EpicBeamMID->SetScalarParameterValue(TEXT("DMX Gobo Index"), 0.f);
		// v1.0.52: pin DMX Gobo Disk Rotation Speed to 0. Per the HLSL inside M_Beam_Master:
		//   GoboUV.x = GoboUV.x + (Time * GoboScrollingSpeed)
		//   GoboUV.x = GoboUV.x / NumGobos
		// "Disk Rotation Speed" is a U-axis SCROLL that cycles through the wheel slots (with
		// NumMask=1 it slides the single gobo image horizontally / wraps, which the user reported
		// as "rotates through the various gobos" in v1.0.50). Epic exposes NO image-rotation
		// param in M_Beam_Master / MF_DMXGobo / M_Light_Master (verified v1.0.52 by enumerating
		// the uasset string tables). To actually SPIN the selected gobo image in place, v1.0.52
		// composes a per-tick component-axis roll on the SpotLight (rotates the cookie projection
		// on the floor) and on the Epic beam canvas mesh (its GoboUV samples in mesh-local
		// transverse coords, so rolling the mesh around its local +Z emission axis rotates the
		// in-cone gobo). See Tick + RefreshMotion + DriveEpicBeamFromSpotLight. The Disk Rotation
		// Speed param is held at 0 so no U-scroll happens regardless of gobo or animation speed.
		EpicBeamMID->SetScalarParameterValue(TEXT("DMX Gobo Disk Rotation Speed"), 0.f);

		UE_LOG(LogRebusVisualiser, Verbose,
			TEXT("Fixture %s epic-beam gobo: tex=%s default=%s rotation via component-roll (gobo=%.2f anim=%.2f combined=%.2f) -- material wheel-scroll pinned to 0"),
			*FixtureId,
			CurrentGoboTexture ? *CurrentGoboTexture->GetName() : TEXT("<default>"),
			EpicBeamDefaultGoboTex ? *EpicBeamDefaultGoboTex->GetName() : TEXT("(none)"),
			CurrentGoboRotationSpeed, CurrentAnimationWheelSpeed,
			CurrentGoboRotationSpeed + CurrentAnimationWheelSpeed);
	}

	// v1.0.49: same texture/rotation also drives the SpotLight cookie via M_Light_Master.
	// Kept as a tail-call so every existing caller (UpdateEpicBeamParams, ApplyGoboTextureFromBytes,
	// ClearGoboToOpen) updates both the cone AND the lit-pool gobo in one shot.
	ApplyCurrentGoboToLightFn();
}

void ARebusFixtureActor::ApplyCurrentGoboToLightFn()
{
	if (!SpotLight) return;

	// Lazy MID creation: load Epic's MI_Light (parent = M_Light_Master, MD_LightFunction, samples
	// MF_DMXGobo) on first use. Same atlas convention as MI_Beam (DMX Gobo Disk Frosted + Num Mask
	// + Index + Rotation Speed) so the cone and the cookie share one source of truth. If Epic's
	// DMXFixtures content is missing, we log once and leave the light function unchanged.
	if (!GoboLightFnMID)
	{
		UMaterialInterface* LightFnSrc = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/DMXFixtures/LightFixtures/DMX_Materials/MI_Light.MI_Light"));
		if (!LightFnSrc)
		{
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("Fixture %s gobo cookie: MI_Light not found at /DMXFixtures/LightFixtures/DMX_Materials/MI_Light -- ensure the DMX Fixtures plugin content is installed. Lit pool will not show the gobo."),
				*FixtureId);
			return;
		}
		GoboLightFnMID = UMaterialInstanceDynamic::Create(LightFnSrc, this);
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Fixture %s gobo cookie: MID'd %s for SpotLight->LightFunctionMaterial."),
			*FixtureId, *LightFnSrc->GetPathName());
	}

	if (!GoboLightFnMID)
	{
		return;
	}

	if (CurrentGoboTexture)
	{
		// v1.0.50: MegaLights (enabled per-fixture via bAllowMegaLights=1 in BuildSpotLight) only
		// renders light functions through LightFunctionAtlas (gated by r.MegaLights.LightFunctions
		// AND atlas-compatibility of the material). M_Light_Master's MF_DMXGobo sampling pattern
		// is NOT atlas-compatible, so the cookie silently never projects. Opt this specific light
		// OUT of MegaLights while a gobo is active so the standard deferred path renders the
		// LightFunctionMaterial directly. Restored to MegaLights on clear (ClearGoboToOpen).
		// Cost: this light loses MegaLights' clustering perf while a gobo is up -- acceptable;
		// fixtures with gobos are typically hero lights.
		const bool bPrevMegaLights = SpotLight->bAllowMegaLights != 0;
		SpotLight->bAllowMegaLights = 0;
		// Push the same single-cell atlas params as the cone. The MF_DMXGobo inside M_Light_Master
		// reads the texture identically; "Num Mask = 1, Index = 0" sweeps the entire texture.
		// v1.0.52: as on EpicBeamMID, pin "DMX Gobo Disk Rotation Speed" to 0 -- it's a U-scroll
		// not an image rotation.
		// v1.0.53: push the per-fixture GoboRT (drawn rotated every tick in OnGoboRTUpdate)
		// instead of CurrentGoboTexture directly, so the cookie spins in plane around the
		// projection's out-of-screen axis. EnsureGoboRT was already called by
		// ApplyCurrentGoboToEpicBeam (this is its tail call), so GoboRT is ready.
		UTexture* CookieTex = GoboRT ? static_cast<UTexture*>(GoboRT.Get()) : static_cast<UTexture*>(CurrentGoboTexture.Get());
		GoboLightFnMID->SetTextureParameterValue(TEXT("DMX Gobo Disk Frosted"), CookieTex);
		GoboLightFnMID->SetScalarParameterValue(TEXT("DMX Gobo Num Mask"), 1.f);
		GoboLightFnMID->SetScalarParameterValue(TEXT("DMX Gobo Index"), 0.f);
		GoboLightFnMID->SetScalarParameterValue(TEXT("DMX Gobo Disk Rotation Speed"), 0.f);
		UE_LOG(LogRebusVisualiser, Verbose,
			TEXT("Fixture %s gobo TEX param: beamMID=set lightFnMID=set src=%s push=%s (RT=%s)"),
			*FixtureId,
			CurrentGoboTexture ? *CurrentGoboTexture->GetName() : TEXT("<none>"),
			CookieTex ? *CookieTex->GetName() : TEXT("<none>"),
			GoboRT ? TEXT("ready") : TEXT("none"));
		SpotLight->SetLightFunctionMaterial(GoboLightFnMID);
		// v1.0.51: bAllowMegaLights is read by FLightSceneInfo at proxy-creation time
		// (LightSceneInfo.cpp:55 -> Proxy->AllowMegaLights()), so the value MUST be present on a
		// freshly-created proxy to take effect. MarkRenderStateDirty alone scheduled a deferred
		// recreate that proved unreliable in v1.0.50 (the user reported no cookie). A full
		// ReregisterComponent() on a TRANSITION (not every gobo update) guarantees the proxy is
		// rebuilt with bAllowMegaLights=0. Cost: brief one-frame blackout on the toggle -- fine.
		if (bPrevMegaLights)
		{
			SpotLight->ReregisterComponent();
		}
		else
		{
			SpotLight->MarkRenderStateDirty();
		}

		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Fixture %s gobo cookie: lightFn=MI_Light tex=%s(%dx%d) gobo=%.2f anim=%.2f combined=%.2f (rotation via SpotLight roll, material wheel-scroll pinned to 0) castShadows=%d bAllowMegaLights=%d (was %d, %s for light function)"),
			*FixtureId, *CurrentGoboTexture->GetName(),
			CurrentGoboTexture->GetSizeX(), CurrentGoboTexture->GetSizeY(),
			CurrentGoboRotationSpeed, CurrentAnimationWheelSpeed,
			CurrentGoboRotationSpeed + CurrentAnimationWheelSpeed,
			SpotLight->CastShadows ? 1 : 0,
			SpotLight->bAllowMegaLights ? 1 : 0, bPrevMegaLights ? 1 : 0,
			bPrevMegaLights ? TEXT("REREGISTERED") : TEXT("MarkRenderStateDirty"));

		// v1.0.51: next-tick verification log so we can SEE what the runtime proxy ended up with
		// after the reregister/markdirty. The component value above is the GAME-thread value; this
		// reads it again one tick later to confirm it survived render-thread setup.
		TWeakObjectPtr<const ARebusFixtureActor> WeakSelf(this);
		GetWorld()->GetTimerManager().SetTimerForNextTick(
			[WeakSelf]()
			{
				const ARebusFixtureActor* Self = WeakSelf.Get();
				if (!Self || !Self->SpotLight) return;
				const UMaterialInterface* LightFnMat = Self->SpotLight->LightFunctionMaterial;
				const UTextureLightProfile* Ies = Self->SpotLight->IESTexture;
				UE_LOG(LogRebusVisualiser, Log,
					TEXT("Fixture %s cookie NEXT-TICK verify: bAllowMegaLights=%d castShadows=%d castVolumetricShadow=%d intensity=%.1f units=%d attenRadius=%.0f outerCone=%.1f LightFn=%s IES=%s"),
					*Self->FixtureId,
					Self->SpotLight->bAllowMegaLights ? 1 : 0,
					Self->SpotLight->CastShadows ? 1 : 0,
					Self->SpotLight->bCastVolumetricShadow ? 1 : 0,
					Self->SpotLight->Intensity,
					(int32)Self->SpotLight->IntensityUnits,
					Self->SpotLight->AttenuationRadius,
					Self->SpotLight->OuterConeAngle,
					LightFnMat ? *LightFnMat->GetPathName() : TEXT("nullptr"),
					Ies ? *Ies->GetName() : TEXT("nullptr"));
			});
	}
	else
	{
		// Open / clear: drop the light function so the lit pool shows no gobo. (We don't push the
		// MI default here because for a LightFunction material the default would project a frosted
		// disc onto the entire lit cone, dimming it noticeably -- null is the true "no gobo".)
		// v1.0.50: also re-enable MegaLights so this light goes back to the perf-optimised path.
		const bool bPrevMegaLights = SpotLight->bAllowMegaLights != 0;
		SpotLight->bAllowMegaLights = 1;
		SpotLight->SetLightFunctionMaterial(nullptr);
		// v1.0.51: ReregisterComponent on the OFF->ON transition for the same proxy-baked reason.
		if (!bPrevMegaLights)
		{
			SpotLight->ReregisterComponent();
		}
		else
		{
			SpotLight->MarkRenderStateDirty();
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Fixture %s gobo cookie: lightFn=nullptr (Open / clear). bAllowMegaLights=%d (was %d, %s to MegaLights path)."),
			*FixtureId, SpotLight->bAllowMegaLights ? 1 : 0, bPrevMegaLights ? 1 : 0,
			bPrevMegaLights ? TEXT("MarkRenderStateDirty") : TEXT("REREGISTERED restoring"));
	}
}

void ARebusFixtureActor::ClearGoboToOpen(const TCHAR* Reason)
{
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s gobo OPEN: clearing cone+cookie (reason=%s)."),
		*FixtureId, Reason ? Reason : TEXT("unspecified"));

	CurrentGoboTexture = nullptr;
	bGoboActive = false;
	// v1.0.53: clear the gobo RT to transparent so the next non-Open assignment doesn't briefly
	// flash the previous gobo. We keep the RT allocated (cheap to keep, expensive to recreate
	// every gobo change) -- just blank it. OnGoboRTUpdate early-outs on CurrentGoboTexture==null
	// (just-set above), so the UpdateResource here only fires the default clear-to-transparent.
	if (GoboRT)
	{
		GoboRT->UpdateResource();
	}
	// Push the cleared state into BOTH the cone (reverts EpicBeamMID to its MI default) and the
	// cookie (nulls SpotLight->LightFunctionMaterial). Then reassert CastShadows so the now-cleared
	// bGoboActive removes any gobo-driven shadow override on non-hero beams.
	ApplyCurrentGoboToEpicBeam();
	RefreshBeamShadowMode();
}

bool ARebusFixtureActor::IsOpenSlotName(const FString& Name)
{
	const FString Trimmed = Name.TrimStartAndEnd();
	if (Trimmed.IsEmpty()) return false;
	// Common portal/GDTF "no gobo" slot names. Case-insensitive, exact-match (a slot literally
	// called e.g. "Open Star" is NOT Open). "0" and "Off" are belt-and-braces for portals that
	// use a numeric/legacy convention.
	static const TCHAR* const Names[] = {
		TEXT("Open"), TEXT("None"), TEXT("Empty"), TEXT("Clear"),
		TEXT("No Gobo"), TEXT("NoGobo"), TEXT("Open Hole"), TEXT("OpenHole"),
		TEXT("Off"), TEXT("0")
	};
	for (const TCHAR* Candidate : Names)
	{
		if (Trimmed.Equals(Candidate, ESearchCase::IgnoreCase)) return true;
	}
	return false;
}

void ARebusFixtureActor::AssignGobo(int32 GoboIndex, int32 WheelIndex, const FString& WheelName)
{
	if (!SpotLight) return;

	// 1) Prefer an inline base64 image pushed via RegisterFixtureGobos (no REST fetch). The
	//    wheel is resolved from the selectors (wheelIndex > wheel name > first gobo wheel).
	if (const FRebusInlineGobo* Inline = SelectInlineGobo(GoboIndex, WheelIndex, WheelName))
	{
		// v1.0.49: explicit OPEN slot wins. The portal flags it via slotName/Name; finalize keeps
		// the entry with bIsOpen=true even though it has no bytes/url, so a "clear gobo" arrives
		// here as a real match rather than a "no inline -> fallback" miss.
		if (Inline->bIsOpen || IsOpenSlotName(Inline->SlotName) || IsOpenSlotName(Inline->Name))
		{
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Fixture %s gobo OPEN slot detected (wheelIndex=%d slot=%d slotName='%s' name='%s') -> applying clear."),
				*FixtureId, Inline->WheelIndex, Inline->Slot, *Inline->SlotName, *Inline->Name);
			ClearGoboToOpen(TEXT("inline Open slot"));
			return;
		}
		if (Inline->Bytes.Num() > 0 && ApplyGoboTextureFromBytes(Inline->Bytes))
		{
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Fixture %s AssignGobo: source=inline reqWheelIdx=%d -> wheelIndex=%d wheel='%s' slot=%d bytes=%d"),
				*FixtureId, WheelIndex, Inline->WheelIndex, *Inline->Wheel, Inline->Slot, Inline->Bytes.Num());
			return;
		}
		// 2) Inline entry carries only a signed url fallback (or its bytes failed to decode).
		if (!Inline->ImageUrl.IsEmpty())
		{
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Fixture %s AssignGobo: source=inline-url (bytes empty or decode failed) reqWheelIdx=%d -> wheelIndex=%d wheel='%s' slot=%d url=%s"),
				*FixtureId, WheelIndex, Inline->WheelIndex, *Inline->Wheel, Inline->Slot, *Inline->ImageUrl);
			FetchAndAssignGoboFromUrl(Inline->ImageUrl);
			return;
		}
		// 3) Inline entry matched but has neither payload nor url and isn't tagged Open. Treat as
		// "empty slot" (same effect as Open) so the cone+cookie don't keep the last image stuck.
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Fixture %s AssignGobo: inline entry empty (no bytes, no url, no Open tag) for wheelIndex=%d slot=%d slotName='%s' -> treating as Open."),
			*FixtureId, Inline->WheelIndex, Inline->Slot, *Inline->SlotName);
		ClearGoboToOpen(TEXT("inline empty slot"));
		return;
	}

	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s AssignGobo: no inline match (reqWheelIdx=%d wheel='%s' slot=%d, inlineCount=%d) -> falling back to profile URL."),
		*FixtureId, WheelIndex, *WheelName, GoboIndex, InlineGobos.Gobos.Num());

	// 3) Fall back to the profile wheel's signed imageUrl (existing REST path; legacy single
	//    gobo wheel -- the multi-wheel selectors apply to the inline cache above).
	FetchAndAssignGobo(GoboIndex);
}

void ARebusFixtureActor::FetchAndAssignGobo(int32 GoboIndex)
{
	if (!SpotLight) return;

	const int32 WheelIdx = FindFirstGoboWheel(Profile);
	if (WheelIdx == INDEX_NONE || !Profile.Wheels.IsValidIndex(WheelIdx))
	{
		// v1.0.49: also clear the live gobo here. The fixture has no gobo wheel at all, so any
		// previously-applied texture must be wiped from both the cone and the cookie.
		UE_LOG(LogRebusVisualiser, Log, TEXT("Fixture %s gobo: no inline image and no profile wheel (source=none) -> clearing."), *FixtureId);
		ClearGoboToOpen(TEXT("no profile wheel"));
		return;
	}
	const FRebusWheel& Wheel = Profile.Wheels[WheelIdx];
	if (!Wheel.Slots.IsValidIndex(GoboIndex) || Wheel.Slots[GoboIndex].ImageUrl.IsEmpty())
	{
		// v1.0.49: pre-v1.0.48 nulled only SpotLight->SetLightFunctionMaterial (which was the
		// stub light-fn path); the Epic cone kept the last gobo. Now route through the proper
		// clear so cone + cookie revert together.
		ClearGoboToOpen(TEXT("profile slot has no media (Open)"));
		return;
	}
	FetchAndAssignGoboFromUrl(Wheel.Slots[GoboIndex].ImageUrl);
}

void ARebusFixtureActor::FetchAndAssignGoboFromUrl(const FString& ImageUrl)
{
	if (!RestClient.IsValid() || !SpotLight || ImageUrl.IsEmpty()) return;

	TWeakObjectPtr<ARebusFixtureActor> WeakThis(this);
	RestClient->FetchBytes(ImageUrl, FRebusBytesFetched::CreateLambda(
		[WeakThis](bool bOk, const TArray<uint8>& Bytes)
		{
			ARebusFixtureActor* Self = WeakThis.Get();
			if (!Self || !bOk || !Self->SpotLight) return;
			const bool bApplied = Self->ApplyGoboTextureFromBytes(Bytes);
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Fixture %s gobo URL fetched: %d bytes, applied=%d"),
				*Self->FixtureId, Bytes.Num(), bApplied);
		}));
}

int32 ARebusFixtureActor::FindFirstGoboWheel(const FRebusFixtureProfile& InProfile)
{
	for (int32 i = 0; i < InProfile.Wheels.Num(); ++i)
	{
		if (InProfile.Wheels[i].Kind.Equals(TEXT("gobo"), ESearchCase::IgnoreCase))
		{
			return i;
		}
	}
	return INDEX_NONE;
}

// ---- Selection ------------------------------------------------------------------------

void ARebusFixtureActor::SetSelected(bool bSelected, bool bPrimary)
{
	const int32 Stencil = bPrimary ? RebusSelectionStencil : RebusSelectionStencilSecondary;
	for (UProceduralMeshComponent* PMC : MeshComponents)
	{
		if (!PMC) continue;
		PMC->SetRenderCustomDepth(bSelected);
		PMC->SetCustomDepthStencilValue(bSelected ? Stencil : 0);
	}
}

// ---- Tick (fades + strobe + gobo spin) ------------------------------------------------

void ARebusFixtureActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	bool bStillAnimating = false;

	const bool bMotionAnim = PanDeg.Tick(DeltaSeconds) | TiltDeg.Tick(DeltaSeconds);
	const bool bIntensityAnim = Dimmer.Tick(DeltaSeconds) | ColorR.Tick(DeltaSeconds) | ColorG.Tick(DeltaSeconds) | ColorB.Tick(DeltaSeconds);
	const bool bConeAnim = ZoomDeg.Tick(DeltaSeconds) | Iris.Tick(DeltaSeconds) | Frost.Tick(DeltaSeconds);
	Focus.Tick(DeltaSeconds);

	// v1.0.53: integrate the gobo image rotation into a per-tick angle that is consumed by the
	// gobo RT redraw (OnGoboRTUpdate draws CurrentGoboTexture into GoboRT rotated by GoboAngle).
	// Epic's stock materials expose no image-rotation parameter (verified v1.0.52: only DMX Gobo
	// Disk Frosted/Index/Num Mask + the U-scroll "Disk Rotation Speed"), and v1.0.52's SpotLight
	// component roll didn't satisfy the user ("rotating around x instead of z"). v1.0.53 spins
	// the TEXTURE itself in a transparent-clear UCanvasRenderTarget2D, which is bound as the
	// "DMX Gobo Disk Frosted" texture param on both cone + cookie MIDs, so the projected pattern
	// rotates in plane around the cookie's out-of-screen axis without touching any transform.
	const float CombinedSpin = FMath::Clamp(CurrentGoboRotationSpeed + CurrentAnimationWheelSpeed, -2.f, 2.f);
	const bool bGoboSpinActive = !FMath::IsNearlyZero(CombinedSpin);
	if (bGoboSpinActive)
	{
		GoboAngle = FMath::Fmod(GoboAngle + DeltaSeconds * CombinedSpin * RebusGoboMaxRotRateDegPerSec, 360.f);
		bStillAnimating = true;
		// Trigger the RT redraw with the new angle. UpdateResource synchronously clears the RT
		// (ClearColor = transparent) and fires OnCanvasRenderTargetUpdate -> OnGoboRTUpdate,
		// which draws the source texture rotated by GoboAngle. Only meaningful when a real gobo
		// is loaded (CurrentGoboTexture != null) -- on Open/clear the RT was released.
		if (GoboRT && CurrentGoboTexture)
		{
			GoboRT->UpdateResource();
		}
	}

	if (bMotionAnim) { RefreshMotion(); bStillAnimating = true; }
	if (bIntensityAnim) { RefreshIntensity(); bStillAnimating = true; }
	if (bConeAnim) { RecomputeConeAngles(); SelectIesForZoom(); bStillAnimating = true; }

	// Strobe gating.
	if (ShutterMode == ERebusShutterMode::Strobe && ShutterRateHz > KINDA_SMALL_NUMBER)
	{
		ShutterPhase += DeltaSeconds * ShutterRateHz;
		ShutterPhase = FMath::Fmod(ShutterPhase, 1.f);
		RefreshIntensity();
		bStillAnimating = true;
	}

	bAnimating = bStillAnimating;
}
