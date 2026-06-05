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
// v1.0.87: SpotLight VolumetricScatteringIntensity pushed onto every fixture while InternalBeam
// mode is ON. The InternalBeam pose hides the Epic / cone-mesh beam and promotes the same SpotLight
// that already lights the floor to ALSO be the visible volumetric shaft -- so a scatter value of
// 1.0 (the default) produces a clearly visible beam in the default exponential-height-fog without
// blasting the lit pool. Live-tunable via `Rebus.InternalBeamScatter <float>`; on change every
// fixture currently in InternalBeam mode re-pushes its volumetric state immediately.
float GRebusInternalBeamScatter = 1.0f;

// v1.0.89: sign multiplier applied to the InternalBeam back-offset. The offset itself is the
// SCALAR `lensRadius / tan(maxZoomHalfAngle)` (always >= 0); whether we ADD it along the
// SpotLight's local +X (the GDTF emission axis after MakeFromXZ) or SUBTRACT it depends on
// which way that axis empirically resolves on the user's GDTF profile.
//
// v1.0.87 hard-coded the sign as `-LiveFwd` (subtract along emission, intending to push the
// spotlight INTO the head body so the cone exits at the lens diameter). For the user's
// observed GDTF the spotlight ended up IN FRONT of the lens instead, proving that on this
// content the spotlight's local +X actually points INTO the head -- inverting the sign needed
// to push the spot UP-stream from the lens. v1.0.89 default is `+1` (the corrected direction
// for that report); the operator can flip to `-1` via `Rebus.InternalBeamOffsetSign -1` if a
// different GDTF authoring convention emerges. The CVar's refresh sink walks every live
// fixture and re-runs RefreshMotion so the offset re-applies in-place without a respawn.
float GRebusInternalBeamOffsetSign = 1.0f;
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

FAutoConsoleVariableRef CVarRebusInternalBeamScatter(
	TEXT("Rebus.InternalBeamScatter"),
	GRebusInternalBeamScatter,
	TEXT("SpotLight VolumetricScatteringIntensity pushed onto every fixture while InternalBeam mode (v1.0.87) is ON. Live -- changing this re-pushes the value to every fixture currently in InternalBeam mode."),
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
					if (F->IsInternalBeamModeEnabled())
					{
						F->RefreshBeamShadowMode();
						++Refreshed;
					}
				}
			}
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.InternalBeamScatter -> %.2f, refreshed %d InternalBeam fixture(s)."),
			CVar->GetFloat(), Refreshed);
	}),
	ECVF_Default);

// v1.0.94 -- HARD FLOOR for MegaLights routing on every Rebus SpotLight. Default 0 (legacy
// clustered/deferred path forced on every fixture, regardless of mode -- InternalBeam OR Epic-beam,
// regardless of gobo state). The user reported (against v1.0.93) that "with the Epic beam system
// we have, we are not seeing the shadow of an object in the footprint": objects placed between a
// fixture and the floor were NOT casting shadow into the lit pool when the fixture was running in
// the v1.0.x DEFAULT (Epic DMX-Fixtures beam) mode. Root cause: UE 5.5+ MegaLights routes
// per-light shadow casting through the tile-clustered sampling path, which on the user's
// hardware/quality tier silently drops dynamic occluders below MegaLights' shadow-fidelity floor
// -- so a hanging truss / a person / a prop between the fixture and the floor lit up but cast no
// silhouette. v1.0.92's per-fixture opt-out was scoped to InternalBeam-mode fixtures only; v1.0.94
// extends the opt-out to ALL Rebus SpotLights so dynamic occluders ALWAYS cast hard shadows in
// the floor footprint of EVERY Rebus fixture, in EVERY mode.
//
// Trade-off: every Rebus fixture loses MegaLights' clustering perf -- in show-context rigs (tens to
// hundreds of fixtures) this is the right default because shadow fidelity is non-negotiable for
// stage visualisation. Deployments that prioritise MegaLights' clustering over per-fixture shadow
// fidelity can flip the gate to 1 (`Rebus.AllowMegaLights 1`); the CVar refresh sink walks every
// Rebus fixture and re-resolves bAllowMegaLights based on the new value AND the fixture's current
// gobo / InternalBeam state.
//
// Precedence vs the v1.0.92 `Rebus.InternalBeamForceLegacy` CVar: `Rebus.AllowMegaLights = 0` is
// the HARD FLOOR -- ALL Rebus SpotLights run on the legacy path; the v1.0.92 CVar is then
// redundant (every InternalBeam fixture is already off MegaLights). When `Rebus.AllowMegaLights =
// 1`, the v1.0.92 CVar becomes effective again: it ONLY adds the InternalBeam-mode reassertion
// (so an InternalBeam fixture with `Rebus.InternalBeamForceLegacy = 1` still goes legacy for the
// volumetric LF/IES shaping reason documented in the v1.0.92 block, while non-InternalBeam
// fixtures stay on MegaLights for perf). Both CVars live; new releases prefer
// `Rebus.AllowMegaLights` because it is the more fundamental gate.
int32 GRebusAllowMegaLights = 0;

// Resolve the desired `bAllowMegaLights` value for a Rebus SpotLight given a per-call requested
// value (typically 1 for "MegaLights-on" / 0 for "explicitly opt-out for gobo/InternalBeam"). The
// `Rebus.AllowMegaLights` CVar is the hard floor: when 0, ALWAYS return 0; when 1, pass the
// requested value through unchanged. Used at every assignment site of `SpotLight->bAllowMegaLights`
// so every code path agrees on the policy. v1.0.94 introduced.
static FORCEINLINE uint32 ResolveAllowMegaLights(uint32 RequestedAllow)
{
	return (GRebusAllowMegaLights == 0) ? 0u : (RequestedAllow ? 1u : 0u);
}

// v1.0.92 -- gate for the InternalBeam-mode `bAllowMegaLights = 0` push. Default 1 (force the
// legacy clustered/deferred path, so the SpotLight's IES profile AND its LightFunctionMaterial
// modulate the volumetric beam shaft). The user reported (against v1.0.90) that gobo and IES
// shape the lit floor footprint correctly but DON'T reach the visible volumetric cone, even
// after the v1.0.89 `r.LightFunctionAtlas.Enabled 1` push. Root cause: UE 5.7 MegaLights bypasses
// the IES texture AND the LightFunctionMaterial when computing volumetric scattering -- the
// many-lights-per-pixel sampling pipeline doesn't sample either. So on every InternalBeam-enabled
// fixture v1.0.92 forces `bAllowMegaLights = 0` (cached prior value, restored byte-exact on
// disable) so the spotlight goes through the legacy path that DOES route both onto the
// volumetric integrator. Cost: this fixture loses MegaLights' clustering perf for the duration
// of the mode -- acceptable, the InternalBeam mode is itself a high-fidelity hero-beam choice.
//
// Operators on deployments that prefer MegaLights' efficiency over volumetric LF/IES shaping can
// flip the gate to 0 (`Rebus.InternalBeamForceLegacy 0`) -- the per-fixture push is then skipped
// and `bAllowMegaLights` is left at whatever BuildSpotLight + the gobo-active path already set
// it to. The CVar refresh sink walks every fixture currently in InternalBeam mode and toggles
// the flag in-place: ON re-applies the opt-out (re-caching the current value), OFF restores the
// cached value (so a deployment can A/B at runtime without a full Rebus.InternalBeam 0/1 cycle).
//
// v1.0.94 -- this CVar is now SUBORDINATE to `Rebus.AllowMegaLights`. When `Rebus.AllowMegaLights
// = 0` (the v1.0.94 default) every Rebus SpotLight is already on the legacy path; the
// InternalBeam-specific push is then redundant but harmless. When `Rebus.AllowMegaLights = 1`,
// flipping `Rebus.InternalBeamForceLegacy` to 1 still adds the InternalBeam-mode reassertion as
// the sole way to keep the volumetric LF/IES shaping for hero beams while non-InternalBeam
// fixtures stay on MegaLights for perf.
int32 GRebusInternalBeamForceLegacy = 1;

// v1.0.89 -- live-toggleable sign for the InternalBeam back-offset application. See the comment
// on `GRebusInternalBeamOffsetSign` above for the rationale. On change, every fixture currently
// in InternalBeam mode re-runs RefreshMotion so the offset re-applies in-place; this is the only
// state that depends on the sign (volumetric scatter / cast-shadow / body-shadow walk are
// independent), so RefreshMotion is sufficient -- ApplyInternalBeamPose is not required.
FAutoConsoleVariableRef CVarRebusInternalBeamOffsetSign(
	TEXT("Rebus.InternalBeamOffsetSign"),
	GRebusInternalBeamOffsetSign,
	TEXT("Sign multiplier (+1 default / -1 inverted) for the InternalBeam back-offset application. v1.0.87 hard-coded the sign as -1 along the SpotLight's local +X; v1.0.89 fixes the reported 'spotlight ended up in FRONT of the lens' regression by defaulting to +1 (the corrected direction for the observed GDTF convention). Operator flip-flop for fixture profiles that resolve the emission axis the other way: Rebus.InternalBeamOffsetSign -1."),
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
					if (F->IsInternalBeamModeEnabled())
					{
						F->RefreshInternalBeamOffset();
						++Refreshed;
					}
				}
			}
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.InternalBeamOffsetSign -> %+.0f, refreshed %d InternalBeam fixture(s)."),
			CVar->GetFloat(), Refreshed);
	}),
	ECVF_Default);

