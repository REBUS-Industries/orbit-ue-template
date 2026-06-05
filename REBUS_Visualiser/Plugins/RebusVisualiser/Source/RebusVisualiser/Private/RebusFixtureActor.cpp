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
#include "Engine/ExponentialHeightFog.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "HAL/IConsoleManager.h"

namespace
{
	// v1.0.76 reflection helpers for UCanvasRenderTarget2D::bShouldClearRenderTargetOnReceive-
	// Update. The field is `protected` on UCanvasRenderTarget2D in UE 5.7 (UPROPERTY-exposed
	// for Blueprint via meta=(AllowPrivateAccess="true"), but C++ outside the class can't read
	// or write it directly -- v1.0.74's `GoboRT->b... = true` failed with C2248). Defaults to
	// true in 5.7 (CanvasRenderTarget2D.h), so the no-op fallback (when FindPropertyByName
	// returns null on a future engine rename) is correct. v1.0.77: helpers MUST live at the
	// top of the file because DumpGoboStateForDebug at line ~360 calls the reader -- v1.0.76
	// put them in an anonymous namespace down near EnsureGoboRT at line ~2783, which broke
	// the build with "identifier not found" at the dump line.
	FBoolProperty* FindClearOnUpdateProperty(UCanvasRenderTarget2D* RT)
	{
		if (!RT) return nullptr;
		return CastField<FBoolProperty>(
			RT->GetClass()->FindPropertyByName(TEXT("bShouldClearRenderTargetOnReceiveUpdate")));
	}
	void SetGoboRTClearOnUpdate(UCanvasRenderTarget2D* RT, bool bValue)
	{
		if (FBoolProperty* Prop = FindClearOnUpdateProperty(RT))
		{
			Prop->SetPropertyValue_InContainer(RT, bValue);
		}
	}
	int32 ReadGoboRTClearOnUpdate(UCanvasRenderTarget2D* RT) // -1 = couldn't read, 0/1 = value
	{
		FBoolProperty* Prop = FindClearOnUpdateProperty(RT);
		if (!Prop) return -1;
		return Prop->GetPropertyValue_InContainer(RT) ? 1 : 0;
	}

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
	// v1.0.108 -- raised from 2.5 to 6.0. The raymarch's radial cross-section is
	// `core = exp(-rN^2 * BeamSharpness)` where `rN = radial / radiusAt` (axial=1 at the
	// cone-mesh edge). At the v1.0.42..v1.0.107 default `BeamSharpness = 2.5` the visible
	// glow at `rN = 1` is `exp(-2.5) = 8.2 %` of axis density -- the soft Gaussian visibly
	// extends ALL THE WAY to the geometric cone edge, so the visible shaft reads as a fat
	// soft cylinder filling the entire `OuterConeAngle` cone, while the lit floor disc
	// (driven by the IES profile + the SpotLight's linear inner..outer taper) sits at maybe
	// half that width on the floor -- the user's "cone is much wider than the spotlight
	// footprint" v1.0.108 report. At `BeamSharpness = 6.0` the cross-section is
	// `core(rN=0.5) = 22 %`, `core(rN=0.7) = 5 %`, `core(rN=1.0) = 0.25 %` -- the visible
	// bright shaft pinches to roughly `rN < 0.6` (about 60 % of the geometric cone-mesh
	// radius), which combined with the v1.0.108 half-intensity FarRadius geometry math
	// (UpdateBeamConeGeometry) brings the visible shaft edge into ~5 % alignment with the
	// bright floor disc on the canonical test scene. Live-tunable via
	// `Rebus.BeamSharpness <float>` (default 6.0; recommended 4..12); operators tune
	// upward for tight-aperture profile fixtures, downward for soft / frosted looks.
	constexpr float RebusBeamSharpness = 6.0f;
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

// v1.0.95: default SpotLight `VolumetricScatteringIntensity` for the Epic-beam mode (the only
// beam path going forward). The cone-mesh `M_RebusBeam` raymarch is the dense visible shaft;
// this per-light scattering is the SEPARATE soft fog-interaction layer that gets carved by
// occluders via `bCastVolumetricShadow = true` (the v1.0.95 fix for "make volumetric shadowing
// work with the Epic beam"). 0.5 is intentionally modest -- it's a soft halo around the
// crisp cone, not a competing dense shaft. Live-tunable via `Rebus.SpotLightScatter <float>`;
// on change every fixture re-pushes the value through `RefreshBeamShadowMode`.
float GRebusSpotLightScatter = 0.5f;

// v1.0.96 screen-space shadow trace on the M_RebusBeam raymarch shaft. See the
// `_BEAM_RAYMARCH_HLSL` doc-comment in build_rebus_base_level.py for the full algorithm. The
// CVars below push directly onto every fixture's BeamMID via `RefreshBeamShadowParams`,
// driving the matching scalar params on the master. The `Rebus.BeamShadow [0|1]`
// MASTER TOGGLE lives in RebusVisualiser.cpp (`HandleBeamShadowCommand`) and routes through
// the `Rebus.BeamShadowStrength` CVar's refresh sink so flicking the master off/on never
// loses the operator's tuned strength (the prior is stored file-static in RebusVisualiser.cpp).
//
// Defaults paired with the master's scalar defaults so a fresh project shows the shadow trace
// at full strength with no portal push needed:
//   * BeamShadowSteps  = 8  (the design point; shader clamps to [1, 16])
//   * BeamShadowStrength = 1  (full shadow on any sample whose ray hits an occluder)
//   * BeamShadowBias = 5.0 cm (v1.0.99 raised from 0.5 -- FIRST-STEP MINIMUM cm so the
//     trace can't read the shaft sample's own pixel as its own occluder; see the v1.0.99
//     PROJECTION + SELF-OCCLUSION FIX block in _BEAM_RAYMARCH_HLSL).
//   * BeamShadowDebug = 0 (off; 1 = shadow-factor heatmap; 2 = first-UV sanity view).
//
// v1.0.99: bias + debug are CVar-tunable now (v1.0.96 only exposed steps + strength, on the
// theory the per-fixture material default would be enough). The user reported "Im not seeing
// this work at all" so the live debug + bias knobs are needed to diagnose the trace in the
// field. See the v1.0.99 README release block for the operator checklist.
float GRebusBeamShadowSteps = 8.f;
float GRebusBeamShadowStrength = 1.f;
float GRebusBeamShadowBias = 5.f;
int32 GRebusBeamShadowDebug = 0;

// v1.0.109 -- pan-edge / sky / far-distance guard CVars on the same M_RebusBeam screen-space
// shadow trace. Push through the same `RefreshBeamShadowParamsOnEveryFixture` sink the
// v1.0.96 / v1.0.99 trace scalars use so the four-scalar v1.0.99 contract becomes a
// seven-scalar v1.0.109 contract -- one chokepoint, one log line per flip, no risk of half-
// pushed state. See the v1.0.109 PAN-EDGE GUARDS block in build_rebus_base_level.py::
// _BEAM_RAYMARCH_HLSL for the diagnostic + per-guard rationale + the README v1.0.109
// release block for the operator checklist.
//
// Defaults paired with the master's authored defaults so a fresh project / regen agrees
// with a fresh-spawn fixture without any portal push:
//   * BeamShadowFarCullCm = 50000.0 cm = 500 m (Guard B). Shadow march steps with
//     camera-Z beyond this are skipped entirely -- reverse-Z precision is sub-LSB beyond
//     ~500 m, so the depth comparison gives random answers. The longest realistic stage
//     throw is ~200 m; the default leaves a 2.5x headroom before the cull kicks in.
//   * BeamShadowEdgeGuard = 1 (ON; Guard D). Master toggle for the off-screen NDC guard.
//     0 restores the v1.0.99 broken behaviour so the operator can A/B verify the diagnosis
//     ("pan-edge clipping returns when 0, stays fixed when 1" is the verification gate).
//   * BeamShadowBiasScale = 0.002 (Guard C). 0.2 percent of sample depth in cm added to
//     the per-step depth-comparison tolerance on top of the v1.0.99 absolute floor
//     (`BeamShadowBias` cm, the FIRST-STEP MINIMUM). At 30 m the added term is ~6 cm,
//     at 200 m it is ~40 cm -- grows linearly with depth so the test stays meaningful in
//     the long-throw low-precision regime.
float GRebusBeamShadowFarCullCm = 50000.f;
int32 GRebusBeamShadowEdgeGuard = 1;
float GRebusBeamShadowBiasScale = 0.002f;

// Refresh sink shared by both shadow CVars: walk every Rebus fixture and re-push the new
// values to the BeamMID. Cheap (a handful of float param sets per fixture); no proxy rebuild
// needed since these are translucent-shaft scalars only.
static void RefreshBeamShadowParamsOnEveryFixture(const TCHAR* CVarLabel, float NewVal)
{
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
				F->RefreshBeamShadowParams();
				++Refreshed;
			}
		}
	}
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("%s -> %.3f, refreshed %d fixture(s) (BeamMID re-pushed; M_RebusBeam Custom HLSL screen-space shadow trace)."),
		CVarLabel, NewVal, Refreshed);
}

FAutoConsoleVariableRef CVarRebusBeamShadowSteps(
	TEXT("Rebus.BeamShadowSteps"),
	GRebusBeamShadowSteps,
	TEXT("v1.0.96 -- M_RebusBeam screen-space shadow trace step count per shaft sample (default 8, "
		 "clamped [1, 16] inside the shader). Cost ~ StepCount * BeamShadowSteps SceneDepth taps "
		 "per beam pixel; 32 * 8 = 256 taps/pixel is the design point. Live -- changing this "
		 "walks every Rebus fixture and re-pushes the scalar onto the BeamMID."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
		RefreshBeamShadowParamsOnEveryFixture(TEXT("Rebus.BeamShadowSteps"), CVar->GetFloat());
	}),
	ECVF_Default);

FAutoConsoleVariableRef CVarRebusBeamShadowStrength(
	TEXT("Rebus.BeamShadowStrength"),
	GRebusBeamShadowStrength,
	TEXT("v1.0.96 -- M_RebusBeam screen-space shadow trace strength (default 1.0). 1 = a "
		 "shaft sample whose shadow ray hits an occluder contributes NO density (full shadow); "
		 "0 = the trace runs but does nothing (visually equivalent to disabling shadows, but "
		 "the [branch] gate in the shader takes the trace OUT of the per-pixel cost when "
		 "strength is exactly 0). Use Rebus.BeamShadow for the binary master toggle. Live -- "
		 "changing this walks every Rebus fixture and re-pushes the scalar onto the BeamMID."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
		RefreshBeamShadowParamsOnEveryFixture(TEXT("Rebus.BeamShadowStrength"), CVar->GetFloat());
	}),
	ECVF_Default);

// v1.0.99 -- M_RebusBeam screen-space shadow trace FIRST-STEP MINIMUM cm. The very first
// SceneDepth tap is at least `BeamShadowBias` cm out from the shaft sample toward the light
// (any closer step is skipped) so the trace can't read the shaft's own pixel as its own
// occluder against nearby fixture / prop geometry. v1.0.96 default (0.5 cm) was too small in
// practice -- the v1.0.99 default of 5 cm escapes self-occlusion on the cone-mesh's near
// neighbours; raise to 50+ cm only if the user reports the beam still passes through
// nearby props after the v1.0.99 LWC projection fix lands. See the v1.0.99 PROJECTION +
// SELF-OCCLUSION FIX block in _BEAM_RAYMARCH_HLSL.
FAutoConsoleVariableRef CVarRebusBeamShadowBias(
	TEXT("Rebus.BeamShadowBias"),
	GRebusBeamShadowBias,
	TEXT("v1.0.99 -- M_RebusBeam screen-space shadow trace FIRST-STEP MINIMUM cm (default 5.0). "
		 "Any candidate shadow step within this many cm of the shaft sample is skipped, so the "
		 "first SceneDepth tap is at least Bias cm out from the sample toward the light -- "
		 "prevents self-occlusion against nearby fixture / scene geometry. Raise to 50+ if "
		 "the operator reports the beam still passes through nearby props in BeamShadowDebug=1 "
		 "view. Live -- changing this walks every Rebus fixture and re-pushes the scalar onto "
		 "the BeamMID."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
		RefreshBeamShadowParamsOnEveryFixture(TEXT("Rebus.BeamShadowBias"), CVar->GetFloat());
	}),
	ECVF_Default);

// v1.0.99 -- M_RebusBeam screen-space shadow trace DEBUG-VIEW knob.
//   0 = off (default; ship the regular composed beam).
//   1 = shadow-factor heatmap (green where unshadowed, red where shadowed). The cube/prop
//       between the fixture and the floor should appear RED in this view; if the cube is
//       still green, the trace found NO occluders (the v1.0.96 LWC projection bug, or
//       BeamShadowStrength=0 via `Rebus.BeamShadow 0` master toggle).
//   2 = first shadow-step UV sanity. Beam tints orange/yellow across the screen as the UV
//       walks [0..1]^2. Constant near-zero black means the LWC projection is broken.
FAutoConsoleVariableRef CVarRebusBeamShadowDebug(
	TEXT("Rebus.BeamShadowDebug"),
	GRebusBeamShadowDebug,
	TEXT("v1.0.99 + v1.0.109 -- M_RebusBeam screen-space shadow trace debug-view (default 0 = "
		 "off). 1 = shadow-factor heatmap (green=unshadowed, red=shadowed -- a cube placed "
		 "between fixture + floor should appear red in this view; if the cube is still green, "
		 "the trace found no occluders). 2 (v1.0.109 REPURPOSED) = per-pixel colour-by-GUARD-"
		 "REASON: RED depth-occluded, GREEN off-screen guard (Guard D -- the v1.0.109 pan-edge "
		 "rescue), BLUE sky / no-geometry guard (Guard A), YELLOW far-distance cull (Guard B), "
		 "WHITE clean. With mode 2, the pan-edge regions of the user's v1.0.106 bug report "
		 "should now show GREEN -- the off-screen guard is doing its job; if they still show "
		 "RED with Rebus.BeamShadowEdgeGuard 1, the v1.0.110+ follow-up has work to do. The "
		 "pre-v1.0.109 mode-2 'first projected UV' view is retired (the LWC projection has "
		 "been three releases dead). Live -- changing this walks every Rebus fixture and "
		 "re-pushes the scalar onto the BeamMID. See the v1.0.99 / v1.0.109 README release "
		 "blocks for the operator checklist."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
		RefreshBeamShadowParamsOnEveryFixture(TEXT("Rebus.BeamShadowDebug"), (float)CVar->GetInt());
	}),
	ECVF_Default);

// v1.0.109 -- pan-edge / sky / far-distance guard CVars on the M_RebusBeam screen-space
// shadow trace. All three push through the same `RefreshBeamShadowParamsOnEveryFixture`
// sink as the v1.0.96 / v1.0.99 trace scalars (one chokepoint, four-scalar v1.0.99
// contract becomes seven-scalar v1.0.109). See the v1.0.109 PAN-EDGE GUARDS block in
// build_rebus_base_level.py::_BEAM_RAYMARCH_HLSL for the diagnostic + per-guard rationale
// + README v1.0.109 release block for the full operator checklist.
FAutoConsoleVariableRef CVarRebusBeamShadowFarCullCm(
	TEXT("Rebus.BeamShadowFarCullCm"),
	GRebusBeamShadowFarCullCm,
	TEXT("v1.0.109 -- M_RebusBeam screen-space shadow trace FAR-DISTANCE CULL in cm "
		 "(default 50000.0 = 500 m). Shadow march steps with camera-Z beyond this are "
		 "skipped entirely (Guard B) -- reverse-Z precision is sub-LSB beyond ~500 m, the "
		 "depth comparison gives essentially random answers and the trace fires noise-driven "
		 "false-occlusions. The default leaves a 2.5x headroom past the longest realistic "
		 "stage throw (~200 m). Raise for arena-class throws if the operator reports the "
		 "trace clipping legitimate downrange occluders; lower for tighter rigs to skip "
		 "more long-distance tap cost. Live -- changing this walks every Rebus fixture and "
		 "re-pushes the scalar onto the BeamMID; `Rebus.BeamShadowDebug 2` paints YELLOW "
		 "on pixels where this guard fired."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
		RefreshBeamShadowParamsOnEveryFixture(TEXT("Rebus.BeamShadowFarCullCm"), CVar->GetFloat());
	}),
	ECVF_Default);

FAutoConsoleVariableRef CVarRebusBeamShadowEdgeGuard(
	TEXT("Rebus.BeamShadowEdgeGuard"),
	GRebusBeamShadowEdgeGuard,
	TEXT("v1.0.109 -- M_RebusBeam screen-space shadow trace OFF-SCREEN GUARD master toggle "
		 "(default 1 = ON). When 1, shadow march steps whose projected UV falls outside "
		 "[-1, 1]^2 NDC are SKIPPED (Guard D) -- v1.0.99 had this guard unconditionally; "
		 "v1.0.109 puts the master switch behind it so the operator can A/B verify the fix. "
		 "Set 0 to restore the v1.0.99 broken behaviour: the pan-edge clipping the user "
		 "reported against v1.0.106 should return (the canonical screen-space-shadow failure "
		 "mode where the trace samples sky / undefined depth pixels at the screen edge and "
		 "concludes 'occluded'). Set back to 1 to confirm the diagnosis -- the clipping "
		 "should disappear again. Live -- changing this walks every Rebus fixture and "
		 "re-pushes the scalar onto the BeamMID; `Rebus.BeamShadowDebug 2` paints GREEN on "
		 "pixels where this guard fired."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
		RefreshBeamShadowParamsOnEveryFixture(TEXT("Rebus.BeamShadowEdgeGuard"), (float)CVar->GetInt());
	}),
	ECVF_Default);

FAutoConsoleVariableRef CVarRebusBeamShadowBiasScale(
	TEXT("Rebus.BeamShadowBiasScale"),
	GRebusBeamShadowBiasScale,
	TEXT("v1.0.109 -- M_RebusBeam screen-space shadow trace DISTANCE-SCALED BIAS multiplier "
		 "(default 0.002 = 0.2 percent of sample depth in cm). Per-step depth-comparison "
		 "tolerance = (0.01 * sdt) + sd * BeamShadowBiasScale + `BeamShadowBias` cm (the "
		 "v1.0.99 absolute floor / FIRST-STEP MINIMUM). At 30 m the added term contributes "
		 "~6 cm; at 200 m ~40 cm -- grows linearly with depth so the comparison stays "
		 "meaningful in the reverse-Z low-precision regime at the long end of the beam "
		 "throw. Raise to 0.003-0.005 if the operator reports the beam shaft self-shadowing "
		 "at extreme distances despite `BeamShadowEdgeGuard 1`; lower to 0.001 for tighter "
		 "rigs where the v1.0.99 absolute floor is already sufficient. Live -- the refresh "
		 "sink walks every Rebus fixture and re-pushes the scalar onto the BeamMID."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
		RefreshBeamShadowParamsOnEveryFixture(TEXT("Rebus.BeamShadowBiasScale"), CVar->GetFloat());
	}),
	ECVF_Default);

// v1.0.101 -- per-fixture cone-mesh visible far-radius scale, applied identically to the
// procedural M_RebusBeam cone (UpdateBeamConeGeometry) AND the Epic-beam canvas
// `DMX Zoom` (UpdateEpicBeamParams). Default 1.0 = the visible shaft is sized strictly
// to the GDTF zoom-range half-angle (the same angle the SpotLight outer cone uses for
// the lit footprint). Operators flip to ~0.85..0.95 to match the perceived bright-disc
// edge on the floor, which sits at roughly the average of the SpotLight's inner +
// outer cone angles thanks to UE's linear-taper light model -- the user's "beam is
// slightly larger than the footprint" report. The CVar refresh sink walks every
// fixture and re-pushes the cone geometry + Epic DMX Zoom param.
//
// Crucially this scalar does NOT pinch the SpotLight outer cone, so the lit footprint,
// IES sampling, and 1/r^2 falloff continue to track the GDTF zoom-range specification
// verbatim. The scalar exists exclusively to bring the visible shaft inward to
// coincide with the perceived (half-intensity) lit-disc edge, NOT to redefine where
// the light reaches.
float GRebusBeamConeRadiusScale = 1.0f;
FAutoConsoleVariableRef CVarRebusBeamConeRadiusScale(
	TEXT("Rebus.BeamConeRadiusScale"),
	GRebusBeamConeRadiusScale,
	TEXT("v1.0.101 -- multiplier on the visible cone-mesh shaft radius (procedural M_RebusBeam "
		 "AND Epic-beam canvas DMX Zoom) so the visible shaft can be tightened to coincide with "
		 "the perceived bright-disc edge of the lit footprint on the floor. Default 1.0 = "
		 "geometric truth (visible shaft sized to the GDTF zoom-range half-angle, identical to "
		 "the SpotLight outer cone). Operators typically flip to ~0.85..0.95 to bring the shaft "
		 "edge in to the bright-disc edge (which sits ~mid of inner..outer cone thanks to UE's "
		 "linear-taper light model). Does NOT pinch the SpotLight outer cone, so the lit "
		 "footprint stays anchored to the GDTF zoom-range spec verbatim. Live -- the refresh "
		 "sink walks every fixture and re-pushes cone geometry + Epic DMX Zoom. Pair with "
		 "`Rebus.DumpFixtureZoom [fixtureId]` to verify the change landed."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
		const float NewVal = FMath::Max(0.05f, CVar->GetFloat());
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
					F->RefreshBeamConeRadiusScaleFromCVar(NewVal);
					++Refreshed;
				}
			}
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.BeamConeRadiusScale -> %.3f, refreshed %d fixture(s) (procedural cone-mesh + Epic-beam DMX Zoom both re-pushed; SpotLight outer cone untouched)."),
			NewVal, Refreshed);
	}),
	ECVF_Default);

// v1.0.108 -- M_RebusBeam radial-attenuation tuning CVars. The raymarch composes density
// per shaft sample as `d = BeamDensity * core * widthNorm * srcAtten * nf * softOcc *
// shadowAtten` where `core = exp(-rN^2 * BeamSharpness)` (radial Gaussian; rN=0 on axis,
// rN=1 at the geometric cone-mesh edge), `srcAtten = 1 / (1 + BeamFalloff * dn^2)`
// (length-fade from lens to throw, dn=axial/length 0..1), and `BeamDensity` is the
// per-step density gain. Together these knobs control how WIDE / how BRIGHT the visible
// shaft reads INSIDE the geometric cone-mesh (which is sized by the v1.0.108
// half-intensity-FarRadius math in `UpdateBeamConeGeometry`).
//
// `Rebus.BeamSharpness <float>` (default 6.0; recommended [4..12]) -- the heavy hitter
// for the v1.0.108 "cone size doesn't match spotlight size" fix. Higher = the soft
// Gaussian glow narrows to the bright core (visible disc on the floor matches the lit
// disc edge); lower = soft / frosted look that fills the geometric cone-mesh. The
// pre-v1.0.108 default of 2.5 visibly bled the shaft across the entire geometric
// outer-cone (the user's image showed a cylindrical shaft 2-3x wider than the lit
// floor disc); v1.0.108 lifts the default to 6.0 so the visible bright shaft pinches
// to ~60% of the geometric cone-mesh radius, which combined with the half-intensity
// FarRadius math lands the visible shaft edge within ~5% of the bright floor disc
// edge on the canonical test scene. Operators who liked the soft v1.0.107 look can
// flip `Rebus.BeamSharpness 2.5`; show-context fixtures with tight apertures can push
// to 8-12 for an even tighter core.
//
// `Rebus.BeamDensity <float>` (default 0.015; recommended [0.005..0.06]) -- per-step
// density gain. Higher = denser / brighter shaft (harder to see through, may saturate
// the additive accumulator); lower = wispy / faint shaft. Default unchanged from
// v1.0.40; exposed in v1.0.108 so an operator can tune total opacity without touching
// the Sharpness/Falloff radial profile.
//
// `Rebus.BeamFalloff <float>` (default 1.6; recommended [0..4]) -- length-fade strength
// (`srcAtten = 1 / (1 + BeamFalloff * dn^2)`). 0 = flat along the shaft (visible same
// brightness from lens to throw); higher = dims faster downrange (lens reads brightest,
// throw reads dim). Default unchanged from v1.0.40; exposed in v1.0.108 as a
// companion to BeamSharpness/Density so the full radial+axial gradient can be tuned
// live.
//
// All three CVars share `RefreshBeamRadialParamsOnEveryFixture` which walks every
// Rebus fixture and re-pushes onto the BeamMID (which routes into the Custom HLSL
// Custom node's BeamSharpness / BeamDensity / BeamFalloff scalar inputs). Cheap; no
// proxy rebuild required (these are radial scalars only -- the cone mesh's geometry
// is unchanged by these knobs).
float GRebusBeamSharpness = 6.0f;
float GRebusBeamDensity   = 0.015f;
float GRebusBeamFalloff   = 1.6f;

static void RefreshBeamRadialParamsOnEveryFixture(const TCHAR* CVarLabel, float NewVal)
{
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
				F->RefreshBeamRadialParams();
				++Refreshed;
			}
		}
	}
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("%s -> %.4f, refreshed %d fixture(s) (M_RebusBeam radial-attenuation scalar re-pushed; cone-mesh geometry unchanged)."),
		CVarLabel, NewVal, Refreshed);
}

FAutoConsoleVariableRef CVarRebusBeamSharpness(
	TEXT("Rebus.BeamSharpness"),
	GRebusBeamSharpness,
	TEXT("v1.0.108 -- M_RebusBeam radial-Gaussian sharpness (default 6.0; recommended [4..12]). "
		 "Per-shaft-sample density `core = exp(-rN^2 * BeamSharpness)` where rN=radial/coneRadius. "
		 "Higher = visible shaft pinches to a tight bright core (matches the bright floor disc "
		 "edge); lower = soft / frosted shaft fills the geometric cone-mesh. Pre-v1.0.108 "
		 "default 2.5 visibly bled the shaft across the entire OuterConeAngle cone, reading "
		 "as a fat soft cylinder ~2-3x wider than the lit footprint (the v1.0.108 user "
		 "report). Live -- changing this walks every Rebus fixture and re-pushes the scalar "
		 "onto the BeamMID; cone-mesh geometry is unchanged."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
		RefreshBeamRadialParamsOnEveryFixture(TEXT("Rebus.BeamSharpness"), CVar->GetFloat());
	}),
	ECVF_Default);

FAutoConsoleVariableRef CVarRebusBeamDensity(
	TEXT("Rebus.BeamDensity"),
	GRebusBeamDensity,
	TEXT("v1.0.108 -- M_RebusBeam per-step density gain (default 0.015; recommended "
		 "[0.005..0.06]). Higher = denser / brighter shaft (may saturate the additive "
		 "accumulator); lower = wispy / faint shaft. Companion to Rebus.BeamSharpness; the "
		 "Sharpness knob shapes the radial profile (where the shaft fades to black laterally), "
		 "this knob scales the overall opacity (how dense the visible shaft reads regardless "
		 "of profile). Live -- the refresh sink walks every fixture and re-pushes the BeamMID "
		 "scalar."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
		RefreshBeamRadialParamsOnEveryFixture(TEXT("Rebus.BeamDensity"), CVar->GetFloat());
	}),
	ECVF_Default);

FAutoConsoleVariableRef CVarRebusBeamFalloff(
	TEXT("Rebus.BeamFalloff"),
	GRebusBeamFalloff,
	TEXT("v1.0.108 -- M_RebusBeam length-fade strength (default 1.6; recommended [0..4]). "
		 "Per-shaft-sample axial attenuation `srcAtten = 1 / (1 + BeamFalloff * dn^2)` "
		 "where dn = axial/BeamLength (0 at lens, 1 at throw). 0 = flat along the shaft "
		 "(visible same brightness from lens to throw); higher = dims faster downrange "
		 "(lens reads brightest, throw reads dim). Companion to Rebus.BeamSharpness and "
		 "Rebus.BeamDensity for full radial+axial control. Live -- the refresh sink walks "
		 "every fixture and re-pushes the BeamMID scalar."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
		RefreshBeamRadialParamsOnEveryFixture(TEXT("Rebus.BeamFalloff"), CVar->GetFloat());
	}),
	ECVF_Default);

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

// v1.0.95 -- live-toggleable default `VolumetricScatteringIntensity` for the Epic-beam SpotLight.
// See the comment on `GRebusSpotLightScatter` above for the rationale (per-light scattering
// layer for occluder carving, separate from the cone-mesh raymarch). Refresh sink re-runs
// `RefreshBeamShadowMode` on every fixture so the new value is picked up live.
FAutoConsoleVariableRef CVarRebusSpotLightScatter(
	TEXT("Rebus.SpotLightScatter"),
	GRebusSpotLightScatter,
	TEXT("SpotLight VolumetricScatteringIntensity default for Epic-beam mode (v1.0.95). The cone-mesh raymarch is the dense visible shaft; this per-light scattering is the soft fog-interaction layer that gets CARVED by occluders thanks to bCastVolumetricShadow=true (forced ON in BuildSpotLight). Requires a scene AExponentialHeightFog with bEnableVolumetricFog=true to be visible. Live."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
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
			TEXT("Rebus.SpotLightScatter -> %.2f, refreshed %d fixture(s)."),
			CVar->GetFloat(), Refreshed);
	}),
	ECVF_Default);

// v1.0.94 -- HARD FLOOR for MegaLights routing on every Rebus SpotLight. Default 0 (legacy
// clustered/deferred path forced on every fixture, regardless of gobo state). The user reported
// (against v1.0.93) that "with the Epic beam system we have, we are not seeing the shadow of an
// object in the footprint": objects placed between a fixture and the floor were NOT casting
// shadow into the lit pool. Root cause: UE 5.5+ MegaLights routes per-light shadow casting
// through the tile-clustered sampling path, which on the user's hardware/quality tier silently
// drops dynamic occluders below MegaLights' shadow-fidelity floor -- so a hanging truss / a
// person / a prop between the fixture and the floor lit up but cast no silhouette. v1.0.94
// opts EVERY Rebus SpotLight off MegaLights so dynamic occluders ALWAYS cast hard shadows in
// the floor footprint.
//
// Trade-off: every Rebus fixture loses MegaLights' clustering perf -- in show-context rigs (tens to
// hundreds of fixtures) this is the right default because shadow fidelity is non-negotiable for
// stage visualisation. Deployments that prioritise MegaLights' clustering over per-fixture shadow
// fidelity can flip the gate to 1 (`Rebus.AllowMegaLights 1`); the CVar refresh sink walks every
// Rebus fixture and re-resolves bAllowMegaLights based on the new value AND the fixture's current
// gobo state.
//
// v1.0.95: `Rebus.AllowMegaLights` is the sole MegaLights gate (see README v1.0.95 for
// the full audit of what was removed and why).
int32 GRebusAllowMegaLights = 0;

// Resolve the desired `bAllowMegaLights` value for a Rebus SpotLight given a per-call requested
// value (typically 1 for "MegaLights-on" / 0 for "explicitly opt-out for gobo"). The
// `Rebus.AllowMegaLights` CVar is the hard floor: when 0, ALWAYS return 0; when 1, pass the
// requested value through unchanged. Used at every assignment site of `SpotLight->bAllowMegaLights`
// so every code path agrees on the policy. v1.0.94 introduced.
static FORCEINLINE uint32 ResolveAllowMegaLights(uint32 RequestedAllow)
{
	return (GRebusAllowMegaLights == 0) ? 0u : (RequestedAllow ? 1u : 0u);
}

// v1.0.94 -- live-toggleable HARD FLOOR for MegaLights routing on every Rebus SpotLight. See
// the comment on `GRebusAllowMegaLights` above for the rationale. Default 0 (every Rebus
// SpotLight uses the legacy clustered/deferred path so dynamic occluders ALWAYS cast hard
// shadows in the floor footprint). Refresh sink walks EVERY Rebus fixture and re-resolves
// `bAllowMegaLights` per the new value via `RefreshAllowMegaLightsFromCVar`, re-registering
// the SpotLight component when the value transitions so the FLightSceneInfo proxy is rebuilt
// with the new value on the next frame.
FAutoConsoleVariableRef CVarRebusAllowMegaLights(
	TEXT("Rebus.AllowMegaLights"),
	GRebusAllowMegaLights,
	TEXT("0|1 -- when 0 (default since v1.0.94), every Rebus SpotLight is forced off MegaLights "
		 "(`bAllowMegaLights = 0`) so the legacy clustered/deferred path renders shadow casting "
		 "for dynamic occluders -- the v1.0.94 fix for 'we are not seeing the shadow of an object "
		 "in the [Epic-beam] footprint'. When 1, MegaLights routing is allowed: per-fixture state "
		 "(gobo cookie active) can still force the legacy path on a per-fixture basis, but "
		 "non-special fixtures get MegaLights' clustering perf back -- at the cost of dropping "
		 "dynamic-occluder shadows in their footprint on low-fidelity tiers. Live -- changing "
		 "this re-pushes / restores on every Rebus fixture in every loaded world."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
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
					F->RefreshAllowMegaLightsFromCVar();
					++Refreshed;
				}
			}
		}
		const bool bAllow = (CVar->GetInt() != 0);
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.AllowMegaLights -> %d, refreshed %d fixture(s) (%s)."),
			CVar->GetInt(), Refreshed,
			bAllow ? TEXT("MegaLights routing now permitted -- per-fixture gobo path still forces legacy when active; non-special fixtures get clustering perf back")
			       : TEXT("HARD FLOOR -- every Rebus SpotLight forced off MegaLights so dynamic occluders cast hard shadows in the footprint"));
	}),
	ECVF_Default);

// v1.0.88 A/B toggle: force the synthetic single-disc lens fallback even when the portal sent
// mesh-blob v3 with isBeam meshes. Default 0 (real geometry wins when available). On change,
// every spawned fixture re-evaluates its lens-visuals visibility: see
// ARebusFixtureActor::SetUseSyntheticLensFallback for the per-fixture state transitions.
int32 GRebusForceSyntheticLensFallback = 0;
FAutoConsoleVariableRef CVarRebusForceSyntheticLensFallback(
	TEXT("Rebus.ForceSyntheticLensFallback"),
	GRebusForceSyntheticLensFallback,
	TEXT("0 (default) = when /meshes carries isBeam meshes (v3 blob), the real geometry IS the lens disc (mirror/glass material) and the synthetic single-disc is HIDDEN. 1 = hide every isBeam mesh + per-beam flare and re-show the synthetic LensDisc (A/B testing the v1.0.88 real-geometry path against the pre-v1.0.88 synthetic fallback). Live -- changing this re-pushes visibility on every spawned fixture."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
		if (!GEngine) return;
		const bool bForce = (CVar->GetInt() != 0);
		int32 Refreshed = 0;
		int32 WithIsBeam = 0;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* W = Ctx.World();
			if (!W) continue;
			for (TActorIterator<ARebusFixtureActor> It(W); It; ++It)
			{
				if (ARebusFixtureActor* F = *It)
				{
					F->SetUseSyntheticLensFallback(bForce);
					if (F->GetIsBeamMeshCount() > 0) ++WithIsBeam;
					++Refreshed;
				}
			}
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.ForceSyntheticLensFallback -> %d, refreshed %d fixture(s) (%d with isBeam meshes; %d on v2-blob/no-flag fallback regardless)."),
			CVar->GetInt(), Refreshed, WithIsBeam, Refreshed - WithIsBeam);
	}),
	ECVF_Default);

// v1.0.106 -- HARD FLOOR: prefer the procedural `M_RebusBeam` cone (which carries the
// v1.0.96 / v1.0.99 screen-space self-shadow trace) over Epic's `MI_Beam` canvas as the
// visible beam shaft. Default 1 (procedural wins). The user reported across v1.0.96,
// v1.0.99, v1.0.103 that "the beam mesh / beam cone is not stopping when it hits an
// object" -- every iteration of the shadow-trace work has been on `M_RebusBeam` (the
// procedural cone), but `TryBuildEpicBeam()` (v1.0.43) succeeds whenever the DMX
// Fixtures plugin content is installed at `/DMXFixtures/...`, sets `bUsingEpicBeam=true`,
// and HIDES the procedural cone (`BeamCone->SetVisibility(false)`). The visible shaft
// then becomes `EpicBeamMID` (`M_Beam_Master`), onto which `RefreshBeamShadowParams`
// has NEVER pushed any of the `BeamShadow*` scalars -- so every iteration of the v1.0.96+
// work has been editing a hidden material. The truth table:
//
//   bUsingEpicBeam | visible-shaft material | carries v1.0.99 shadow trace?
//   ---------------|------------------------|------------------------------
//   true           | Epic MI_Beam           | NO -- our shadow params are ignored
//   false          | M_RebusBeam MID        | YES (when master regen has run)
//
// v1.0.106 flips the default so `bUsingEpicBeam` is forced false in `BuildBeamCone()`
// (`TryBuildEpicBeam()` is skipped entirely). The procedural cone is unhidden and
// `RefreshBeamShadowParams` re-pushes the trace scalars onto its MID -- the v1.0.99
// shadow trace now actually renders. Operators preferring Epic's beam fidelity (its
// lens-flare integration + smarter zoom-normalised distribution) can flip back with
// `Rebus.PreferProceduralBeam 0`; the next v1.0.107 release will port the screen-
// space trace to Epic's MI_Beam too so both paths get the self-shadow correctness.
//
// Per-fixture override: `ARebusFixtureActor::bPreferProceduralBeam` (Details panel,
// `EditAnywhere` only -- private UPROPERTY, no `BlueprintReadWrite`, matches the
// v1.0.101 -> v1.0.102 fix on `BeamConeRadiusScale`). The CVar refresh sink walks
// every fixture; the per-fixture knob is the post-spawn editor-instance override
// (a hero fixture can keep Epic-beam while the rest of the rig flips procedural).
//
// Refresh sink (below) ALSO performs a one-shot probe of the on-disk M_RebusBeam
// master on every flip-to-1 transition: if `BeamShadowStrength` / `BeamShadowDebug`
// scalars are missing the master predates v1.0.99 and the trace will SILENTLY
// NO-OP. The Warning names `Rebus.RebuildBeamMaterial` (the v1.0.103 runtime regen
// command) so the operator-recovery action is on the same log line as the flip
// (catches the v1.0.103 operator-action-required case at exactly the moment it
// becomes relevant -- before the operator looks at the now-visible cone-mesh and
// wrongly concludes "still broken").
int32 GRebusPreferProceduralBeam = 1;
FAutoConsoleVariableRef CVarRebusPreferProceduralBeam(
	TEXT("Rebus.PreferProceduralBeam"),
	GRebusPreferProceduralBeam,
	TEXT("v1.0.106 -- 0|1 (default 1). When 1 (default since v1.0.106), every Rebus fixture's "
		 "visible beam shaft is the procedural `M_RebusBeam` cone -- which carries the v1.0.96 / "
		 "v1.0.99 screen-space self-shadow trace -- and `TryBuildEpicBeam()` is skipped at "
		 "spawn (Epic's `MI_Beam` canvas is not built). When 0, Epic's `MI_Beam` canvas IS the "
		 "visible shaft (the pre-v1.0.106 default since v1.0.43), and the screen-space self-"
		 "shadow trace is BYPASSED (it lives on `M_RebusBeam`, not on `M_Beam_Master`); "
		 "v1.0.107 will port the trace to Epic's beam too. Live -- changing this walks every "
		 "Rebus fixture and flips the visible shaft without a respawn (the procedural cone + "
		 "Epic canvas both stay alive in the scene -- visibility-only toggle). On flip to 1 "
		 "the on-disk M_RebusBeam master is probed for the v1.0.99 parameter contract; a "
		 "stale master logs a Warning naming `Rebus.RebuildBeamMaterial` (the v1.0.103 "
		 "runtime regen). Pair with `Rebus.DumpBeamShadow` -- the per-fixture `Beam=Epic|"
		 "Procedural` field reports which path is live. See the v1.0.106 README release "
		 "block for the diagnosis chain that motivated the default flip."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
		if (!GEngine) return;
		const bool bPrefer = (CVar->GetInt() != 0);

		// v1.0.106 stale-master probe on flip-to-1: a fresh-pull operator who never ran
		// `build_rebus_base_level.py` keeps the v1.0.96 cooked master with the LWC
		// projection bug -- the screen-space trace then silently no-ops on every step.
		// Catch this at the moment the flip becomes operator-visible so the recovery
		// action is on the same log line as the flip itself. Re-uses the same scalar
		// contract `URebusVisualiserSubsystem::ProbeBeamMasterAtStartup` checks at
		// session boot (BeamShadowStrength + BeamShadowDebug -- the two scalars added
		// post-v1.0.96 that prove the master is on or after v1.0.99).
		if (bPrefer)
		{
			if (UMaterialInterface* BeamMaster = LoadObject<UMaterialInterface>(nullptr,
				TEXT("/Game/REBUS/Materials/M_RebusBeam.M_RebusBeam")))
			{
				float V = 0.f;
				const bool bHasStrength = BeamMaster->GetScalarParameterValue(
					FMaterialParameterInfo(TEXT("BeamShadowStrength")), V);
				const bool bHasDebug = BeamMaster->GetScalarParameterValue(
					FMaterialParameterInfo(TEXT("BeamShadowDebug")), V);
				if (!bHasStrength || !bHasDebug)
				{
					UE_LOG(LogRebusVisualiser, Warning,
						TEXT("Rebus.PreferProceduralBeam 1: STALE BEAM MASTER detected -- "
							 "M_RebusBeam is missing the v1.0.99 parameter contract "
							 "(BeamShadowStrength=%d BeamShadowDebug=%d). The procedural "
							 "cone is now the visible shaft, but the screen-space shadow "
							 "trace will SILENTLY NO-OP against this master -- the cone "
							 "will read additive but UNSHADOWED (cubes between fixture + "
							 "floor will appear to let the beam pass through). Operator "
							 "recovery: run `Rebus.RebuildBeamMaterial` (editor-only "
							 "v1.0.103 runtime regen), then ClearScene+LoadScene from the "
							 "portal (or restart the editor) so each fixture respawns + "
							 "rebuilds its BeamMID off the freshly-regenerated master. "
							 "Verify with `Rebus.DumpBeamShadow` (every MID scalar should "
							 "show EXISTS) and `Rebus.BeamShadowDebug 1` (a cube should "
							 "appear RED inside the beam region behind it)."),
						bHasStrength ? 1 : 0, bHasDebug ? 1 : 0);
				}
			}
		}

		int32 Flipped = 0;
		int32 Total = 0;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* W = Ctx.World();
			if (!W) continue;
			for (TActorIterator<ARebusFixtureActor> It(W); It; ++It)
			{
				if (ARebusFixtureActor* F = *It)
				{
					if (F->RefreshPreferProceduralBeamFromCVar(bPrefer))
					{
						++Flipped;
					}
					++Total;
				}
			}
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.PreferProceduralBeam -> %d, walked %d fixture(s) (%d visible-shaft transitions). "
				 "%s"),
			bPrefer ? 1 : 0, Total, Flipped,
			bPrefer
				? TEXT("Procedural M_RebusBeam cone is the live shaft (v1.0.96 / v1.0.99 screen-space self-shadow trace is now bound to the visible MID).")
				: TEXT("Epic MI_Beam canvas is the live shaft (v1.0.99 trace is BYPASSED until v1.0.107 ports it to Epic's beam too; use `Rebus.DumpBeamShadow` to confirm `Beam=Epic` per fixture)."));
	}),
	ECVF_Default);

// v1.0.102 -- live A/B multiplier on the LENS-MATERIAL emissive intensity, independent
// of the actual SpotLight intensity. Default 1.0 (no scaling). Operators tweak when the
// lens reads too bright/dim relative to the lit floor pool -- "the lens is blowing out
// the sensor at full dimmer" -> drop to 0.5; "the lens is barely visible against the
// beam" -> push to 2.0. Refresh sink walks every Rebus fixture in every Game/PIE world
// and re-pushes `RefreshLensEmissive()` so the new value lands without a respawn. See
// the v1.0.102 README release block for the operator checklist + clamps (the per-MID
// EmissiveIntensity is hard-capped at 100 in `RefreshLensEmissive` regardless of CVar
// value -- a guard against an accidental `Rebus.LensEmissiveScale 1e6`).
float GRebusLensEmissiveScale = 1.0f;
FAutoConsoleVariableRef CVarRebusLensEmissiveScale(
	TEXT("Rebus.LensEmissiveScale"),
	GRebusLensEmissiveScale,
	TEXT("v1.0.102 -- multiplier on the lens-material EmissiveIntensity push (default 1.0). "
		 "Operators tweak per-show to balance the lens glow against the lit floor pool; the "
		 "live SpotLight intensity is unchanged. Hard-capped at 100 inside RefreshLensEmissive "
		 "to prevent runaway exposure on a portal mis-push. Live -- changing this walks every "
		 "Rebus fixture and re-pushes the lens MIDs."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
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
					F->RefreshLensEmissive();
					++Refreshed;
				}
			}
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.LensEmissiveScale -> %.3f, refreshed %d fixture(s) (lens MIDs re-pushed)."),
			CVar->GetFloat(), Refreshed);
	}),
	ECVF_Default);

// v1.0.102 -- whether the lens-material emissive layer samples the per-fixture cookie
// `GoboRT` to show the gobo silhouette on the lens face. Default 1 (lens face shows the
// gobo while a cookie is active -- matches the v1.0.102 user request). Some shows
// prefer the lens to glow UNIFORM colour regardless of gobo state (the "lens-as-eye"
// look rather than the "projector-port" look); flipping to 0 forces `bUseGobo = 0` on
// every per-fixture lens MID so the emissive output is `Emissive * EmissiveIntensity`
// alone, regardless of `bGoboActive`. Live -- refresh sink walks every fixture.
int32 GRebusLensFollowGobo = 1;
FAutoConsoleVariableRef CVarRebusLensFollowGobo(
	TEXT("Rebus.LensFollowGobo"),
	GRebusLensFollowGobo,
	TEXT("v1.0.102 -- 0|1 (default 1). When 1, the lens-material emissive samples the live "
		 "GoboRT cookie texture so the gobo silhouette shows on the lens face while a gobo "
		 "is active (the v1.0.102 user request). When 0, the lens always glows UNIFORM colour "
		 "regardless of gobo state -- some shows prefer the cleaner 'lens-as-eye' look. Live -- "
		 "changing this walks every Rebus fixture and re-pushes the lens MIDs."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
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
					F->RefreshLensEmissive();
					++Refreshed;
				}
			}
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.LensFollowGobo -> %d, refreshed %d fixture(s) (lens MIDs re-pushed)."),
			CVar->GetInt(), Refreshed);
	}),
	ECVF_Default);

// v1.0.102 -- base intensity scale folded into the EmissiveIntensity push so a typical
// "full dimmer" lens reads as ~5x the ambient surrounding glow at the dimmer ceiling.
// Stage venue empirical default; tweak if the lens reads too bright or too dim after
// material regen. NOT a CVar (operators tune via `Rebus.LensEmissiveScale` instead --
// this is the "physical baseline" knob, while the CVar is the live "what does this
// show want" knob).
static constexpr float RebusLensEmissiveBaseScale = 5.0f;
// Hard cap on the EmissiveIntensity scalar pushed onto the lens MID, regardless of
// dimmer / CVar / base-scale product. Guards against an accidental
// `Rebus.LensEmissiveScale 1e6` exposing the post-process auto-exposure to a runaway
// value that blows the rest of the scene to white.
static constexpr float RebusLensEmissiveIntensityCap = 100.0f;

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

void ARebusFixtureActor::DumpGoboStateForDebug() const
{
	// v1.0.74 per-fixture gobo dump for the Rebus.DumpGoboState console command. Surfaces the
	// ingredients that determine whether the rotating-cookie pipeline can ghost on the floor:
	//   * CurrentGoboTexture: source bitmap (path printed so it's grep-friendly)
	//   * GoboRT: size + pointer + the clear-flag we re-assert in EnsureGoboRT (v1.0.74)
	//   * GoboAngle / spin speeds: integration is in Tick(), so a non-zero CombinedSpin with
	//     a static GoboAngle proves Tick isn't running (the gobo would look frozen, not ghosty)
	//   * SpotLight.bAllowMegaLights: must be 0 while a gobo is active so the LF flows through
	//     the legacy deferred path (the gobo-active branch of ApplyCurrentGoboToCookie sets
	//     this; a 1 here proves the per-light opt-out regressed and MegaLights' temporal
	//     denoiser is now in the path)
	//   * LightFunctionMaterial: must be GoboLightFnMID, not nullptr -- nullptr means the LF
	//     never bound and the floor wouldn't see any pattern, ghosting or otherwise
	const float CombinedSpin = CurrentGoboRotationSpeed + CurrentAnimationWheelSpeed;
	const int32 SrcW = CurrentGoboTexture ? CurrentGoboTexture->GetSizeX() : 0;
	const int32 SrcH = CurrentGoboTexture ? CurrentGoboTexture->GetSizeY() : 0;
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("DumpGoboState '%s': bGoboActive=%d srcTex=%s(%dx%d) GoboRT=%p (%dx%d clearOnUpdate=%d mips=%d filter=%d) GoboAngle=%.1fdeg goboSpd=%.3f animSpd=%.3f combined=%.3f -- SpotLight: allowMega=%d LightFn=%s LensFn-MID=%p EpicBeamMID=%p"),
		*FixtureId,
		bGoboActive ? 1 : 0,
		CurrentGoboTexture ? *CurrentGoboTexture->GetName() : TEXT("<null>"), SrcW, SrcH,
		GoboRT.Get(),
		GoboRT ? GoboRT->SizeX : 0, GoboRT ? GoboRT->SizeY : 0,
		ReadGoboRTClearOnUpdate(GoboRT), // v1.0.76: protected field, reflection reader.
		GoboRT ? (GoboRT->bAutoGenerateMips ? 1 : 0) : -1,
		GoboRT ? (int32)GoboRT->Filter : -1,
		GoboAngle,
		CurrentGoboRotationSpeed, CurrentAnimationWheelSpeed, CombinedSpin,
		SpotLight ? (SpotLight->bAllowMegaLights ? 1 : 0) : -1,
		(SpotLight && SpotLight->LightFunctionMaterial) ? *SpotLight->LightFunctionMaterial->GetName() : TEXT("<null>"),
		GoboLightFnMID.Get(),
		EpicBeamMID.Get());
}