// v1.0.94 -- live-toggleable HARD FLOOR for MegaLights routing on every Rebus SpotLight. See
// the comment on `GRebusAllowMegaLights` above for the rationale. Default 0 (every Rebus
// SpotLight uses the legacy clustered/deferred path so dynamic occluders ALWAYS cast hard
// shadows in the floor footprint, regardless of mode). Refresh sink walks EVERY Rebus fixture
// (not just InternalBeam ones, unlike the v1.0.92 `Rebus.InternalBeamForceLegacy` sink) and
// re-resolves `bAllowMegaLights` per the new value via `RefreshAllowMegaLightsFromCVar`,
// re-registering the SpotLight component when the value transitions so the FLightSceneInfo
// proxy is rebuilt with the new value on the next frame.
FAutoConsoleVariableRef CVarRebusAllowMegaLights(
	TEXT("Rebus.AllowMegaLights"),
	GRebusAllowMegaLights,
	TEXT("0|1 -- when 0 (default since v1.0.94), every Rebus SpotLight is forced off MegaLights "
		 "(`bAllowMegaLights = 0`) so the legacy clustered/deferred path renders shadow casting "
		 "for dynamic occluders -- the v1.0.94 fix for 'we are not seeing the shadow of an object "
		 "in the [Epic-beam] footprint'. When 1, MegaLights routing is allowed: per-fixture state "
		 "(gobo cookie active / InternalBeam mode + `Rebus.InternalBeamForceLegacy = 1`) can still "
		 "force the legacy path on a per-fixture basis, but non-special fixtures get MegaLights' "
		 "clustering perf back -- at the cost of dropping dynamic-occluder shadows in their "
		 "footprint on low-fidelity tiers. Live -- changing this re-pushes / restores on every "
		 "Rebus fixture in every loaded world. PRECEDENCE: this is the hard floor; "
		 "`Rebus.InternalBeamForceLegacy` only adds InternalBeam-mode reassertion ON TOP and is "
		 "redundant while `Rebus.AllowMegaLights = 0`."),
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
			bAllow ? TEXT("MegaLights routing now permitted -- per-fixture gobo/InternalBeam paths still force legacy when active; non-special fixtures get clustering perf back")
			       : TEXT("HARD FLOOR -- every Rebus SpotLight forced off MegaLights so dynamic occluders cast hard shadows in the footprint (Epic-beam mode AND InternalBeam mode)"));
	}),
	ECVF_Default);

// v1.0.92 -- live-toggleable gate for the InternalBeam-mode `bAllowMegaLights=0` push. See the
// comment on `GRebusInternalBeamForceLegacy` above for the rationale. The refresh sink walks
// every InternalBeam fixture and either re-applies the opt-out (CVar -> 1) or restores the
// cached prior value (CVar -> 0). The two transitions go through dedicated helpers on the
// actor (PushInternalBeamMegaLightsOptOut + RestoreInternalBeamMegaLights) so the cache state
// stays consistent regardless of how many times the operator toggles the CVar -- a 0->1->0
// sequence on a fixture that started with bAllowMegaLights=1 always lands back on 1.
//
// v1.0.94 -- when `Rebus.AllowMegaLights = 0` is in force this CVar is redundant (the hard
// floor already opted every Rebus fixture out of MegaLights). The refresh sink still runs and
// is harmless -- the `RefreshAllowMegaLightsFromCVar` chokepoint resolves the same final value.
FAutoConsoleVariableRef CVarRebusInternalBeamForceLegacy(
	TEXT("Rebus.InternalBeamForceLegacy"),
	GRebusInternalBeamForceLegacy,
	TEXT("0|1 -- when 1 (default since v1.0.92), every fixture in InternalBeam mode (v1.0.87+) has "
		 "`bAllowMegaLights=0` forced on its SpotLight so the legacy clustered/deferred path renders "
		 "the IES profile AND the LightFunctionMaterial through the volumetric scattering integrator. "
		 "Without this push, UE 5.7 MegaLights bypasses both on the volumetric beam shaft (the "
		 "many-lights-per-pixel pipeline doesn't sample either), so a gobo cookie + IES profile are "
		 "visible on the lit floor footprint but the visible volumetric cone reads as a uniform "
		 "untextured beam. Cost: the spotlight loses MegaLights' clustering perf for the duration of "
		 "InternalBeam mode (the floor lighting falls back to the legacy path). When 0, "
		 "`bAllowMegaLights` is left at whatever BuildSpotLight + the gobo-active path set it to "
		 "(MegaLights stays on except where ApplyCurrentGoboToLightFn explicitly turns it off for "
		 "the cookie). Live -- changing this re-pushes / restores on every fixture currently in "
		 "InternalBeam mode."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
		if (!GEngine) return;
		const bool bForce = (CVar->GetInt() != 0);
		int32 Refreshed = 0;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* W = Ctx.World();
			if (!W) continue;
			for (TActorIterator<ARebusFixtureActor> It(W); It; ++It)
			{
				if (ARebusFixtureActor* F = *It)
				{
					if (F->IsInternalBeamModeEnabled())
					{
						if (bForce) F->PushInternalBeamMegaLightsOptOut();
						else        F->RestoreInternalBeamMegaLights();
						++Refreshed;
					}
				}
			}
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.InternalBeamForceLegacy -> %d, refreshed %d InternalBeam fixture(s) (%s)."),
			CVar->GetInt(), Refreshed,
			bForce ? TEXT("forced bAllowMegaLights=0 -- IES + light function modulate the volumetric shaft via the legacy path")
			       : TEXT("restored cached bAllowMegaLights -- MegaLights perf back, volumetric IES/LF on the shaft NOT guaranteed"));
	}),
	ECVF_Default);

// v1.0.93 -- gate for the cookie-cone path inside InternalBeam mode. Default 1 (cone-mesh
// shaft on; the v1.0.92 per-light volumetric scattering is suppressed, the gobo cookie
// reaches the shaft via the new `M_RebusGoboLightFunction` LF MID, and the visible cone is
// the new `InternalBeamShaft` translucent additive mesh that starts AT the lens plane).
//
// User report (v1.0.92 -> v1.0.93): the gobo cookie was visible on the floor footprint but
// NOT in the volumetric beam shaft; AND the volumetric beam was visible inside the head
// (the v1.0.87 back-offset pushes the SpotLight inside the head body, so the engine's
// per-light volumetric pass paints scattering everywhere in the cone including behind the
// lens). v1.0.93 fixes BOTH by decoupling "lit footprint" from "visible shaft": the
// SpotLight only lights surfaces (VolumetricScatteringIntensity = 0); the shaft is a
// translucent additive cone-mesh that starts AT the lens and samples the GoboRT directly.
//
// Flip to 0 to revert to the v1.0.92 per-light volumetric scattering path for A/B (useful
// to diagnose whether a Volumetric-Fog tuning issue is in the new shaft material vs the
// engine's per-light path). The CVar refresh sink walks every fixture currently in
// InternalBeam mode and toggles the per-fixture state in-place (Apply or Restore) so the
// operator can A/B at runtime without a `Rebus.InternalBeam 0/1` cycle.
int32 GRebusInternalBeamCookieCone = 1;
FAutoConsoleVariableRef CVarRebusInternalBeamCookieCone(
	TEXT("Rebus.InternalBeamCookieCone"),
	GRebusInternalBeamCookieCone,
	TEXT("0|1 -- when 1 (default since v1.0.93), every fixture in InternalBeam mode replaces "
		 "the v1.0.92 per-light volumetric scattering with a translucent additive cone-mesh "
		 "(`M_RebusInternalBeamShaft`) that starts AT the lens plane, samples the per-fixture "
		 "GoboRT directly so the cookie pattern appears IN the shaft (not just on the floor "
		 "footprint), and avoids the v1.0.87 'shaft inside the head' artefact (the SpotLight "
		 "is back-offset INSIDE the head body; the engine's per-light volumetric pass painted "
		 "scattering everywhere in the cone including behind the lens). The SpotLight also "
		 "gets `M_RebusGoboLightFunction` (a Python-authored LF with `bUsedWithVolumetricFog = "
		 "true`, the smoking-gun flag Epic's stock MI_Light is missing) so its volumetric "
		 "contribution -- separate from the cone-mesh shaft, ungated by Rebus.InternalBeamForce "
		 "Legacy -- is also gobo-shaped when the engine routes the LF through. Flip to 0 to "
		 "revert to v1.0.92's per-light scattering for A/B (the cone-mesh is hidden, "
		 "VolumetricScatteringIntensity is restored to Rebus.InternalBeamScatter). Live."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
		if (!GEngine) return;
		const bool bOn = (CVar->GetInt() != 0);
		int32 Refreshed = 0;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* W = Ctx.World();
			if (!W) continue;
			for (TActorIterator<ARebusFixtureActor> It(W); It; ++It)
			{
				if (ARebusFixtureActor* F = *It)
				{
					if (F->IsInternalBeamModeEnabled())
					{
						if (bOn) F->ApplyInternalBeamShaft(/*bForceVisible*/ true);
						else     F->RestoreInternalBeamShaft();
						++Refreshed;
					}
				}
			}
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.InternalBeamCookieCone -> %d, refreshed %d InternalBeam fixture(s) (%s)."),
			CVar->GetInt(), Refreshed,
			bOn ? TEXT("cone-mesh shaft ON; SpotLight VolumetricScatteringIntensity forced to 0; gobo cookie on shaft via M_RebusInternalBeamShaft + M_RebusGoboLightFunction")
			    : TEXT("cone-mesh shaft hidden; SpotLight VolumetricScatteringIntensity restored to Rebus.InternalBeamScatter (v1.0.92 path)"));
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
	if (UserLensMatFinder.Succeeded()) FixtureLensMaterialOverride = UserLensMatFinder.Object;

	// v1.0.93 -- cook-safe hard refs to the two new Python-authored masters (baked by
	// `build_rebus_base_level.py::ensure_gobo_light_function_material` +
	// `ensure_internal_beam_shaft_material` on startup):
	//
	//   * `M_RebusGoboLightFunction` -- domain = MD_LightFunction, `bUsedWithVolumetricFog =
	//     true`. The smoking-gun flag that's MISSING from Epic's stock MI_Light: it's a
	//     per-UMaterial gate that decides whether the SpotLight->LightFunctionMaterial
	//     reaches the volumetric scattering integrator. v1.0.49+ bound MI_Light which lacks
	//     this flag, so every CVar push (r.LightFunctionAtlas.Enabled / r.VolumetricFog.
	//     LightFunction / r.LightFunctionQuality) was necessary but not sufficient -- the
	//     material itself never opted into the volumetric path. v1.0.93 authors our own LF.
	//
	//   * `M_RebusInternalBeamShaft` -- unlit additive cone-mesh shader. v1.0.87 promoted
	//     the SpotLight INSIDE the head for the back-offset trick, then set Volumetric
	//     ScatteringIntensity > 0 so the same SpotLight provided the visible shaft -- but
	//     the engine then painted scattering INSIDE the head too (the user's report). The
	//     v1.0.93 cone-mesh shaft starts AT the lens plane and is the only visible cone, so
	//     the SpotLight's per-light scattering can be turned off (no painting inside head).
	//
	// FObjectFinder resolves in-editor / during cook. If a path is missing (Python builder
	// hasn't run yet on a fresh checkout, or older project drop predates v1.0.93) the ref
	// stays null and `EnsureFixtureInternalBeamMIDs` logs a benign one-shot Warning telling
	// the operator to run `build_rebus_base_level.py` from the editor.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> GoboLfMatFinder(
		TEXT("/Game/REBUS/Materials/M_RebusGoboLightFunction.M_RebusGoboLightFunction"));
	if (GoboLfMatFinder.Succeeded()) GoboLightFunctionMaterial = GoboLfMatFinder.Object;
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> InternalBeamShaftMatFinder(
		TEXT("/Game/REBUS/Materials/M_RebusInternalBeamShaft.M_RebusInternalBeamShaft"));
	if (InternalBeamShaftMatFinder.Succeeded()) InternalBeamShaftMaterial = InternalBeamShaftMatFinder.Object;
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