void ARebusFixtureActor::DumpBeamShadowStateForDebug() const
{
	// v1.0.99 per-fixture screen-space-shadow-trace dump for `Rebus.DumpBeamShadow`. One line per
	// fixture so an operator can paste the whole block in a bug report.
	//
	// Two columns: the LIVE values on this fixture's BeamMID (the values the per-pixel shader
	// is actually reading right now) and the CURRENT global CVar values. They MUST agree --
	// `RefreshBeamShadowParams` runs the push at every CVar change and on each fixture build,
	// so a divergence means a portal/scene push has overridden the CVar (or the master is
	// pre-v1.0.99 and silently ignored the parameter set).
	//
	// v1.0.103 -- per-scalar EXISTS / MISSING flag.
	//   The user reported v1.0.99's fix didn't materialise: the trace still runs through
	//   occluders. Investigation showed the v1.0.99 release didn't ship a force-regen at
	//   visualiser-subsystem startup -- it only fires when `build_rebus_base_level.py`'s
	//   `ensure_beam_material()` runs (Tools > Execute Python Script). So an operator who
	//   pulls v1.0.99..v1.0.102 and opens the editor without re-running the Python script
	//   keeps the v1.0.96 cooked master with the LWC projection bug. The C++ then pushes
	//   BeamShadowStrength=1.0 onto a MID whose master never declared it, and
	//   `SetScalarParameterValue` silently no-ops -- the trace runs the v1.0.96 broken
	//   shader and concludes "always unoccluded".
	//
	//   v1.0.103 surfaces the silent no-op directly: the MID column now reports
	//   `Steps=8.0/EXISTS` vs `Steps=8.0/MISSING`, by querying
	//   `UMaterialInstanceDynamic::GetScalarParameterValue` per scalar (returns false when
	//   the master never declared it). That distinguishes "param missing" (pre-v1.0.99
	//   stale master -- the actual root cause) from "param=0" (operator turned the CVar
	//   off or the trace is otherwise mis-tuned), which the v1.0.99 -999 sentinel
	//   conflated. Pair with the new `Rebus.RebuildBeamMaterial` runtime regen command.
	if (!BeamMID)
	{
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("DumpBeamShadow '%s': BeamMID=<null> (M_RebusBeam load failed in BuildBeamCone, "
				 "or the cone build never ran -- pre-v1.0.99 master self-heal will rebuild on "
				 "next editor launch). CVars: Steps=%.1f Strength=%.3f Bias=%.2f Debug=%d "
				 "FarCullCm=%.0f EdgeGuard=%d BiasScale=%.4f. "
				 "Operator action: run `Rebus.RebuildBeamMaterial` (editor only) to regenerate "
				 "the master, then ClearScene+LoadScene from the portal (or restart the editor) "
				 "to rebuild the per-fixture BeamMID."),
			*FixtureId,
			GRebusBeamShadowSteps, GRebusBeamShadowStrength, GRebusBeamShadowBias,
			GRebusBeamShadowDebug,
			GRebusBeamShadowFarCullCm, GRebusBeamShadowEdgeGuard, GRebusBeamShadowBiasScale);
		return;
	}

	// Read back what's actually on the MID right now AND whether the master declared the
	// scalar in the first place. SetScalarParameterValue / GetScalar agree on the missing-
	// param contract: if the master never declared the scalar (pre-v1.0.96 / pre-v1.0.99 /
	// pre-v1.0.109) the get returns false. v1.0.103: surface the boolean directly per scalar
	// so an operator can tell stale-master apart from CVar/MID misconfiguration without
	// grepping for a -999 sentinel. The float value when missing is still reported (it'll be
	// whatever default the lookup wrote -- usually 0.0) so the column shape stays uniform.
	// v1.0.109: extends the read-back to the three new pan-edge / sky / far-distance guard
	// scalars so a stale (pre-v1.0.109) master is named directly in the dump.
	auto ReadMidScalar = [this](const TCHAR* Name, bool& bOutExists) -> float
	{
		float V = 0.f;
		bOutExists = BeamMID->GetScalarParameterValue(FMaterialParameterInfo(Name), V);
		return V;
	};
	bool bStepsOk = false, bStrengthOk = false, bBiasOk = false, bDebugOk = false;
	const float MidSteps    = ReadMidScalar(TEXT("BeamShadowSteps"),    bStepsOk);
	const float MidStrength = ReadMidScalar(TEXT("BeamShadowStrength"), bStrengthOk);
	const float MidBias     = ReadMidScalar(TEXT("BeamShadowBias"),     bBiasOk);
	const float MidDebug    = ReadMidScalar(TEXT("BeamShadowDebug"),    bDebugOk);
	// v1.0.109 -- pan-edge / sky / far-distance guard scalars (the v1.0.109 contract).
	bool bFarCullOk = false, bEdgeGuardOk = false, bBiasScaleOk = false;
	const float MidFarCull   = ReadMidScalar(TEXT("BeamShadowFarCullCm"), bFarCullOk);
	const float MidEdgeGuard = ReadMidScalar(TEXT("BeamShadowEdgeGuard"), bEdgeGuardOk);
	const float MidBiasScale = ReadMidScalar(TEXT("BeamShadowBiasScale"), bBiasScaleOk);

	auto Tag = [](bool bOk) -> const TCHAR* { return bOk ? TEXT("EXISTS") : TEXT("MISSING"); };

	// One-line diagnostic. v1.0.103/109:
	//   * "shadowing ENABLED" requires both Strength>0 AND Strength is actually present on
	//     the master (otherwise the shader is reading the v1.0.96 default of 0). When any
	//     scalar is MISSING we add a stale-master note + the operator-recovery command.
	//   * v1.0.109: bAnyMissing now ORs the three new guard scalars too -- a pre-v1.0.109
	//     master shows MISSING on EdgeGuard / FarCullCm / BiasScale and the stale-master
	//     note then names the v1.0.109 pan-edge fix specifically.
	const bool  bV99Missing    = !(bStepsOk && bStrengthOk && bBiasOk && bDebugOk);
	const bool  bV109Missing   = !(bFarCullOk && bEdgeGuardOk && bBiasScaleOk);
	const bool  bAnyMissing    = bV99Missing || bV109Missing;
	const bool  bShadowEnabled = bStrengthOk && (MidStrength > 0.001f);
	const int32 DebugMode      = bDebugOk ? ((MidDebug > 1.5f) ? 2 : ((MidDebug > 0.5f) ? 1 : 0)) : 0;
	const TCHAR* MasterStaleNote = bAnyMissing
		? (bV99Missing
			? TEXT(" -- STALE MASTER (v1.0.99 scalars MISSING; the master predates v1.0.99 and the LWC projection fix DID NOT LAND -- the trace runs the broken v1.0.96 shader). Operator action: run `Rebus.RebuildBeamMaterial` (editor only) then ClearScene+LoadScene OR restart the editor")
			: TEXT(" -- STALE MASTER (v1.0.109 pan-edge guard scalars MISSING; the master predates the v1.0.109 sky / far-cull / edge-toggle / distance-scaled-bias fix -- the trace runs the v1.0.99 shader that causes the pan-edge beam clipping the user reported against v1.0.106). Operator action: run `Rebus.RebuildBeamMaterial` (editor only) then ClearScene+LoadScene OR restart the editor"))
		: TEXT("");

	// v1.0.106 -- report which beam path is currently rendering on this fixture so the
	// operator can tell at a glance whether the v1.0.96 / v1.0.99 shadow trace is even
	// LIVE on the visible MID:
	//   * Beam=Procedural -- M_RebusBeam is the visible shaft (the MID column above IS the
	//     material the per-pixel shader is reading; shadow scalars actually matter).
	//   * Beam=Epic       -- Epic's MI_Beam is the visible shaft; the MID column above
	//     describes the HIDDEN procedural cone (the v1.0.96..v1.0.103 shadow trace work
	//     has been editing a hidden material -- the v1.0.106 diagnosis). The trace
	//     SCALARS are still pushed onto the procedural BeamMID for parity, but they have
	//     no on-screen effect until the operator flips `Rebus.PreferProceduralBeam 1`
	//     (default since v1.0.106) OR the v1.0.107 follow-up ports the trace to Epic's
	//     beam too.
	// `Prefer=N/Y` also reported so the operator can confirm the per-fixture override
	// flag matches the live visible path (a mismatch indicates the operator pushed the
	// CVar but a sibling editor pin holds an instance override -- mirrors the
	// `BeamConeRadiusScale` per-fixture override pattern in `DumpFixtureZoom`).
	const TCHAR* BeamPath = bUsingEpicBeam ? TEXT("Epic") : TEXT("Procedural");
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("DumpBeamShadow '%s' Beam=%s Prefer=%s "
			 "MID(Steps=%.1f/%s Strength=%.3f/%s Bias=%.2f/%s Debug=%d/%s "
				 "FarCullCm=%.0f/%s EdgeGuard=%d/%s BiasScale=%.4f/%s) "
			 "CVars(Steps=%.1f Strength=%.3f Bias=%.2f Debug=%d "
				 "FarCullCm=%.0f EdgeGuard=%d BiasScale=%.4f) "
			 "-- shadowing %s, debug mode %d%s%s."),
		*FixtureId, BeamPath, bPreferProceduralBeam ? TEXT("Y") : TEXT("N"),
		MidSteps, Tag(bStepsOk),
		MidStrength, Tag(bStrengthOk),
		MidBias, Tag(bBiasOk),
		(int32)MidDebug, Tag(bDebugOk),
		MidFarCull, Tag(bFarCullOk),
		(int32)MidEdgeGuard, Tag(bEdgeGuardOk),
		MidBiasScale, Tag(bBiasScaleOk),
		GRebusBeamShadowSteps, GRebusBeamShadowStrength, GRebusBeamShadowBias, GRebusBeamShadowDebug,
		GRebusBeamShadowFarCullCm, GRebusBeamShadowEdgeGuard, GRebusBeamShadowBiasScale,
		bShadowEnabled ? TEXT("ENABLED") : TEXT("DISABLED"),
		DebugMode, MasterStaleNote,
		bUsingEpicBeam
			? TEXT(" -- WARNING: Beam=Epic means the v1.0.96 / v1.0.99 / v1.0.109 shadow trace is "
				   "BYPASSED on this fixture (the trace lives on M_RebusBeam, not on M_Beam_Master). "
				   "The MID column above describes the HIDDEN procedural cone. To make the trace "
				   "render, flip `Rebus.PreferProceduralBeam 1` (default since v1.0.106) -- the "
				   "procedural cone becomes the visible shaft. Epic-beam parity for the trace is "
				   "queued for v1.0.150+ as a class-of-lift (mirroring the v1.0.96..v1.0.106 work "
				   "on Epic's M_Beam_Master).")
			: TEXT(""));
}

void ARebusFixtureActor::DumpIesStateForDebug() const
{
	// v1.0.91 per-fixture IES dump for `Rebus.DumpFixtureIes`. Surfaces the complete chain so
	// the operator can confirm in ONE line:
	//   * which IES profile is loaded (inline iesText vs URL, plus the source profileId),
	//   * the zoomDmx that selected it (paired with the live zoom half-angle for cross-check),
	//   * the IESTexture UObject name (proves SpotLight->SetIESTexture landed),
	//   * the PEAK CANDELA parsed from the .ies file (drives Intensity, see RefreshIntensity),
	//   * the SpotLight's IntensityUnits (must be `2` = Candelas for the cd values to be
	//     physically meaningful -- BuildSpotLight sets this once at construction),
	//   * the live Intensity that just landed, plus the formula breakdown so the operator
	//     can see whether dimmer or shutter-gate are zeroing it out vs. the IES being missing.
	// All fields read from the LIVE SpotLight state (no cached snapshot) so a value mid-fade
	// is reported accurately. Counts of inline / URL IES entries are included so the operator
	// can tell whether the portal ever pushed a profile in the first place.
	if (!SpotLight)
	{
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("DumpFixtureIes '%s': NO SpotLight component -- fixture not fully constructed."),
			*FixtureId);
		return;
	}

	float Gate = 1.f;
	switch (ShutterMode)
	{
	case ERebusShutterMode::Closed: Gate = 0.f; break;
	case ERebusShutterMode::Strobe: Gate = (ShutterPhase < 0.5f) ? 1.f : 0.f; break;
	default: break;
	}
	const float Dim = FMath::Clamp(Dimmer.Current, 0.f, 1.f);
	const float Cd  = (IesCandelaMax >= 0.f) ? IesCandelaMax : BaseCandela;
	const float Expected = Cd * Dim * Gate;
	const TCHAR* Src =
		(!ActiveIesProfile)                  ? TEXT("none(synthetic-cone)") :
		bActiveIesInline                     ? TEXT("inline") :
		                                       TEXT("url");

	UE_LOG(LogRebusVisualiser, Log,
		TEXT("DumpFixtureIes '%s' source=%s profileId='%s' zoomDmx=%d zoomHalfDeg=%.2f "
			 "iesTexture=%s candelaMax=%.1f baseCandela(flux-derived)=%.1f -> activeBase=%.1f "
			 "SpotLight intensityUnits=%d intensityLive=%.1f expected(=base*dim*gate)=%.1f "
			 "dimmer=%.3f shutterMode=%d gate=%.2f inlineCount=%d urlCount=%d "
			 "bUseIESBrightness=%d IESBrightnessScale=%.3f"),
		*FixtureId, Src,
		ActiveIesProfileId.IsEmpty() ? TEXT("<none>") : *ActiveIesProfileId,
		CurrentIesZoomDmx, ZoomDeg.Current,
		ActiveIesProfile ? *ActiveIesProfile->GetName() : TEXT("<null>"),
		IesCandelaMax, BaseCandela, Cd,
		(int32)SpotLight->IntensityUnits, SpotLight->Intensity, Expected,
		Dim, (int32)ShutterMode, Gate,
		InlineIes.Profiles.Num(), Profile.IesProfiles.Num(),
		SpotLight->bUseIESBrightness ? 1 : 0, SpotLight->IESBrightnessScale);
}

void ARebusFixtureActor::DumpFixtureZoomStateForDebug() const
{
	// v1.0.101 per-fixture zoom / cone-mesh / SpotLight outer-cone dump for the new
	// `Rebus.DumpFixtureZoom` console command. Surfaces the entire single-source-of-truth
	// chain in one line so the operator can confirm:
	//   * the live wire half-angle target (ZoomDeg.Current; pre v1.0.84 this WAS the wire,
	//     post v1.0.84 the wire is full-angle and SetFixtureZoom halves it before storing),
	//   * the GDTF zoom range from the profile (Profile.Zoom.MinDeg/MaxDeg, "n/a" when the
	//     profile didn't carry a Zoom payload -- the helper then falls back to the global
	//     [0.5, 80] safe clamp),
	//   * the canonical resolved half-angle (ResolveZoomHalfDeg(Current * 2) -- the
	//     SINGLE source of truth that drives both the SpotLight outer cone and the
	//     visible cone-mesh radius),
	//   * the SpotLight's LIVE OuterConeAngle + InnerConeAngle (must equal the resolved
	//     half within float precision; an InnerCone substantially smaller than the outer
	//     is what makes the visible bright disc on the floor read smaller than the
	//     geometric cone -- the user's "slightly larger than the footprint" report; see
	//     the BeamConeRadiusScale UPROPERTY rationale in the header for the math),
	//   * the procedural cone-mesh BeamLength + last-built far-radius + per-fixture
	//     BeamConeRadiusScale (the operator-facing knob),
	//   * the BeamMID's live FarRadius scalar param read back from the MID (proves the
	//     UpdateBeamConeGeometry push won the race against any portal/scene-property
	//     override; mirrors `Rebus.DumpBeamShadow`'s read-back pattern).
	if (!SpotLight)
	{
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("DumpFixtureZoom '%s': NO SpotLight component -- fixture not fully constructed."),
			*FixtureId);
		return;
	}

	const float ResolvedHalf = ResolveOuterHalfDeg();
	const float MatchHalf    = ResolveBeamFootprintMatchHalfDeg();
	const float FootprintInnerRatio = ResolveFootprintInnerRatio();
	const float TanMatch     = FMath::Tan(FMath::DegreesToRadians(MatchHalf));
	const float ConeScale    = FMath::Max(0.05f, BeamConeRadiusScale);
	const float ExpectedFar  = FMath::Max(BeamLengthUnreal * TanMatch * ConeScale,
	                                      BeamBaseRadiusUnreal + 0.1f);

	float MidFarRadius = -999.f;
	float MidSharpness = -999.f;
	float MidDensity   = -999.f;
	float MidFalloff   = -999.f;
	if (BeamMID)
	{
		BeamMID->GetScalarParameterValue(FMaterialParameterInfo(TEXT("FarRadius")), MidFarRadius);
		BeamMID->GetScalarParameterValue(FMaterialParameterInfo(TEXT("BeamSharpness")), MidSharpness);
		BeamMID->GetScalarParameterValue(FMaterialParameterInfo(TEXT("BeamDensity")), MidDensity);
		BeamMID->GetScalarParameterValue(FMaterialParameterInfo(TEXT("BeamFalloff")), MidFalloff);
	}

	const FString ZoomRange = Profile.Zoom.bValid
		? FString::Printf(TEXT("[%.2f..%.2fdeg full]"), (float)Profile.Zoom.MinDeg, (float)Profile.Zoom.MaxDeg)
		: FString(TEXT("<no profile zoom range>"));

	// v1.0.108 -- the dump now reports BOTH the geometric outer half AND the half-
	// intensity match half (MatchHalf = OuterHalf * (1+InnerRatio)/2 -- the angle the
	// visible cone-mesh is now sized to so its edge coincides with the bright floor
	// disc), the SpotLight's actual InnerCone/OuterCone (which the half-intensity
	// edge derives from), the per-fixture cone-mesh FarRadius BUILT vs EXPECTED at
	// MatchHalf, AND the live radial-attenuation MID scalars (BeamSharpness /
	// BeamDensity / BeamFalloff) so an operator can confirm the v1.0.108 push won the
	// race against any portal/scene-property override AND tune-correlate the
	// `Rebus.BeamSharpness` / `Rebus.BeamDensity` / `Rebus.BeamFalloff` CVar values
	// against what the BeamMID is actually rendering with.
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("DumpFixtureZoom '%s' zoomTarget=%.2fdeg(half) profileZoomRange=%s "
			 "resolvedHalf=%.2fdeg matchHalf=%.2fdeg(footprintInnerRatio=%.3f -> visible-shaft edge target) | "
			 "spotOuterCone=%.2fdeg spotInnerCone=%.2fdeg (InnerRatio=%.3f) | "
			 "beamLength=%.1fcm coneFarRadiusBuilt=%.1fcm coneFarRadiusExpected=%.1fcm "
			 "BeamMID.FarRadius=%.1f | BeamConeRadiusScale=%.3f | "
			 "BeamMID.Sharpness=%.3f BeamMID.Density=%.4f BeamMID.Falloff=%.3f | "
			 "bUsingEpicBeam=%d bMeshBeamEnabled=%d bGoboActive=%d iris=%.3f"),
		*FixtureId,
		ZoomDeg.Current, *ZoomRange,
		ResolvedHalf, MatchHalf, FootprintInnerRatio,
		SpotLight->OuterConeAngle, SpotLight->InnerConeAngle,
		(SpotLight->OuterConeAngle > KINDA_SMALL_NUMBER)
			? (SpotLight->InnerConeAngle / SpotLight->OuterConeAngle) : 0.f,
		BeamLengthUnreal, BeamConeLastFarRadius, ExpectedFar,
		MidFarRadius,
		ConeScale,
		MidSharpness, MidDensity, MidFalloff,
		bUsingEpicBeam ? 1 : 0,
		bMeshBeamEnabled ? 1 : 0,
		bGoboActive ? 1 : 0,
		FMath::Clamp(Iris.Current, 0.f, 1.f));
}

void ARebusFixtureActor::RefreshBeamConeRadiusScaleFromCVar(float NewScale)
{
	// v1.0.101 -- pick up a live `Rebus.BeamConeRadiusScale` change without a respawn.
	// Assigns the per-fixture UPROPERTY first so the rest of the per-fixture state
	// (BeamConeRadiusScale read by UpdateBeamConeGeometry / UpdateEpicBeamParams /
	// DumpFixtureZoomStateForDebug / ApplyZoom) sees the new value immediately. The
	// rebuild gate in UpdateBeamConeGeometry skips when the far-radius is essentially
	// unchanged (sub-half-cm). Forcing the gate through `BeamConeLastFarRadius = -1`
	// makes the next call regenerate the frustum vertices + re-push the BeamMID
	// FarRadius scalar at the new scale, regardless of whether the half-angle moved.
	// Also re-pushes the Epic-beam canvas DMX Zoom (so the Epic-path scale change is
	// picked up too -- the procedural cone is the M_RebusBeam fallback, hidden when
	// bUsingEpicBeam from v1.0.95). Idempotent / safe when no BeamCone yet
	// (BuildBeamCone reads BeamConeRadiusScale on its own initial build).
	BeamConeRadiusScale  = FMath::Max(0.05f, NewScale);
	BeamConeLastFarRadius = -1.f;
	UpdateBeamConeGeometry();
	if (bUsingEpicBeam)
	{
		UpdateEpicBeamParams();
	}
}

bool ARebusFixtureActor::RefreshPreferProceduralBeamFromCVar(bool bNewPrefer)
{
	// v1.0.106 -- pick up a live `Rebus.PreferProceduralBeam` change without a respawn.
	// Assigns the per-fixture UPROPERTY first so a subsequent BuildBeamCone (e.g. on
	// re-spawn from ClearScene+LoadScene) inherits the new value at seed time. The
	// visible-shaft flip below is a pure SetVisibility toggle on `EpicBeamComp` +
	// `BeamCone` -- the components stay alive in the scene either way, so re-toggling is
	// cheap and idempotent. Returns true when the visible shaft actually transitioned
	// (so the CVar refresh sink can count + log how many fixtures changed paths).
	//
	// Idempotence: the no-op early-return checks BOTH the cached preference flag AND the
	// live `bUsingEpicBeam` -- so a fixture whose preference matches but whose visible
	// path got out of sync (e.g. the very first `Rebus.PreferProceduralBeam 0` push after
	// boot where `bPreferProceduralBeam` defaulted true but `EpicBeamComp` was never
	// built) still re-runs the build/show path.
	const bool bWantEpicVisible = !bNewPrefer;
	if (bPreferProceduralBeam == bNewPrefer && bUsingEpicBeam == bWantEpicVisible)
	{
		return false;
	}
	bPreferProceduralBeam = bNewPrefer;

	if (bNewPrefer)
	{
		// Switch TO procedural: hide Epic canvas (kept alive so future flip-back is cheap);
		// unhide the procedural cone if the operator hasn't explicitly disabled mesh beams.
		// Re-push the screen-space shadow trace scalars so the v1.0.96 / v1.0.99 BeamShadow*
		// parameters land on the now-visible procedural BeamMID -- the whole point of the
		// v1.0.106 flip is that the trace ACTUALLY renders, which requires the scalars on
		// the visible MID. `RefreshBeamShadowParams` is silently no-op when BeamMID is null
		// (the M_RebusBeam load failed in BuildBeamCone -- the cone-mesh build path errors
		// loudly there; this flip is a benign no-op in that degenerate case).
		if (EpicBeamComp)
		{
			EpicBeamComp->SetVisibility(false);
			EpicBeamComp->SetHiddenInGame(true);
		}
		bUsingEpicBeam = false;
		if (BeamCone)
		{
			BeamCone->SetVisibility(bMeshBeamEnabled);
		}
		RefreshBeamShadowParams();
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Fixture %s beam: PreferProceduralBeam 1 -- Epic canvas hidden, procedural "
				 "M_RebusBeam visible (v1.0.96 / v1.0.99 self-shadow trace scalars re-pushed "
				 "onto BeamMID)."), *FixtureId);
		return true;
	}

	// Switch TO Epic: unhide existing canvas if we built one earlier; else build it lazily.
	// `TryBuildEpicBeam` returns false (with a logged Warning naming the missing asset path)
	// when the DMX Fixtures plugin content isn't installed -- we then stay on the procedural
	// cone and surface that fact in the log so the operator knows the toggle had no effect.
	bool bEpicReady = false;
	if (EpicBeamComp)
	{
		EpicBeamComp->SetVisibility(bMeshBeamEnabled);
		EpicBeamComp->SetHiddenInGame(false);
		bEpicReady = true;
	}
	else
	{
		bEpicReady = TryBuildEpicBeam();
	}
	bUsingEpicBeam = bEpicReady;
	if (BeamCone)
	{
		// Hide the procedural cone only when Epic's canvas is actually visible -- if the DMX
		// content is missing we MUST keep the procedural shaft visible (otherwise the fixture
		// loses its beam entirely).
		BeamCone->SetVisibility(bMeshBeamEnabled && !bUsingEpicBeam);
	}
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s beam: PreferProceduralBeam 0 -- %s."),
		*FixtureId,
		bUsingEpicBeam
			? TEXT("Epic MI_Beam canvas visible (v1.0.96 / v1.0.99 self-shadow trace is BYPASSED "
				   "until the v1.0.107 follow-up ports the trace to Epic's beam too)")
			: TEXT("Epic DMX Fixtures content NOT installed -- procedural cone remains the "
				   "visible shaft (toggle had no effect on this fixture's visible beam)"));
	return true;
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

	// v1.0.71 fixture body/lens material override -- cook-safe hard refs.
	//
	// FixtureMatParent (default parametric parent): /Engine/BasicShapes/BasicShapeMaterial.
	// This material ships with every UE install (no new content required to make v1.0.71 work
	// out-of-the-box) AND exposes the standard PBR parameter names Color (vector), Metallic
	// (scalar), Roughness (scalar) that EnsureFixtureMIDs writes. If the parameter names ever
	// change in a future engine version the SetVectorParameterValue / SetScalarParameterValue
	// calls become benign no-ops and the MID renders as the parent's default look -- not ideal
	// but never crashes / never breaks the build.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> FixtureMatParentFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (FixtureMatParentFinder.Succeeded()) FixtureMatParent = FixtureMatParentFinder.Object;
	// Optional user-override assets at /Game paths. If present these take precedence over the
	// runtime MIDs (the override IS the user's material verbatim, no parameter mangling). Both
	// are optional -- missing path just leaves the ref null and EnsureFixtureMIDs falls back to
	// the BasicShapeMaterial MID. To create them: in the editor, right-click /Game/REBUS/
	// Materials, New -> Material, save as M_RebusFixtureBody (or M_RebusFixtureLens), configure
	// any PBR shading you want, no parameter naming requirement.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> UserBodyMatFinder(
		TEXT("/Game/REBUS/Materials/M_RebusFixtureBody.M_RebusFixtureBody"));
	if (UserBodyMatFinder.Succeeded()) FixtureBodyMaterialOverride = UserBodyMatFinder.Object;
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> UserLensMatFinder(
		TEXT("/Game/REBUS/Materials/M_RebusFixtureLens.M_RebusFixtureLens"));
	if (UserLensMatFinder.Succeeded())
	{
		FixtureLensMaterialOverride = UserLensMatFinder.Object;
	}
	else
	{
		// v1.0.95: warn the operator at construction when the v1.0.93 Python-baked
		// `M_RebusFixtureLens` material is missing. The Epic-beam lens (synthetic `LensDisc`
		// AND every real `<Beam>` `IsBeamLensComponents` PMC) drives off this material; when
		// it's absent the lens silently falls back to the runtime BasicShapeMaterial MID,
		// which renders as a flat untextured disc instead of the mirror/glass disc the user
		// expects. Run `build_rebus_base_level.py` from the editor (Tools > Execute Python
		// Script) so the startup hook bakes the master, or restart the editor.
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("M_RebusFixtureLens not found at /Game/REBUS/Materials/M_RebusFixtureLens -- the v1.0.93 Python builder hasn't baked it yet. Run Tools > Execute Python Script > build_rebus_base_level.py (or restart the editor so the startup hook bakes the master). The Epic-beam lens will fall back to the runtime BasicShapeMaterial MID until then."));
	}
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
	BuildLensDisc();          // emissive lens-flare disc at the beam origin (synthetic fallback)
	BuildIsBeamLensFlares();  // v1.0.88: per-isBeam-mesh emissive flare disc (real <Beam> path)
	BuildBeamCone();          // hybrid cone-mesh volumetric beam (sized to IES + lens, rides the head)
	// v1.0.88: synchronise lens-visuals visibility. The synthetic LensDisc is hidden when at
	// least one isBeam mesh was built (real geometry wins); the per-beam flares are hidden when
	// the operator has forced the synthetic fallback via `Rebus.ForceSyntheticLensFallback 1`.
	// Reads the GLOBAL CVar value so a freshly-spawned fixture inherits the live A/B setting.
	{
		if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Rebus.ForceSyntheticLensFallback")))
		{
			bUseSyntheticLensFallback = (CVar->GetInt() != 0);
		}
	}
	RefreshIsBeamLensVisuals();
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

// ---- v1.0.71 fixture body/lens material override ----------------------------------------

bool ARebusFixtureActor::IsLensToken(const FString& Token)
{
	// Case-insensitive substring scan for the common front-optic naming conventions across
	// GDTF (Lens / Front Lens / Optic), glb importers (lens, glass), and decorative meshes
	// (crystal-style PAR lenses). Kept loose on purpose -- a false positive at worst slaps a
	// mirrored material on a small accent, which the user can disable per-fixture via the
	// SetFixtureMaterialOverrideEnabled hook.
	if (Token.IsEmpty()) return false;
	const FString T = Token.ToLower();
	return T.Contains(TEXT("lens"))
		|| T.Contains(TEXT("glass"))
		|| T.Contains(TEXT("crystal"))
		|| T.Contains(TEXT("optic"))
		|| T.Contains(TEXT("front"));
}

void ARebusFixtureActor::EnsureFixtureMIDs()
{
	// Lazy build, called from ApplyFixtureMaterialTo. We do NOTHING if user-override .uassets
	// were found in the constructor (FixtureBodyMaterialOverride / LensOverride non-null) --
	// those take precedence and we apply them verbatim. Otherwise we create the parametric MIDs
	// off BasicShapeMaterial once per actor.
	if (!FixtureMatParent) return;

	if (!FixtureBodyMaterialOverride && !FixtureBodyMID)
	{
		FixtureBodyMID = UMaterialInstanceDynamic::Create(FixtureMatParent, this);
		if (FixtureBodyMID)
		{
			// Black satin plastic: near-black diffuse + dielectric + medium-low roughness for
			// the soft sheen highlight you get on injection-moulded fixture housings. NOT pure
			// black (0,0,0) -- that crushes contrast under stage lights so it never reads as
			// "satin"; #050505 gives just enough surface to catch the sheen.
			FixtureBodyMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.02f, 0.02f, 0.02f, 1.f));
			FixtureBodyMID->SetScalarParameterValue(TEXT("Metallic"), 0.0f);
			FixtureBodyMID->SetScalarParameterValue(TEXT("Roughness"), 0.35f);
		}
	}
	if (!FixtureLensMaterialOverride && !FixtureLensMID)
	{
		FixtureLensMID = UMaterialInstanceDynamic::Create(FixtureMatParent, this);
		if (FixtureLensMID)
		{
			// Mirrored glass: near-white "polished chrome" colour + fully metallic + ZERO
			// roughness for a perfect mirror highlight (v1.0.89 -- user requested fully-
			// reflective). True dielectric glass needs translucent shading which
			// BasicShapeMaterial doesn't support -- but a fixture LENS in a venue reads
			// visually as a chrome mirror under stage light (because the dark interior absorbs
			// the back), so metallic-mirror is the right approximation here. Roughness=0 gives
			// a razor-sharp specular response that picks up the surrounding rig and is the
			// hallmark "polished optic" look the operator expects.
			FixtureLensMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.95f, 0.95f, 0.95f, 1.f));
			FixtureLensMID->SetScalarParameterValue(TEXT("Metallic"), 1.0f);
			FixtureLensMID->SetScalarParameterValue(TEXT("Roughness"), 0.0f);
		}
	}
}

bool ARebusFixtureActor::ApplyFixtureMaterialTo(UPrimitiveComponent* Comp, const FString& MeshName,
	const FString& GeomName, bool bIsOrbitComp)
{
	if (!bOverrideFixtureMaterials || !Comp) return false;
	EnsureFixtureMIDs();

	const bool bLens = IsLensToken(MeshName) || IsLensToken(GeomName);
	UMaterialInterface* Mat = bLens
		? (FixtureLensMaterialOverride ? FixtureLensMaterialOverride.Get() : Cast<UMaterialInterface>(FixtureLensMID))
		: (FixtureBodyMaterialOverride ? FixtureBodyMaterialOverride.Get() : Cast<UMaterialInterface>(FixtureBodyMID));
	if (!Mat) return false;

	// Cache the original material slot 0 the FIRST time we override this component, so
	// SetFixtureMaterialOverrideEnabled(false) can restore it byte-exact. For control-channel
	// meshes the cache is index-aligned to MeshComponents; for Orbit comps it's a per-weak-ptr
	// map keyed off the component pointer.
	if (bIsOrbitComp)
	{
		TWeakObjectPtr<USceneComponent> Key(Cast<USceneComponent>(Comp));
		if (!OriginalOrbitMaterials.Contains(Key))
		{
			OriginalOrbitMaterials.Add(Key, TWeakObjectPtr<UMaterialInterface>(Comp->GetMaterial(0)));
		}
	}
	else
	{
		const int32 Idx = MeshComponents.IndexOfByKey(Cast<UProceduralMeshComponent>(Comp));
		if (Idx != INDEX_NONE)
		{
			if (!OriginalMeshMaterials.IsValidIndex(Idx))
			{
				OriginalMeshMaterials.SetNum(MeshComponents.Num());
			}
			if (!OriginalMeshMaterials[Idx])
			{
				OriginalMeshMaterials[Idx] = Comp->GetMaterial(0);
			}
		}
	}

	Comp->SetMaterial(0, Mat);
	return true;
}

ARebusFixtureActor::FFixtureMaterialApplyCount ARebusFixtureActor::SetFixtureMaterialOverrideEnabled(bool bEnabled)
{
	FFixtureMaterialApplyCount Count;
	bOverrideFixtureMaterials = bEnabled;

	if (bEnabled)
	{
		// Re-apply across all owned meshes + Orbit comps. The classifier (lens vs body) repeats
		// because MeshName/GeomName aren't cached per-component -- but the control-channel
		// meshes carry the GDTF name in MeshComponents' parallel arrays only at build time, so
		// we re-derive from the procedural mesh's Outer/Name (best-effort) which is enough for
		// the "lens" keyword scan. Orbit comps use their tags + name as before.
		for (int32 i = 0; i < MeshComponents.Num(); ++i)
		{
			UProceduralMeshComponent* PMC = MeshComponents[i];
			if (!PMC) continue;
			const FString Nm = PMC->GetName(); // generic if the build path didn't set one
			if (ApplyFixtureMaterialTo(PMC, Nm, FString(), /*bIsOrbitComp*/ false))
			{
				if (IsLensToken(Nm)) ++Count.Lens; else ++Count.Body;
			}
		}
		for (const TWeakObjectPtr<USceneComponent>& Weak : OrbitComponents)
		{
			USceneComponent* SC = Weak.Get();
			UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(SC);
			if (!Prim) continue;
			// Concatenate every tag into the geom-name slot so IsLensToken sees them all.
			FString TagStr;
			for (const FName& Tag : SC->ComponentTags) { TagStr += Tag.ToString(); TagStr += TEXT(" "); }
			if (ApplyFixtureMaterialTo(Prim, SC->GetName(), TagStr, /*bIsOrbitComp*/ true))
			{
				if (IsLensToken(SC->GetName()) || IsLensToken(TagStr)) ++Count.Lens; else ++Count.Body;
			}
		}
	}
	else
	{
		// Restore originals from the caches.
		for (int32 i = 0; i < MeshComponents.Num(); ++i)
		{
			UProceduralMeshComponent* PMC = MeshComponents[i];
			if (!PMC || !OriginalMeshMaterials.IsValidIndex(i)) continue;
			if (UMaterialInterface* Orig = OriginalMeshMaterials[i])
			{
				PMC->SetMaterial(0, Orig);
				++Count.Restored;
			}
			else
			{
				// No cache (never overridden) -- nothing to restore; leave whatever's there.
			}
		}
		for (const TPair<TWeakObjectPtr<USceneComponent>, TWeakObjectPtr<UMaterialInterface>>& Pair : OriginalOrbitMaterials)
		{
			USceneComponent* SC = Pair.Key.Get();
			UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(SC);
			if (!Prim) continue;
			Prim->SetMaterial(0, Pair.Value.Get()); // may be null = engine default; weak miss is benign
			++Count.Restored;
		}
	}

	return Count;
}

void ARebusFixtureActor::BuildMeshes(const FRebusMeshBundle& Meshes)
{
	// v1.0.88 self-heal forward-compat diagnostic: one log line per fixture surfacing the
	// mesh-blob version + count so a grep on startup proves whether v3 blobs (carrying the new
	// isBeam / geometryType fields) are arriving end-to-end or whether the portal is still
	// emitting v2. Older blobs simply omit the fields -- the synthetic-disc fallback path takes
	// over and no isBeam meshes are detected below.
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s: MeshBundle version %d, %d mesh(es)."),
		*FixtureId, Meshes.Version, Meshes.Meshes.Num());

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

		// v1.0.71: black satin body / mirrored glass lens override. Uses Mesh.Name + Mesh.
		// GeometryName as the lens-keyword source (the real GDTF naming, not the generic
		// procedural-mesh comp name we'd have in SetFixtureMaterialOverrideEnabled's re-apply
		// path). ApplyFixtureMaterialTo is a no-op when bOverrideFixtureMaterials is false,
		// so opting out via the console command leaves the procedural-mesh default in place.
		ApplyFixtureMaterialTo(PMC, Mesh.Name, Mesh.GeometryName, /*bIsOrbitComp*/ false);

		// v1.0.88: AUTHORITATIVE lens-disc detection from the mesh-blob v3 isBeam flag. The
		// portal stamps this field from the raw GDTF XML type (geometryType == "Beam"), so it
		// is the only path that survives a fixture whose <Beam> node is named anything other
		// than literally "Beam" -- the pre-v1.0.88 IsLensToken path was a best-effort keyword
		// scan and missed many of those. For each isBeam mesh:
		//   1. Cache the procedural-mesh default material BEFORE the lens override so the
		//      synthetic-fallback toggle can revert to "no mirror" cleanly.
		//   2. Tag the component so a grep ('RebusIsBeamLens' in component tags) confirms the
		//      flag landed. Helps the next portal-team debug round.
		//   3. Force the mirror/glass FixtureLensMaterialOverride (.uasset, when the user
		//      authored one at /Game/REBUS/Materials/M_RebusFixtureLens) or the runtime
		//      FixtureLensMID (Metallic=1, Roughness=0 -- v1.0.89 fully-reflective) onto EVERY material slot the
		//      procedural mesh exposes (GetNumMaterials() may be >1 for fan-triangulated
		//      multi-section beams). When BOTH are null (the v1.0.71 mat-parent missed too)
		//      we log a one-shot Warning and leave the mesh's default material -- so a
		//      missing-asset deployment does not crash, just looks like the pre-v1.0.71
		//      procedural-mesh default.
		//   4. Record the component in IsBeamLensComponents (cap on the WEAK arrays grows in
		//      lockstep with IsBeamFlareDiscs once BuildIsBeamLensFlares runs after BuildLens
		//      Disc -- both arrays stay index-aligned so the synthetic-fallback toggle can
		//      flip them in pairs).
		// IMPORTANT: this block deliberately leaves the existing axis bucketing UNTOUCHED. The
		// isBeam mesh is bucketed by RebusMotion::ResolveAxisForMesh(GeometryName, ModelName)
		// above -- the GDTF <Beam> node lives under the tilt axis, so the mesh already
		// pans/tilts with the head. Special-casing it out of the bucket map (e.g. forcing
		// HeadAxisIndex) would detach the lens from the head and the user-doc explicitly
		// warned about this regression. The isBeam flag is an identification hint, never a
		// motion override.
		if (Mesh.bIsBeam)
		{
			// (1) Tag for grep + outside-of-actor lookups (e.g. truss-pass material exclusion).
			PMC->ComponentTags.AddUnique(FName(TEXT("RebusIsBeamLens")));

			// (2) Resolve the mirror/glass material. Re-uses the v1.0.71 lens-material
			// pipeline: prefer the user override .uasset, fall back to the runtime MID.
			EnsureFixtureMIDs();
			UMaterialInterface* LensMat = FixtureLensMaterialOverride
				? FixtureLensMaterialOverride.Get()
				: Cast<UMaterialInterface>(FixtureLensMID);
			if (LensMat)
			{
				const int32 NumSlots = FMath::Max(1, PMC->GetNumMaterials());
				for (int32 Slot = 0; Slot < NumSlots; ++Slot)
				{
					PMC->SetMaterial(Slot, LensMat);
				}
			}
			else
			{
				UE_LOG(LogRebusVisualiser, Warning,
					TEXT("Fixture %s isBeam mesh[%d] geom='%s' name='%s': lens material is null (FixtureLensMaterialOverride + FixtureLensMID both unresolved) -- leaving procedural-mesh default. Check /Game/REBUS/Materials/M_RebusFixtureLens or /Engine/BasicShapes/BasicShapeMaterial."),
					*FixtureId, ComponentIndex, *Mesh.GeometryName, *Mesh.Name);
			}

			// v1.0.95: explicit visibility assertion. The PMC default is "visible", but
			// some scene-load orderings can leave the lens hidden in a cooked / packaged
			// path (bHiddenInGame flicker on procedural meshes during world reload). Force
			// the real <Beam> lens disc visible at construction so Epic-beam mode ALWAYS
			// shows the lens object the user asked
			// to see -- `RefreshIsBeamLensVisuals` then governs the synthetic-fallback toggle.
			PMC->SetVisibility(true, /*bPropagateToChildren*/ true);
			PMC->SetHiddenInGame(false);

			// (3) Record for the per-beam flare build + the visibility toggle.
			IsBeamLensComponents.Add(TWeakObjectPtr<UPrimitiveComponent>(PMC));
		}

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

	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s: built %d mesh proxies (%d tagged isBeam -- %s)."),
		*FixtureId, MeshComponents.Num(), IsBeamLensComponents.Num(),
		IsBeamLensComponents.Num() > 0
			? TEXT("real <Beam> geometry will be the lens disc; synthetic disc hidden")
			: TEXT("no isBeam meshes; synthetic lens disc fallback in force"));

	// v1.0.102: wrap each isBeam mesh's slot-0 lens material in a per-component MID so
	// `RefreshLensEmissive` can drive THIS fixture's lens independently of every other
	// fixture's lens. Must run AFTER the isBeam-mesh loop above has stamped the shared
	// chrome material onto slot 0 of every PMC -- CreateAndSetMaterialInstanceDynamic
	// inherits from the slot's current parent. RefreshLensEmissive is also called from
	// Setup() after BuildMeshes (via the unified RefreshIntensity entry-point at the end
	// of Setup) so the initial dimmer / colour / shutter state lands on the new MIDs.
	EnsurePerLensMIDs();
}

void ARebusFixtureActor::BuildSpotLight()
{
	SpotLight = NewObject<USpotLightComponent>(this, TEXT("SpotLight"));
	SpotLight->SetupAttachment(FixtureRoot);
	SpotLight->RegisterComponent();
	SpotLight->SetMobility(EComponentMobility::Movable);
	SpotLight->SetIntensityUnits(ELightUnits::Candelas);
	SpotLight->SetAttenuationRadius(6000.f);

	// v1.0.95: layered volumetric model. The cone-mesh raymarch (`M_RebusBeam` on BeamCone) is
	// the DENSE visible shaft -- crisp, shadow-occluded against SceneDepth, depth-faded against
	// solid geometry. The SpotLight's per-light VolumetricScatteringIntensity is the SEPARATE
	// SOFT fog-interaction layer that gets CARVED by occluders thanks to `bCastVolumetricShadow
	// = true` (set just below). Pre-v1.0.95 this was 0 in Epic-beam mode (mesh-beam visible
	// shaft was the only contribution), but the user reported "make volumetric shadowing work
	// with the Epic beam" -- the carving REQUIRES a non-zero scattering for the engine to
	// actually compose anything into the fog. `Rebus.SpotLightScatter` (default 0.5) is the
	// tunable; intentionally modest so the cone-mesh stays the dominant visual contribution.
	// In fog-beam fallback mode (`bMeshBeamEnabled == false`, rare) the heavier
	// FogScatteringIntensity (default 2.5) stays in charge -- the visible shaft IS the froxel
	// scattering, and the same volumetric-shadow flag carves through it.
	SpotLight->SetVolumetricScatteringIntensity(
		bMeshBeamEnabled ? FMath::Max(GRebusSpotLightScatter, 0.f) : FogScatteringIntensity);

	// v1.0.94 -- resolve `bAllowMegaLights` through the `Rebus.AllowMegaLights` HARD FLOOR
	// (default 0 -> every Rebus SpotLight opts out of MegaLights at construction so dynamic
	// occluders cast hard shadows in the floor footprint of EVERY fixture, in EVERY mode). When
	// the CVar is 1 we pass through the engine's pre-v1.0.94 default (1 -- MegaLights on per
	// fixture). bAllowMegaLights is the public uint32:1 per-light flag on ULightComponent (UE
	// 5.7.4 has no Set* accessor for it) so we write it directly; a `ReregisterComponent` is NOT
	// required at construction because the FLightSceneInfo proxy is created LATER (on first
	// register, AFTER this assignment).
	SpotLight->bAllowMegaLights = ResolveAllowMegaLights(1);

	// v1.0.94 -- ALWAYS assert per-light shadow casting at construction. UE's
	// USpotLightComponent::CastShadows defaults true, but the v1.0.x `RefreshBeamShadowMode`
	// path used to clear it on non-hero non-gobo fixtures (a perf opt to skip the per-light
	// shadow map when neither volumetric shadows nor a cookie LF needed it). That clear was
	// the actual root cause for "Epic-beam fixtures don't show object shadows in the footprint"
	// -- a SpotLight with `CastShadows = false` produces NO shadows from any occluder, even
	// with MegaLights opted out. v1.0.94 keeps `CastShadows = true` always (shadow cost is
	// comparable to lit-pool cost; the floor footprint must always show silhouettes of trusses
	// / props / people standing between the fixture and the floor). RefreshBeamShadowMode also
	// forces it true on every refresh to defeat any pathway that would clear it later.
	SpotLight->SetCastShadows(true);

	// v1.0.95 -- ALWAYS cast volumetric shadows. Pre-v1.0.95 this was a hero-beam-only opt-in
	// (first N spotlights of the spawn batch); the user reported "make volumetric shadowing
	// work with the Epic beam, that's the only thing that isn't". The fog volumetric-shadow
	// pass is cheaper than the full VSM truss-gap path (the hero-beam budget below STILL caps
	// the latter), and is what makes the per-light scattering (set just above) carve through
	// occluders -- which is the user-visible "shadow in fog" effect they're missing.
	// CastVolumetricShadow is the FOG-volume-shadow flag (occluder carving in the volumetric
	// scattering integrator), SEPARATE from CastShadows above which controls SOLID shadow
	// casting onto surfaces -- the floor footprint shadow only depends on CastShadows.
	SpotLight->SetCastVolumetricShadow(true);

	// Hero-beam cap kept for the per-batch shadow QUALITY budget (RefreshBeamShadowMode picks
	// the higher `Rebus.HeroShadowScatter` value for hero beams, and the M_RebusBeam shader's
	// dense raymarch is the hero-only path). The cap no longer gates `bCastVolumetricShadow`
	// itself.
	const bool bHeroBeam = (VolumetricShadowBeamCount < RebusMaxVolumetricShadowBeams);
	if (bHeroBeam)
	{
		bGrantedShadowHero = true;
		++VolumetricShadowBeamCount;
	}
	SpotLight->MarkRenderStateDirty();

	// v1.0.95 -- one-shot warning if the level has no `AExponentialHeightFog` with
	// `bEnableVolumetricFog = true`. Without that, the SpotLight's volumetric scattering layer
	// + occluder carving (which we just plumbed in) produce nothing visible: the fog volume
	// the engine composites the scattering into doesn't exist. The user's lens-fog occlusion
	// expectation depends on this scene-level switch. We probe ONCE per fixture spawn (cheap)
	// and log a clear hint at Warning so the failure mode is self-diagnosing.
	if (UWorld* World = GetWorld())
	{
		bool bHasVolumetricFog = false;
		for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
		{
			AExponentialHeightFog* Fog = *It;
			if (Fog && Fog->GetComponent() && Fog->GetComponent()->bEnableVolumetricFog)
			{
				bHasVolumetricFog = true;
				break;
			}
		}
		if (!bHasVolumetricFog)
		{
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("Fixture %s SpotLight volumetric scattering=%.2f bCastVolumetricShadow=1, but no AExponentialHeightFog with bEnableVolumetricFog=true was found in the world. Volumetric shadows / fog-occlusion will be invisible until volumetric fog is enabled (place an Exponential Height Fog actor and tick Volumetric Fog under its Details panel, OR run the Python builder which seeds one)."),
				*FixtureId, SpotLight->VolumetricScatteringIntensity);
		}
	}

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
	// v1.0.88: both the synthetic disc (LensDiscMID) AND the per-isBeam flare MIDs need to be
	// refreshed each call -- a fixture whose synthetic disc failed to build (LensDiscMID null)
	// can STILL have isBeam meshes whose per-beam flares need driving, and vice-versa. So
	// guard each branch separately rather than the v1.0.87 single early return.
	if (LensDiscMID)
	{
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

	// Keep the per-isBeam-mesh flare MIDs in lockstep with the synthetic disc above (same
	// EmissiveColor / EmissiveStrength formula, driven by the same live dimmer + colour +
	// shutter-gate state). Cheap when IsBeamFlareMIDs is empty (legacy v2-blob fixtures).
	RefreshIsBeamFlareEmissive();

	// v1.0.102: also push the live dimmer x colour x gobo onto the lens-MATERIAL
	// emissive chain (M_RebusFixtureLens). This drives the GLOW directly ON the lens
	// face -- different from the soft halo discs above, which sit IN FRONT of the lens.
	// User request (v1.0.102): "can the lens material be emissive ... follow the dimmer,
	// colour and gobo of the fixture its part of."
	RefreshLensEmissive();
}

// ---- v1.0.88 real <Beam> (isBeam) per-mesh emissive flares + synthetic-fallback toggle -----
//
// BuildIsBeamLensFlares spawns one M_RebusLensFlare disc PER isBeam mesh, parented to that
// mesh's UProceduralMeshComponent so the flare inherits the mesh's motion automatically
// (no parallel RefreshMotion call required -- the procedural mesh component is already
// driven by Cumulative[MeshAxisBucket[i]] in RefreshMotion). Each flare is:
//   * a static-mesh-component plane (re-using the cook-safe LensPlaneMesh CDO ref from the
//     synthetic-disc path) sized by photometrics.lensDiameter when present, OR by the
//     isBeam mesh's local-space bounding-sphere radius x 2 (so a MAC Aura's per-pixel
//     emitters get appropriately-sized per-pixel flares even when the photometric diameter
//     describes only the whole-fixture lens hole),
//   * oriented with its +Z normal along BeamForwardLocal (the same emission forward the
//     synthetic disc + the SpotLight use), so the visible glow faces straight out the lens,
//   * positioned at the mesh's local-bounds CENTRE (so a lens disc whose vertices are
//     offset from the mesh-local origin still ends up with the flare on the disc, not
//     hovering off to one side), pushed slightly proud along the beam forward to keep
//     opaque head geometry from clipping it (mirrors the synthetic disc's ForwardOffset).
//
// No-op when IsBeamLensComponents is empty (v2 blobs OR v3 blobs whose GDTF <Beam> had no
// <Model> -- the synthetic disc remains the visible flare). LensPlaneMesh / LensMaterial
// missing also short-circuits to keep cooked-build asset-missing failures non-fatal (the
// synthetic-disc path already logs that failure mode).
void ARebusFixtureActor::BuildIsBeamLensFlares()
{
	if (IsBeamLensComponents.Num() == 0) return;

	UStaticMesh* Plane = LensPlaneMesh ? LensPlaneMesh.Get()
		: LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UMaterialInterface* LensMat = LensMaterial ? LensMaterial.Get()
		: LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/REBUS/Materials/M_RebusLensFlare.M_RebusLensFlare"));
	if (!Plane || !LensMat)
	{
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("Fixture %s isBeam flares: SKIP (asset load failed) meshOk=%d matOk=%d -- per-beam emissive flares unavailable for %d isBeam mesh(es), real geometry will still render mirror/glass."),
			*FixtureId, Plane ? 1 : 0, LensMat ? 1 : 0, IsBeamLensComponents.Num());
		return;
	}

	// Photometric lens diameter (metres) when the profile carries one; sentinel < 0 means absent.
	// Sizing precedence:
	//   * Single isBeam mesh (typical moving head): prefer photometrics.lensDiameter so the
	//     flare matches the SpotLight SourceRadius / synthetic-disc diameter exactly.
	//   * Multi isBeam meshes (LED-matrix, e.g. MAC Aura): IGNORE the photometric lens diameter
	//     because it describes the WHOLE-fixture lens hole, not the per-pixel emitter -- using
	//     it would over-size every per-pixel flare. Use the per-mesh local-bounds radius so
	//     each pixel gets its own pixel-sized flare. The user-doc explicitly calls this out.
	//   * Either case with photometric diameter absent: fall back to per-mesh local bounds so
	//     something visible always renders.
	const double PhotoDiamMeters = Profile.Photometrics.LensDiameter; // -1 when absent
	const bool bSingleBeam = (IsBeamLensComponents.Num() == 1);
	const bool bUsePhotoDiam = bSingleBeam && (PhotoDiamMeters > KINDA_SMALL_NUMBER);

	IsBeamFlareDiscs.Reset();
	IsBeamFlareMIDs.Reset();
	IsBeamFlareDiscs.Reserve(IsBeamLensComponents.Num());
	IsBeamFlareMIDs.Reserve(IsBeamLensComponents.Num());

	int32 SpawnedFlares = 0;
	for (int32 i = 0; i < IsBeamLensComponents.Num(); ++i)
	{
		UPrimitiveComponent* Beam = IsBeamLensComponents[i].Get();
		if (!Beam)
		{
			IsBeamFlareDiscs.Add(nullptr);
			IsBeamFlareMIDs.Add(nullptr);
			continue;
		}

		// Local-space bounds (computed at identity so we read the mesh-LOCAL extent, not the
		// motion-transformed world extent). CalcBounds is the right entry-point for procedural
		// meshes; for an LED-matrix fixture this returns the per-pixel disc's local extent.
		const FBoxSphereBounds LocalBounds = Beam->CalcBounds(FTransform::Identity);
		const float LocalRadiusCm = FMath::Max(LocalBounds.SphereRadius, 1.f); // floor at 1 cm
		const FVector LocalCentre = LocalBounds.Origin;                         // mesh-local

		float FlareDiamCm = bUsePhotoDiam
			? (float)(PhotoDiamMeters * RebusCoords::METERS_TO_UNREAL)
			: LocalRadiusCm * 2.f;
		FlareDiamCm = FMath::Max(FlareDiamCm, 1.f);

		const float PlaneScale = FlareDiamCm / 100.f; // engine Plane is 100 cm wide
		const float ForwardOffsetCm = FMath::Max(FlareDiamCm * 0.10f, 0.5f);
		const FQuat FlareRot = LensDiscRotationFromForward(BeamForwardLocal, BeamUpLocal);
		const FVector FlareOrigin = LocalCentre + BeamForwardLocal.GetSafeNormal() * ForwardOffsetCm;

		UStaticMeshComponent* Flare = NewObject<UStaticMeshComponent>(this);
		Flare->SetupAttachment(Beam);
		Flare->RegisterComponent();
		Flare->SetMobility(EComponentMobility::Movable);
		Flare->SetStaticMesh(Plane);
		Flare->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Flare->SetCastShadow(false);
		Flare->bCastDynamicShadow = false;
		Flare->ComponentTags.AddUnique(FName(TEXT("RebusIsBeamFlare")));
		Flare->SetVisibility(true);
		Flare->SetHiddenInGame(false);

		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(LensMat, this);
		if (MID)
		{
			Flare->SetMaterial(0, MID);
		}

		Flare->SetRelativeTransform(FTransform(FlareRot, FlareOrigin, FVector(PlaneScale, PlaneScale, PlaneScale)));

		IsBeamFlareDiscs.Add(Flare);
		IsBeamFlareMIDs.Add(MID);
		++SpawnedFlares;

		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Fixture %s isBeam flare[%d]: spawned diam=%.2fcm (src=%s) planeScale=%.4f localRadius=%.2fcm"),
			*FixtureId, i, FlareDiamCm,
			bUsePhotoDiam ? TEXT("photometrics.lensDiameter (single-beam)") : TEXT("mesh-local-bounds-radius*2 (multi-beam or no photo diam)"),
			PlaneScale, LocalRadiusCm);
	}

	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s isBeam flares: built %d/%d (synthetic LensDisc kept alive as fallback; toggle with Rebus.ForceSyntheticLensFallback)."),
		*FixtureId, SpawnedFlares, IsBeamLensComponents.Num());

	// Push the initial emissive seed so the flare lights up immediately if Dimmer.Current > 0
	// (which it isn't on a fresh spawn, but RefreshLensDisc is the unified entry-point and we
	// want to behave identically to the synthetic disc -- see the call from BuildLensDisc).
	RefreshIsBeamFlareEmissive();
}

void ARebusFixtureActor::RefreshIsBeamFlareEmissive()
{
	if (IsBeamFlareMIDs.Num() == 0) return;

	// Identical formula to RefreshLensDisc above -- one source of truth, copied here only
	// because the per-flare MID list is the new thing. If RefreshLensDisc ever evolves
	// (e.g. a flicker model), refactor both into a shared helper.
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
	const float Strength = RebusLensFlareMaxEmissive
		* FMath::Clamp(Dimmer.Current, 0.f, 1.f) * Gate;

	for (UMaterialInstanceDynamic* MID : IsBeamFlareMIDs)
	{
		if (!MID) continue;
		MID->SetVectorParameterValue(TEXT("EmissiveColor"), Linear);
		MID->SetScalarParameterValue(TEXT("EmissiveStrength"), Strength);
	}
}

// v1.0.102 -- wrap the lens material (slot 0) of every real <Beam> isBeam mesh in a
// per-component UMaterialInstanceDynamic so live state pushes from RefreshLensEmissive
// address THIS fixture only, not the shared project-wide chrome master. Also wraps
// the synthetic LensDisc (which already MIDs the lens-flare material in BuildLensDisc;
// the same plane mesh ALSO carries the chrome lens material when the v1.0.71 override
// pipeline routes it, depending on the asset stack). Idempotent: skips entries whose
// MID is already populated. Called from the END of BuildMeshes after every isBeam
// PMC has been recorded in IsBeamLensComponents.
//
// CreateAndSetMaterialInstanceDynamic returns the existing MID when the slot already
// has a dynamic instance (idempotent on re-call). When the lens material resolves to
// null (the missing-asset deployment path that v1.0.95 already logs a Warning for) we
// silently skip the MID creation -- RefreshLensEmissive then no-ops on the slot.
void ARebusFixtureActor::EnsurePerLensMIDs()
{
	IsBeamLensMIDs.Reset();
	IsBeamLensMIDs.Reserve(IsBeamLensComponents.Num());
	int32 Wrapped = 0;
	for (int32 i = 0; i < IsBeamLensComponents.Num(); ++i)
	{
		UPrimitiveComponent* Beam = IsBeamLensComponents[i].Get();
		if (!Beam)
		{
			IsBeamLensMIDs.Add(nullptr);
			continue;
		}
		// CreateAndSetMaterialInstanceDynamic uses the CURRENT slot-0 material as the
		// MID's parent. BuildMeshes already assigned the mirror/glass LensMat to slot 0
		// of every isBeam PMC (lines around the `if (Mesh.bIsBeam)` block) so this MID
		// inherits the v1.0.102 emissive parameter contract from the Python-baked
		// `M_RebusFixtureLens` master verbatim.
		UMaterialInstanceDynamic* MID = Beam->CreateAndSetMaterialInstanceDynamic(0);
		IsBeamLensMIDs.Add(MID);
		if (MID) { ++Wrapped; }
	}
	UE_LOG(LogRebusVisualiser, Verbose,
		TEXT("Fixture %s lens MIDs: wrapped %d/%d isBeam PMC slot-0 materials in per-component MIDs (v1.0.102; RefreshLensEmissive will drive these)."),
		*FixtureId, Wrapped, IsBeamLensComponents.Num());
}