void ARebusFixtureActor::EnsureFixtureInternalBeamMIDs()
{
	// v1.0.93 -- lazy per-actor MIDs off the Python-baked masters
	// (`M_RebusGoboLightFunction` + `M_RebusInternalBeamShaft`). If a master is missing
	// (Python builder hasn't run yet on this checkout, or older project drop), we log a
	// one-shot Warning per missing material and leave the MID null -- the v1.0.92 fallback
	// path then keeps working (per-light volumetric scattering, gobo via Epic's MI_Light).
	if (!GoboLightFunctionMID)
	{
		if (GoboLightFunctionMaterial)
		{
			GoboLightFunctionMID = UMaterialInstanceDynamic::Create(GoboLightFunctionMaterial, this);
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Fixture %s v1.0.93: MID'd %s for SpotLight->LightFunctionMaterial (LF with bUsedWithVolumetricFog=true; the v1.0.92 MI_Light path was MISSING that flag, which is the smoking-gun reason the gobo cookie didn't reach the volumetric beam shaft)."),
				*FixtureId, *GoboLightFunctionMaterial->GetPathName());
		}
		else
		{
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("Fixture %s v1.0.93: M_RebusGoboLightFunction not found at /Game/REBUS/Materials/M_RebusGoboLightFunction -- the Python builder hasn't run yet. Run Tools > Execute Python Script > build_rebus_base_level.py (or restart the editor so the startup hook bakes it) so the gobo cookie can reach the volumetric beam shaft."),
				*FixtureId);
		}
	}
	if (!InternalBeamShaftMID)
	{
		if (InternalBeamShaftMaterial)
		{
			InternalBeamShaftMID = UMaterialInstanceDynamic::Create(InternalBeamShaftMaterial, this);
			// Seed sensible defaults so the shaft renders SOMETHING the first frame -- the
			// per-frame refresh paths (RefreshInternalBeamShaftEmissive + UpdateInternal
			// BeamShaftGeometry) will overwrite these immediately.
			InternalBeamShaftMID->SetVectorParameterValue(TEXT("Color"), FLinearColor::White);
			InternalBeamShaftMID->SetScalarParameterValue(TEXT("Intensity"), 0.f);
			InternalBeamShaftMID->SetScalarParameterValue(TEXT("BeamLength"), 6000.f);
			InternalBeamShaftMID->SetScalarParameterValue(TEXT("LensRadius"), 2.f);
			InternalBeamShaftMID->SetScalarParameterValue(TEXT("FarRadius"), 1000.f);
		}
		else
		{
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("Fixture %s v1.0.93: M_RebusInternalBeamShaft not found at /Game/REBUS/Materials/M_RebusInternalBeamShaft -- the Python builder hasn't run yet. Run build_rebus_base_level.py (or restart the editor); the cone-mesh shaft path will fall back to v1.0.92's per-light scattering until the master exists."),
				*FixtureId);
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

		// v1.0.89: if this fixture is already in InternalBeam mode at the moment this mesh
		// arrives (BuildMeshes typically runs once during Setup BEFORE the subsystem's
		// ReapplyAll re-asserts bInternalBeam=true, but a delayed mesh-bundle push could
		// land in BuildMeshes AFTER the per-actor SetBodyMeshesCastShadow walk has already
		// run and would otherwise leave the new mesh at CastShadow=true -- which the user
		// reported showing up specifically on the v1.0.88 isBeam lens disc), opt it out
		// of shadow casting + cache its original flags so the OFF transition still restores
		// byte-exact. The call is a no-op on first-spawn flow (bInternalBeamEnabled=false).
		OptPrimitiveOutOfInternalBeamShadow(PMC);

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

	// Hero-beam cap: volumetric shadows are costly, so only the first N spotlights of the spawn
	// batch cast them; the rest still scatter but skip the volumetric shadow pass. The session
	// subsystem resets the budget (ResetVolumetricShadowBudget) before each (re)spawn. Note:
	// CastVolumetricShadow is the FOG-volume-shadow flag (truss gaps in the volumetric beam),
	// SEPARATE from CastShadows above which controls SOLID shadow casting onto surfaces -- the
	// floor footprint shadow only depends on CastShadows, not CastVolumetricShadow.
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
		}
		UStaticMeshComponent* Flare = IsBeamFlareDiscs.IsValidIndex(i) ? IsBeamFlareDiscs[i] : nullptr;
		if (Flare)
		{
			Flare->SetVisibility(bShowReal, /*bPropagateToChildren*/ true);
			Flare->SetHiddenInGame(!bShowReal);
		}
	}
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

float ARebusFixtureActor::ResolveOuterHalfDeg() const
{
	// The CURRENT outer (field) cone half-angle, matching the SpotLight's lit cone so the mesh
	// beam and the real light agree: the zoom half-angle clamped to the fixture's zoom range, then
	// (depending on cookie state) pinched by the iris. Frost is intentionally NOT applied here
	// (it softens the inner cone + source radius, not the outer field extent).
	float OuterHalf = ZoomDeg.Current;
	if (Profile.Zoom.bValid)
	{
		OuterHalf = FMath::Clamp(OuterHalf, (float)(Profile.Zoom.MinDeg * 0.5), (float)(Profile.Zoom.MaxDeg * 0.5));
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

// ---- v1.0.93 InternalBeam cookie-cone shaft ---------------------------------------------
//
// The visible volumetric shaft while InternalBeam-cookie-cone mode is on. Mirrors the
// procedural cone-mesh + MID pattern of `BuildBeamCone` / `UpdateBeamConeGeometry`, with
// three meaningful differences:
//
//   1. Anchored at the LENS plane, NOT at the SpotLight location. v1.0.87's back-offset
//      pushes the SpotLight INSIDE the head; the shaft must originate at the lens or it
//      would also be visible inside the head body (the user-reported regression).
//   2. CastShadow = false. Purely emissive additive geometry; the SpotLight still casts
//      shadows for surface lighting + IES / LF projection.
//   3. Tagged `RebusInternalBeamShaft` so any future shadow walker / material-override
//      pass can grep-skip it. The cone is also explicitly skipped in
//      `SetBodyMeshesCastShadow` (pointer-equality on `InternalBeamShaft.Get()`).
void ARebusFixtureActor::UpdateInternalBeamShaftGeometry()
{
	if (!InternalBeamShaft || !InternalBeamShaftMID) return;

	// Resolve length / lens / outer-half identically to the v1.0.92 BeamCone path so the
	// shaft envelope matches the SpotLight footprint (single source of truth: zoom changes
	// re-derive both meshes from the same SpotLight outer cone half-angle).
	const float L = SpotLight ? SpotLight->AttenuationRadius : 6000.f;
	const float OuterHalf = ResolveOuterHalfDeg();
	const float TanHalf = FMath::Tan(FMath::DegreesToRadians(OuterHalf));
	const float r0 = FMath::Max(BeamBaseRadiusUnreal, RebusBeamLensRadiusFloorCm);
	const float rF = FMath::Max(L * TanHalf, r0 + 0.1f);

	if (InternalBeamShaftLastFarRadius >= 0.f
		&& FMath::Abs(rF - InternalBeamShaftLastFarRadius) < 0.5f)
	{
		// Skip the rebuild but always re-push the spatial params (BeamLength can change
		// independently of FarRadius -- e.g. AttenuationRadius retune at runtime).
		InternalBeamShaftMID->SetScalarParameterValue(TEXT("BeamLength"), L);
		InternalBeamShaftMID->SetScalarParameterValue(TEXT("LensRadius"), r0);
		InternalBeamShaftMID->SetScalarParameterValue(TEXT("FarRadius"), rF);
		return;
	}
	InternalBeamShaftLastFarRadius = rF;

	const int32 Segs = RebusBeamConeSegments;

	TArray<FVector> Positions;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	Positions.Reserve(Segs * 2 + 2);
	Normals.Reserve(Segs * 2 + 2);
	UVs.Reserve(Segs * 2 + 2);
	Triangles.Reserve(Segs * 12);

	// Generated along local +X (the spotlight emission axis); base ring at x=0 (lens) and
	// far ring at x=+L (throw). The shader does its OWN world-space projection of pixels
	// into the cone's cross-section, so the mesh UVs we hand it here are unused -- we still
	// set them so the procedural-mesh component is happy.
	for (int32 S = 0; S < Segs; ++S)
	{
		const float Angle = (2.f * PI * S) / Segs;
		const float C = FMath::Cos(Angle);
		const float Sn = FMath::Sin(Angle);
		Positions.Add(FVector(0.f, r0 * C, r0 * Sn));
		Positions.Add(FVector(L,   rF * C, rF * Sn));
		const FVector N = FVector(0.f, C, Sn).GetSafeNormal();
		Normals.Add(N);
		Normals.Add(N);
		UVs.Add(FVector2D((float)S / Segs, 0.f));
		UVs.Add(FVector2D((float)S / Segs, 1.f));
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
	// Caps: same scheme as BeamCone -- the additive material is two-sided + EXIT = own
	// depth so the cap never double-adds. Cap at the FAR end only (no base cap, so the
	// camera can fly INTO the shaft without hitting a wall right at the lens).
	const int32 FarCenter = Positions.Num();
	Positions.Add(FVector(L, 0.f, 0.f));
	Normals.Add(FVector(1.f, 0.f, 0.f));
	UVs.Add(FVector2D(0.5f, 1.f));
	for (int32 S = 0; S < Segs; ++S)
	{
		const int32 F0 = 2 * S + 1;
		const int32 F1 = 2 * ((S + 1) % Segs) + 1;
		Triangles.Add(FarCenter); Triangles.Add(F1); Triangles.Add(F0);
	}

	const TArray<FColor> NoColors;
	const TArray<FProcMeshTangent> NoTangents;
	InternalBeamShaft->ClearMeshSection(0);
	InternalBeamShaft->CreateMeshSection(0, Positions, Triangles, Normals, UVs, NoColors, NoTangents, /*bCreateCollision*/ false);

	InternalBeamShaftMID->SetScalarParameterValue(TEXT("BeamLength"), L);
	InternalBeamShaftMID->SetScalarParameterValue(TEXT("LensRadius"), r0);
	InternalBeamShaftMID->SetScalarParameterValue(TEXT("FarRadius"), rF);
}

void ARebusFixtureActor::RefreshInternalBeamShaftEmissive()
{
	if (!InternalBeamShaftMID) return;

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
	const float Dim = FMath::Clamp(Dimmer.Current, 0.f, 1.f);
	// Use the same RebusMeshBeamMaxIntensity envelope as M_RebusBeam (v1.0.40 default 4.0
	// reads as a clearly-visible volumetric shaft against the standard exponential-height-
	// fog tuning). The IES profile shapes the SpotLight footprint; the shaft is a separate
	// emissive draw that doesn't need IES-scaled physical units. MeshBeamUserScale rides
	// the existing SetFixtureBeamVolumetrics path so the operator's per-fixture intensity
	// multiplier applies to BOTH the v1.0.92 per-light scattering AND the v1.0.93 shaft.
	InternalBeamShaftMID->SetVectorParameterValue(TEXT("Color"), Linear);
	InternalBeamShaftMID->SetScalarParameterValue(TEXT("Intensity"),
		RebusMeshBeamMaxIntensity * Dim * Gate * MeshBeamUserScale);
}

void ARebusFixtureActor::DriveInternalBeamShaftFromSpotLight()
{
	if (!SpotLight || !InternalBeamShaft || !InternalBeamShaftMID) return;

	// The SpotLight is back-offset INSIDE the head by ComputeInternalBeamBackOffsetCm() *
	// GRebusInternalBeamOffsetSign along its LiveFwd (the spotlight's world emission axis).
	// To anchor the shaft AT the LENS PLANE we subtract that same offset along LiveFwd from
	// the SpotLight's world location -- end result: the shaft origin is co-located with the
	// LENS, not with the SpotLight, so the shaft can never be visible inside the head body.
	const FVector SpotFwd = SpotLight->GetForwardVector().GetSafeNormal();
	const FVector SpotLoc = SpotLight->GetComponentLocation();
	const float OffsetCm = bInternalBeamEnabled
		? GRebusInternalBeamOffsetSign * ComputeInternalBeamBackOffsetCm()
		: 0.f;
	// Subtract the offset along LiveFwd to recover the lens plane (the offset PUSHED the
	// spotlight along LiveFwd; the lens is at SpotLoc - offset * LiveFwd).
	const FVector LensPlane = SpotLoc - OffsetCm * SpotFwd;

	InternalBeamShaft->SetWorldLocationAndRotation(LensPlane, FRotationMatrix::MakeFromX(SpotFwd).ToQuat());

	// World-space spatial params for the shader's cross-section projection.
	InternalBeamShaftMID->SetVectorParameterValue(TEXT("BeamOrigin"),
		FLinearColor((float)LensPlane.X, (float)LensPlane.Y, (float)LensPlane.Z, 0.f));
	InternalBeamShaftMID->SetVectorParameterValue(TEXT("BeamDir"),
		FLinearColor((float)SpotFwd.X, (float)SpotFwd.Y, (float)SpotFwd.Z, 0.f));
}

void ARebusFixtureActor::PushGoboRTToInternalBeamMaterials()
{
	// Cookie texture: prefer the per-fixture GoboRT (live-rotated cookie) when a non-Open
	// gobo is bound; revert to the engine white texture (the LF + shaft both default-sample
	// white = "let the light through unchanged" = uniform untextured cone, matching the
	// pre-v1.0.93 visual).
	UTexture* CookieTex = nullptr;
	if (bGoboActive && GoboRT)
	{
		CookieTex = static_cast<UTexture*>(GoboRT.Get());
	}
	else
	{
		CookieTex = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture"));
	}
	if (!CookieTex) return;
	if (GoboLightFunctionMID)
	{
		GoboLightFunctionMID->SetTextureParameterValue(TEXT("GoboRT"), CookieTex);
	}
	if (InternalBeamShaftMID)
	{
		InternalBeamShaftMID->SetTextureParameterValue(TEXT("GoboRT"), CookieTex);
	}
}

// (Re)apply / restore the v1.0.93 cookie-cone path on this fixture. Two entry points:
//   * `ApplyInternalBeamPose` calls `ApplyInternalBeamShaft(false)` on the ON edge, gated
//     by `GRebusInternalBeamCookieCone` -- when 0 the v1.0.92 per-light path runs unchanged.
//   * The `Rebus.InternalBeamCookieCone` CVar refresh sink calls it with bForceVisible=true
//     so the operator can flip the cone-mesh on/off without a full `Rebus.InternalBeam` cycle.
void ARebusFixtureActor::ApplyInternalBeamShaft(bool bForceVisible)
{
	if (!SpotLight) return;
	if (!bForceVisible && GRebusInternalBeamCookieCone == 0)
	{
		// Operator explicitly opted out via the CVar -- leave the v1.0.92 per-light path in
		// charge. Just hide the shaft component if it was previously built (idempotent
		// "make sure cone is hidden").
		if (InternalBeamShaft)
		{
			InternalBeamShaft->SetVisibility(false);
		}
		return;
	}

	EnsureFixtureInternalBeamMIDs();

	// Lazy-build the shaft component the first time we need it. Subsequent toggles just
	// flip visibility.
	if (!InternalBeamShaft)
	{
		if (!InternalBeamShaftMID)
		{
			// Master missing -- log already fired in EnsureFixtureInternalBeamMIDs. Without a
			// material there's nothing to render, so leave the shaft un-built; the OFF path
			// will fall back to v1.0.92 per-light scattering via RefreshBeamShadowMode.
			return;
		}
		InternalBeamShaft = NewObject<UProceduralMeshComponent>(this, TEXT("InternalBeamShaft"));
		InternalBeamShaft->SetupAttachment(FixtureRoot);
		InternalBeamShaft->RegisterComponent();
		InternalBeamShaft->SetMobility(EComponentMobility::Movable);
		InternalBeamShaft->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		// Purely emissive additive geometry -- never shadows anything (the SpotLight handles
		// surface shadow casting). Also opt out of being an occluder so the long translucent
		// shaft doesn't fight HZB / frustum culling.
		InternalBeamShaft->SetCastShadow(false);
		InternalBeamShaft->bCastDynamicShadow = false;
		InternalBeamShaft->bCastHiddenShadow = false;
		InternalBeamShaft->bUseAttachParentBound = false;
		InternalBeamShaft->bUseAsOccluder = false;
		// Skip-list grep tag for the body-shadow walker + truss-material pass.
		InternalBeamShaft->ComponentTags.AddUnique(FName(TEXT("RebusInternalBeamShaft")));
		// Generous bounds scale -- same trick as BeamCone, because an elongated additive
		// shaft mostly overlapping closer opaque floor reads as a culling candidate.
		InternalBeamShaft->SetBoundsScale(RebusBeamBoundsScale);
		InternalBeamShaft->SetMaterial(0, InternalBeamShaftMID);
		InternalBeamShaftLastFarRadius = -1.f;
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Fixture %s v1.0.93: spawned InternalBeamShaft cone-mesh -- gobo cookie now appears IN the volumetric shaft (not just on the floor footprint), and the shaft starts AT the lens plane (no longer visible inside the head body)."),
			*FixtureId);
	}

	// Cache the SpotLight's prior LightFunctionMaterial + VolumetricScatteringIntensity the
	// FIRST time we install the cookie-cone path so the OFF restore can return both to the
	// construction-time / pre-shift values byte-exact (the v1.0.92 cache only covered
	// VolumetricScatteringIntensity + bCastVolumetricShadow; LF was previously left alone
	// because the v1.0.92 gobo-cookie path on MI_Light was already installed by the time
	// InternalBeam ON ran). Separate latch so the v1.0.92 + v1.0.93 caches don't stomp.
	if (!bInternalBeamPriorLightFunctionCached)
	{
		InternalBeamPriorLightFunction = SpotLight->LightFunctionMaterial;
		bInternalBeamPriorLightFunctionCached = true;
	}

	// Push the v1.0.93 LF MID onto the SpotLight (it has bUsedWithVolumetricFog=true, unlike
	// Epic's MI_Light) AND force VolumetricScatteringIntensity = 0 so the engine's per-light
	// path doesn't paint scattering INSIDE the head. The shaft cone-mesh is the visible
	// cone now; the SpotLight is just a light source for the floor footprint + LF cookie.
	if (GoboLightFunctionMID)
	{
		SpotLight->SetLightFunctionMaterial(GoboLightFunctionMID);
	}
	SpotLight->SetVolumetricScatteringIntensity(0.f);
	SpotLight->MarkRenderStateDirty();

	// Push the live gobo, color/intensity, and spatial params; then resize + show.
	PushGoboRTToInternalBeamMaterials();
	UpdateInternalBeamShaftGeometry();
	RefreshInternalBeamShaftEmissive();
	DriveInternalBeamShaftFromSpotLight();
	InternalBeamShaft->SetVisibility(true);
}

void ARebusFixtureActor::RestoreInternalBeamShaft()
{
	if (!SpotLight) return;

	// Hide the cone (don't destroy -- the next ON toggle is cheaper if we keep it alive).
	if (InternalBeamShaft)
	{
		InternalBeamShaft->SetVisibility(false);
	}

	// Restore the cached SpotLight LightFunctionMaterial byte-exact, then re-let the
	// existing v1.0.92 path own VolumetricScatteringIntensity (RefreshBeamShadowMode runs
	// at the tail of RestoreInternalBeamPose and re-derives the value from bInternalBeam
	// Enabled + bMeshBeamEnabled + the hero-shadow budget). When a gobo is still active
	// and we just restored a NON-MI_Light LF, the next ApplyCurrentGoboToLightFn call will
	// re-assert the v1.0.92 cookie-on-MI_Light path -- that's the correct end state
	// outside of InternalBeam mode (the user keeps their gobo on the floor).
	if (bInternalBeamPriorLightFunctionCached)
	{
		SpotLight->SetLightFunctionMaterial(InternalBeamPriorLightFunction.Get());
		bInternalBeamPriorLightFunctionCached = false;
		InternalBeamPriorLightFunction = nullptr;
		SpotLight->MarkRenderStateDirty();
	}
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

	// v1.0.87 InternalBeam A/B mode is the strongest override: when ON the SpotLight IS the visible
	// volumetric shaft (the Epic / cone-mesh beams are hidden + deactivated by ApplyInternalBeamPose
	// alongside this push), so we force volumetric scattering + Cast Volumetric Shadow on regardless
	// of the bMeshBeamEnabled / bShadowActive paths below -- the hero-shadow budget is bypassed
	// because there is no longer a competing mesh-cone "primary" beam to ration against.
	if (bInternalBeamEnabled)
	{
		SpotLight->SetVolumetricScatteringIntensity(FMath::Max(GRebusInternalBeamScatter, 0.f));
		SpotLight->SetCastShadows(true);
		SpotLight->SetCastVolumetricShadow(true);
		SpotLight->MarkRenderStateDirty();
		return;
	}

	// A hero shadow beam is one that asked for volumetric shadows AND won a per-batch budget slot.
	const bool bShadowActive = bWantsVolumetricShadow && bGrantedShadowHero;

	if (bMeshBeamEnabled)
	{
		// Mesh cone is the crisp shaft. Hero shadow beams ALSO emit a modest native fog scattering
		// with Cast Volumetric Shadow so VSM (which works on runtime-imported glTF meshes that lack
		// distance fields) carves real truss gaps into the volume. Non-hero beams stay mesh-only
		// (scattering 0) so there's no competing froxel noise.
		SpotLight->SetVolumetricScatteringIntensity(bShadowActive ? GRebusHeroShadowScatter : 0.f);
		// v1.0.94 -- ALWAYS keep per-light shadow casting ON. The pre-v1.0.94 logic
		// (`bShadowActive || bGoboActive`) cleared `CastShadows` on every non-hero non-gobo fixture
		// as a perf opt; combined with MegaLights routing on the same fixtures, this is what
		// produced the user-reported "Epic-beam mode shows no object shadows in the footprint"
		// (root cause C of the v1.0.94 audit -- a SpotLight with CastShadows=false casts NO
		// shadows from any occluder, regardless of the MegaLights opt-out). The VSM volumetric-
		// shadow path (CastVolumetricShadow below) and the v1.0.49 cookie LF path BOTH need
		// CastShadows on, and so does the user-visible solid-shadow path that motivated this
		// release; setting it true unconditionally is the simplest correct policy.
		SpotLight->SetCastShadows(true);
		SpotLight->SetCastVolumetricShadow(bShadowActive);
	}
	else
	{
		// Fog-beam A/B mode: restore the froxel beam; hero beams still cast volumetric shadow.
		SpotLight->SetVolumetricScatteringIntensity(FogScatteringIntensity);
		// v1.0.94 -- same policy as the mesh-beam path above (always on -- see the comment there
		// for the root-cause discussion).
		SpotLight->SetCastShadows(true);
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

// ---- v1.0.87 InternalBeam A/B mode -----------------------------------------------------
//
// Resolve the spotlight back-offset (UE cm) so the visible cone exits the fixture head AT the
// lens diameter (max zoom) and strictly INSIDE the lens diameter at every narrower zoom.
//   offset = lensRadius / tan(MaxZoomHalfAngleRad)
// MaxZoomHalfAngleRad = (Profile.Zoom.MaxDeg or InternalBeamMaxZoomFullDeg fallback) / 2.
// The offset is FIXED at the MAX zoom value -- we deliberately do NOT key it off the current
// ZoomDeg, because a wide-then-narrow zoom must keep the lens-diameter envelope (otherwise a
// narrow zoom would project a cone that started inside the lens and ended outside on the head).
float ARebusFixtureActor::ComputeInternalBeamBackOffsetCm() const
{
	// Resolve the LENS RADIUS in UE cm: prefer the same source BuildSpotLight cached as
	// BaseSourceRadiusUnreal (lensDiameter/2 -> source.radius -> source.diameter/2 -> dims-fallback)
	// so the offset uses the SAME physical lens the user sees in the lens-flare disc + the
	// cone-mesh start. Synthetic fallback: 3 cm matches the RebusBeamLensRadiusFloorCm floor.
	float LensRadiusCm = BaseSourceRadiusUnreal;
	if (LensRadiusCm <= KINDA_SMALL_NUMBER)
	{
		const TCHAR* DiamSrc = TEXT("none");
		const double DiamMeters = ResolveLensDiameterMeters(DiamSrc);
		LensRadiusCm = (DiamMeters > KINDA_SMALL_NUMBER)
			? (float)(DiamMeters * 0.5 * RebusCoords::METERS_TO_UNREAL)
			: 3.f;
	}
	// Resolve the MAX zoom full angle in degrees, then half-angle in radians. Clamped to a sane
	// minimum (1 deg full -> 0.5 deg half) so tan(0) doesn't blow up the divide.
	float MaxZoomFullDeg = InternalBeamMaxZoomFullDeg;
	if (Profile.Zoom.bValid && Profile.Zoom.MaxDeg > 0.0)
	{
		MaxZoomFullDeg = (float)Profile.Zoom.MaxDeg;
	}
	const float MaxHalfRad = FMath::DegreesToRadians(FMath::Max(MaxZoomFullDeg * 0.5f, 0.5f));
	const float Tan = FMath::Max(FMath::Tan(MaxHalfRad), KINDA_SMALL_NUMBER);
	const float OffsetCm = LensRadiusCm / Tan;
	return FMath::Max(OffsetCm, 0.f);
}

// Walk this actor's UPrimitiveComponents and either opt them out of shadow casting (bRestore
// Original == false) or restore their cached flags (bRestoreOriginal == true). The filter
// deliberately TARGETS the GDTF body proxies (UProceduralMeshComponent[] populated by BuildMeshes)
// and EXCLUDES the SpotLight (not a primitive anyway -- ULightComponentBase extends USceneComponent),
// the Epic beam canvas (EpicBeamComp), the procedural cone (BeamCone), the lens-flare disc (LensDisc),
// and any future debug-arrow / origin-gizmo / IES-debug primitive added later. Class-based filter
// (UStaticMesh / UProceduralMesh) plus a same-object skip-list keeps the blast radius surgical.
void ARebusFixtureActor::SetBodyMeshesCastShadow(bool bRestoreOriginal)
{
	if (bRestoreOriginal)
	{
		int32 Restored = 0;
		for (const FInternalBeamShadowEntry& Entry : InternalBeamShadowCache)
		{
			UPrimitiveComponent* Comp = Entry.Comp.Get();
			if (!Comp) continue;
			Comp->SetCastShadow(Entry.bCastShadow != 0);
			Comp->bCastDynamicShadow = Entry.bCastDynamicShadow != 0;
			Comp->bCastHiddenShadow = Entry.bCastHiddenShadow != 0;
			Comp->bCastShadowAsTwoSided = Entry.bCastShadowAsTwoSided != 0;
			Comp->MarkRenderStateDirty();
			++Restored;
		}
		UE_LOG(LogRebusVisualiser, Verbose,
			TEXT("Fixture %s InternalBeam: restored CastShadow flags on %d body primitive(s)."),
			*FixtureId, Restored);
		InternalBeamShadowCache.Reset();
		return;
	}

	// Build the per-comp shadow opt-out cache from scratch. Same-name skip ensures EpicBeamComp /
	// BeamCone / LensDisc never land in the cache regardless of how the actor was built.
	InternalBeamShadowCache.Reset();
	TArray<UPrimitiveComponent*> AllPrims;
	GetComponents<UPrimitiveComponent>(AllPrims);
	int32 Touched = 0;
	for (UPrimitiveComponent* Comp : AllPrims)
	{
		if (!Comp) continue;
		// Skip the beam-functional primitives. Pointer equality on the cached UPROPERTY refs is
		// the most defensive filter (resilient to a rename or a derived type substitution).
		if (Comp == (UPrimitiveComponent*)BeamCone.Get()) continue;
		if (Comp == (UPrimitiveComponent*)EpicBeamComp.Get()) continue;
		if (Comp == (UPrimitiveComponent*)LensDisc.Get()) continue;
		// v1.0.93 -- skip the InternalBeamShaft cone-mesh: it's already CastShadow=false by
		// construction (purely emissive additive geometry); including it in the cache + the
		// restore loop would (a) corrupt the cache for the OFF restore (the OFF transition
		// expects every cached primitive to have a non-trivial original shadow flag to put
		// back) and (b) churn the render state for zero behavioural change.
		if (Comp == (UPrimitiveComponent*)InternalBeamShaft.Get()) continue;
		// Defensive: never touch any USpotLightComponent / ULightComponent -- they aren't
		// UPrimitiveComponents (ULightComponentBase extends USceneComponent, not UPrimitiveComponent)
		// so they shouldn't show up here, but the filter keeps a future engine inheritance change
		// from accidentally casting the spotlight's own shadow off (would break IES + LightFunction
		// projection, since both require the light to be casting shadows).
		if (Comp->IsA<ULightComponent>()) continue;

		FInternalBeamShadowEntry Entry;
		Entry.Comp = Comp;
		Entry.bCastShadow = Comp->CastShadow ? 1 : 0;
		Entry.bCastDynamicShadow = Comp->bCastDynamicShadow ? 1 : 0;
		Entry.bCastHiddenShadow = Comp->bCastHiddenShadow ? 1 : 0;
		Entry.bCastShadowAsTwoSided = Comp->bCastShadowAsTwoSided ? 1 : 0;
		InternalBeamShadowCache.Add(Entry);

		Comp->SetCastShadow(false);
		Comp->bCastDynamicShadow = false;
		Comp->bCastHiddenShadow = false;
		Comp->bCastShadowAsTwoSided = false;
		Comp->MarkRenderStateDirty();
		++Touched;
	}
	UE_LOG(LogRebusVisualiser, Verbose,
		TEXT("Fixture %s InternalBeam: opted %d body primitive(s) out of shadow casting (skipped SpotLight + BeamCone + EpicBeam + LensDisc; v1.0.89 INCLUDES RebusIsBeamLens-tagged real <Beam> lens meshes -- they are body geometry as far as the spotlight is concerned)."),
		*FixtureId, Touched);
}

// v1.0.89 -- defensive single-primitive entry-point for the shadow opt-out + cache. Used by
// BuildMeshes when a freshly-created procedural mesh component arrives AFTER InternalBeam mode
// is already ON (rare on first spawn, but a delayed /meshes push -- or a re-build in some future
// reload path -- would otherwise leave the new comp at CastShadow=true). Symmetric with the
// per-actor walker's filter rules so the cache stays consistent (RestoreInternalBeamPose must
// still byte-exactly restore everything that was opted out).
void ARebusFixtureActor::OptPrimitiveOutOfInternalBeamShadow(UPrimitiveComponent* Comp)
{
	if (!bInternalBeamEnabled || !Comp) return;
	// Mirror the SetBodyMeshesCastShadow skip filter so we never opt out a beam-functional comp
	// that the walker also skips (would corrupt the cache for the OFF restore).
	if (Comp == (UPrimitiveComponent*)BeamCone.Get()) return;
	if (Comp == (UPrimitiveComponent*)EpicBeamComp.Get()) return;
	if (Comp == (UPrimitiveComponent*)LensDisc.Get()) return;
	// v1.0.93 -- same skip as the walker (InternalBeamShaft never enters the shadow cache).
	if (Comp == (UPrimitiveComponent*)InternalBeamShaft.Get()) return;
	if (Comp->IsA<ULightComponent>()) return;
	// Already in the cache (re-entrancy guard). Linear scan is cheap because this list is
	// per-actor and bounded by the body-primitive count (~5-15 typical, ~30 for a pixel-LED
	// matrix). Avoids restoring the SAME component twice on disable.
	for (const FInternalBeamShadowEntry& Existing : InternalBeamShadowCache)
	{
		if (Existing.Comp.Get() == Comp) return;
	}

	FInternalBeamShadowEntry Entry;
	Entry.Comp = Comp;
	Entry.bCastShadow = Comp->CastShadow ? 1 : 0;
	Entry.bCastDynamicShadow = Comp->bCastDynamicShadow ? 1 : 0;
	Entry.bCastHiddenShadow = Comp->bCastHiddenShadow ? 1 : 0;
	Entry.bCastShadowAsTwoSided = Comp->bCastShadowAsTwoSided ? 1 : 0;
	InternalBeamShadowCache.Add(Entry);

	Comp->SetCastShadow(false);
	Comp->bCastDynamicShadow = false;
	Comp->bCastHiddenShadow = false;
	Comp->bCastShadowAsTwoSided = false;
	Comp->MarkRenderStateDirty();

	UE_LOG(LogRebusVisualiser, Verbose,
		TEXT("Fixture %s InternalBeam: opted late-arrival primitive '%s' (tags: %s) out of shadow casting AND cached its original flags so the next OFF transition restores byte-exact."),
		*FixtureId,
		*Comp->GetName(),
		Comp->ComponentTags.Num() > 0 ? *FString::JoinBy(Comp->ComponentTags, TEXT(","), [](const FName& T) { return T.ToString(); }) : TEXT("<none>"));
}

void ARebusFixtureActor::ApplyInternalBeamPose()
{
	if (!SpotLight) return;

	// Snapshot the construction state on the FIRST enable so subsequent ON / OFF cycles always
	// land back on the original values (a second enable in the same session must NOT re-snapshot
	// volumetrics that this very mode just pushed). bInternalBeamPoseCached is the latch.
	if (!bInternalBeamPoseCached)
	{
		InternalBeamSpotVolScatterOrig = SpotLight->VolumetricScatteringIntensity;
		bInternalBeamSpotCastVolShadowOrig = SpotLight->bCastVolumetricShadow != 0;
		bInternalBeamEpicCompVisOrig = EpicBeamComp ? EpicBeamComp->IsVisible() : false;
		bInternalBeamConeVisOrig = BeamCone ? BeamCone->IsVisible() : false;
		bInternalBeamPoseCached = true;
	}

	// 1) Hide + deactivate the Epic beam canvas + the procedural cone-mesh fallback. We
	//    deliberately do NOT destroy either component -- a subsequent ON -> OFF must return the
	//    Epic beam INTACT, so we just clip visibility (which makes the canvas mesh stop drawing)
	//    and let the cached visibility govern the restore on the OFF toggle.
	if (EpicBeamComp)
	{
		EpicBeamComp->SetVisibility(false);
	}
	if (BeamCone)
	{
		BeamCone->SetVisibility(false);
	}

	// 2) Opt this actor's body meshes out of shadow casting so the head itself can't shadow
	//    the now-internal spotlight (would otherwise extinguish the entire beam).
	SetBodyMeshesCastShadow(/*bRestoreOriginal*/ false);

	// 3) v1.0.92 -- force the SpotLight off the MegaLights path so the IES profile AND the
	//    LightFunctionMaterial both modulate the volumetric beam shaft. v1.0.89's
	//    `r.LightFunctionAtlas.Enabled 1` push was necessary but not sufficient: UE 5.7
	//    MegaLights bypasses both IES + LF in the volumetric integrator (the many-lights-per-
	//    pixel sample pipeline doesn't sample either, see the engine
	//    MegaLightsRendering.cpp / VolumetricFog.cpp for the gating logic), so a MegaLight
	//    spotlight reads as a uniform untextured cone in fog regardless of its IES file or its
	//    cookie. Gated by `Rebus.InternalBeamForceLegacy` (default 1); when 0 the push is
	//    skipped and `bAllowMegaLights` is left at whatever BuildSpotLight + the gobo path set
	//    it to, so a deployment that prefers MegaLights' clustering perf can opt out.
	if (GRebusInternalBeamForceLegacy != 0)
	{
		PushInternalBeamMegaLightsOptOut();
	}

	// 4) Push the spatial back-offset onto the SpotLight RIGHT NOW. RefreshMotion's post-step
	//    will re-apply this every subsequent frame, but the operator toggled the mode in this
	//    frame and we want the visible change immediately rather than waiting for the next
	//    motion tick. RefreshMotion is idempotent so this redundant call is cheap (cone
	//    geometry only rebuilds when zoom changes, etc.).
	RefreshMotion();

	// 5) Push the volumetric beam state onto the SpotLight (RefreshBeamShadowMode honors
	//    bInternalBeamEnabled when true and forces the volumetric scatter + cast volumetric shadow
	//    regardless of the hero-budget / mesh-beam paths).
	RefreshBeamShadowMode();

	// v1.0.93 -- install the cookie-cone path (LF MID with bUsedWithVolumetricFog=true onto
	// SpotLight->LightFunctionMaterial, force VolumetricScatteringIntensity=0 to suppress the
	// engine per-light scattering pass which v1.0.87's back-offset made visible INSIDE the
	// head, and spawn / refresh / show the additive cone-mesh shaft that starts AT the lens
	// plane). Gated by Rebus.InternalBeamCookieCone (default 1) -- when 0 the cone-mesh is
	// hidden and the prior RefreshBeamShadowMode leaves scattering at the v1.0.92 per-light
	// value. ApplyInternalBeamShaft is idempotent so re-entry via the CVar sink is cheap.
	ApplyInternalBeamShaft(false);
}

void ARebusFixtureActor::PushInternalBeamMegaLightsOptOut()
{
	if (!SpotLight) return;

	// Cache the prior value the FIRST time we push, mirroring `bInternalBeamPoseCached` for
	// the rest of the InternalBeam pose state. A second push in the same InternalBeam session
	// (e.g. ApplyCurrentGoboToLightFn already toggled the flag because a gobo went active and
	// then we re-entered the push via the CVar refresh sink) must NOT re-snapshot the value
	// we just installed -- the prior value the OFF transition needs is the construction-time
	// value, not "0".
	if (!bInternalBeamMegaLightsOrigCached)
	{
		bInternalBeamAllowMegaLightsOrig = (SpotLight->bAllowMegaLights != 0);
		bInternalBeamMegaLightsOrigCached = true;
	}

	const bool bWasMega = (SpotLight->bAllowMegaLights != 0);
	if (!bWasMega)
	{
		// Already opted out (the gobo-active path beat us to it). The cache is primed, so the
		// OFF restore will still push the byte-exact original value back. No render-state churn.
		UE_LOG(LogRebusVisualiser, Verbose,
			TEXT("Fixture %s InternalBeam MegaLights opt-out: already 0 (gobo-active path?); cache primed (orig=%d)."),
			*FixtureId, bInternalBeamAllowMegaLightsOrig ? 1 : 0);
		return;
	}

	// v1.0.94 -- always force 0 (the InternalBeam-specific opt-out reason, ungated by
	// `Rebus.AllowMegaLights`: the volumetric LF/IES-on-shaft path needs the legacy clustered
	// path even when the global gate would otherwise allow MegaLights). Routed through
	// `ResolveAllowMegaLights(0)` for consistency with every other assignment site -- always
	// returns 0 here.
	SpotLight->bAllowMegaLights = ResolveAllowMegaLights(0);
	// `bAllowMegaLights` is read at FLightSceneInfo proxy creation (LightSceneInfo.cpp ->
	// Proxy->AllowMegaLights()), so the value MUST be present on a freshly-created proxy to
	// take effect. MarkRenderStateDirty alone schedules a deferred recreate that has proven
	// unreliable in v1.0.50 -- a full ReregisterComponent on the transition guarantees the
	// proxy is rebuilt with bAllowMegaLights=0 on the next frame. Cost: brief one-frame
	// blackout on the toggle.
	SpotLight->ReregisterComponent();

	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s InternalBeam MegaLights opt-out: bAllowMegaLights 1 -> 0 (orig=%d cached). "
			 "Volumetric scattering integrator now samples IES + LightFunction; perf trade-off: this fixture loses MegaLights' clustering."),
		*FixtureId, bInternalBeamAllowMegaLightsOrig ? 1 : 0);
}

void ARebusFixtureActor::RestoreInternalBeamMegaLights()
{
	if (!SpotLight) return;

	// Symmetric with PushInternalBeamMegaLightsOptOut. When the cache was never primed (the ON
	// edge skipped the push because Rebus.InternalBeamForceLegacy was 0), there's nothing to
	// restore -- the bAllowMegaLights value is whatever BuildSpotLight + the gobo path set it
	// to, which is exactly what the operator wanted in that mode.
	if (!bInternalBeamMegaLightsOrigCached)
	{
		UE_LOG(LogRebusVisualiser, Verbose,
			TEXT("Fixture %s InternalBeam MegaLights restore: no cached value (push never ran), no-op."),
			*FixtureId);
		return;
	}

	const bool bIsMega = (SpotLight->bAllowMegaLights != 0);
	// v1.0.94 -- route through `ResolveAllowMegaLights` so `Rebus.AllowMegaLights = 0` (the
	// hard floor) clamps the restore to 0 even if the originally-cached value was 1. The cache
	// is still cleared (so a later InternalBeam ON/OFF cycle re-snapshots the live value) and
	// the hard floor wins regardless of the InternalBeam-specific cache state.
	const uint32 WantValue = ResolveAllowMegaLights(bInternalBeamAllowMegaLightsOrig ? 1u : 0u);
	const bool bWantMega = (WantValue != 0);
	bInternalBeamMegaLightsOrigCached = false;
	bInternalBeamAllowMegaLightsOrig = true; // back to the field's struct default

	if (bIsMega == bWantMega)
	{
		UE_LOG(LogRebusVisualiser, Verbose,
			TEXT("Fixture %s InternalBeam MegaLights restore: already at orig=%d, no proxy rebuild."),
			*FixtureId, bWantMega ? 1 : 0);
		return;
	}

	SpotLight->bAllowMegaLights = WantValue;
	SpotLight->ReregisterComponent();
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s InternalBeam MegaLights restore: bAllowMegaLights %d -> %d (cached orig, clamped by Rebus.AllowMegaLights=%d)."),
		*FixtureId, bIsMega ? 1 : 0, bWantMega ? 1 : 0, GRebusAllowMegaLights);
}

void ARebusFixtureActor::RefreshAllowMegaLightsFromCVar()
{
	if (!SpotLight) return;

	// v1.0.94 -- single chokepoint for resolving the desired `bAllowMegaLights` per the live
	// global gate AND the per-fixture state (gobo cookie active / InternalBeam mode +
	// `Rebus.InternalBeamForceLegacy = 1`). When `Rebus.AllowMegaLights = 0` (the v1.0.94
	// default), every Rebus SpotLight runs on the legacy path -- the hard floor for shadow
	// casting in the floor footprint. When `Rebus.AllowMegaLights = 1`, MegaLights is permitted
	// EXCEPT where the per-fixture state explicitly opts out:
	//   * `bGoboActive` -- the v1.0.50 cookie LF path needs the legacy deferred renderer
	//     (M_Light_Master's MF_DMXGobo is not LightFunctionAtlas-compatible).
	//   * `bInternalBeamEnabled && GRebusInternalBeamForceLegacy != 0` -- the v1.0.92
	//     volumetric-LF/IES-on-shaft path needs the legacy clustered path.
	// Called from the `Rebus.AllowMegaLights` CVar refresh sink for every Rebus fixture; safe
	// to call directly when the gobo / InternalBeam state changes (idempotent / cheap when
	// nothing transitioned -- ReregisterComponent is gated on a value change).
	uint32 Desired = ResolveAllowMegaLights(1u);
	if (Desired != 0)
	{
		if (bGoboActive) Desired = 0;
		if (bInternalBeamEnabled && GRebusInternalBeamForceLegacy != 0) Desired = 0;
	}

	const uint32 IsValue = SpotLight->bAllowMegaLights ? 1u : 0u;
	if (IsValue == Desired)
	{
		UE_LOG(LogRebusVisualiser, Verbose,
			TEXT("Fixture %s Rebus.AllowMegaLights refresh: already at %d (gobo=%d internalBeam=%d forceLegacy=%d), no proxy rebuild."),
			*FixtureId, Desired, bGoboActive ? 1 : 0,
			bInternalBeamEnabled ? 1 : 0, GRebusInternalBeamForceLegacy);
		return;
	}

	SpotLight->bAllowMegaLights = Desired;
	// `bAllowMegaLights` is read at FLightSceneInfo proxy creation; a full ReregisterComponent
	// guarantees the proxy is rebuilt on the next frame with the new value. Cost: brief one-frame
	// blackout on the toggle, identical to the v1.0.92 path.
	SpotLight->ReregisterComponent();

	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s Rebus.AllowMegaLights refresh: bAllowMegaLights %d -> %d (CVar=%d gobo=%d internalBeam=%d forceLegacy=%d)."),
		*FixtureId, IsValue, Desired,
		GRebusAllowMegaLights, bGoboActive ? 1 : 0,
		bInternalBeamEnabled ? 1 : 0, GRebusInternalBeamForceLegacy);
}