// v1.0.102 -- user request (verbatim): "can the lens material be emiissive as well
// and follow the dimmer, colour and gobo of the fixture its part of."
//
// Push the live fixture state onto the v1.0.102 emissive parameter contract of the
// `M_RebusFixtureLens` master (Emissive vector + EmissiveIntensity scalar + GoboTexture
// sampler + bUseGobo scalar). The material's emissive chain is ADDITIVE on top of the
// existing chrome PBR: at Dimmer.Current=0 the lens reads identical to pre-v1.0.102
// (chrome mirror), at Dimmer.Current=1 the lens glows in the live colour modulated by
// the cookie when a gobo is active. Pushes to BOTH the synthetic LensDisc material-
// override pipeline (when its slot-0 material is the chrome lens MID, NOT the lens-
// flare material the v1.0.49 path normally builds it with) and EVERY per-beam lens MID
// in IsBeamLensMIDs (one push per multi-beam pixel for LED matrices; per-pixel
// emission is a future enhancement -- see README v1.0.102 "Multi-beam fixtures").
//
// IMPORTANT: this is INDEPENDENT of `RefreshLensDisc` / `RefreshIsBeamFlareEmissive`
// which drive the v1.0.49 `M_RebusLensFlare` material's EmissiveColor / EmissiveStrength
// params on the per-beam flare DISCS (the additive lens-flare planes co-located with
// each lens). The flare discs are the SOFT halo around the lens; this push is the
// HARD glow ON the lens face itself. Both layers compose to the final lens visual.
//
// Called from EVERY existing intensity/colour/gobo update path so the lens stays in
// lockstep with the SpotLight:
//   * Tail-called from `RefreshLensDisc` (unified emissive entry-point reached by
//     `RefreshIntensity` -> the same path called by `ApplyDimmer` / `ApplyColor` /
//     the shutter strobe tick / `RefreshIesAfterZoom`).
//   * Called from gobo-state apply paths: `ApplyGoboTextureFromBytes` (after
//     bGoboActive=true), `ClearGoboToOpen` (after bGoboActive=false), `OnGoboRTUpdate`
//     (every RT redraw so the lens face animates with the cookie rotation).
//   * Called once from `BuildMeshes` (via the seed at the end of `Setup`) after
//     `EnsurePerLensMIDs` so the initial state matches even if Dimmer.Current > 0 at
//     spawn time (rare but possible via inline state push).
void ARebusFixtureActor::RefreshLensEmissive()
{
	float Gate = 1.f;
	switch (ShutterMode)
	{
	case ERebusShutterMode::Closed: Gate = 0.f; break;
	case ERebusShutterMode::Strobe: Gate = (ShutterPhase < 0.5f) ? 1.f : 0.f; break;
	default: break;
	}

	const FLinearColor LiveColour(
		FMath::Clamp(ColorR.Current, 0.f, 1.f),
		FMath::Clamp(ColorG.Current, 0.f, 1.f),
		FMath::Clamp(ColorB.Current, 0.f, 1.f), 1.f);

	const float DimmerNorm = FMath::Clamp(Dimmer.Current, 0.f, 1.f);
	const float CVarScale = FMath::Max(GRebusLensEmissiveScale, 0.f);
	const float RawIntensity = DimmerNorm * Gate * RebusLensEmissiveBaseScale * CVarScale;
	const float ClampedIntensity = FMath::Clamp(RawIntensity, 0.f, RebusLensEmissiveIntensityCap);

	// v1.0.102: GoboRT is a UCanvasRenderTarget2D -> UTextureRenderTarget2D -> UTexture, so
	// the implicit upcast is legal. SetTextureParameterValue accepts UTexture* and silently
	// no-ops when the parameter name doesn't exist on the underlying master (e.g.
	// operator-authored M_RebusFixtureLens that hasn't picked up the v1.0.102 contract
	// yet -- see the _fixture_lens_master_has_emissive Python self-heal).
	UTexture* GoboPattern = Cast<UTexture>(GoboRT.Get());
	const float UseGoboScalar = (bGoboActive && GRebusLensFollowGobo != 0 && GoboPattern != nullptr) ? 1.f : 0.f;

	auto PushOnMID = [&](UMaterialInstanceDynamic* MID)
	{
		if (!MID) return;
		MID->SetVectorParameterValue(TEXT("Emissive"), LiveColour);
		MID->SetScalarParameterValue(TEXT("EmissiveIntensity"), ClampedIntensity);
		if (GoboPattern)
		{
			MID->SetTextureParameterValue(TEXT("GoboTexture"), GoboPattern);
		}
		MID->SetScalarParameterValue(TEXT("bUseGobo"), UseGoboScalar);
	};

	// Push onto every real <Beam> isBeam mesh's per-component MID. Multi-beam LED-matrix
	// fixtures get the SAME fixture-master state on every pixel-lens (per-pixel emission
	// is a future enhancement -- README v1.0.102 "Multi-beam fixtures" notes this).
	int32 PushedReal = 0;
	for (const TWeakObjectPtr<UMaterialInstanceDynamic>& WeakMID : IsBeamLensMIDs)
	{
		UMaterialInstanceDynamic* MID = WeakMID.Get();
		if (MID)
		{
			PushOnMID(MID);
			++PushedReal;
		}
	}

	// Synthetic LensDisc also gets the push: pre-v1.0.102 the disc's MID is built from
	// the lens-FLARE material (M_RebusLensFlare -- the soft halo with its own
	// EmissiveColor/EmissiveStrength chain), so the new Emissive/EmissiveIntensity
	// params silently no-op there. But the v1.0.71 lens-override pipeline can ALSO
	// route the chrome M_RebusFixtureLens onto the synthetic plane in some
	// configurations, in which case the push lands and the synthetic disc glows in
	// lockstep with the real-beam lenses. The double-push is intentional and cheap
	// (UMaterialInstanceDynamic parameter setters skip unknown names). The
	// ApplyLensEmissive-to-both-arrays rule in the v1.0.102 task spec covers the rare
	// respawn-race where BOTH paths are live simultaneously.
	if (LensDiscMID)
	{
		PushOnMID(LensDiscMID);
	}

	UE_LOG(LogRebusVisualiser, Verbose,
		TEXT("Fixture %s lens emissive: color=(%.2f,%.2f,%.2f) intensity=%.3f goboActive=%d useGobo=%.0f scale=%.2f pushedReal=%d/%d syntheticMID=%s"),
		*FixtureId,
		LiveColour.R, LiveColour.G, LiveColour.B, ClampedIntensity,
		bGoboActive ? 1 : 0, UseGoboScalar, CVarScale,
		PushedReal, IsBeamLensMIDs.Num(),
		LensDiscMID ? TEXT("set") : TEXT("absent"));
}

void ARebusFixtureActor::RefreshIsBeamLensVisuals()
{
	// Visibility rules (see RebusFixtureActor.h header comment on SetUseSyntheticLensFallback):
	//   * IsBeamLensComponents.Num() == 0 -> always show the synthetic disc (the per-beam
	//     arrays are empty and there is nothing to A/B). The toggle is a no-op.
	//   * bUseSyntheticLensFallback == true -> hide every isBeam mesh + every per-beam flare,
	//     re-show the synthetic disc.
	//   * Otherwise (default) -> show every isBeam mesh + every per-beam flare, hide the
	//     synthetic disc.
	const bool bHaveIsBeam = (IsBeamLensComponents.Num() > 0);
	const bool bShowSynthetic = (!bHaveIsBeam) || bUseSyntheticLensFallback;
	const bool bShowReal = bHaveIsBeam && !bUseSyntheticLensFallback;

	if (LensDisc)
	{
		LensDisc->SetVisibility(bShowSynthetic, /*bPropagateToChildren*/ true);
		LensDisc->SetHiddenInGame(!bShowSynthetic);
	}

	for (int32 i = 0; i < IsBeamLensComponents.Num(); ++i)
	{
		UPrimitiveComponent* Beam = IsBeamLensComponents[i].Get();
		if (Beam)
		{
			// Hide the procedural mesh itself when forcing synthetic fallback -- the user-doc
			// asked for the real isBeam meshes to be "invisible/passthrough" so the synthetic
			// disc is the sole lens visual in fallback mode.
			Beam->SetVisibility(bShowReal, /*bPropagateToChildren*/ true);
			// v1.0.95: also drive bHiddenInGame in lockstep so the cooked / packaged path
			// agrees with the editor path (`SetVisibility` alone leaves bHiddenInGame at its
			// last value; for a freshly-built isBeam comp that means "false" -- but a future
			// path that ever sets bHiddenInGame=true elsewhere would otherwise silently
			// override the visibility we just asserted).
			Beam->SetHiddenInGame(!bShowReal);
		}
		UStaticMeshComponent* Flare = IsBeamFlareDiscs.IsValidIndex(i) ? IsBeamFlareDiscs[i] : nullptr;
		if (Flare)
		{
			Flare->SetVisibility(bShowReal, /*bPropagateToChildren*/ true);
			Flare->SetHiddenInGame(!bShowReal);
		}
	}

	// v1.0.95: verbose lens-visibility log (kept at Verbose so the steady-state spam stays
	// out of the user-facing log; flip the category to Verbose with `LogRebusVisualiser.SetVerbosity Verbose`
	// to see it). The triple (synthetic-shown, real-beam-count, fallback-forced) is the
	// minimum information the lens visibility bug report needed to diagnose by inspection.
	UE_LOG(LogRebusVisualiser, Verbose,
		TEXT("Fixture %s lens visibility: synthetic=%d isBeamMeshes=%d (fallbackForced=%d, syntheticVisible=%d, realLensVisible=%d)."),
		*FixtureId,
		bShowSynthetic ? 1 : 0, IsBeamLensComponents.Num(),
		bUseSyntheticLensFallback ? 1 : 0,
		bShowSynthetic ? 1 : 0, bShowReal ? 1 : 0);
}

void ARebusFixtureActor::SetUseSyntheticLensFallback(bool bForceSynthetic)
{
	if (bForceSynthetic == bUseSyntheticLensFallback)
	{
		return;
	}
	bUseSyntheticLensFallback = bForceSynthetic;
	RefreshIsBeamLensVisuals();
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s lens-fallback toggle: %s (isBeamMeshes=%d perBeamFlares=%d syntheticDisc=%s)."),
		*FixtureId,
		bUseSyntheticLensFallback ? TEXT("FORCED synthetic disc (isBeam meshes hidden)")
		                          : TEXT("real <Beam> geometry (synthetic disc hidden)"),
		IsBeamLensComponents.Num(), IsBeamFlareDiscs.Num(),
		LensDisc ? (LensDisc->IsVisible() ? TEXT("visible") : TEXT("hidden")) : TEXT("not-built"));
}

// ---- Hybrid cone-mesh volumetric beam (Phase 1, §8.4a) --------------------------------

float ARebusFixtureActor::ResolveZoomHalfDeg(float ZoomFullDeg) const
{
	// v1.0.101 -- canonical single-source-of-truth zoom half-angle in degrees. Used by the
	// SpotLight outer-cone (RecomputeConeAngles), the procedural cone-mesh far-radius
	// (UpdateBeamConeGeometry), and the Epic-beam canvas zoom (UpdateEpicBeamParams via
	// SpotLight->OuterConeAngle which itself reads this). Both the lit footprint and the
	// visible cone shaft therefore start from the SAME half-angle; the only knob that
	// pulls them apart is `BeamConeRadiusScale` applied *only* to the visible cone-mesh
	// (see UpdateBeamConeGeometry). Helper signature (full-angle in -> half-angle out)
	// matches the wire protocol's full-angle convention -- pre-v1.0.84 the wire was
	// half-angle; v1.0.84 standardised on full and converts to half in
	// URebusFixtureControlSubsystem::SetFixtureZoom before calling ApplyZoom.
	//
	// Frost is intentionally NOT applied here -- it softens the INNER cone + source
	// radius, not the outer field extent (RecomputeConeAngles handles that).
	float OuterHalf = 0.5f * ZoomFullDeg;
	if (Profile.Zoom.bValid)
	{
		OuterHalf = FMath::Clamp(OuterHalf,
			(float)(Profile.Zoom.MinDeg * 0.5),
			(float)(Profile.Zoom.MaxDeg * 0.5));
	}
	OuterHalf = FMath::Clamp(OuterHalf, 0.5f, 80.f);
	// v1.0.63: when a gobo cookie is live, iris is applied as a circular alpha mask baked into
	// GoboRT (see OnGoboRTUpdate + EnsureIrisMaskTexture). Also pinching the SpotLight outer cone
	// here would double-iris the footprint AND -- more visibly -- scale the gobo pattern with
	// the cone (the user's "iris is zooming instead of circular cropping" report -- shrinking the
	// cone maps the same RT onto a smaller floor area, making gobo features look bigger). With
	// NO gobo, there is no RT to mask, so the cone-pinch is preserved so iris-only still has an
	// effect on the lit pool (back-compatible with the pre-v1.0.63 behaviour).
	if (!bGoboActive)
	{
		const float IrisScale = FMath::Lerp(0.4f, 1.f, FMath::Clamp(Iris.Current, 0.f, 1.f));
		OuterHalf *= IrisScale;
	}
	return OuterHalf;
}

float ARebusFixtureActor::ResolveOuterHalfDeg() const
{
	// v1.0.101 -- thin wrapper around the canonical helper, kept for call-site readability
	// (SpotLight outer-cone callers asking "what's the lit cone half-angle right now?"
	// read more clearly with this name; the canonical helper carries the design rationale).
	// `ZoomDeg.Current` is already a HALF angle (ApplyZoom stores the half it received from
	// URebusFixtureControlSubsystem::SetFixtureZoom which divided the wire FullDeg by 2),
	// so the round-trip via `ZoomDeg.Current * 2.f` recovers the full-angle the canonical
	// helper expects. The arithmetic round-trip is fine -- no precision loss vs IEEE-754
	// half-of-double-of-half, and clamping inside the helper makes any micro-rounding moot.
	return ResolveZoomHalfDeg(ZoomDeg.Current * 2.f);
}

float ARebusFixtureActor::ResolveFootprintInnerRatio() const
{
	// v1.0.108 -- mirror `RecomputeConeAngles`'s `InnerRatio` derivation so the visible
	// cone-mesh FarRadius math (UpdateBeamConeGeometry / UpdateEpicBeamParams) and the
	// SpotLight's actual linear-taper taper-floor angle stay in lockstep by construction.
	// When the GDTF profile carries both BeamAngle (peak-intensity full angle) and
	// FieldAngle (zero-intensity full angle), the ratio of the two IS the inner/outer
	// ratio that drives the SpotLight's `InnerConeAngle / OuterConeAngle`; the 0.8
	// fallback matches the v1.0.101 design point for fixtures that arrive without
	// per-shape photometrics (most theatrical movers do, but architectural / library
	// fixtures may not -- the fallback errs on the wider-inner-cone side which produces
	// a slightly LARGER visible-shaft target than 0.5 x outer would).
	float InnerRatio = 0.8f;
	if (Profile.Photometrics.BeamAngle.IsSet() && Profile.Photometrics.FieldAngle.IsSet()
		&& Profile.Photometrics.FieldAngle.GetValue() > KINDA_SMALL_NUMBER)
	{
		InnerRatio = (float)(Profile.Photometrics.BeamAngle.GetValue() / Profile.Photometrics.FieldAngle.GetValue());
		InnerRatio = FMath::Clamp(InnerRatio, 0.05f, 0.98f);
	}
	return InnerRatio;
}

float ARebusFixtureActor::ResolveBeamFootprintMatchHalfDeg() const
{
	// v1.0.108 -- the visible cone-mesh should be sized to the HALF-INTENSITY edge of the
	// SpotLight's linear inner..outer taper, NOT the geometric outer cone (where the
	// SpotLight's contribution has already faded to zero). For a SpotLight that ramps
	// brightness linearly from `InnerConeAngle` (peak) to `OuterConeAngle` (zero), the
	// half-brightness ring sits at `(InnerHalf + OuterHalf) / 2 = OuterHalf * (1 +
	// InnerRatio) / 2`. With the v1.0.101 default `InnerRatio = 0.8` the match angle is
	// `0.9 * OuterHalf` (the 10 % gap the v1.0.101 release block already documented; the
	// v1.0.108 user image showed a much bigger visual gap which the v1.0.108
	// `Rebus.BeamSharpness 6.0` Gaussian-tightening fix addresses on top of this). For
	// a profile with `InnerRatio = 0.5` the match angle is `0.75 * OuterHalf`; for a
	// near-uniform profile `InnerRatio -> 1.0` the match angle approaches the geometric
	// outer cone (i.e. the SpotLight reads as an even disc with a hard edge, which is
	// what flat-field fixtures want).
	const float OuterHalf  = ResolveOuterHalfDeg();
	const float InnerRatio = ResolveFootprintInnerRatio();
	return OuterHalf * (1.0f + InnerRatio) * 0.5f;
}

void ARebusFixtureActor::BuildBeamCone()
{
	// v1.0.101 -- seed the per-fixture cone-mesh radius scale from the CURRENT live CVar
	// so a fresh-spawn fixture inherits the operator's chosen scale (the CVar refresh sink
	// only walks already-spawned fixtures; without this seed, fixtures spawned AFTER an
	// operator pushed `Rebus.BeamConeRadiusScale 0.9` would default-construct to 1.0 and
	// the operator would see the same fixture flicker between scales as the spawn batch
	// fired). Picks up the CVar at the same point BuildSpotLight reads BeamShadowSteps /
	// BeamShadowStrength via RefreshBeamShadowParams below -- consistent seed point for
	// every per-fixture beam scalar that has a global CVar pair.
	if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Rebus.BeamConeRadiusScale")))
	{
		BeamConeRadiusScale = FMath::Max(0.05f, CVar->GetFloat());
	}

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
	// Constant-default seed for the radial-attenuation scalars. `RefreshBeamRadialParams`
	// below overwrites these with the live `Rebus.BeamSharpness` / `Rebus.BeamDensity` /
	// `Rebus.BeamFalloff` CVar values so a fresh-spawn fixture inherits the operator's
	// current tuning (the v1.0.108 fix shape: same per-fixture seed pattern as
	// `RefreshBeamShadowParams` for the v1.0.96 trace scalars).
	BeamMID->SetScalarParameterValue(TEXT("BeamSharpness"), RebusBeamSharpness);
	BeamMID->SetScalarParameterValue(TEXT("BeamFalloff"), RebusBeamFalloff);
	// Raymarch tuning (Phase 2): march step count + per-step density for the Custom HLSL body.
	BeamMID->SetScalarParameterValue(TEXT("StepCount"), RebusBeamStepCount);
	BeamMID->SetScalarParameterValue(TEXT("BeamDensity"), RebusBeamDensity);
	// v1.0.96 -- seed the screen-space shadow-trace scalars from the live CVar values so a
	// fresh-spawn fixture starts with the operator's current `Rebus.BeamShadowSteps` /
	// `Rebus.BeamShadowStrength`, not the master's authored defaults. Live CVar changes after
	// this point re-push through `RefreshBeamShadowParams` via the CVar refresh sinks above.
	RefreshBeamShadowParams();
	// v1.0.108 -- same shape, but for the radial-attenuation scalars. Pushes
	// `Rebus.BeamSharpness` / `Rebus.BeamDensity` / `Rebus.BeamFalloff` onto the BeamMID so
	// the fresh-spawn cone reads with the operator's CURRENT Gaussian-tightening tune
	// (default sharpness raised to 6.0 in v1.0.108 to match the bright floor disc edge --
	// see the constexpr `RebusBeamSharpness` doc-comment).
	RefreshBeamRadialParams();

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

	// v1.0.106 -- seed the per-fixture preference from the live CVar so a fresh-spawn fixture
	// inherits the operator's current `Rebus.PreferProceduralBeam` choice (default 1 = prefer
	// procedural; the CVar refresh sink only walks already-spawned fixtures, so a fixture
	// spawned AFTER an operator pushed `Rebus.PreferProceduralBeam 0` would default-construct
	// to 1 and the operator would see a mid-batch flicker between paths without this seed).
	// Same shape as the BeamConeRadiusScale seed above -- consistent seed point for every
	// per-fixture beam scalar/flag that has a global CVar pair.
	if (IConsoleVariable* PrefCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Rebus.PreferProceduralBeam")))
	{
		bPreferProceduralBeam = (PrefCVar->GetInt() != 0);
	}

	// v1.0.43 introduced / v1.0.106 GATED: prefer Epic's REAL DMX beam (SM_Beam_RM + MI_Beam)
	// when the DMX Fixtures content is installed AND the operator has not opted into the
	// procedural cone (the default). On success the procedural cone above becomes the hidden
	// fallback (it stays built so the integration is fully reversible / robust to the content
	// being removed). On failure we keep the M_RebusBeam cone as the visible beam.
	//
	// v1.0.106 -- when `bPreferProceduralBeam` is true (the new default) `TryBuildEpicBeam()`
	// is SKIPPED entirely: the procedural cone is the visible shaft and the v1.0.96 / v1.0.99
	// screen-space self-shadow trace (seeded via `RefreshBeamShadowParams` above) actually
	// renders. EpicBeamComp / EpicBeamMID stay null until the operator flips the toggle off,
	// at which point `RefreshPreferProceduralBeamFromCVar` builds them lazily.
	if (!bPreferProceduralBeam)
	{
		bUsingEpicBeam = TryBuildEpicBeam();
		if (bUsingEpicBeam && BeamCone)
		{
			BeamCone->SetVisibility(false);
		}
	}
	else
	{
		bUsingEpicBeam = false;
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Fixture %s beam: PreferProceduralBeam=1 (v1.0.106 default) -- skipping "
				 "TryBuildEpicBeam(); M_RebusBeam procedural cone IS the visible shaft so the "
				 "v1.0.96 / v1.0.99 screen-space self-shadow trace actually renders. Flip with "
				 "`Rebus.PreferProceduralBeam 0` to restore Epic's MI_Beam canvas (the v1.0.107 "
				 "follow-up will port the trace to Epic's beam too)."),
			*FixtureId);
	}
}