void ARebusFixtureActor::RestoreInternalBeamPose()
{
	if (!SpotLight) return;

	// 1) Restore SpotLight volumetric state from the cached construction values. RefreshBeam
	//    ShadowMode is called immediately after flipping bInternalBeamEnabled to false (it
	//    falls back to the bMeshBeamEnabled / hero-shadow paths), but the explicit set here is
	//    defensive for a future code path that calls RestoreInternalBeamPose without flipping
	//    the flag first.
	if (bInternalBeamPoseCached)
	{
		SpotLight->SetVolumetricScatteringIntensity(InternalBeamSpotVolScatterOrig);
		SpotLight->SetCastVolumetricShadow(bInternalBeamSpotCastVolShadowOrig);
		if (EpicBeamComp)
		{
			EpicBeamComp->SetVisibility(bInternalBeamEpicCompVisOrig);
		}
		if (BeamCone)
		{
			BeamCone->SetVisibility(bInternalBeamConeVisOrig);
		}
		SpotLight->MarkRenderStateDirty();
	}

	// 2) Restore the per-body-primitive shadow flags from the cache. No-op if the cache is empty
	//    (Apply was never run).
	SetBodyMeshesCastShadow(/*bRestoreOriginal*/ true);

	// 3) v1.0.92: restore the cached `bAllowMegaLights` byte-exact (no-op if the push never
	//    ran -- e.g. Rebus.InternalBeamForceLegacy was 0 on the ON edge). When a gobo is still
	//    active the very next ApplyCurrentGoboToLightFn call will re-assert
	//    `bAllowMegaLights = 0` (the legacy LF path is required for the cookie even outside
	//    InternalBeam mode), so the restore lands the spotlight exactly where it would be in
	//    the non-InternalBeam-with-gobo state -- which is the correct end state.
	RestoreInternalBeamMegaLights();

	// 4) Re-push motion so the spotlight returns to its un-offset position byte-exact (Refresh
	//    Motion's offset post-step skips when bInternalBeamEnabled is now false, and the prior
	//    SetRelativeTransform(BeamRestTransform * Head) re-puts the spotlight at the
	//    construction-time recipe).
	RefreshMotion();

	// 5) Final volumetric refresh.
	RefreshBeamShadowMode();

	// 6) v1.0.93 -- tear down the cookie-cone path (restore the cached SpotLight LightFunc
	//    tion material byte-exact, hide the cone-mesh shaft component -- not destroy, so the
	//    next ON toggle is cheap). The cached LF is whatever the SpotLight had bound before
	//    InternalBeam took over (typically null, or the v1.0.49 cookie MID on MI_Light when a
	//    gobo was active before InternalBeam was toggled on). If a gobo is still active, the
	//    very next ApplyCurrentGoboToLightFn call will re-assert the v1.0.49 cookie path on
	//    MI_Light (no volumetric, but the floor footprint keeps its cookie) -- which is the
	//    correct end state outside of InternalBeam mode.
	RestoreInternalBeamShaft();
}