void ARebusFixtureActor::UpdateBeamConeGeometry()
{
	if (!BeamCone) return;

	// v1.0.101 -- both the SpotLight outer cone (RecomputeConeAngles) AND the visible
	// cone-mesh far-radius derive from the SAME canonical half-angle (ResolveOuterHalfDeg
	// -> ResolveZoomHalfDeg -- the GDTF zoom-range spec). Single source of truth.
	// `BeamConeRadiusScale` is applied ONLY to the visible cone-mesh radius below, NOT
	// to the half-angle the SpotLight uses, so the lit footprint and IES sampling stay
	// anchored to the GDTF zoom range while the operator can pinch the visible shaft to
	// match the perceived bright disc edge.
	//
	// v1.0.108 -- the FarRadius BASE is now the HALF-INTENSITY match angle
	// (`ResolveBeamFootprintMatchHalfDeg() = OuterHalf * (1 + InnerRatio) / 2`) instead
	// of the geometric `OuterHalf`. The SpotLight's `USpotLightComponent` linear taper
	// drops brightness from `InnerConeAngle` (peak) to `OuterConeAngle` (zero) so the
	// VISIBLE bright disc on the floor sits at exactly the half-intensity ring; sizing
	// the cone-mesh to `OuterHalf * tan(...)` (the v1.0.101..v1.0.107 behaviour) over-
	// sizes the visible shaft to the geometric cone where the SpotLight has already
	// faded to zero. With default `InnerRatio = 0.8` the match angle is `0.9 *
	// OuterHalf` -- a 10 % geometric pinch -- but the v1.0.108 user image showed the
	// visible shaft reading 2-3x wider than the lit floor disc, which the geometric
	// 10 % alone could not explain: the dominant cause was the soft Gaussian raymarch
	// (`BeamSharpness = 2.5`) bleeding the visible glow all the way to the geometric
	// edge -- v1.0.108 raises the default sharpness to 6.0 (constexpr `RebusBeamSharpness`)
	// so the visible bright shaft pinches to ~60 % of the cone-mesh radius, and the
	// half-intensity FarRadius math here makes that 60 % radius coincide with the
	// SpotLight's actual half-bright ring on the floor.
	//
	// `BeamConeRadiusScale` (default 1.0 since v1.0.101, kept at 1.0 in v1.0.108) is
	// multiplied AT THE END so it stays a pure polish knob on top of the corrected
	// geometric base. Operators who tuned `BeamConeRadiusScale` to 0.85..0.95 in
	// v1.0.101..v1.0.107 to compensate for this gap will find their cones too narrow
	// after v1.0.108 and should reset to 1.0 (documented in the v1.0.108 README
	// release block under "Operator migration").
	const float MatchHalf = ResolveBeamFootprintMatchHalfDeg();
	const float TanMatch  = FMath::Tan(FMath::DegreesToRadians(MatchHalf));
	const float ConeScale = FMath::Max(0.05f, BeamConeRadiusScale);
	const float FarRadius = FMath::Max(BeamLengthUnreal * TanMatch * ConeScale, BeamBaseRadiusUnreal + 0.1f);

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

void ARebusFixtureActor::RefreshBeamShadowParams()
{
	// v1.0.96/99/109 -- push the screen-space-shadow-trace scalars onto this fixture's BeamMID.
	// Safe to call when BeamMID is null (the M_RebusBeam load failed in BuildBeamCone, or this
	// fixture pre-dates the v1.0.96 self-heal regen): the early return matches the rest of the
	// MID-push helpers and the CVar refresh sink walks every fixture, including those without
	// a BeamMID, without crashing.
	//
	// v1.0.99: pushes ALL FOUR v1.0.96/99 scalars -- BeamShadowSteps + BeamShadowStrength were
	// the v1.0.96 set, BeamShadowBias is now CVar-driven (was hard-coded to 0.5 in v1.0.96),
	// BeamShadowDebug is the new v1.0.99 debug-view selector.
	//
	// v1.0.109: extends the push to the three new pan-edge / sky / far-distance guards
	// (BeamShadowFarCullCm + BeamShadowEdgeGuard + BeamShadowBiasScale). SetScalarParameter
	// Value on a missing parameter is a silent no-op so the call is safe even when the editor
	// hasn't yet regenerated the master to v1.0.109 -- the first launch will trigger
	// `_beam_master_has_pan_edge_guard` self-heal in `ensure_beam_material`
	// (build_rebus_base_level.py) and the next refresh will land all SEVEN cleanly. The
	// v1.0.103 `Rebus.DumpBeamShadow` per-scalar EXISTS/MISSING flag surfaces the pre-
	// v1.0.109-master case to the operator directly so the silent no-op isn't a silent
	// regression.
	if (!BeamMID) return;

	BeamMID->SetScalarParameterValue(TEXT("BeamShadowSteps"),
		FMath::Clamp(GRebusBeamShadowSteps, 1.f, 16.f));
	BeamMID->SetScalarParameterValue(TEXT("BeamShadowStrength"),
		FMath::Clamp(GRebusBeamShadowStrength, 0.f, 1.f));
	BeamMID->SetScalarParameterValue(TEXT("BeamShadowBias"),
		FMath::Max(GRebusBeamShadowBias, 0.f));
	// Debug is an int (0/1/2) on the CVar side; the master scalar is a float that the shader
	// branches on `> 0.5` and `> 1.5` thresholds, so pushing the int verbatim is fine.
	BeamMID->SetScalarParameterValue(TEXT("BeamShadowDebug"),
		(float)FMath::Clamp(GRebusBeamShadowDebug, 0, 2));

	// v1.0.109 guard scalars. Clamps mirror the shader-side floors (the HLSL caches
	// `max(BeamShadowFarCullCm, 100.0)` per shaft sample so a runaway negative push can't
	// disable the cull entirely; the C++ clamp here keeps the value sane BEFORE the shader's
	// own floor takes over, so the EXISTS/MISSING dump reflects what the operator pushed
	// rather than what the shader resolved).
	BeamMID->SetScalarParameterValue(TEXT("BeamShadowFarCullCm"),
		FMath::Max(GRebusBeamShadowFarCullCm, 100.f));
	// EdgeGuard is a 0/1 int on the CVar side, pushed as a float so the shader's
	// `if (BeamShadowEdgeGuard > 0.5)` gate has unambiguous endpoints.
	BeamMID->SetScalarParameterValue(TEXT("BeamShadowEdgeGuard"),
		(float)FMath::Clamp(GRebusBeamShadowEdgeGuard, 0, 1));
	// BiasScale: 0 disables the multiplicative term (the v1.0.99 absolute floor is then the
	// only bias source -- equivalent to pre-v1.0.109 behaviour). Upper bound 0.02 (2 percent
	// of sample depth) is well past useful but stays finite so a 1e6 portal push doesn't
	// produce a NaN at extreme distances.
	BeamMID->SetScalarParameterValue(TEXT("BeamShadowBiasScale"),
		FMath::Clamp(GRebusBeamShadowBiasScale, 0.f, 0.02f));
}

void ARebusFixtureActor::RefreshBeamRadialParams()
{
	// v1.0.108 -- push the three radial-attenuation scalars (BeamSharpness / BeamDensity /
	// BeamFalloff) onto the BeamMID. Mirrors the `RefreshBeamShadowParams` shape verbatim
	// so the per-fixture seed point (BuildBeamCone) and the global CVar refresh sinks
	// (`Rebus.BeamSharpness` / `Rebus.BeamDensity` / `Rebus.BeamFalloff`) share one
	// chokepoint -- a future `bRadialOverridden` per-fixture flag could opt a hero
	// fixture out of the global push without changing the call sites.
	//
	// Clamps:
	//   * BeamSharpness clamped to [0.05, 32]. Lower bound = the shader's own
	//     `max(BeamSharpness, 0.01)` floor (so `core` stays a finite Gaussian at the
	//     minimum); upper bound is well past the operator-recommended [4..12] range
	//     (32 makes the shaft a hairline core only -- already past useful) but stays
	//     finite so a portal mis-push can't blow up the shader.
	//   * BeamDensity clamped to [0, 1]. 0 = invisible shaft (the trace runs but no
	//     density accumulates); upper 1 is well past the recommended [0.005..0.06]
	//     band (1 saturates the additive accumulator instantly).
	//   * BeamFalloff clamped to [0, 32]. 0 = flat along the shaft (no length-fade);
	//     32 effectively kills the shaft a few cm downrange. Operator-recommended
	//     [0..4].
	//
	// Safe to call when BeamMID is null (the M_RebusBeam load failed in BuildBeamCone, or
	// this fixture pre-dates the v1.0.108 self-heal regen): the early return matches the
	// rest of the MID-push helpers and the CVar refresh sink walks every fixture, including
	// those without a BeamMID, without crashing.
	if (!BeamMID) return;

	BeamMID->SetScalarParameterValue(TEXT("BeamSharpness"),
		FMath::Clamp(GRebusBeamSharpness, 0.05f, 32.f));
	BeamMID->SetScalarParameterValue(TEXT("BeamDensity"),
		FMath::Clamp(GRebusBeamDensity, 0.f, 1.f));
	BeamMID->SetScalarParameterValue(TEXT("BeamFalloff"),
		FMath::Clamp(GRebusBeamFalloff, 0.f, 32.f));
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
	//
	// v1.0.101 -- ALSO multiply by `BeamConeRadiusScale` so the same operator-tweakable
	// scalar that pinches the procedural cone-mesh shaft (UpdateBeamConeGeometry) ALSO
	// pinches the Epic-beam canvas. Without this, the user's `Rebus.BeamConeRadiusScale`
	// would only narrow the M_RebusBeam fallback (hidden when Epic-beam is the live path
	// from v1.0.95 onwards) and have no visible effect on the live shaft. The SpotLight's
	// own OuterConeAngle stays at the un-scaled GDTF zoom-range half-angle (the lit
	// footprint is unchanged), so the Epic-beam canvas + the procedural cone mesh tighten
	// in sync while the lit footprint, IES sampling, and 1/r^2 falloff continue to track
	// the GDTF zoom-range specification verbatim.
	//
	// v1.0.108 -- DMX Zoom is now driven from the HALF-INTENSITY match angle
	// (`ResolveBeamFootprintMatchHalfDeg() = OuterHalf * (1 + InnerRatio) / 2`) instead
	// of the live `SpotLight->OuterConeAngle`. The Epic-beam canvas is sized by the same
	// geometric truth as the procedural cone (UpdateBeamConeGeometry), so the
	// `Rebus.PreferProceduralBeam 0` (Epic-beam) path inherits the v1.0.108 visible-
	// shaft-vs-lit-disc parity for free. The SpotLight's own OuterConeAngle is still
	// untouched (the lit footprint is unchanged); only the visible shaft tightens.
	const float MatchHalfDeg = ResolveBeamFootprintMatchHalfDeg();
	const float ConeScale = FMath::Max(0.05f, BeamConeRadiusScale);
	const float ZoomFullDeg = FMath::Clamp(RebusEpicBeamZoomScale * ConeScale * MatchHalfDeg, 1.f, 179.f);
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

	// v1.0.57 introduced, v1.0.58 corrected vocabulary, v1.0.59 stripped strobe pushes, v1.0.60
	// forced DMX Dimmer = 1 to stop double-dim: re-push the SpotLight cookie material (MI_Light /
	// M_Light_Master, LightFunction domain) on the same cadence as the volumetric beam. M_Light_
	// Master exposes a different vocabulary than M_Beam_Master (DMX Color / DMX Max Light
	// Intensity / DMX Zoom etc. don't exist on the light function material -- v1.0.57 mistakenly
	// pushed them and they silently no-oped; verified by unpacking the on-disk M_Light_Master.
	// uasset string table at /DMXFixtures/LightFixtures/DMX_Materials/Masters/M_Light_Master).
	// The exposed scalars are DMX Dimmer / DMX Frost + the MF_DMXStrobe inputs (DMX Strobe Open
	// / DMX Strobe Frequency / DMX Strobe Disable Burst) + the MF_DMXGobo atlas (DMX Gobo Disk /
	// DMX Gobo Disk Frosted / DMX Gobo Num Mask / DMX Gobo Index / DMX Gobo Disk Rotation Speed)
	// + a "Use Gobo" StaticSwitchParameter (verified ON in MI_Light because Epic ships a separate
	// MI_LightNoGobo for the opposite case, 5441 bytes larger to carry the static permutation
	// override). UpdateEpicLightFnParams now pushes DMX Dimmer = 1.0 (cookie is a pure spatial
	// pattern, the SpotLight's SetIntensity does all dimmer + shutter + IES + 1/r^2 work) and
	// DMX Frost = live frost. v1.0.59's Dim * Gate push double-dimmed the footprint because the
	// SpotLight intensity already contains that envelope and UE multiplies cookie x light
	// per-pixel -- the beam mesh fades linearly with Dim, but the footprint was collapsing
	// quadratically (Dim^2), which the user observed as "footprint doesn't match the beam or
	// allow for distance change".
	UpdateEpicLightFnParams();
}

void ARebusFixtureActor::UpdateEpicLightFnParams()
{
	// Light function MID is lazily MID'd by ApplyCurrentGoboToLightFn on the first gobo apply --
	// before that there's nothing to push (and the SpotLight's LightFunctionMaterial is null
	// anyway, so no cookie projects regardless of params).
	if (!GoboLightFnMID) return;

	// v1.0.60: collapsed shutter + dimmer into the SpotLight's intensity ONLY. The cookie material
	// is now a PURE SPATIAL PATTERN (DMX Dimmer = 1.0 forced, no shutter-gate multiplied in). The
	// v1.0.59 push of DMX Dimmer = Dim * Gate was double-dimming the footprint: a LightFunction
	// material in UE is multiplied with the light's per-pixel illumination contribution, and that
	// contribution already has SpotLight->SetIntensity (which IS BaseCandela * Dim * Gate -- see
	// RefreshIntensity) baked in. Effective footprint brightness was therefore
	//   BaseCandela * Dim * Gate * IES(angle) * (1/r^2) * pattern * (Dim * Gate)
	//   = BaseCandela * Dim^2 * Gate^2 * IES * (1/r^2) * pattern
	// while the volumetric beam (M_Beam_Master) fades linearly with Dim * Gate via its own DMX
	// Dimmer push -- so at Dim=0.5 the beam was at 50% but the footprint collapsed to 25%; at
	// Dim=0.1 the footprint was 1% of expected. The user observed this as "intensity doesn't
	// match the beam" and "doesn't allow for distance change" (the 1/r^2 falloff WAS being
	// applied via the SpotLight's Candelas-mode physical attenuation, but at low Dim the
	// double-dim crushed the pattern to near-black regardless of distance). v1.0.60 makes the
	// SpotLight the SINGLE source of truth for dimmer + shutter + IES + 1/r^2 + colour: it's
	// the only thing that responds to the dimmer / shutter state, and the cookie just modulates
	// the pre-attenuated illumination by the gobo pattern (texture multiply, no extra envelope).
	// Footprint per-pixel = BaseCandela * Dim * Gate * IES * (1/r^2) * pattern -- linear in Dim,
	// physically correct in distance, IES-accurate in angle. Frost still uses the live Frost.
	// Current because that drives M_Light_Master's blur penumbra, which IS a per-pixel material
	// operation (not a light-source-level thing) and the SpotLight has no equivalent control.
	float Gate = 1.f;
	switch (ShutterMode)
	{
	case ERebusShutterMode::Closed: Gate = 0.f; break;
	case ERebusShutterMode::Strobe: Gate = (ShutterPhase < 0.5f) ? 1.f : 0.f; break;
	default: break;
	}

	const float FrostNorm = FMath::Clamp(Frost.Current, 0.f, 1.f);

	// M_Light_Master vocabulary (verified on disk; unknown params silently no-op so this remains
	// safe across DMXFixtures content revisions). DMX Dimmer = 1.0 is FORCED on the cookie --
	// dimmer/shutter live exclusively on SpotLight->SetIntensity. The Color_Component in Epic's
	// stock BP fires DMX Color at the Beam/Lens materials ONLY -- NOT at DynamicMaterialSpotLight
	// -- so we deliberately do NOT push DMX Color here. The SpotLight's own SetLightColor (in
	// RefreshIntensity) IS the cookie's colour; pushing DMX Color here would silently no-op
	// anyway and add confusion to the trace.
	GoboLightFnMID->SetScalarParameterValue(TEXT("DMX Dimmer"), 1.f);
	GoboLightFnMID->SetScalarParameterValue(TEXT("DMX Frost"), FrostNorm);

	UE_LOG(LogRebusVisualiser, Verbose,
		TEXT("Fixture %s gobo cookie params (v1.0.60): cookie is pure spatial pattern (DMX Dimmer=1 forced, no Dim/Gate baked into the material); SpotLight->SetIntensity = BaseCandela * %.2f * %.2f handles all dimmer + shutter + IES + 1/r^2 falloff. Frost=%.2f (live)."),
		*FixtureId, FMath::Clamp(Dimmer.Current, 0.f, 1.f), Gate, FrostNorm);
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

	// v1.0.95 -- the Epic DMX-Fixtures cone mesh + raymarched `M_RebusBeam` is the only
	// visible volumetric shaft. The
	// per-light scattering computed below is the SEPARATE soft fog-interaction layer that
	// the v1.0.95 `bCastVolumetricShadow=true` makes occluder-carvable. This is the
	// user-reported "make volumetric shadowing work with the Epic beam" feature.

	// A hero shadow beam is one that asked for volumetric shadows AND won a per-batch budget slot.
	const bool bShadowActive = bWantsVolumetricShadow && bGrantedShadowHero;

	if (bMeshBeamEnabled)
	{
		// Mesh cone is the crisp shaft. The per-light scattering layered on top is gated by
		// `Rebus.SpotLightScatter` (v1.0.95 default 0.5) and is what gets carved by occluders
		// thanks to `bCastVolumetricShadow = true` (set unconditionally just below). Hero
		// shadow beams use the higher `Rebus.HeroShadowScatter` value so their VSM truss-gap
		// shafts read clearly through the brighter scattering.
		const float Scatter = bShadowActive
			? GRebusHeroShadowScatter
			: FMath::Max(GRebusSpotLightScatter, 0.f);
		SpotLight->SetVolumetricScatteringIntensity(Scatter);
		// v1.0.94 -- ALWAYS keep per-light shadow casting ON. The pre-v1.0.94 logic
		// (`bShadowActive || bGoboActive`) cleared `CastShadows` on every non-hero non-gobo fixture
		// as a perf opt; combined with MegaLights routing on the same fixtures, this is what
		// produced the user-reported "Epic-beam mode shows no object shadows in the footprint"
		// (root cause C of the v1.0.94 audit -- a SpotLight with CastShadows=false casts NO
		// shadows from any occluder, regardless of the MegaLights opt-out). The VSM volumetric-
		// shadow path (CastVolumetricShadow below) and the v1.0.49 cookie LF path BOTH need
		// CastShadows on, and so does the user-visible solid-shadow path that motivated that
		// release; setting it true unconditionally is the simplest correct policy.
		SpotLight->SetCastShadows(true);
		// v1.0.95: always allow occluder carving. Pre-v1.0.95 this was `bShadowActive` (only
		// hero beams) -- the user wanted "volumetric shadowing to work with the Epic beam" on
		// every fixture, so the gate is dropped. Requires `bEnableVolumetricFog = true` on a
		// scene `AExponentialHeightFog` to be visible (BuildSpotLight logs a Warning per
		// fixture when that's missing, so the failure mode is self-diagnosing).
		SpotLight->SetCastVolumetricShadow(true);
	}
	else
	{
		// Fog-beam A/B mode: restore the froxel beam; volumetric shadow stays on (v1.0.95).
		SpotLight->SetVolumetricScatteringIntensity(FogScatteringIntensity);
		// v1.0.94 -- same policy as the mesh-beam path above (always on -- see the comment there
		// for the root-cause discussion).
		SpotLight->SetCastShadows(true);
		SpotLight->SetCastVolumetricShadow(true);
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

void ARebusFixtureActor::RefreshAllowMegaLightsFromCVar()
{
	if (!SpotLight) return;

	// v1.0.94 -- single chokepoint for resolving the desired `bAllowMegaLights` per the live
	// global gate AND the per-fixture state. When `Rebus.AllowMegaLights = 0` (the v1.0.94
	// default), every Rebus SpotLight runs on the legacy path -- the hard floor for shadow
	// casting in the floor footprint. When `Rebus.AllowMegaLights = 1`, MegaLights is permitted
	// EXCEPT where the per-fixture state explicitly opts out:
	//   * `bGoboActive` -- the v1.0.50 cookie LF path needs the legacy deferred renderer
	//     (M_Light_Master's MF_DMXGobo is not LightFunctionAtlas-compatible).
	// Called from the `Rebus.AllowMegaLights` CVar refresh sink for every Rebus fixture; safe
	// to call directly when the gobo state changes (idempotent / cheap when nothing
	// transitioned -- ReregisterComponent is gated on a value change).
	uint32 Desired = ResolveAllowMegaLights(1u);
	if (Desired != 0)
	{
		if (bGoboActive) Desired = 0;
	}

	const uint32 IsValue = SpotLight->bAllowMegaLights ? 1u : 0u;
	if (IsValue == Desired)
	{
		UE_LOG(LogRebusVisualiser, Verbose,
			TEXT("Fixture %s Rebus.AllowMegaLights refresh: already at %d (gobo=%d), no proxy rebuild."),
			*FixtureId, Desired, bGoboActive ? 1 : 0);
		return;
	}

	SpotLight->bAllowMegaLights = Desired;
	// `bAllowMegaLights` is read at FLightSceneInfo proxy creation; a full ReregisterComponent
	// guarantees the proxy is rebuilt on the next frame with the new value. Cost: brief one-frame
	// blackout on the toggle, identical to the v1.0.92 path.
	SpotLight->ReregisterComponent();

	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s Rebus.AllowMegaLights refresh: bAllowMegaLights %d -> %d (CVar=%d gobo=%d)."),
		*FixtureId, IsValue, Desired,
		GRebusAllowMegaLights, bGoboActive ? 1 : 0);
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
		// with an empty Cumulative -- every per-component axis bucket is INDEX_NONE, so each
		// component holds its imported rest pose in A/B lock-step with the (also static) control
		// meshes rather than diverging.
		DriveOrbitModel(TArray<FTransform>{});
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

	// Drive the bound Orbit-imported model with the SAME per-axis solve that moved the
	// control-channel mesh proxies above. v1.0.68: pass the FULL Cumulative array so each Orbit
	// component rides Cumulative[OrbitAxisBucket[i]] -- base components stay put, yoke arms pan,
	// head pans+tilts. Pre-v1.0.68 we forwarded only Cumulative[HeadAxisIndex] and applied it
	// uniformly to every component, which is why the whole fixture tilted instead of just the
	// head. No-op when not driving / unbound.
	DriveOrbitModel(Cumulative);
}

// ---- Orbit-imported model binding (v1.0.35 introduced; v1.0.65 default ON) --------------

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
	OrbitAxisBucket.Reset();
	BoundOrbitObjectId = MatchedObjectId;
	LastOrbitLogPanTilt = FVector2D(FLT_MAX, FLT_MAX);

	// The imported model corresponds to the fixture's REST pose (pan=tilt=0): every motion axis'
	// cumulative solve is identity at rest (verified: RotateAboutPivot with a zero-angle quat is
	// the identity transform), so for EVERY axis bucket AxisWorldRest = AxisLocalRest * ActorWorld
	// = Identity * ActorWorld = ActorWorld. That collapses the per-axis bind base to a single
	// formula: OrbitBindBase[i] = CompRest * ActorWorld^-1. At runtime each component's driven
	// world is OrbitBindBase[i] * (Cumulative[OrbitAxisBucket[i]] * ActorWorld), so base
	// components (bucket=INDEX_NONE -> Cumulative is Identity) stay at CompRest while yoke/head
	// components ride the pan/tilt cumulative around the rig pivots.
	const FTransform ActorWorld = GetActorTransform();
	const FTransform ActorWorldInv = ActorWorld.Inverse();
	OrbitHeadWorldRest = ComputeHeadLocal(0.f, 0.f) * ActorWorld; // kept for diagnostic legacy

	// v1.0.69: ID-ONLY axis classification. v1.0.68's six-strategy chain (incl. nearest-pivot
	// "position fallback" + default-head safety net) was misclassifying every component as TILT
	// for the user's GLBs -- every mesh on a typical moving-head sits in/around the head volume,
	// so the nearest-pivot heuristic put base, yoke and head ALL on the deepest (tilt) axis. The
	// user's report: "the tilt is no longer moving the orbit fixture but the entire orbit fixture
	// is rotating on pan. No individual yoke/head control. ... can we just use IDs and not
	// location bounding box". Position-based bucketing dropped here; the only strategies left
	// are the ones that read EXPLICIT identifying info off the components themselves
	// (tag-name, comp-name, name-keyword scan, attach-hierarchy depth). When the importer hasn't
	// surfaced any naming, components bucket to INDEX_NONE (static base) -- "nothing moves" is
	// preferable to "wrong thing moves", and the per-fixture warning below tells the orbit-cli /
	// portal team exactly what naming to add.
	const FRebusMotionRig& Rig = Profile.MotionRig;
	int32 DeepestPanAxis = INDEX_NONE;
	int32 DeepestTiltAxis = INDEX_NONE;
	if (Rig.bValid && Rig.Axes.Num() > 0)
	{
		int32 BestPanDepth = -1, BestTiltDepth = -1;
		for (int32 i = 0; i < Rig.Axes.Num(); ++i)
		{
			int32 Depth = 0;
			int32 P = Rig.Axes[i].ParentAxisIndex;
			while (P != INDEX_NONE && Rig.Axes.IsValidIndex(P)) { ++Depth; P = Rig.Axes[P].ParentAxisIndex; }
			if (Rig.Axes[i].Kind == ERebusAxisKind::Pan && Depth > BestPanDepth)
			{
				BestPanDepth = Depth; DeepestPanAxis = i;
			}
			else if (Rig.Axes[i].Kind == ERebusAxisKind::Tilt && Depth > BestTiltDepth)
			{
				BestTiltDepth = Depth; DeepestTiltAxis = i;
			}
		}
	}

	int32 NBase = 0, NPan = 0, NTilt = 0, NOther = 0, NUnclassified = 0;
	TArray<FString> ClassifyDiag;
	ClassifyDiag.Reserve(6);

	for (USceneComponent* Comp : Components)
	{
		if (!Comp) continue;
		const FTransform CompRest = Comp->GetComponentTransform();
		OrbitComponents.Add(Comp);
		OrbitCompRestWorld.Add(CompRest);
		OrbitBindBase.Add(CompRest * ActorWorldInv);

		int32 AxisBucket = INDEX_NONE;
		const TCHAR* Strategy = TEXT("base"); // when there's no rig OR nothing matched (static)

		if (Rig.bValid && Rig.Axes.Num() > 0)
		{
			bool bMatched = false;

			// Strategy 1: each tag (lowercased by ResolveAxisForMesh) against AffectedGeometryNames.
			// Catches the case where the orbit-cli exposes GDTF geometry names on the
			// components (e.g. tag = "yoke" / "head" / "movinghead_head").
			for (const FName& Tag : Comp->ComponentTags)
			{
				const int32 A = RebusMotion::ResolveAxisForMesh(Rig, Tag.ToString(), TEXT(""));
				if (A != INDEX_NONE) { AxisBucket = A; bMatched = true; Strategy = TEXT("tag-name"); break; }
			}

			// Strategy 2: component name (the imported USceneComponent's Outer name) against
			// AffectedGeometryNames. Covers importers that put the GDTF name on the comp itself
			// rather than as a tag.
			if (!bMatched)
			{
				const int32 A = RebusMotion::ResolveAxisForMesh(Rig, Comp->GetName(), TEXT(""));
				if (A != INDEX_NONE) { AxisBucket = A; bMatched = true; Strategy = TEXT("comp-name"); }
			}

			// Strategy 3: substring keyword scan on name + every tag (case-insensitive). Tolerates
			// glb node names like "Light_Head_001" or "MovingHead.yoke.arm" that don't match the
			// GDTF AffectedGeometryNames vocabulary exactly. Priority: head/tilt -> tilt axis,
			// yoke/arm/pan -> pan axis, base/body -> static (INDEX_NONE).
			if (!bMatched)
			{
				FString Hay = Comp->GetName().ToLower();
				for (const FName& Tag : Comp->ComponentTags) { Hay += TEXT(" "); Hay += Tag.ToString().ToLower(); }
				if (Hay.Contains(TEXT("head")) || Hay.Contains(TEXT("tilt")))
				{
					AxisBucket = (DeepestTiltAxis != INDEX_NONE) ? DeepestTiltAxis : HeadAxisIndex;
					if (AxisBucket != INDEX_NONE) { bMatched = true; Strategy = TEXT("keyword-head"); }
				}
				else if (Hay.Contains(TEXT("yoke")) || Hay.Contains(TEXT("arm")) || Hay.Contains(TEXT("pan")))
				{
					AxisBucket = DeepestPanAxis;
					if (AxisBucket != INDEX_NONE) { bMatched = true; Strategy = TEXT("keyword-pan"); }
				}
				else if (Hay.Contains(TEXT("base")) || Hay.Contains(TEXT("body")))
				{
					AxisBucket = INDEX_NONE; bMatched = true; Strategy = TEXT("keyword-base");
				}
			}

			// Strategy 4: attach-hierarchy depth. If the import preserved a parent-child tree
			// (Base -> Yoke -> Head), the deepest component (max GetAttachParent chain length
			// among components in THIS bind) is the head, max-1 is the yoke, etc. Works for
			// any GLB importer that respects glTF node hierarchy. Skipped when all components
			// are at the same depth (typical when the importer flattens everything under one
			// root, which is exactly the case for the user's `StaticMeshComponent_N` set --
			// they all sit at the same attach depth so the heuristic falls through to "static"
			// instead of guessing).
			if (!bMatched)
			{
				int32 MaxDepth = -1;
				TArray<int32> Depths; Depths.Reserve(Components.Num());
				for (USceneComponent* C : Components)
				{
					int32 D = 0;
					for (USceneComponent* P = C ? C->GetAttachParent() : nullptr; P != nullptr; P = P->GetAttachParent()) ++D;
					Depths.Add(D);
					if (D > MaxDepth) MaxDepth = D;
				}
				int32 MinDepth = MaxDepth;
				for (int32 D : Depths) if (D < MinDepth) MinDepth = D;
				if (MaxDepth > MinDepth) // there IS a hierarchy (not all flat)
				{
					int32 MyDepth = 0;
					for (USceneComponent* P = Comp->GetAttachParent(); P != nullptr; P = P->GetAttachParent()) ++MyDepth;
					if (MyDepth == MaxDepth)
					{
						AxisBucket = (DeepestTiltAxis != INDEX_NONE) ? DeepestTiltAxis : HeadAxisIndex;
						if (AxisBucket != INDEX_NONE) { bMatched = true; Strategy = TEXT("depth-head"); }
					}
					else if (MyDepth > MinDepth)
					{
						AxisBucket = DeepestPanAxis;
						if (AxisBucket != INDEX_NONE) { bMatched = true; Strategy = TEXT("depth-yoke"); }
					}
					else
					{
						AxisBucket = INDEX_NONE; bMatched = true; Strategy = TEXT("depth-base");
					}
				}
			}

			// v1.0.69: NO position fallback, NO default-head. If strategies 1-4 didn't fire,
			// AxisBucket stays INDEX_NONE -> the component sits static at its imported rest pose.
			// "Nothing moves" is preferable to "wrong thing moves" -- v1.0.68's position fallback
			// put every component on the deepest (tilt) axis for the user's GLBs because all
			// meshes were geometrically near the head, which fully defeated the per-part split.
			if (!bMatched)
			{
				AxisBucket = INDEX_NONE;
				Strategy = TEXT("unclassified-static");
				++NUnclassified;
			}
		}

		OrbitAxisBucket.Add(AxisBucket);
		if (AxisBucket == INDEX_NONE)
		{
			++NBase;
		}
		else if (Rig.Axes.IsValidIndex(AxisBucket))
		{
			switch (Rig.Axes[AxisBucket].Kind)
			{
				case ERebusAxisKind::Pan:  ++NPan; break;
				case ERebusAxisKind::Tilt: ++NTilt; break;
				default:                   ++NOther; break;
			}
		}
		if (ClassifyDiag.Num() < 6)
		{
			ClassifyDiag.Add(FString::Printf(TEXT("'%s'->%s(a=%d)"), *Comp->GetName(), Strategy, AxisBucket));
		}

		// The bound Orbit model sits right on top of this fixture's light source, so exclude it from
		// its own beam's volumetric-fog shadow (it would otherwise double-occlude with the body).
		DisableSelfBeamVolumetricShadow(Cast<UPrimitiveComponent>(Comp));

		// v1.0.71: apply the body/lens material override to the bound Orbit component too. Uses
		// the comp's own name + concatenated ComponentTags as the lens-keyword source -- catches
		// MVR/glb importers that surface the GDTF geometry name on either field. No-op when
		// bOverrideFixtureMaterials is off.
		if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Comp))
		{
			FString TagStr;
			for (const FName& Tag : Comp->ComponentTags) { TagStr += Tag.ToString(); TagStr += TEXT(" "); }
			ApplyFixtureMaterialTo(Prim, Comp->GetName(), TagStr, /*bIsOrbitComp*/ true);
		}
	}

	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s: BOUND %d Orbit-imported component(s) by objectId '%s' (drive=%s) | axis-buckets: base=%d pan=%d tilt=%d other=%d unclassified=%d | sample: %s"),
		*FixtureId, OrbitComponents.Num(), *MatchedObjectId,
		bDriveOrbitModel ? TEXT("ON") : TEXT("off"),
		NBase, NPan, NTilt, NOther, NUnclassified,
		ClassifyDiag.Num() > 0 ? *FString::Join(ClassifyDiag, TEXT(" | ")) : TEXT("(none)"));

	// v1.0.69: when EVERY component fell through to "unclassified-static" (none of the four
	// ID/name-based strategies matched), surface a one-shot warning per fixture telling the
	// orbit-cli / portal team exactly what naming the visualiser is looking for. Without this
	// the user sees a silently-static Orbit fixture and no indication that the geometry needs
	// labels. We log even when SOME comps classified (NUnclassified > 0 but < total) so a
	// partial classification is visible too.
	if (NUnclassified > 0 && Rig.bValid && Rig.Axes.Num() > 0)
	{
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("Fixture %s: %d/%d Orbit component(s) UNCLASSIFIED (static rest). To enable per-part motion, ")
			TEXT("the orbit-cli / portal should expose ONE of these per mesh component, case-insensitive: ")
			TEXT("(1) a ComponentTag matching a GDTF AffectedGeometryNames entry on this fixture's rig, ")
			TEXT("(2) the component name matching the same, ")
			TEXT("(3) any tag OR name containing the substrings 'head'/'tilt' (-> tilt axis), 'yoke'/'arm'/'pan' (-> pan axis), or 'base'/'body' (-> static), ")
			TEXT("(4) a preserved glTF parent-child hierarchy under OrbitImportRoot (deepest = head, mid = yoke, root = base). ")
			TEXT("Current component names are generic (e.g. '%s') -- recommend tagging each mesh with 'base' / 'yoke' / 'head' on the orbit-cli side."),
			*FixtureId, NUnclassified, OrbitComponents.Num(),
			Components.IsValidIndex(0) && Components[0] ? *Components[0]->GetName() : TEXT("?"));
	}

	// Snap the model to the fixture's current pose now, so a late bind doesn't pop on the next tick.
	if (bDriveOrbitModel)
	{
		DriveOrbitModelFromPanTilt(PanDeg.Current, TiltDeg.Current);
	}

	// v1.0.98: re-assert the cached desired visibility on every freshly-bound component. The
	// scene-property seed `bShowOrbitFixtures=false` lands in the scene-settings catalogue at
	// Initialize time and ApplySceneProperty fires SetOrbitVisibility(false) once the fixtures
	// spawn -- but that runs BEFORE RebindOrbitModels has matched and bound the Orbit-import
	// components on its 1Hz tick, so OrbitComponents was empty and no comp was actually hidden.
	// Apply the cached state here so this (and every subsequent) bind respects the operator's
	// chosen state without an extra round-trip through the scene subsystem. SetOrbitVisibility
	// is happy with bOrbitDesiredVisibility's default true => existing call sites that never
	// touched the new path still get UE-default-visible behaviour.
	if (!bOrbitDesiredVisibility)
	{
		for (const TWeakObjectPtr<USceneComponent>& Weak : OrbitComponents)
		{
			USceneComponent* C = Weak.Get();
			if (!C) continue;
			C->SetVisibility(false, /*bPropagateToChildren*/ true);
		}
	}
}