void ARebusFixtureActor::SetInternalBeamModeEnabled(bool bEnabled)
{
	if (bEnabled == bInternalBeamEnabled)
	{
		// Idempotent: the per-fixture wire path (subsystem ReapplyAll on every fixture spawn)
		// can call this with the existing value; bail to avoid an unnecessary log + re-snapshot.
		return;
	}
	bInternalBeamEnabled = bEnabled;
	if (bEnabled)
	{
		ApplyInternalBeamPose();
	}
	else
	{
		RestoreInternalBeamPose();
	}
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("Fixture %s InternalBeam mode %s (backOffset=%.1fcm sign=%+.0f maxZoomFullDeg=%.1f scatter=%.2f bodyPrimsOptedOut=%d forceLegacy=%d allowMegaLights=%d cachedOrig=%d)."),
		*FixtureId,
		bEnabled ? TEXT("ENABLED") : TEXT("DISABLED -> Epic beam restored"),
		bEnabled ? (GRebusInternalBeamOffsetSign * ComputeInternalBeamBackOffsetCm()) : 0.f,
		GRebusInternalBeamOffsetSign,
		(Profile.Zoom.bValid && Profile.Zoom.MaxDeg > 0.0) ? (float)Profile.Zoom.MaxDeg : InternalBeamMaxZoomFullDeg,
		GRebusInternalBeamScatter,
		InternalBeamShadowCache.Num(),
		GRebusInternalBeamForceLegacy,
		SpotLight ? (SpotLight->bAllowMegaLights ? 1 : 0) : -1,
		bInternalBeamMegaLightsOrigCached ? (bInternalBeamAllowMegaLightsOrig ? 1 : 0) : -1);

	// v1.0.89: lock-in diagnostic so the user can confirm volumetric shadow + light function
	// state landed as intended on every InternalBeam toggle. CastShadows must be ON (the
	// SpotLight light-function projection AND the volumetric shadow path both gate on it),
	// CastVolumetricShadow must be ON (so the shaft carves through the fog), and the live
	// LightFunctionMaterial pointer should match GoboLightFnMID when a gobo is bound -- a
	// null here while a gobo is selected is the diagnostic for "gobo on floor, not on shaft".
	if (bEnabled && SpotLight)
	{
		const UMaterialInterface* LF = SpotLight->LightFunctionMaterial;
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Fixture %s InternalBeam: scatter=%.2f castShadows=%d volumetric-shadow=%d allowMegaLights=%d light-function=%s gobo-active=%d"),
			*FixtureId,
			SpotLight->VolumetricScatteringIntensity,
			SpotLight->CastShadows ? 1 : 0,
			SpotLight->bCastVolumetricShadow ? 1 : 0,
			SpotLight->bAllowMegaLights ? 1 : 0,
			LF ? *LF->GetName() : TEXT("<none>"),
			bGoboActive ? 1 : 0);
	}
}