void ARebusFixtureActor::ClearOrbitBinding()
{
	OrbitComponents.Reset();
	OrbitCompRestWorld.Reset();
	OrbitBindBase.Reset();
	OrbitAxisBucket.Reset();
	BoundOrbitObjectId.Reset();
	// v1.0.71: drop the cached "original material per Orbit comp" map too. After ClearOrbit-
	// Binding the comps we tracked may be destroyed (re-import) -- their weak entries become
	// stale, and a subsequent ApplyFixtureMaterialTo on a freshly bound comp must re-capture
	// the now-current original (not the dead one from the previous import).
	OriginalOrbitMaterials.Reset();
}

int32 ARebusFixtureActor::SetOrbitVisibility(bool bVisible)
{
	// v1.0.70 helper for `Rebus.ShowOrbitFixtures`. Walks the bound components and toggles
	// their rendered visibility (propagating to children so a parent mesh comp with sub-meshes
	// hides as one unit). Returns the count actually toggled so the console command can log
	// a meaningful summary -- weak handles that have died (re-import / late teardown) are
	// silently skipped.
	//
	// v1.0.98: cache the desired state on the actor before iterating. BindOrbitComponents
	// reads this cache at the end of every (re)bind and re-applies the visibility, so a
	// `bShowOrbitFixtures=false` push that arrives BEFORE the 1Hz RebindOrbitModels tick has
	// populated OrbitComponents still hides the components on the first bind (rather than
	// silently iterating an empty list and leaving the freshly-bound components UE-default
	// visible). Required for the v1.0.98 "default-hide Orbit fixtures" seed to actually fire
	// on first launch -- see the v1.0.98 README release block for the timing analysis.
	bOrbitDesiredVisibility = bVisible;
	int32 Affected = 0;
	for (const TWeakObjectPtr<USceneComponent>& Weak : OrbitComponents)
	{
		USceneComponent* C = Weak.Get();
		if (!C) continue;
		C->SetVisibility(bVisible, /*bPropagateToChildren*/ true);
		++Affected;
	}
	return Affected;
}

void ARebusFixtureActor::GetBoundOrbitPrimitives(TSet<UPrimitiveComponent*>& OutSet) const
{
	// v1.0.85: emit every live primitive currently in the fixture's Orbit binding so the
	// subsystem's truss-material pass can skip them. We also walk children of each bound
	// scene component because BindOrbitComponents stores the top of each axis bucket and
	// (depending on the import shape) the actual renderable meshes can sit one level deeper
	// (a parent SceneComponent with StaticMeshComponent children). Recursively unrolling
	// keeps the skip behaviour correct on imports that nest geometry under transform-only
	// nodes (Speckle / glTFRuntime both do this for grouped objects).
	for (const TWeakObjectPtr<USceneComponent>& Weak : OrbitComponents)
	{
		USceneComponent* Top = Weak.Get();
		if (!Top) continue;
		if (UPrimitiveComponent* P = Cast<UPrimitiveComponent>(Top))
		{
			OutSet.Add(P);
		}
		// Local is named ChildComps (not Children) so it doesn't shadow AActor::Children --
		// the Editor target compiles with C4458 (declaration hides class member) treated as an
		// error and would otherwise fail the build (introduced in v1.0.85; fixed v1.0.87.1).
		TArray<USceneComponent*> ChildComps;
		Top->GetChildrenComponents(/*bIncludeAllDescendants*/ true, ChildComps);
		for (USceneComponent* Child : ChildComps)
		{
			if (UPrimitiveComponent* PC = Cast<UPrimitiveComponent>(Child))
			{
				OutSet.Add(PC);
			}
		}
	}
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
			DriveOrbitModelFromPanTilt(PanDeg.Current, TiltDeg.Current);
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

void ARebusFixtureActor::DriveOrbitModelFromPanTilt(float InPanDeg, float InTiltDeg)
{
	// Caller-friendly entry point used by BindOrbitComponents + SetDriveOrbitModel: solve the rig
	// for the requested pan/tilt and forward the Cumulative to DriveOrbitModel. RefreshMotion's
	// already-solved Cumulative is passed straight in to avoid a redundant Solve per tick.
	if (Profile.MotionRig.bValid && Profile.MotionRig.Axes.Num() > 0)
	{
		TArray<FTransform> Cumulative;
		RebusMotion::Solve(Profile.MotionRig, InPanDeg, InTiltDeg, Cumulative);
		DriveOrbitModel(Cumulative);
	}
	else
	{
		DriveOrbitModel(TArray<FTransform>{});
	}
}

void ARebusFixtureActor::DriveOrbitModel(const TArray<FTransform>& Cumulative)
{
	if (!bDriveOrbitModel || OrbitComponents.Num() == 0) return;

	// v1.0.68: per-component drive. Each component rides the Cumulative transform of the axis
	// it was bucketed onto in BindOrbitComponents -- base components (bucket=INDEX_NONE OR an
	// invalid axis index) use Identity, so they stay at CompRest while yoke comps pan and head
	// comps pan+tilt. Pre-v1.0.68 the same HeadLocal got applied to every component, so the
	// entire fixture tilted (the user's report). The bind-base captured at rest collapses to
	// CompRest * ActorWorld^-1 for ALL buckets (every axis' RestCumulative is identity), which
	// is why one OrbitBindBase array can drive every axis without per-axis storage.
	const FTransform ActorWorld = GetActorTransform();
	int32 Driven = 0;
	for (int32 i = 0; i < OrbitComponents.Num(); ++i)
	{
		USceneComponent* Comp = OrbitComponents[i].Get();
		if (!Comp || !OrbitBindBase.IsValidIndex(i)) continue;
		const int32 Axis = OrbitAxisBucket.IsValidIndex(i) ? OrbitAxisBucket[i] : INDEX_NONE;
		const FTransform AxisLocal = (Axis != INDEX_NONE && Cumulative.IsValidIndex(Axis))
			? Cumulative[Axis] : FTransform::Identity;
		Comp->SetWorldTransform(OrbitBindBase[i] * (AxisLocal * ActorWorld));
		++Driven;
	}
	if (Driven == 0) return;

	// Per-update sync log (throttled to meaningful pan/tilt changes) so the Orbit model motion can
	// be compared against the control-channel head meshes for the A/B confirmation. HeadLocal is
	// recomputed for the log only -- the per-component drive already used the per-bucket axis.
	const FVector2D PanTilt(PanDeg.Current, TiltDeg.Current);
	if (FMath::Abs(PanTilt.X - LastOrbitLogPanTilt.X) + FMath::Abs(PanTilt.Y - LastOrbitLogPanTilt.Y) > 0.5f)
	{
		LastOrbitLogPanTilt = PanTilt;
		const FTransform HeadLocal = (HeadAxisIndex != INDEX_NONE && Cumulative.IsValidIndex(HeadAxisIndex))
			? Cumulative[HeadAxisIndex] : FTransform::Identity;
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
	// v1.0.64: Focus (bipolar around 0.5) contributes a defocus amount that ADDITIVELY combines
	// with Frost to soften the inner cone and enlarge the apparent source. This is what makes
	// "pull the beam in/out of focus" visible WITHOUT a gobo -- defocused -> wider penumbra,
	// soft edge; sharp -> crisp edge. Combined amount is clamped to 1 so either alone (or both
	// together) reaches the same maximum softening (matches the GoboRT blur cap in OnGoboRTUpdate).
	const float DefocusAmount = FMath::Clamp(FMath::Abs(Focus.Current - 0.5f) * 2.f, 0.f, 1.f);
	const float SoftenAmount = FMath::Clamp(FMath::Clamp(Frost.Current, 0.f, 1.f) + DefocusAmount, 0.f, 1.f);
	const float FrostSoften = FMath::Lerp(1.f, 0.2f, SoftenAmount);
	const float InnerHalf = OuterHalf * InnerRatio * FrostSoften;

	SpotLight->SetOuterConeAngle(OuterHalf);
	SpotLight->SetInnerConeAngle(FMath::Min(InnerHalf, OuterHalf));

	// Frost+defocus also enlarge the apparent source for softer penumbra. Scale the resolved
	// base source radius (the lens-opening disc set in BuildSpotLight) so the beam-origin
	// diameter stays consistent with the lens-flare disc; left untouched when no source size
	// was known.
	if (BaseSourceRadiusUnreal >= 0.f)
	{
		SpotLight->SetSourceRadius(BaseSourceRadiusUnreal * FMath::Lerp(1.f, 4.f, SoftenAmount));
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

	// v1.0.91 -- the per-fixture .ies file's PEAK CANDELA is now the authoritative BASE
	// intensity when an IES is loaded (`IesCandelaMax >= 0`). The pre-v1.0.91 fallback path
	// derived BaseCandela from photometrics.LuminousFlux / FieldAngle (Setup, §8.1) -- that
	// estimate is still the floor for fixtures that arrive without an .ies file at all, but
	// when the .ies IS present its candela max wins because:
	//   * the .ies file is the photometric authority (real measurement, not an estimate from
	//     a single flux + full angle),
	//   * IES units mode = Candelas was already set in BuildSpotLight, so the value is fed
	//     directly to UE in physically-meaningful units (no unit conversion), and
	//   * the existing dimmer + shutter-gate stay as linear MULTIPLIERS on top, so the
	//     operator's wire surface (SetFixtureDimmer / shutter) and DMX intensity flow are
	//     untouched -- this change is BASE only.
	// `bUseIESBrightness` stays false so the IES texture only reshapes the spatial falloff
	// (peak texel = 1.0 -> the SpotLight's Intensity acts as the per-fixture peak candela
	// directly); flipping it true would also bake the candela max into the texture sample
	// and double-up with this Intensity write.
	const float Cd = (IesCandelaMax >= 0.f) ? IesCandelaMax : BaseCandela;
	SpotLight->SetIntensity(Cd * FMath::Clamp(Dimmer.Current, 0.f, 1.f) * Gate);

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

	// v1.0.101 -- one Verbose log per ApplyZoom proving the single-source-of-truth chain
	// for the user's "beam slightly larger than footprint" diagnosis. Prints the input
	// half (= ZoomDeg target), the canonical resolved outer half (= ResolveOuterHalfDeg
	// = ResolveZoomHalfDeg(half * 2), GDTF zoom-range clamp + iris pinch), the live
	// SpotLight OuterConeAngle (the lit footprint extent -- MUST equal the resolved
	// outer half within float precision), the cone-mesh far-radius the procedural
	// frustum would build at this half-angle (BeamLength * tan(half) * scale), and the
	// per-fixture BeamConeRadiusScale knob that pulls the visible shaft tighter than
	// the lit footprint. Verbose-level so a busy show isn't spammed; flip
	// `Log LogRebusVisualiser Verbose` (or use `Rebus.DumpFixtureZoom`) to surface.
	// v1.0.108 -- the verbose ApplyZoom log now also reports the half-intensity match
	// half (`ResolveBeamFootprintMatchHalfDeg` = OuterHalf * (1 + InnerRatio) / 2) which
	// is the angle the visible cone-mesh is actually sized to (so its edge coincides
	// with the bright floor disc the SpotLight's linear taper produces). The
	// `coneFarRadius` value below derives from MatchHalf, NOT from the geometric outer
	// half -- that's the v1.0.108 fix for the "cone is much wider than the spotlight
	// footprint" report. SpotLight outer cone stays at the un-pinched ResolvedHalf
	// (its lit footprint is unchanged).
	const float ResolvedHalf = ResolveOuterHalfDeg();
	const float MatchHalf    = ResolveBeamFootprintMatchHalfDeg();
	const float SpotOuterDeg = SpotLight ? SpotLight->OuterConeAngle : -1.f;
	const float SpotInnerDeg = SpotLight ? SpotLight->InnerConeAngle : -1.f;
	const float TanMatch = FMath::Tan(FMath::DegreesToRadians(MatchHalf));
	const float ConeScale = FMath::Max(0.05f, BeamConeRadiusScale);
	const float ConeFarRadius = FMath::Max(BeamLengthUnreal * TanMatch * ConeScale, BeamBaseRadiusUnreal + 0.1f);
	UE_LOG(LogRebusVisualiser, Verbose,
		TEXT("Fixture %s ApplyZoom: zoomTarget=%.2fdeg(half) resolvedHalf=%.2fdeg matchHalf=%.2fdeg "
			 "spotOuterCone=%.2fdeg spotInnerCone=%.2fdeg coneFarRadius=%.1fcm "
			 "beamLength=%.1fcm beamConeRadiusScale=%.3f fade=%.2fs (v1.0.108: cone-mesh "
			 "FarRadius is sized to matchHalf -- the SpotLight's half-intensity ring -- so "
			 "the visible shaft edge coincides with the bright floor disc; spotOuter is "
			 "unchanged at resolvedHalf so the lit footprint is unchanged)"),
		*FixtureId, ZoomHalfAngleDeg, ResolvedHalf, MatchHalf, SpotOuterDeg, SpotInnerDeg,
		ConeFarRadius, BeamLengthUnreal, ConeScale, FadeSeconds);
}

void ARebusFixtureActor::ApplyIris(float Iris01, float FadeSeconds)
{
	Iris.SetTarget(FMath::Clamp(Iris01, 0.f, 1.f), FadeSeconds);
	bAnimating = true;
	if (FadeSeconds <= 0.f) RecomputeConeAngles();
	// v1.0.63: iris now bakes a circular mask into GoboRT (in OnGoboRTUpdate). Without a kick
	// here, a single-shot iris change (FadeSeconds == 0) would update the SpotLight cone but
	// NOT the cookie until the next gobo-spin/Tick redraw, leaving the floor stencil stale. With
	// a fade, the Tick fall-through (bConeAnim path) will keep the RT in sync.
	if (bGoboActive && GoboRT && FadeSeconds <= 0.f)
	{
		GoboRT->UpdateResource();
	}
}

void ARebusFixtureActor::ApplyFocus(float Focus01, float FadeSeconds)
{
	// v1.0.64: Focus is BIPOLAR around 0.5 (sharp at midpoint, max defocus at 0 or 1). The
	// default reset value is 0.5 (ResetAnimatedToDefaults), so a fresh fixture starts perfectly
	// focused. Defocus = abs(Focus - 0.5) * 2 folds into both the GoboRT multi-tap blur
	// (OnGoboRTUpdate) and the inner-cone / source-radius soften (RecomputeConeAngles), so the
	// effect is visible on the gobo AND on the no-gobo beam edge. Same redraw-kick contract as
	// ApplyIris / ApplyFrost: instant single-shot changes need to refresh both the cone and the
	// cookie immediately; fades fall through the Tick bConeAnim path.
	Focus.SetTarget(FMath::Clamp(Focus01, 0.f, 1.f), FadeSeconds);
	bAnimating = true;
	if (FadeSeconds <= 0.f) RecomputeConeAngles();
	if (bGoboActive && GoboRT && FadeSeconds <= 0.f)
	{
		GoboRT->UpdateResource();
	}
}

void ARebusFixtureActor::ApplyFrost(float Frost01, float FadeSeconds)
{
	Frost.SetTarget(FMath::Clamp(Frost01, 0.f, 1.f), FadeSeconds);
	bAnimating = true;
	if (FadeSeconds <= 0.f) RecomputeConeAngles();
	// v1.0.63: frost now also drives a multi-tap blur of the gobo INSIDE GoboRT (in
	// OnGoboRTUpdate). Same redraw-kick rationale as ApplyIris -- a single-shot frost change
	// must refresh the cookie so the new tap count / offset takes effect immediately.
	if (bGoboActive && GoboRT && FadeSeconds <= 0.f)
	{
		GoboRT->UpdateResource();
	}
}

void ARebusFixtureActor::ApplyColorTemp(float Kelvin)
{
	if (!SpotLight) return;
	SpotLight->SetUseTemperature(true);
	SpotLight->SetTemperature(FMath::Clamp(Kelvin, 1000.f, 15000.f));
}

void ARebusFixtureActor::ApplyShutter(ERebusShutterMode Mode, float RateHz)
{
	// v1.0.61: harden against three field bugs the user hit:
	//   (1) Strobe + RateHz=0 silently did nothing (the Tick branch advancing ShutterPhase
	//       requires ShutterRateHz > KINDA_SMALL_NUMBER), so the portal could request a
	//       strobe and see the light just stay continuously lit. We now coerce to a sensible
	//       default (RebusDefaultStrobeHz, 5Hz) and log a Warning so the portal team can
	//       see they need to send rateHz alongside mode=2.
	//   (2) Every call reset ShutterPhase = 0, which combined with portals that re-send the
	//       SAME shutter state every tick (the user's logs show 4 duplicates per ms) meant
	//       the strobe never progressed past phase 0 (Gate = 1 throughout). Now we only
	//       reset ShutterPhase when Mode or Rate actually changed, so duplicate "stay
	//       strobing" pushes are no-ops on the phase accumulator.
	//   (3) No echo log: previously ApplyShutter was silent; the user could only see
	//       "Descriptor type 'SetFixtureShutter'" with nothing after. Now we log the
	//       resolved (Mode, Rate) so the chain is fully visible.
	constexpr float RebusDefaultStrobeHz = 5.f;
	constexpr float RebusMaxStrobeHz = 30.f;

	float ResolvedRate = FMath::Clamp(RateHz, 0.f, RebusMaxStrobeHz);
	if (Mode == ERebusShutterMode::Strobe && ResolvedRate <= KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("Fixture %s ApplyShutter(Strobe) with rateHz=%.2f -- defaulting to %.1fHz so the strobe actually progresses. Portal should send {\"mode\":2,\"rateHz\":<1..30>} for explicit control."),
			*FixtureId, RateHz, RebusDefaultStrobeHz);
		ResolvedRate = RebusDefaultStrobeHz;
	}

	const bool bModeChanged = (ShutterMode != Mode);
	const bool bRateChanged = !FMath::IsNearlyEqual(ShutterRateHz, ResolvedRate);
	ShutterMode = Mode;
	ShutterRateHz = ResolvedRate;
	if (bModeChanged || bRateChanged)
	{
		ShutterPhase = 0.f;
	}

	const TCHAR* ModeName =
		(Mode == ERebusShutterMode::Open)   ? TEXT("Open")   :
		(Mode == ERebusShutterMode::Closed) ? TEXT("Closed") : TEXT("Strobe");
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s ApplyShutter: mode=%s rate=%.2fHz (changed: mode=%s rate=%s) -- gate now drives SpotLight->SetIntensity in RefreshIntensity + EpicBeamMID DMX Dimmer in UpdateEpicBeamParams (cookie inherits transitively)."),
		*FixtureId, ModeName, ShutterRateHz,
		bModeChanged ? TEXT("yes") : TEXT("no"),
		bRateChanged ? TEXT("yes") : TEXT("no"));

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
		float ParsedCandelaMax = -1.f;
		if (UTextureLightProfile* Prof = RebusIes::BuildLightProfile(this, Inline->Bytes, &ParsedCandelaMax))
		{
			ActiveIesProfile = Prof;
			SpotLight->SetIESTexture(Prof);
			// Keep the portal's brightness authority for the SPATIAL distribution (§8.2 step 4).
			// The PEAK CANDELA from the .ies file is folded into SpotLight->Intensity via
			// IesCandelaMax + RefreshIntensity below (v1.0.91), so the user's request "use the
			// IES profile + IES intensity" is satisfied without flipping bUseIESBrightness on
			// (which would double-bake the candela max -- see RefreshIntensity comment).
			SpotLight->bUseIESBrightness = false;
			SpotLight->IESBrightnessScale = 1.f;
			SpotLight->MarkRenderStateDirty();
			bActiveIesInline = true;
			CurrentIesZoomDmx = Inline->ZoomDmx;
			IesCandelaMax = ParsedCandelaMax;
			ActiveIesProfileId = Inline->ProfileId;
			RefreshIntensity();
			UE_LOG(LogRebusVisualiser, Verbose,
				TEXT("Fixture %s IES applied: profile=%s zoomDmx=%d candelaMax=%.0f intensityUnits=Candelas finalIntensity=%.0f (source=inline)"),
				*FixtureId, *Inline->ProfileId, Inline->ZoomDmx,
				IesCandelaMax, SpotLight->Intensity);
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
		// v1.0.91 -- also drop the IES candela max so RefreshIntensity falls back to the
		// flux-derived BaseCandela on the next push (otherwise the SpotLight would keep
		// shining at the prior IES's peak even after the .ies file was removed).
		SpotLight->SetIESTexture(nullptr);
		ActiveIesProfile = nullptr;
		CurrentIesZoomDmx = -1;
		bActiveIesInline = false;
		IesCandelaMax = -1.f;
		ActiveIesProfileId.Reset();
		RefreshIntensity();
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

	const int32 PendingZoomDmx = CurrentIesZoomDmx;
	TWeakObjectPtr<ARebusFixtureActor> WeakThis(this);
	RestClient->FetchBytes(IesUrl, FRebusBytesFetched::CreateLambda(
		[WeakThis, PendingZoomDmx](bool bOk, const TArray<uint8>& Bytes)
		{
			ARebusFixtureActor* Self = WeakThis.Get();
			if (!Self || !bOk || !Self->SpotLight) return;
			float ParsedCandelaMax = -1.f;
			UTextureLightProfile* Prof = RebusIes::BuildLightProfile(Self, Bytes, &ParsedCandelaMax);
			if (!Prof) return;
			Self->ActiveIesProfile = Prof;
			Self->SpotLight->SetIESTexture(Prof);
			// Keep the portal's brightness authority for the SPATIAL distribution (§8.2 step 4).
			// PEAK CANDELA is folded into SpotLight->Intensity below (v1.0.91, same path as the
			// inline branch above) -- see RefreshIntensity comment for why bUseIESBrightness
			// stays false.
			Self->SpotLight->bUseIESBrightness = false;
			Self->SpotLight->IESBrightnessScale = 1.f;
			Self->SpotLight->MarkRenderStateDirty();
			Self->IesCandelaMax = ParsedCandelaMax;
			Self->ActiveIesProfileId = FString::Printf(TEXT("url:%d"), PendingZoomDmx);
			Self->RefreshIntensity();
			UE_LOG(LogRebusVisualiser, Verbose,
				TEXT("Fixture %s IES applied: profile=%s zoomDmx=%d candelaMax=%.0f intensityUnits=Candelas finalIntensity=%.0f (source=url)"),
				*Self->FixtureId, *Self->ActiveIesProfileId, PendingZoomDmx,
				Self->IesCandelaMax, Self->SpotLight->Intensity);
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
	RefreshGoboLumenIsolation();  // v1.0.83: removes the spotlight from Lumen GI while the
	                              //          cookie is animating -- kills the GI ghost layer
	                              //          without touching global Lumen CVars.
	// v1.0.102: push the new GoboRT + bGoboActive=1 onto every per-fixture lens MID so the
	// lens face starts showing the gobo silhouette immediately (alongside the cookie footprint
	// landing on the floor). Cheap when IsBeamLensMIDs is empty.
	RefreshLensEmissive();
	return true;
}

// v1.0.75: configurable per-actor gobo RT resolution.
//   * Pre-v1.0.75 default was 512 (a balance for cost vs. cookie clarity). User reported the
//     projected pattern looks pixelated -- expected, because 512 across a typical 60-degree
//     stage throw at 8m gives ~3cm/texel on the floor. Bumping the default to 1024 (4x area)
//     halves the texel footprint and the cookie reads crisp at typical throws with no
//     measurable perf impact (canvas redraw of 8 textured quads is bandwidth-bound, not fill-
//     bound, at this size). Hero shows can push to 2048/4096 via Rebus.GoboRTSize.
//   * Mipmaps NOT generated pre-v1.0.75 -- so distant footprints aliased hard. We now enable
//     bAutoGenerateMips + TF_Trilinear so the LF sampler picks the right LOD by screen
//     footprint and the small-on-screen lights stay clean.
namespace
{
	constexpr int32 GRebusGoboRTDefaultSize = 1024;
	constexpr int32 GRebusGoboRTMinSize = 128;
	constexpr int32 GRebusGoboRTMaxSize = 8192;

	int32 ClampAndPow2(int32 N)
	{
		N = FMath::Clamp(N, GRebusGoboRTMinSize, GRebusGoboRTMaxSize);
		int32 Pow2 = 1; while (Pow2 < N) Pow2 <<= 1; return Pow2;
	}
}

void ARebusFixtureActor::EnsureGoboRT()
{
	// v1.0.53: lazy per-fixture RT used to redraw the source gobo texture rotated by GoboAngle.
	// v1.0.75: default size bumped from 512 -> 1024 + auto-mips + trilinear filter (see the
	// namespace comment above for the cost/quality reasoning). The 1024 default lives in
	// GRebusGoboRTDefaultSize; per-fixture/per-call overrides use RebuildGoboRTAtSize.
	if (GoboRT) return;

	GoboRT = UCanvasRenderTarget2D::CreateCanvasRenderTarget2D(this, UCanvasRenderTarget2D::StaticClass(),
		GRebusGoboRTDefaultSize, GRebusGoboRTDefaultSize);
	if (!GoboRT)
	{
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("Fixture %s gobo RT: CreateCanvasRenderTarget2D returned null; falling back to direct CurrentGoboTexture push (no in-plane rotation)."),
			*FixtureId);
		return;
	}
	GoboRT->ClearColor = FLinearColor::Transparent;
	// v1.0.74: ASSERT the clear-before-update flag explicitly. UCanvasRenderTarget2D defaults
	// this to true in 5.7, but a future engine default flip OR an external write (one of the
	// editor utility callbacks does set it false on some assets) would silently turn the RT
	// into an accumulator -- successive K2_DrawTexture calls with BLEND_Translucent on top of
	// the unCLEARED prior frame would build up a smear of every recent gobo orientation,
	// looking exactly like ghosting on the floor projection. v1.0.76: the field is `protected`
	// in 5.7 so direct assignment was C2248; we go through FProperty reflection (see
	// SetGoboRTClearOnUpdate above) which writes the UPROPERTY regardless of C++ access.
	SetGoboRTClearOnUpdate(GoboRT, true);
	// v1.0.75: mipmap chain + trilinear filtering. Without these the LF sampler reads a single
	// LOD regardless of screen footprint, so a 1024 RT projected onto a tiny floor patch alias
	// hard (every other texel skipped). bAutoGenerateMips defers chain generation to the GPU
	// each UpdateResource(), which is essentially free at our sizes (1k-2k); the cost is one
	// mip-pyramid blit per redraw, negligible compared to the per-frame ApplyCurrentGobo path.
	GoboRT->bAutoGenerateMips = true;
	GoboRT->Filter = TF_Trilinear;
	GoboRT->OnCanvasRenderTargetUpdate.AddDynamic(this, &ARebusFixtureActor::OnGoboRTUpdate);
	GoboRT->UpdateResource(); // first redraw so the param push isn't blank
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s gobo RT allocated %dx%d (mips=%d filter=Trilinear) at %p (src=%s GoboAngle=%.1fdeg)"),
		*FixtureId, GoboRT->SizeX, GoboRT->SizeY,
		GoboRT->bAutoGenerateMips ? 1 : 0,
		GoboRT.Get(),
		CurrentGoboTexture ? *CurrentGoboTexture->GetName() : TEXT("<null>"),
		GoboAngle);
}

int32 ARebusFixtureActor::RebuildGoboRTAtSize(int32 RequestedSizePixels)
{
	// v1.0.75: rebuild GoboRT at a new pow2 size. If we already have one at the requested
	// size, no-op (still pushes through the MIDs in case a re-bind is wanted -- cheap). If
	// not, drop the old, allocate fresh via EnsureGoboRT after nulling, then re-bind through
	// the same ApplyCurrentGoboToEpicBeam tail-call that the normal gobo-load path uses so
	// EpicBeamMID + GoboLightFnMID pick up the new RT pointer.
	const int32 Size = ClampAndPow2(RequestedSizePixels);

	if (GoboRT && GoboRT->SizeX == Size && GoboRT->SizeY == Size)
	{
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Fixture %s RebuildGoboRTAtSize: already %dx%d; no-op (re-binding MIDs anyway)."),
			*FixtureId, Size, Size);
		ApplyCurrentGoboToEpicBeam();
		return Size;
	}

	// Drop the old RT. UE GC will collect it; the delegate binding was on the OLD object so
	// nothing leaks from us. (No explicit RemoveDynamic needed -- the old object dying takes
	// the delegate entry with it.)
	if (GoboRT)
	{
		GoboRT->OnCanvasRenderTargetUpdate.RemoveDynamic(this, &ARebusFixtureActor::OnGoboRTUpdate);
		GoboRT = nullptr;
		LastGoboRTUpdateTex = nullptr; // force a fresh draw on the new RT
	}

	// Allocate at the requested size by temporarily overriding the const-default path. We
	// duplicate the EnsureGoboRT body here instead of parameterising it because callers of
	// the normal path always want the default size; the override is a per-actor explicit ask.
	GoboRT = UCanvasRenderTarget2D::CreateCanvasRenderTarget2D(this, UCanvasRenderTarget2D::StaticClass(),
		Size, Size);
	if (!GoboRT)
	{
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("Fixture %s RebuildGoboRTAtSize: CreateCanvasRenderTarget2D returned null at %dx%d."),
			*FixtureId, Size, Size);
		return 0;
	}
	GoboRT->ClearColor = FLinearColor::Transparent;
	SetGoboRTClearOnUpdate(GoboRT, true); // v1.0.76: protected field, reflection writer.
	GoboRT->bAutoGenerateMips = true;
	GoboRT->Filter = TF_Trilinear;
	GoboRT->OnCanvasRenderTargetUpdate.AddDynamic(this, &ARebusFixtureActor::OnGoboRTUpdate);
	GoboRT->UpdateResource();

	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s RebuildGoboRTAtSize: rebuilt at %dx%d (mips=1 filter=Trilinear) at %p; re-binding MIDs."),
		*FixtureId, Size, Size, GoboRT.Get());

	// Re-push the new RT pointer into the cookie + cone MIDs so the new frame's
	// LightFunctionMaterial / DMX Gobo Disk Frosted slot points at it.
	ApplyCurrentGoboToEpicBeam();
	return Size;
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
	//
	// v1.0.63: this single draw is now extended with TWO baking passes:
	//   1. Multi-tap FROST blur: M_Light_Master's "DMX Frost" scalar (pushed in
	//      UpdateEpicLightFnParams) does NOT visibly soften the gobo edges -- it governs the
	//      surrounding penumbra fall-off (RecomputeConeAngles also widens the source radius +
	//      contracts the inner cone), so frost without a gobo softens the lit pool but with a
	//      gobo the projected stencil stays razor-sharp. We compensate by box-blurring the gobo
	//      INSIDE the RT: one centre tap (1/N alpha) + (N-1) ring taps at sub-pixel offsets,
	//      where N and the offset radius both scale with Frost.Current. At Frost=0 only the
	//      centre tap runs (identical to pre-v1.0.63 behaviour).
	//   2. Circular IRIS crop: a procedurally-generated white-on-transparent disc
	//      (EnsureIrisMaskTexture) is drawn full-RT-size with BLEND_Modulate so dst.alpha *=
	//      mask.alpha -- the cookie projects through a circular aperture matching the iris
	//      position instead of pinching the SpotLight outer cone (which had the side-effect of
	//      ZOOMING the gobo pattern). The mask is skipped entirely at Iris >= ~1.0 (open).
	if (!Canvas || !CurrentGoboTexture) return;
	const float Side = (float)FMath::Min(Width, Height);
	if (Side <= 0.f) return;
	const FVector2D ScreenPos(((float)Width - Side) * 0.5f, ((float)Height - Side) * 0.5f);
	const FVector2D ScreenSize(Side, Side);
	const FVector2D CoordPos(0.f, 0.f);
	const FVector2D CoordSize(1.f, 1.f);
	const FVector2D Pivot(0.5f, 0.5f);

	// ---- Pass 1: gobo draw (centre tap + (frost+defocus)-scaled ring taps) ------------------
	// v1.0.64: Focus is bipolar around 0.5 (sharp at midpoint, max defocus at 0 or 1). Defocus
	// is folded ADDITIVELY into the same multi-tap blur Frost already drives -- both produce a
	// soft gobo halo, so re-using the path avoids a second 8-tap pass. The combined amount is
	// clamped to 1 so the maximum tap offset stays inside the gobo's circular boundary regardless
	// of whether the softness came from frost, defocus, or both.
	const float FrostNorm = FMath::Clamp(Frost.Current, 0.f, 1.f);
	const float DefocusNorm = FMath::Clamp(FMath::Abs(Focus.Current - 0.5f) * 2.f, 0.f, 1.f);
	const float BlurNorm = FMath::Clamp(FrostNorm + DefocusNorm, 0.f, 1.f);
	const int32 RingTaps = (BlurNorm > KINDA_SMALL_NUMBER) ? 8 : 0;
	const int32 TotalTaps = 1 + RingTaps;
	// Offset is a fraction of the RT side -- small at low blur, ~2.5% of RT (~13 px on a 512 RT,
	// which projects to a noticeable halo at typical throws) at max blur. Keep small enough not
	// to bleed off the gobo's circular boundary.
	const float TapOffsetPx = BlurNorm * (Side * 0.025f);
	const float TapAlpha = 1.f / (float)TotalTaps;
	const FLinearColor TapTint(1.f, 1.f, 1.f, TapAlpha);

	Canvas->K2_DrawTexture(
		CurrentGoboTexture.Get(),
		ScreenPos, ScreenSize,
		CoordPos, CoordSize,
		TapTint,
		BLEND_Translucent,
		GoboAngle,
		Pivot);
	for (int32 i = 0; i < RingTaps; ++i)
	{
		const float Ang = (float)i / (float)RingTaps * 2.f * PI;
		const float Ox = FMath::Cos(Ang) * TapOffsetPx;
		const float Oy = FMath::Sin(Ang) * TapOffsetPx;
		Canvas->K2_DrawTexture(
			CurrentGoboTexture.Get(),
			ScreenPos + FVector2D(Ox, Oy), ScreenSize,
			CoordPos, CoordSize,
			TapTint,
			BLEND_Translucent,
			GoboAngle,
			Pivot);
	}

	// ---- Pass 2: iris circular crop (BLEND_Modulate) ----------------------------------------
	const float IrisNorm = FMath::Clamp(Iris.Current, 0.f, 1.f);
	if (IrisNorm < 0.999f)
	{
		EnsureIrisMaskTexture(IrisNorm); // cheap no-op when quantised value hasn't changed
		if (IrisMaskTex)
		{
			Canvas->K2_DrawTexture(
				IrisMaskTex.Get(),
				FVector2D(0.f, 0.f), FVector2D((float)Width, (float)Height),
				CoordPos, CoordSize,
				FLinearColor::White,
				BLEND_Modulate,
				0.f,
				FVector2D(0.5f, 0.5f));
		}
	}

	// v1.0.102: keep the lens-material emissive in lockstep with the cookie redraw.
	// SetTextureParameterValue cached the GoboRT pointer once; the lens material samples
	// the RT's current contents automatically, so the gobo silhouette on the lens face
	// rotates in lockstep with the cone/cookie without an explicit re-push. We still
	// call RefreshLensEmissive here to keep bUseGobo / EmissiveIntensity in sync with
	// any dimmer/colour/shutter changes that happened during the same Tick frame the RT
	// got redrawn -- cheap (few SetParameter calls) and removes a class of "lens drifts
	// behind the cone by 1 frame" failure modes.
	RefreshLensEmissive();
}

void ARebusFixtureActor::EnsureIrisMaskTexture(float Iris01)
{
	// v1.0.63: lazy / debounced procedural circular mask for the iris pass in OnGoboRTUpdate.
	// We quantise to 0.01 so a 1 s iris fade only regenerates ~100 times (~50 us each at 128 px),
	// avoiding per-frame allocations. The mask is 128 BGRA8 -- bilinear sampling on the RT
	// smooths the disc edge nicely when drawn at 512 RT-side. ALL FOUR channels are set to the
	// same circular alpha value so BLEND_Modulate (`Dst *= Src` on RGBA) zeroes RGB and A
	// outside the iris circle in lockstep: that matters because `M_Light_Master` samples the
	// cookie's RGB (not alpha) to weight the light function output -- a pure alpha mask would
	// leave the gobo pattern bright in the cropped area, breaking the iris effect.
	const float Q = FMath::Clamp(FMath::RoundToFloat(Iris01 * 100.f) / 100.f, 0.f, 1.f);
	if (IrisMaskTex && FMath::IsNearlyEqual(LastIrisMaskValue, Q))
	{
		return;
	}
	constexpr int32 Size = 128;
	if (!IrisMaskTex)
	{
		IrisMaskTex = UTexture2D::CreateTransient(Size, Size, PF_B8G8R8A8,
			FName(*FString::Printf(TEXT("RebusIrisMask_%s"), *FixtureId)));
		if (!IrisMaskTex)
		{
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("Fixture %s iris mask: UTexture2D::CreateTransient returned null; iris-crop disabled."),
				*FixtureId);
			return;
		}
		IrisMaskTex->SRGB = false;
		IrisMaskTex->Filter = TF_Bilinear;
		IrisMaskTex->AddressX = TA_Clamp;
		IrisMaskTex->AddressY = TA_Clamp;
		IrisMaskTex->NeverStream = true;
	}
	LastIrisMaskValue = Q;

	FTexturePlatformData* PD = IrisMaskTex->GetPlatformData();
	if (!PD || PD->Mips.Num() == 0) return;
	FTexture2DMipMap& Mip = PD->Mips[0];
	uint8* Data = (uint8*)Mip.BulkData.Lock(LOCK_READ_WRITE);
	if (!Data) return;

	const float Cx = (float)Size * 0.5f;
	const float Cy = (float)Size * 0.5f;
	const float MaxR = (float)Size * 0.5f - 0.5f;
	// Iris radius spans 0..MaxR. A floor of 0.5 px stops the disc collapsing to no pixels at
	// Iris=0 (the cookie is already alpha-zeroed everywhere then, but the BLEND_Modulate pass
	// still expects a sane texture).
	const float IrisR = FMath::Max(MaxR * Q, 0.5f);
	constexpr float EdgePx = 1.5f; // soft anti-alias edge in mask pixels (~6 RT pixels at 512)
	for (int32 y = 0; y < Size; ++y)
	{
		for (int32 x = 0; x < Size; ++x)
		{
			const float Dx = (float)x + 0.5f - Cx;
			const float Dy = (float)y + 0.5f - Cy;
			const float D = FMath::Sqrt(Dx * Dx + Dy * Dy);
			const float A = FMath::Clamp((IrisR - D) / EdgePx, 0.f, 1.f);
			const uint8 Au = (uint8)FMath::RoundToInt(A * 255.f);
			uint8* Px = &Data[(y * Size + x) * 4];
			Px[0] = Au; // B
			Px[1] = Au; // G
			Px[2] = Au; // R
			Px[3] = Au; // A
		}
	}
	Mip.BulkData.Unlock();
	IrisMaskTex->UpdateResource();
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
			// v1.0.59: only kick a redraw when the SOURCE gobo has actually changed since the
			// previous call. The v1.0.53 unconditional UpdateResource() ran on every
			// ApplyCurrentGoboToEpicBeam invocation, including the per-Tick path through
			// UpdateEpicBeamParams during dimmer/motion/colour fades -- each call cleared the
			// RT to transparent before OnGoboRTUpdate redrew, and the cookie LightFunction
			// material sampled the "clear" frame between the clear and the draw, producing a
			// per-fade flash on the footprint that the user reported as the cookie strobing
			// even with shutter Open. Comparing TObjectPtr equality on the underlying UObject*
			// gates the redraw to actual gobo-change events (ApplyGoboTextureFromBytes,
			// SetInlineGobos re-push, ClearGoboToOpen). The Tick spin block still kicks a
			// redraw every frame when GoboAngle is animating -- that's correct because the
			// RT contents need to change to show the rotation; LastGoboRTUpdateTex is not
			// reset there because the SOURCE texture hasn't changed.
			if (GoboRT && CurrentGoboTexture != LastGoboRTUpdateTex)
			{
				GoboRT->UpdateResource();
				LastGoboRTUpdateTex = CurrentGoboTexture;
			}
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
		// v1.0.94 -- gobo-active branch ALWAYS forces 0 (independent of `Rebus.AllowMegaLights`):
		// the cookie LF path needs the legacy deferred renderer regardless of the global gate.
		// `ResolveAllowMegaLights(0)` returns 0 in either CVar state, so this is identity here
		// but routing through the helper keeps every assignment site consistent.
		SpotLight->bAllowMegaLights = ResolveAllowMegaLights(0);
		// Push the same single-cell atlas params as the cone. The MF_DMXGobo inside M_Light_Master
		// reads the texture identically; "Num Mask = 1, Index = 0" sweeps the entire texture.
		// v1.0.52: as on EpicBeamMID, pin "DMX Gobo Disk Rotation Speed" to 0 -- it's a U-scroll
		// not an image rotation.
		// v1.0.53: push the per-fixture GoboRT (drawn rotated every tick in OnGoboRTUpdate)
		// instead of CurrentGoboTexture directly, so the cookie spins in plane around the
		// projection's out-of-screen axis. EnsureGoboRT was already called by
		// ApplyCurrentGoboToEpicBeam (this is its tail call), so GoboRT is ready.
		UTexture* CookieTex = GoboRT ? static_cast<UTexture*>(GoboRT.Get()) : static_cast<UTexture*>(CurrentGoboTexture.Get());
		// v1.0.58: push BOTH gobo texture params on the cookie material. Epic's stock GoboWheel
		// _Component writes to BOTH `DMX Gobo Disk Frosted` AND `DMX Gobo Disk` (the clean +
		// frosted disc textures) on DynamicMaterialSpotLight; M_Light_Master internally blends /
		// picks one of them. We were only pushing Frosted, so if M_Light_Master is sampling the
		// clean disc instead, it sampled the default texture and never showed our gobo.
		GoboLightFnMID->SetTextureParameterValue(TEXT("DMX Gobo Disk Frosted"), CookieTex);
		GoboLightFnMID->SetTextureParameterValue(TEXT("DMX Gobo Disk"), CookieTex);
		GoboLightFnMID->SetScalarParameterValue(TEXT("DMX Gobo Num Mask"), 1.f);
		GoboLightFnMID->SetScalarParameterValue(TEXT("DMX Gobo Index"), 0.f);
		GoboLightFnMID->SetScalarParameterValue(TEXT("DMX Gobo Disk Rotation Speed"), 0.f);
		// v1.0.57 introduced, v1.0.58 corrected vocabulary, v1.0.59 stripped strobe pushes,
		// v1.0.60 forced DMX Dimmer = 1 to stop double-dim: prime the non-gobo M_Light_Master
		// scalars so the cookie isn't multiplied by 0 the very first frame after the MID is
		// created. v1.0.57 mistakenly pushed M_Beam_Master vocabulary here (DMX Color / DMX
		// Max Light Intensity / DMX Zoom etc.) which DOES NOT EXIST on M_Light_Master -- silent
		// no-ops. v1.0.58 switched to the correct vocabulary (DMX Dimmer + DMX Strobe Open +
		// DMX Strobe Frequency + DMX Strobe Disable Burst + DMX Frost). v1.0.59 dropped DMX
		// Strobe Open / Frequency / Disable Burst because they feed MF_DMXStrobe's internal Sine
		// and were self-modulating the cookie (footprint flashed even with our gate at 1).
		// v1.0.60 forced DMX Dimmer to 1.0 because v1.0.59's Dim * Gate push double-dimmed the
		// cookie -- the LightFunction material is multiplied with the per-pixel light
		// contribution, which already contains Dim * Gate via SpotLight->SetIntensity (see
		// RefreshIntensity), so cookie brightness collapsed quadratically (Dim^2 * Gate^2) while
		// the beam mesh fades linearly (Dim * Gate). The cookie is now a pure spatial pattern:
		// SpotLight->SetIntensity handles dimmer + shutter + IES + 1/r^2 in physical units, and
		// the cookie just modulates the result by the gobo texture. The cookie became visible
		// in v1.0.58 thanks to the DMX Gobo Disk push above (M_Light_Master's MF_DMXGobo samples
		// the clean disc texture, which we'd never written until then), not Strobe Open.
		UpdateEpicLightFnParams();
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
		// v1.0.94: route through `ResolveAllowMegaLights` so `Rebus.AllowMegaLights = 0` (the
		// default) keeps the SpotLight on the legacy path even after the cookie clears -- the
		// hard floor wins (dynamic occluders cast shadows in the footprint regardless of gobo
		// state). When `Rebus.AllowMegaLights = 1` the pre-v1.0.94 behaviour is preserved (back
		// to MegaLights perf when no gobo is active).
		const bool bPrevMegaLights = SpotLight->bAllowMegaLights != 0;
		SpotLight->bAllowMegaLights = ResolveAllowMegaLights(1);
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
	// v1.0.59: also reset LastGoboRTUpdateTex so the next non-Open assignment (CurrentGoboTexture
	// becomes non-null) is correctly detected as a CHANGE by ApplyCurrentGoboToEpicBeam and
	// triggers a single deliberate redraw of the new source.
	if (GoboRT)
	{
		GoboRT->UpdateResource();
	}
	LastGoboRTUpdateTex = nullptr;
	// Push the cleared state into BOTH the cone (reverts EpicBeamMID to its MI default) and the
	// cookie (nulls SpotLight->LightFunctionMaterial). Then reassert CastShadows so the now-cleared
	// bGoboActive removes any gobo-driven shadow override on non-hero beams.
	ApplyCurrentGoboToEpicBeam();
	RefreshBeamShadowMode();
	RefreshGoboLumenIsolation(); // v1.0.83: re-enable Lumen GI contribution now that the cookie
	                             //          is no longer animating.
	// v1.0.102: push bGoboActive=false (=> bUseGobo=0) onto every per-fixture lens MID so the
	// lens face stops showing the (now cleared) gobo silhouette and reverts to uniform colour
	// glow modulated by dimmer x intensity. Cheap when IsBeamLensMIDs is empty.
	RefreshLensEmissive();
}

void ARebusFixtureActor::RefreshGoboLumenIsolation()
{
	if (!SpotLight) return;

	// While a gobo is active, the spotlight is projecting a per-frame-changing cookie pattern
	// onto whatever the cone hits. Lumen samples that lit surface sparsely for indirect bounce
	// and accumulates the result temporally -- which is exactly what produces the residual
	// "ghost in the GI" trail behind a rotating cookie that survived TSR-side mitigations. By
	// removing this single light from Lumen's indirect-lighting set while bGoboActive is true,
	// the bounce contribution from a cookie-projecting fixture no longer feeds Lumen's
	// temporal history, so there's nothing for Lumen to ghost.
	//
	// Direct lighting (the lit floor itself) is unaffected; only the bounce off that floor onto
	// surrounding geometry is suppressed for the cookie's duration. For a stage lighting
	// visualiser this is invisible 95%+ of the time -- spotlights with cookies are aimed at the
	// floor / set pieces, and the missing-bounce hit is just a slightly less-glowy room.
	// Cleared the second the cookie is removed (ClearGoboToOpen -> RefreshGoboLumenIsolation
	// reasserts !bGoboActive which is now false -> GI back on).
	const bool bDesired = !bGoboActive;
	if (SpotLight->bAffectGlobalIllumination == bDesired)
	{
		return; // no-op fast path
	}
	SpotLight->SetAffectGlobalIllumination(bDesired);
	UE_LOG(LogRebusVisualiser, Verbose,
		TEXT("Fixture %s gobo Lumen-isolation: SetAffectGlobalIllumination(%d) (bGoboActive=%d)"),
		*FixtureId, bDesired ? 1 : 0, bGoboActive ? 1 : 0);
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

// ---- v1.0.80 live state snapshot for the FixtureStates outbound stream ----------------

FRebusFixtureStateSnapshot ARebusFixtureActor::GetFixtureStateSnapshot() const
{
	FRebusFixtureStateSnapshot S;
	S.FixtureId      = FixtureId;
	// Read the LIVE faded values (.Current), not the .Target -- the portal then sees the
	// dimmer / pan / tilt / zoom / iris / frost / focus / colour MOVING through the fade
	// rather than snapping to the endpoint.
	S.Dimmer         = Dimmer.Current;
	S.PanDeg         = PanDeg.Current;
	S.TiltDeg        = TiltDeg.Current;
	// v1.0.84: ZoomDeg.Current is the INTERNAL half-angle (matches SpotLight->OuterConeAngle
	// semantics). The wire convention -- both `SetFixtureZoom`'s inbound `zoomDeg` field and
	// this outbound `FixtureStates.zoomDeg` -- is FULL beam angle so the portal can use the
	// same number it ships with no conversion. Multiply by 2 here mirrors the * 0.5 conversion
	// in URebusFixtureControlSubsystem::SetFixtureZoom.
	S.ZoomDeg        = ZoomDeg.Current * 2.f;
	S.Iris           = Iris.Current;
	S.Frost          = Frost.Current;
	S.Focus          = Focus.Current;
	S.Color          = FLinearColor(ColorR.Current, ColorG.Current, ColorB.Current, 1.f);
	// SpotLight->Temperature is only meaningful when bUseTemperature is true; otherwise we
	// report -1 so the portal knows the colour-temp slider is currently inactive.
	if (SpotLight && SpotLight->bUseTemperature)
	{
		S.ColorTempK = SpotLight->Temperature;
	}
	S.ShutterMode    = static_cast<int32>(ShutterMode);
	S.ShutterRateHz  = ShutterRateHz;
	S.GoboIndex      = CurrentGoboIndex;
	S.GoboWheelIndex = CurrentGoboWheelIndex;
	// CurrentGoboRotationSpeed already folds the gobo + animation wheel inputs the way Epic's
	// reference materials do (single rotation param); AnimWheelSpeed is the standalone
	// animation-wheel input so the portal can show both sliders rather than one combined.
	S.GoboRotSpeed   = CurrentGoboRotationSpeed;
	S.AnimWheelSpeed = CurrentAnimationWheelSpeed;
	return S;
}

// ---- Tick (fades + strobe + gobo spin) ------------------------------------------------

void ARebusFixtureActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	bool bStillAnimating = false;

	const bool bMotionAnim = PanDeg.Tick(DeltaSeconds) | TiltDeg.Tick(DeltaSeconds);
	const bool bIntensityAnim = Dimmer.Tick(DeltaSeconds) | ColorR.Tick(DeltaSeconds) | ColorG.Tick(DeltaSeconds) | ColorB.Tick(DeltaSeconds);
	// v1.0.64: Focus is now bipolar around 0.5 (sharp at midpoint, max defocus at 0 or 1) and
	// folds into BOTH the multi-tap GoboRT blur (OnGoboRTUpdate) and the inner-cone / source-
	// radius softening (RecomputeConeAngles) so the user gets a visible "pull in/out of focus"
	// on both the gobo stencil AND the no-gobo beam edge. Hoisting Focus.Tick into bConeAnim
	// drives the redraw + cone-recompute on every fade frame, matching how Frost is wired.
	const bool bConeAnim = ZoomDeg.Tick(DeltaSeconds) | Iris.Tick(DeltaSeconds) | Frost.Tick(DeltaSeconds) | Focus.Tick(DeltaSeconds);

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

	// v1.0.63: bConeAnim covers zoom/iris/frost. Iris + frost now bake into the GoboRT (circular
	// mask + multi-tap blur in OnGoboRTUpdate), so the cookie has to redraw whenever either
	// fades -- without this kick the cone-angle/source-radius would update each frame but the
	// floor stencil would freeze until the next gobo selection / spin tick. Only redraws while
	// a real gobo is loaded (bGoboActive + non-null GoboRT); when no gobo is in, the cone-pinch
	// path in ResolveOuterHalfDeg already handles iris and there's nothing to bake.
	if (bConeAnim && bGoboActive && GoboRT)
	{
		GoboRT->UpdateResource();
	}

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