void ARebusFixtureActor::RefreshInternalBeamOffset()
{
	// The offset is applied as the post-step at the bottom of RefreshMotion (both branches:
	// rig-path and synthetic-pan/tilt fallback), reading the LIVE GRebusInternalBeamOffsetSign
	// each call, so a CVar flip lands instantly without needing a per-actor cache invalidation.
	RefreshMotion();
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
			// v1.0.87 InternalBeam back-offset: push the spotlight back along its LIVE aim (Dir,
			// the synthetic pan/tilt-rotated emission forward) so the cone exits the lens plane
			// at the lens diameter at MAX zoom. Applied in the no-rig synthetic-aim path so the
			// offset rotates with the synthetic aim instead of staying locked to the static rest.
			//
			// v1.0.89 -- the sign is now operator-flippable via `Rebus.InternalBeamOffsetSign`
			// (default +1). v1.0.87 hard-coded `-Dir`, but the user reported the spotlight
			// ending up IN FRONT of the lens on their GDTF profile (proving the GDTF +Y / beam
			// direction resolved INTO the head, not out of the lens, so the legacy `-Dir` was
			// pushing the spot DOWN-stream). The default `+1` direction matches the reported
			// fix; toggling to `-1` restores the legacy v1.0.87 behaviour for diagnostics.
			if (bInternalBeamEnabled)
			{
				SpotLight->AddRelativeLocation(GRebusInternalBeamOffsetSign * ComputeInternalBeamBackOffsetCm() * Dir);
			}

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
			// v1.0.93 -- ride the InternalBeam cone-mesh shaft off the same live SpotLight
			// world emission, but anchor at the LENS plane (= SpotLoc - back-offset*LiveFwd)
			// so the shaft can never be visible inside the head body. No-op when the shaft
			// hasn't been spawned (cookie-cone path not active).
			DriveInternalBeamShaftFromSpotLight();
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
		// v1.0.87 InternalBeam back-offset: push the spotlight back along its LIVE FixtureRoot-
		// space forward (= the spotlight's local +X rotated into FixtureRoot, which is the head-
		// rotated GDTF emission forward) so the cone exits the lens plane at the lens diameter
		// at MAX zoom and strictly inside the lens diameter at every narrower zoom. The offset
		// rides through pan/tilt because we sample the LIVE relative rotation (set just above)
		// instead of a static fixture-local axis.
		//
		// v1.0.89 -- the sign is now operator-flippable via `Rebus.InternalBeamOffsetSign`
		// (default +1, the corrected direction for the user's reported GDTF profile). v1.0.87
		// hard-coded `-LiveFwd` and the user observed the spotlight ending up IN FRONT of the
		// lens, proving the spotlight's local +X resolved INTO the head on this content (so
		// `-LiveFwd` pushed the spot DOWN-stream, not up). Toggle `-1` for legacy behaviour.
		if (bInternalBeamEnabled)
		{
			const FVector LiveFwd = SpotLight->GetRelativeRotation().RotateVector(FVector::ForwardVector);
			SpotLight->AddRelativeLocation(GRebusInternalBeamOffsetSign * ComputeInternalBeamBackOffsetCm() * LiveFwd);
			UE_LOG(LogRebusVisualiser, Verbose,
				TEXT("Fixture %s InternalBeam offset: applied %+.2fcm along LiveFwd=(%.3f,%.3f,%.3f) (sign=%+.0f, restLoc=(%.1f,%.1f,%.1f), spotLoc=(%.1f,%.1f,%.1f))"),
				*FixtureId,
				GRebusInternalBeamOffsetSign * ComputeInternalBeamBackOffsetCm(),
				LiveFwd.X, LiveFwd.Y, LiveFwd.Z,
				GRebusInternalBeamOffsetSign,
				BeamRestTransform.GetLocation().X, BeamRestTransform.GetLocation().Y, BeamRestTransform.GetLocation().Z,
				SpotLight->GetRelativeLocation().X, SpotLight->GetRelativeLocation().Y, SpotLight->GetRelativeLocation().Z);
		}

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
		// v1.0.93 -- ride the InternalBeam cone-mesh shaft off the same live SpotLight world
		// emission, but anchor at the LENS plane so the shaft never reads inside the head body.
		DriveInternalBeamShaftFromSpotLight();
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

	// v1.0.93 -- and the InternalBeam shaft cone matches in lock-step (same OuterHalf -> same
	// far radius), so the visible shaft envelope tracks zoom live without the operator having
	// to cycle InternalBeam. No-op when the shaft hasn't been spawned (cookie-cone path not
	// active OR the master material wasn't baked yet).
	UpdateInternalBeamShaftGeometry();
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

	// v1.0.93 -- same path for the InternalBeam shaft cone (RefreshInternalBeamShaftEmissive
	// pushes Color + Intensity = Dimmer * Gate * RebusMeshBeamMaxIntensity * MeshBeamUserScale,
	// matching M_RebusBeam's envelope so a shutter close / dimmer fade collapses the visible
	// shaft in lock-step with the SpotLight intensity). No-op when the shaft MID isn't built.
	RefreshInternalBeamShaftEmissive();
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
	// v1.0.87: re-apply the InternalBeam back-offset on every zoom change. Currently the offset
	// is derived from the fixture's MAX zoom (constant for the actor's lifetime), so this is a
	// no-op pose-wise, but it future-proofs a per-fixture-descriptor MaxZoom retune (where the
	// max can shift between zoom messages) without needing a separate "MaxZoom changed" hook.
	if (bInternalBeamEnabled)
	{
		ApplyInternalBeamPose();
	}
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

	// v1.0.93 -- ALSO push the per-fixture GoboRT pointer to the new InternalBeam LF MID +
	// shaft MID (whichever exist on this fixture; the call is a no-op for either MID that
	// isn't built yet). Safe to call from BOTH branches: if a gobo is active GoboRT carries
	// the live-rotated cookie, if no gobo is active we revert the param to engine white so
	// the LF + shaft both sample "let the light through unchanged" (untextured uniform cone,
	// matching the pre-v1.0.93 visual). When the SpotLight is currently bound to the v1.0.93
	// LF MID (InternalBeam mode + cookie-cone path on) the cookie pattern reaches the
	// volumetric integrator via this push; when bound to the v1.0.49 MI_Light MID (outside
	// of InternalBeam mode, or with Rebus.InternalBeamCookieCone 0) the v1.0.49 MID's own
	// `DMX Gobo Disk Frosted` push above runs the floor footprint as before.
	PushGoboRTToInternalBeamMaterials();
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
