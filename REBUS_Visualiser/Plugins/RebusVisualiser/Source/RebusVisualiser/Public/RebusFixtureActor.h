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
//
// v1.0.94 -- LEGACY-PATH-ALWAYS POLICY (`Rebus.AllowMegaLights = 0` by default).
// Every Rebus SpotLight is opted out of MegaLights at construction (`bAllowMegaLights = 0`)
// regardless of mode (Epic-beam OR InternalBeam) and regardless of gobo state, so dynamic
// occluders ALWAYS cast hard shadows in the floor footprint. This was the v1.0.94 fix for
// "we are not seeing the shadow of an object in the [Epic-beam] footprint" -- MegaLights'
// tile-clustered shadow path silently drops dynamic occluders below its shadow-fidelity
// floor on low-end tiers, so a hanging truss / a person / a prop between the fixture and
// the floor lit up but cast no silhouette. Trade-off: every Rebus SpotLight loses
// MegaLights' clustering perf -- in show-context rigs (tens-to-hundreds of fixtures) this
// is the right default because shadow fidelity is non-negotiable for stage visualisation.
// `SpotLight->CastShadows` is also now ALWAYS asserted true in BuildSpotLight + every
// RefreshBeamShadowMode call (the pre-v1.0.94 logic cleared it on non-hero non-gobo
// fixtures as a perf opt -- a SpotLight with CastShadows=false casts NO shadows from any
// occluder even with MegaLights opted out). Operators can flip `Rebus.AllowMegaLights 1`
// at runtime to swap perf for fidelity (the CVar refresh sink walks every Rebus fixture
// and re-resolves the per-light flag through `RefreshAllowMegaLightsFromCVar`).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RebusSceneTypes.h"
#include "RebusFixtureActor.generated.h"

class USpotLightComponent;
class UStaticMeshComponent;
class UPrimitiveComponent;
class UStaticMesh;
class UMaterialInterface;
class UProceduralMeshComponent;
class UTextureLightProfile;
class UCanvas;
class UCanvasRenderTarget2D;
class FRebusRestClient;

UENUM()
enum class ERebusShutterMode : uint8
{
	Open = 0,
	Closed = 1,
	Strobe = 2
};

// A single scalar channel that can either snap or ease-in-out over fadeMs.
// v1.0.80: per-fixture state snapshot used by the live `FixtureStates` data-channel stream.
// Each subsystem tick (~10Hz) snapshots every spawned fixture into one of these, compares to
// the per-fixture last-sent snapshot, and includes the fixture in the outbound batch only if
// any field changed beyond a dead zone (so multi-client portals stay in sync without idle
// fixtures producing traffic). The snapshot is intentionally flat + small so per-fixture JSON
// serialisation is cheap; field names mirror the inbound SetFixture* descriptors so the
// portal can hold ONE state object and use the same field names in both directions.
struct REBUSVISUALISER_API FRebusFixtureStateSnapshot
{
	FString FixtureId;
	float   Dimmer = 0.f;          // 0..1 (live faded value, not target)
	float   PanDeg = 0.f;
	float   TiltDeg = 0.f;
	float   ZoomDeg = 0.f;         // FULL beam angle deg (v1.0.84 -- matches the wire SetFixtureZoom convention; portal can render the slider value directly)
	float   Iris = 1.f;
	float   Frost = 0.f;
	float   Focus = 0.f;
	FLinearColor Color = FLinearColor::White; // RGB (alpha unused)
	float   ColorTempK = -1.f;     // <0 => not in colour-temp mode (untouched)
	int32   ShutterMode = 0;       // 0=Open, 1=Closed, 2=Strobe (mirrors ERebusShutterMode)
	float   ShutterRateHz = 0.f;
	int32   GoboIndex = -1;        // -1 = none
	int32   GoboWheelIndex = -1;
	float   GoboRotSpeed = 0.f;    // -1..1 (combined wheel speed currently driving the MID)
	float   AnimWheelSpeed = 0.f;  // -1..1 (animation-wheel input, separate from gobo wheel)
	bool    bSelected = false;
	bool    bPrimarySelected = false;
};

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
	// Gobo wheel rotation; signed normalised speed in [-1..1] (clamped). Sign = direction:
	// positive = CW (looking down the beam), negative = CCW, zero = stop. WheelIndex is the
	// portal's selector (currently informational, logged; the actor pushes one rotation to the
	// single Epic gobo param).
	void ApplyGoboRotation(float Speed, int32 WheelIndex = INDEX_NONE);
	// v1.0.50: animation-wheel rotation; same units as ApplyGoboRotation. Stored separately and
	// added to the gobo rotation when pushing to Epic's DMX Gobo Disk Rotation Speed (Epic's
	// reference materials don't model a dedicated animation-wheel disc -- see header note above).
	void ApplyAnimationWheelRotation(float Speed);
	void ApplyPrism(int32 Facets, float RotationDeg);
	void ApplyBeamVolumetrics(float Intensity, bool bCastVolumetricShadow);

	// Selection highlight (§5.3).
	void SetSelected(bool bSelected, bool bPrimary);

	// Toggle the hybrid cone-mesh volumetric beam on/off (SetSceneProperty bMeshBeams, §8.4a).
	// When ON the mesh beam is the visible shaft and the SpotLight's fog VolumetricScatteringIntensity
	// is forced to 0 (no competing noisy froxel beam). When OFF the cone mesh is hidden and the
	// SpotLight's fog scattering is restored (the old fog beam), so the two can be A/B'd at runtime.
	void SetMeshBeamEnabled(bool bEnabled);

	// v1.0.87: temporary InternalBeam A/B mode. When ON:
	//   * the Epic DMX-Fixtures beam canvas + the procedural cone-mesh fallback are hidden +
	//     deactivated (but NOT destroyed -- toggling OFF returns the Epic beam intact);
	//   * the same `SpotLightComponent` that already lights the floor / projects gobos is promoted
	//     to ALSO be the visible volumetric beam: VolumetricScatteringIntensity is pushed to a
	//     sensible default (1.0) and CastVolumetricShadow is forced ON so the spotlight's beam
	//     carves through the exponential-height-fog froxels;
	//   * the SpotLight is pushed BACK along its local -X (the negation of the GDTF emission
	//     forward) by `offset = lensRadius / tan(MaxZoomHalfAngleRadians)`. With the offset fixed
	//     at the MAX zoom half-angle, the cone at the lens plane is exactly the lens diameter at
	//     MaxZoom and strictly INSIDE the lens at every narrower zoom -- the visible shaft exits
	//     the fixture head at the correct size at every zoom setting;
	//   * every fixture-body `UPrimitiveComponent` on this actor (the GDTF-derived head / yoke /
	//     base proxies -- NOT the SpotLight, the Epic beam, the cone, the lens disc, etc.) is
	//     opted out of the SpotLight's shadow casting (CastShadow + bCastDynamicShadow +
	//     bCastHiddenShadow + bCastShadowAsTwoSided -> false). Otherwise the head body would
	//     self-shadow the internal spotlight and no light could escape the fixture. World lights
	//     (sun / sky / key) still light the head from outside -- this is the per-primitive flag,
	//     not a lighting-channel reshuffle, so the blast radius is intentionally small.
	// When OFF every change above is byte-exact reversed: the Epic beam's prior visibility is
	// restored, the SpotLight relative location + volumetric flags go back to construction
	// defaults, and every cached primitive's CastShadow flags are restored from the per-comp
	// cache captured the first time the mode was enabled.
	//
	// v1.0.92 ALSO forces `SpotLight->bAllowMegaLights = 0` on the ON transition (regardless of
	// gobo state) so the legacy clustered/deferred light path renders the IES profile AND the
	// LightFunctionMaterial through the volumetric scattering integrator -- MegaLights bypasses
	// BOTH on the shaft, which was the v1.0.92 root-cause for "gobo + IES on the floor footprint
	// but not on the visible volumetric beam". The pre-toggle value is cached into
	// `bInternalBeamAllowMegaLightsOrig` and restored on the OFF transition. Gated by the new
	// `Rebus.InternalBeamForceLegacy` CVar (default 1; flip to 0 to leave MegaLights alone if a
	// deployment prefers MegaLights perf and doesn't need volumetric gobo/IES shaping).
	void SetInternalBeamModeEnabled(bool bEnabled);
	bool IsInternalBeamModeEnabled() const { return bInternalBeamEnabled; }

	// v1.0.89: re-apply the InternalBeam back-offset by re-running RefreshMotion (the offset is
	// applied as the post-step there, both in the rig path and the synthetic-pan/tilt fallback).
	// Public so the v1.0.89 `Rebus.InternalBeamOffsetSign` CVar refresh sink can re-push the new
	// sign onto every fixture currently in InternalBeam mode without forcing a full pose-rebuild
	// (volumetric / cast-shadow state is independent of the sign and stays untouched).
	void RefreshInternalBeamOffset();

	// v1.0.92: force `SpotLight->bAllowMegaLights = 0` on this fixture (idempotent), caching the
	// pre-push value into `bInternalBeamAllowMegaLightsOrig` the FIRST time the push lands so a
	// subsequent OFF transition can restore byte-exact. Called from `ApplyInternalBeamPose` on
	// the InternalBeam ON edge AND from the `Rebus.InternalBeamForceLegacy` CVar refresh sink
	// (when the CVar flips 0 -> 1 while fixtures are already in InternalBeam mode). No-op when
	// the SpotLight is already opted out (e.g. ApplyCurrentGoboToLightFn already pushed
	// bAllowMegaLights=0 because a gobo is active) -- the cache is still primed so the OFF
	// path knows what to restore. Triggers a `ReregisterComponent()` when the value transitions
	// because the engine reads `bAllowMegaLights` at proxy creation time (FLightSceneInfo, see
	// the v1.0.51 comment on the same flag near ApplyCurrentGoboToLightFn).
	void PushInternalBeamMegaLightsOptOut();

	// v1.0.94 -- single chokepoint that re-resolves `SpotLight->bAllowMegaLights` from the
	// CURRENT global gate `Rebus.AllowMegaLights` AND the per-fixture state (gobo cookie
	// active / InternalBeam mode + `Rebus.InternalBeamForceLegacy = 1`). Called from the
	// `Rebus.AllowMegaLights` CVar refresh sink for EVERY Rebus fixture (not just InternalBeam
	// ones, unlike the v1.0.92 sink); also safe to call directly when per-fixture state
	// changes. Idempotent / cheap when nothing transitioned -- ReregisterComponent is gated on
	// a value change. Resolves the desired value as: 0 when the global gate is 0 (the v1.0.94
	// HARD FLOOR -- legacy path always so dynamic occluders cast hard shadows in the floor
	// footprint); else 0 when a gobo is active OR InternalBeam mode is on with
	// `Rebus.InternalBeamForceLegacy = 1`; else 1 (MegaLights routing permitted).
	void RefreshAllowMegaLightsFromCVar();

	// v1.0.92: restore the cached `bAllowMegaLights` (push the ORIGINAL value back). Called from
	// `RestoreInternalBeamPose` on the InternalBeam OFF edge AND from the
	// `Rebus.InternalBeamForceLegacy` CVar refresh sink (when the CVar flips 1 -> 0 while
	// fixtures are still in InternalBeam mode). When a gobo is currently active the
	// ApplyCurrentGoboToLightFn path will re-assert bAllowMegaLights=0 on its next call (gobo
	// cookies still need the legacy LF path), so the restore reverts to whatever the gobo path
	// chooses -- which is the correct end state.
	void RestoreInternalBeamMegaLights();

	// v1.0.93 -- (re)apply the InternalBeam cookie-cone state to this fixture (gated by the
	// global `Rebus.InternalBeamCookieCone` CVar, default 1). When ON, the visible volumetric
	// shaft is a translucent additive cone mesh (`InternalBeamShaft`, built off
	// `M_RebusInternalBeamShaft`) that starts AT the lens plane and is sized to the live
	// SpotLight throw -- per-light VolumetricScatteringIntensity is forced to 0 so the engine
	// doesn't paint extra scattering INSIDE the head, and the cone mesh samples the per-fixture
	// GoboRT directly so the cookie appears IN the shaft (not just on the floor footprint).
	// When OFF, the shaft component is hidden and `RefreshBeamShadowMode` restores the v1.0.92
	// per-light scattering path. Called from `ApplyInternalBeamPose`, from `ApplyZoom` /
	// `RefreshMotion` (to re-size the cone), and from the `Rebus.InternalBeamCookieCone` CVar
	// refresh sink. Idempotent / cheap when nothing changed.
	void ApplyInternalBeamShaft(bool bForceVisible = false);
	// v1.0.93 -- restore the cached SpotLight `LightFunctionMaterial` + `VolumetricScattering
	// Intensity` from the v1.0.93 cache (separate from the v1.0.92 `bAllowMegaLights` cache so
	// each toggle can roundtrip independently). Called from `RestoreInternalBeamPose` on the
	// InternalBeam OFF edge AND from the `Rebus.InternalBeamCookieCone 0` CVar refresh sink
	// (when the operator opts out of the cone-mesh path while the mode is still on).
	void RestoreInternalBeamShaft();

	// v1.0.88 -- operator-flippable A/B toggle for the new <Beam> (isBeam) lens path.
	//
	// Default (bForceSynthetic=false): when /meshes carries at least one mesh flagged isBeam=true
	// (mesh-blob v3 from the portal), that REAL geometry IS the lens disc -- the mirror/glass
	// material is applied to it and one emissive lens-flare disc is co-located per isBeam mesh
	// (MAC-Aura-style LED matrices send N isBeam meshes; each gets its own correctly-sized
	// flare). The synthetic single-disc fallback (BuildLensDisc / LensDisc) is HIDDEN but kept
	// alive so re-enabling fallback at runtime doesn't require a respawn.
	//
	// Forced (bForceSynthetic=true via `Rebus.ForceSyntheticLensFallback 1`): every isBeam mesh
	// component is hidden (SetVisibility(false)), every per-beam emissive flare disc is hidden,
	// and the synthetic LensDisc is re-shown -- restores the pre-v1.0.88 visual exactly. Used
	// to A/B the real-geometry vs synthetic-disc paths on the same fixture without a respawn.
	//
	// Fixtures whose blob carries NO isBeam meshes (v2 blobs, or GDTFs whose <Beam> has no
	// <Model>) ALWAYS show the synthetic disc -- the toggle is a no-op for them.
	void SetUseSyntheticLensFallback(bool bForceSynthetic);
	bool IsUsingSyntheticLensFallback() const { return bUseSyntheticLensFallback; }
	// Diagnostic: count of mesh proxies on this actor tagged isBeam=true. Zero means the blob
	// carried no isBeam flag (or arrived as v2) and the synthetic disc fallback is in force.
	int32 GetIsBeamMeshCount() const { return IsBeamLensComponents.Num(); }

	// ---- Orbit-imported model binding (v1.0.35 introduced; v1.0.65 default ON) --------------
	// Bind the Orbit-imported fixture-model components (already matched to this fixture by object
	// id by the control subsystem) so their world transforms can be driven by this fixture's head
	// motion. Caches each component's imported (rest) world transform + the head world transform at
	// the rest pose (pan=tilt=0), so DriveOrbitModel applies ONLY the incremental head motion since
	// rest -- the Orbit model then tracks pan/tilt exactly like the control-channel head meshes,
	// using the SAME cumulative axis transform (Cumulative[HeadAxisIndex] in RefreshMotion), not a
	// parallel recomputation, so they cannot drift relative to each other. Re-binding replaces any
	// previous binding (e.g. after a re-import). Both meshes stay visible by default: the
	// control-channel proxies and the Orbit-imported geometry render on top of each other and
	// move in lockstep; teams that want only the Orbit geometry visible can hide the control-
	// channel mesh proxies separately.
	void BindOrbitComponents(const TArray<USceneComponent*>& Components, const FString& MatchedObjectId);
	void ClearOrbitBinding();
	// True only while at least one bound Orbit component is still alive (false after a re-import
	// destroyed them, so the control subsystem knows to re-bind).
	bool HasOrbitBinding() const;
	// Enable/disable driving the bound Orbit model from this fixture's motion. Disabling restores
	// the Orbit components to their imported (rest) world transforms.
	void SetDriveOrbitModel(bool bEnabled);
	bool IsDrivingOrbitModel() const { return bDriveOrbitModel; }
	const FString& GetBoundOrbitObjectId() const { return BoundOrbitObjectId; }
	// v1.0.70: show / hide the Orbit-imported components bound to this fixture (e.g. to A/B
	// against the control-channel mesh proxies, or to "remove" duplicate fixture geometry without
	// touching the rest of the Orbit import like trusses and set pieces). Walks OrbitComponents
	// and calls SetVisibility(bVisible, /*bPropagateToChildren*/ true) on each. Returns the
	// number of components affected. The drive loop is unchanged -- hidden components still
	// receive transform updates, so toggling visibility back on lands them in the current pose
	// instead of a stale one.
	int32 SetOrbitVisibility(bool bVisible);

	// v1.0.85: fill OutSet with every live primitive component in this fixture's Orbit
	// binding. Used by URebusVisualiserSubsystem::ApplyTrussMaterialPass to EXCLUDE
	// fixture-bound geometry from the truss/powdercoat override (fixture-bound comps keep
	// the body+lens override path above). Skips dead weak handles and non-primitive
	// scene components. Cheap; bounded by the binding size (typically 2-8 per fixture).
	void GetBoundOrbitPrimitives(TSet<UPrimitiveComponent*>& OutSet) const;

	// v1.0.71: enable/disable the body+lens material override on every mesh owned by this
	// fixture (control-channel UProceduralMeshComponent body meshes AND the Orbit-imported
	// components that were bound by RebindOrbitModels). When enabled, every non-lens mesh gets
	// the cached body MID (black satin plastic) and every mesh whose name/tag matches a "lens"
	// keyword gets the cached lens MID (mirrored glass). When disabled, the procedural meshes
	// drop back to their saved original materials (cached at first override) and the Orbit
	// components drop back to their pre-override materials. Returns (bodyApplied, lensApplied).
	struct FFixtureMaterialApplyCount { int32 Body = 0; int32 Lens = 0; int32 Restored = 0; };
	FFixtureMaterialApplyCount SetFixtureMaterialOverrideEnabled(bool bEnabled);
	bool IsFixtureMaterialOverrideEnabled() const { return bOverrideFixtureMaterials; }

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

	// v1.0.80: read every live control value into a flat snapshot for the FixtureStates
	// outbound stream. Reads the CURRENT (live-faded) fade values, not the target -- so the
	// portal sees the fixture moving through the fade rather than snapping to the endpoint.
	// Selection state is filled in by the caller (URebusVisualiserSubsystem) because that
	// lives in URebusFixtureControlSubsystem, not on the actor.
	FRebusFixtureStateSnapshot GetFixtureStateSnapshot() const;

	// Reset the per-batch "hero beam" volumetric-shadow budget. Called by the session subsystem
	// before each (re)spawn so the first N fixtures of every fresh scene get volumetric shadows
	// (rather than the budget being permanently consumed by the very first scene). See
	// BuildSpotLight + RebusMaxVolumetricShadowBeams.
	static void ResetVolumetricShadowBudget();
	// v1.0.47: per-spawn-batch diagnostic. Reports how many fixtures asked for volumetric shadows
	// vs how many were granted hero slots, so the user can immediately see whether the portal is
	// sending castVolumetricShadow=true and whether the hero budget is filtering anyone out.
	static void LogVolumetricShadowBudget(int32 SpawnedTotal);

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
	// ---- Hybrid cone-mesh volumetric beam (Phase 1, §8.4a) ----
	void BuildBeamCone();          // spawn the procedural cone mesh + per-fixture beam MID
	void UpdateBeamConeGeometry(); // (re)generate the frustum: base=lens radius, far=Length*tan(half)
	void RefreshBeamEmissive();    // drive BeamColor/BeamIntensity from live colour x dimmer x gate
	// Push the WORLD-space beam origin + forward to the raymarch MID (BeamOrigin/BeamDir). The
	// vector is sampled from the LIVE SpotLight (its world emission forward / location) so the
	// marched body always matches where the floor is actually lit; also emits the alignment log.
	void RefreshBeamSpatialParams();
	// Re-orient + co-locate the cone mesh from the live SpotLight world transform (ground truth)
	// so its +X (frustum opening) is the real emission forward, then re-push the raymarch feeds.
	void DriveBeamConeFromSpotLight();
	// v1.0.43: when Epic's official DMX Fixtures content is installed, build the visible beam from
	// Epic's REAL SM_Beam_RM canvas mesh + MI_Beam (M_Beam_Master) world-space raymarch material,
	// driven by our data. Returns true if the Epic assets loaded + the beam was wired (the live
	// path), false to fall back to the procedural cone + M_RebusBeam.
	bool TryBuildEpicBeam();
	// Push our drives onto Epic's M_Beam_Master param vocabulary (DMX Color / DMX Max Light
	// Intensity / DMX Max Light Distance / DMX Lens Radius / DMX Quality Level).
	void UpdateEpicBeamParams();
	// Ride the live SpotLight emission (origin=lens, length axis -> emission) and scale Epic's
	// canvas mesh to ENCLOSE the cone (length x far radius). The M_Beam_Master raymarch is
	// world-space/param-driven, so an over-sized canvas only affects coverage, not the beam shape.
	void DriveEpicBeamFromSpotLight();
	// Resolve the SpotLight's volumetric state for Phase-2 light-blocking shadows: hero shadow
	// beams (bWantsVolumetricShadow + budget) get a modest fog VolumetricScatteringIntensity + Cast
	// Volumetric Shadow (native VSM carves truss gaps on runtime meshes); everyone else is mesh-only
	// (scattering 0). When the mesh beam is toggled off the fog beam is restored. Public so the
	// v1.0.47 `Rebus.HeroShadowScatter` CVar's OnChanged sink can re-apply the new scatter live.
public:
	void RefreshBeamShadowMode();
private:
	float ResolveOuterHalfDeg() const; // current outer cone half-angle (zoom range + iris), degrees
	void RefreshMotion();         // re-solve pan/tilt and push transforms to groups + light
	// v1.0.68: per-component-axis drive. Each bound Orbit component carries its own bucketed
	// motion axis (OrbitAxisBucket[i]) -- INDEX_NONE for base components (no motion), pan axis for
	// yoke arms, tilt axis for the head -- exactly mirroring how MeshAxisBucket drives the
	// control-channel mesh proxies. DriveOrbitModel walks Cumulative (full axis array from
	// RebusMotion::Solve, parent-first) and applies Cumulative[OrbitAxisBucket[i]] per component
	// instead of one uniform HeadLocal for the whole fixture. An empty Cumulative (no-rig fixture)
	// leaves every component at its imported rest pose. No-op when not driving / unbound.
	void DriveOrbitModel(const TArray<FTransform>& Cumulative);
	// The head's fixture-local transform for a given pan/tilt (deepest head axis' cumulative solve;
	// identity for no-rig fixtures). Shared by the Orbit-bind rest capture + live drive.
	FTransform ComputeHeadLocal(float InPanDeg, float InTiltDeg) const;
	// v1.0.68 helper: convenience wrapper that solves the rig for InPan/InTilt and forwards the
	// resulting Cumulative array to DriveOrbitModel. Used by the bind + set-drive code paths
	// where the caller only has scalar pan/tilt, not the full Cumulative.
	void DriveOrbitModelFromPanTilt(float InPanDeg, float InTiltDeg);

	// Stop a primitive owned by THIS fixture (a control-channel body mesh or a bound Orbit model
	// component) from casting the dynamic VSM shadow that mottles its own beam's volumetric fog,
	// while keeping it a shadow caster for contact/RT grounding. UE5.7 exposes no per-primitive
	// volumetric-fog flag, so opting the body out of the dynamic shadow map (the source the fog
	// inscattering samples) is the lever. Applied to control meshes on build + Orbit comps on bind.
	static void DisableSelfBeamVolumetricShadow(UPrimitiveComponent* Comp);
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
	// v1.0.48: push the cached gobo state (texture + Num Mask + Index + rotation) onto Epic's
	// M_Beam_Master MID -- the actual visible cone -- so the user sees the gobo inside the beam.
	// When CurrentGoboTexture is null, reverts to the MI parent's default (EpicBeamDefaultGoboTex).
	void ApplyCurrentGoboToEpicBeam();
	// v1.0.49: same texture/rotation also drives Epic's M_Light_Master (LightFunction domain) so
	// the gobo projects onto the lit floor pool. Lazily MIDs MI_Light on first call; sets
	// SpotLight->LightFunctionMaterial. On clear (CurrentGoboTexture==null) nulls the light fn.
	void ApplyCurrentGoboToLightFn();
	// v1.0.57 introduced, v1.0.58 corrected vocabulary, v1.0.59 collapsed shutter into DMX Dimmer,
	// v1.0.60 forced DMX Dimmer = 1 to stop double-dimming: push the verified M_Light_Master
	// vocabulary onto GoboLightFnMID -- now just DMX Dimmer (FORCED to 1.0 so the cookie is a
	// pure spatial pattern) and DMX Frost (live frost fade). The v1.0.59 push of DMX Dimmer =
	// Dim * Gate was double-dimming the footprint: a LightFunction material in UE is multiplied
	// with the light's per-pixel illumination contribution, and that contribution already has
	// SpotLight->SetIntensity (= BaseCandela * Dim * Gate, see RefreshIntensity) baked in. The
	// footprint was therefore brightness ~ Dim^2 * Gate^2 while the volumetric beam mesh fades
	// linearly with Dim * Gate -- at Dim=0.5 the beam was 50% but the footprint collapsed to
	// 25%, which the user observed as "intensity doesn't match the beam" and "doesn't allow for
	// distance change" (1/r^2 was being applied via the SpotLight's Candelas-mode physical
	// attenuation, but at low Dim the double-dim crushed the pattern to near-black regardless
	// of distance). v1.0.60 makes the SpotLight the SINGLE source of truth for dimmer + shutter
	// + IES + 1/r^2 + colour; the cookie just modulates the pre-attenuated illumination by the
	// gobo pattern. Footprint per-pixel = BaseCandela * Dim * Gate * IES * (1/r^2) * pattern --
	// linear in Dim, physically correct in distance, IES-accurate in angle. SetFixtureShutter
	// is still the unified shutter control: Gate is multiplied into SpotLight->SetIntensity in
	// RefreshIntensity, into EpicBeamMID DMX Dimmer in UpdateEpicBeamParams, and -- crucially --
	// NOT into the cookie here, because the cookie inherits Gate transitively via the SpotLight
	// intensity it modulates. No-op when GoboLightFnMID is null (no gobo active). Called from
	// UpdateEpicBeamParams (every refresh) and from ApplyCurrentGoboToLightFn (lazy-MID-creation
	// primer + every gobo selection).
	void UpdateEpicLightFnParams();
	// v1.0.49: explicit clear path. Drops CurrentGoboTexture, reverts the Epic beam MID to its
	// MI default, nulls the SpotLight light function, clears bGoboActive, and reasserts shadows.
	void ClearGoboToOpen(const TCHAR* Reason);
	// v1.0.83: per-light Lumen isolation while a gobo is active. Sets
	// SpotLight->SetAffectGlobalIllumination(!bGoboActive) so Lumen doesn't sample the lit
	// floor for indirect bounce while the cookie pattern is animating -- removes the
	// Lumen-side ghost trail surgically, without the v1.0.78 global Lumen.Temporal=0 nuke
	// (which added noise everywhere). The direct beam still casts and lights surfaces; only
	// the indirect (bounce) contribution is suppressed. Called from both ApplyGobo paths and
	// from ClearGoboToOpen so the SpotLight always agrees with bGoboActive.
	void RefreshGoboLumenIsolation();
public:
	// v1.0.49: case-insensitive match against known "no-gobo" slot names. Trimmed; returns true
	// for "Open"/"None"/"Empty"/"Clear"/"No Gobo"/"Open Hole"/"Off" and a few common variants.
	// Public so RebusVisualiserSubsystem's RegisterFixtureGobos finalizer can tag Open entries.
	static bool IsOpenSlotName(const FString& Name);
	// v1.0.51: dump THIS fixture's SpotLight + any sibling light components for cookie debugging.
	// Called by the Rebus.DumpFixtureLights console command (registered in RebusVisualiser.cpp).
	void DumpLightStateForDebug() const;

	// v1.0.74: dump THIS fixture's gobo runtime state -- whether a gobo is bound, RT size +
	// pointer, current rotation angle, combined spin rate, MegaLights opt-out flag, light-
	// function material pointer, and last frame the RT was redrawn. Called by the new
	// Rebus.DumpGoboState console command so the user can paste a single block and we can
	// tell whether the RT is actually being refreshed each frame, whether the LF is bound,
	// and whether the MegaLights opt-out is in force -- all the ingredients of the v1.0.73/74
	// anti-ghost diagnosis.
	void DumpGoboStateForDebug() const;

	// v1.0.91: dump THIS fixture's IES runtime state in one line -- which IES profile is
	// loaded (inline iesText vs URL), the zoomDmx that selected it, the IESTexture object
	// name, the parsed peak candela max, the SpotLight's live IntensityUnits + Intensity,
	// and the breakdown of BaseCandela vs IesCandelaMax with the live dimmer/shutter-gate
	// multipliers so the operator can confirm the candela-max -> SpotLight.Intensity chain
	// landed. Called by `Rebus.DumpFixtureIes [fixtureId]`.
	void DumpIesStateForDebug() const;

	// v1.0.75: rebuild the gobo render target at a new square pixel size + re-bind it on the
	// cookie/light-function MIDs. Called by Rebus.GoboRTSize. Size is clamped to [128, 8192]
	// and rounded up to a power of two (mipmap generation requires it for tight LOD chains).
	// Per-actor so a future portal-side "this hero fixture wants 2048 gobos" descriptor can
	// target a single fixture without globally bumping VRAM; the console command iterates
	// every fixture in every Game/PIE world. Returns the resolved size in pixels (0 on no-op
	// when the fixture isn't ready yet or has no active gobo path).
	int32 RebuildGoboRTAtSize(int32 RequestedSizePixels);
private:
	void FetchAndAssignGobo(int32 GoboIndex);            // existing profile-wheel URL path
	void FetchAndAssignGoboFromUrl(const FString& ImageUrl);

	static int32 FindFirstGoboWheel(const FRebusFixtureProfile& Profile);

	// Count of spotlights granted volumetric shadows in the current spawn batch (hero-beam cap).
	static int32 VolumetricShadowBeamCount;

	// Phase-2 hero-shadow budget: count of fixtures granted the native VSM fog volumetric-shadow
	// hybrid (RefreshBeamShadowMode) in the current spawn batch. Capped at RebusMaxShadowFogBeams.
	static int32 ShadowFogBeamCount;

private:
	UPROPERTY() TObjectPtr<USceneComponent> FixtureRoot = nullptr;
	UPROPERTY() TObjectPtr<USpotLightComponent> SpotLight = nullptr;

	// One scene component per motion axis (index-aligned with Profile.MotionRig.Axes), plus
	// the mesh proxies parented under FixtureRoot (their relative transform is driven directly).
	UPROPERTY() TArray<TObjectPtr<UProceduralMeshComponent>> MeshComponents;

	// Index into MeshComponents -> axis index it tracks (INDEX_NONE = static base).
	TArray<int32> MeshAxisBucket;

	UPROPERTY() TObjectPtr<UTextureLightProfile> ActiveIesProfile = nullptr;
	// LEGACY: a light-function MID that was meant to project the gobo on the lit floor pool. It was
	// declared but never instantiated (no /Game/REBUS M_RebusGobo asset exists), so the SpotLight
	// projection path silently no-oped from day one. v1.0.48 leaves the property in place and skips
	// it cleanly when null; the user-visible gobo now goes through the Epic beam MID below.
	UPROPERTY() TObjectPtr<class UMaterialInstanceDynamic> GoboMID = nullptr;

	// v1.0.48 GOBO state: cache the decoded slot image so we can re-push it every time the Epic
	// beam MID is rebuilt or refreshed (UpdateEpicBeamParams) without re-decoding. EpicBeamDefault
	// GoboTex snapshots the MI parent's "DMX Gobo Disk Frosted" at canvas-build time so a "clear
	// gobo" can revert to Epic's default instead of leaving the last-selected image stuck. Rotation
	// speed mirrors ApplyGoboRotation's input and feeds the MI's "DMX Gobo Disk Rotation Speed".
	UPROPERTY() TObjectPtr<UTexture2D> CurrentGoboTexture = nullptr;
	UPROPERTY() TObjectPtr<UTexture> EpicBeamDefaultGoboTex = nullptr;

	// v1.0.53: per-fixture canvas render target that holds the SOURCE gobo texture redrawn
	// rotated by GoboAngle each tick. Bound (instead of CurrentGoboTexture itself) as Epic's
	// "DMX Gobo Disk Frosted" texture param on EpicBeamMID + GoboLightFnMID, so the projected
	// pattern spins in plane around the cookie's out-of-screen axis. Lazily allocated on first
	// non-Open gobo apply (EnsureGoboRT) and torn down with the actor. Update is driven by
	// UCanvasRenderTarget2D::UpdateResource() -> OnCanvasRenderTargetUpdate -> OnGoboRTUpdate.
	UPROPERTY() TObjectPtr<UCanvasRenderTarget2D> GoboRT = nullptr;
	UFUNCTION() void OnGoboRTUpdate(UCanvas* Canvas, int32 Width, int32 Height);
	void EnsureGoboRT();
	// v1.0.59: last CurrentGoboTexture we kicked GoboRT->UpdateResource() for. Used to gate the
	// redraw so it ONLY fires when the source gobo actually changes (new SetFixtureGobo
	// selection / inline-bytes decode / ClearGoboToOpen), NOT every time ApplyCurrentGoboToEpic
	// Beam is called for a per-frame param refresh. The unconditional v1.0.53 redraw was clearing
	// the RT to transparent then redrawing every Tick during dimmer/colour/motion fades --
	// EpicBeamMID and GoboLightFnMID both sample the RT, and the cookie footprint flashed because
	// the user-visible material sampled the "clear" frame between the clear and the OnGoboRTUpdate
	// draw. The beam mesh was less visibly affected because translucent surface materials hide the
	// 1-frame gap better than a hard-edged LightFunction projection. Compare-pointer (not equals)
	// is sufficient -- CurrentGoboTexture is a TObjectPtr so == checks the underlying UObject*.
	UPROPERTY() TObjectPtr<UTexture2D> LastGoboRTUpdateTex = nullptr;

	// v1.0.63: per-fixture procedural circular IRIS MASK texture. Drawn on top of the gobo into
	// GoboRT (BLEND_Modulate) so the cookie projects through a circular aperture instead of
	// pinching the SpotLight outer-cone angle (which had been zooming the gobo pattern -- the
	// user reported "iris is zooming instead of circular cropping like an iris would").
	// EnsureIrisMaskTexture lazily allocates the texture (128x128 BGRA8) and re-fills it with an
	// anti-aliased white-on-transparent disc when Iris.Current changes (quantised to 0.01 to
	// avoid per-frame regen during a fade). Only used while bGoboActive -- no cookie -> no RT to
	// modulate, so iris falls back to the cone-angle scaling path in ResolveOuterHalfDeg.
	UPROPERTY() TObjectPtr<UTexture2D> IrisMaskTex = nullptr;
	float LastIrisMaskValue = -1.f; // sentinel -1 = uninitialised; else quantised 0..1
	void EnsureIrisMaskTexture(float Iris01);

	float CurrentGoboRotationSpeed = 0.f;
	// v1.0.50: animation-wheel rotation, signed normalised [-1..1]. Epic's M_Beam_Master has no
	// dedicated animation-wheel param (only DMX Gobo Disk Rotation Speed), so we add this to the
	// gobo rotation when pushing to the MID -- a best-effort fallback so the user sees SOME
	// rotation change for animation-wheel commands. Tracked separately so the portal can drive
	// either independently and we keep the per-channel state correct for future per-wheel materials.
	float CurrentAnimationWheelSpeed = 0.f;

	// v1.0.49 COOKIE state. M_Light_Master (Epic's DMXFixtures LightFunction-domain master) is
	// MID'd once on first gobo apply and assigned to SpotLight->LightFunctionMaterial so the
	// SAME gobo also projects on the lit pool. It shares the M_Beam_Master gobo params
	// (DMX Gobo Disk Frosted + Num Mask + Index + Disk Rotation Speed) via MF_DMXGobo, so a
	// single texture + rotation feeds both the cone and the cookie. bGoboActive latches whenever
	// a non-Open texture is live; RefreshBeamShadowMode ORs it into SpotLight->SetCastShadows
	// because a SpotLight light-function only projects when the light is also casting shadows.
	UPROPERTY() TObjectPtr<class UMaterialInstanceDynamic> GoboLightFnMID = nullptr;
	bool bGoboActive = false;

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

	// ---- v1.0.88 real <Beam> (isBeam) lens path -------------------------------------------
	//
	// IsBeamLensComponents: the procedural-mesh components built from `/meshes` entries whose
	//   FRebusMesh.bIsBeam == true (mesh-blob v3). Each has the mirror/glass FixtureLensMID (or
	//   the FixtureLensMaterialOverride .uasset) on every material slot, the ComponentTag
	//   "RebusIsBeamLens" for grep, and ALWAYS rides the head via the existing geometry-name
	//   axis bucketing (`RebusMotion::ResolveAxisForMesh`) -- the isBeam flag is purely an
	//   identification hint, never a motion override (the user-doc explicitly warned that
	//   special-casing isBeam meshes out of the bucket map would detach the lens from the
	//   head).
	// IsBeamFlareDiscs / IsBeamFlareMIDs: per-isBeam emissive lens-flare disc + MID (using the
	//   same M_RebusLensFlare material the synthetic LensDisc uses). One flare per isBeam mesh
	//   so an LED matrix like MAC Aura gets one correctly-sized flare per pixel. Sized by
	//   `photometrics.lensDiameter` when a SINGLE isBeam mesh exists; multi-beam fixtures
	//   ALWAYS size from per-mesh local bounds (so the whole-fixture photometric diameter does
	//   not over-size every per-pixel flare). Arrays are index-aligned to IsBeamLensComponents.
	// bUseSyntheticLensFallback: live flag flipped by `Rebus.ForceSyntheticLensFallback` (or
	//   the `SetUseSyntheticLensFallback` public API). When true, the isBeam meshes + per-beam
	//   flares are hidden (SetVisibility(false) -- "invisible/passthrough" on the visible
	//   surface; the procedural-mesh geometry stays in the scene so toggling back is a single
	//   visibility flip with no rebuild) and the synthetic LensDisc is re-shown. Default false.
	TArray<TWeakObjectPtr<UPrimitiveComponent>> IsBeamLensComponents;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> IsBeamFlareDiscs;
	UPROPERTY() TArray<TObjectPtr<class UMaterialInstanceDynamic>> IsBeamFlareMIDs;
	bool bUseSyntheticLensFallback = false;

	// One-time emissive lens-flare build for every IsBeamLensComponents entry, called from
	// Setup() AFTER BuildLensDisc so the synthetic disc + its CDO refs are already resolved
	// (the per-beam flares re-use LensPlaneMesh + LensMaterial for cook-safety). No-op when
	// IsBeamLensComponents is empty (v2 blob / no <Beam>-with-<Model> in the GDTF).
	void BuildIsBeamLensFlares();
	// Re-apply mirror/glass material vs original-material AND visibility on every isBeam
	// component + per-beam flare AND show/hide the synthetic LensDisc, based on the current
	// value of `bUseSyntheticLensFallback` + whether any isBeam meshes were detected. Called
	// from Setup (end), SetUseSyntheticLensFallback, and by the console-CVar refresh sink.
	void RefreshIsBeamLensVisuals();
	// Sister to RefreshLensDisc (which drives the synthetic disc emissive): pushes the live
	// dimmer x colour x shutter-gate onto every per-beam flare MID. Called from the same call
	// sites RefreshLensDisc is called from.
	void RefreshIsBeamFlareEmissive();

	// v1.0.71 fixture body/lens material override -- the user-facing "make every fixture look
	// like a black satin plastic moving head, with mirrored-glass lenses" pass.
	//
	// FixtureMatParent: parametric PBR parent for the runtime-built MIDs. Defaults to
	//   `/Engine/BasicShapes/BasicShapeMaterial` (exposes Color + Metallic + Roughness as
	//   parameters, ships with every UE install, no new content required). The constructor also
	//   probes `/Game/REBUS/Materials/M_RebusFixtureBody`/`M_RebusFixtureLens` -- if the user
	//   drops their own .uasset materials at those paths, they take precedence and the MIDs
	//   are not built (the override is the user's material verbatim).
	// FixtureBodyMaterialOverride / FixtureLensMaterialOverride: optional user overrides
	//   loaded via ConstructorHelpers from /Game (so cook captures them when present).
	// FixtureBodyMID / FixtureLensMID: lazy per-actor MIDs built off FixtureMatParent in
	//   EnsureFixtureMIDs the first time a mesh asks for them. Body MID is configured for
	//   black satin plastic (Color=#050505, Metallic=0, Roughness=0.35); Lens MID for mirrored
	//   glass (Color=#F2F2F2, Metallic=1, Roughness=0.05).
	// OriginalMeshMaterials / OriginalOrbitMaterials: cached slot-0 materials captured the
	//   first time the override is applied to each component, so SetFixtureMaterialOverride
	//   Enabled(false) can restore the pre-override look exactly.
	UPROPERTY() TObjectPtr<UMaterialInterface> FixtureMatParent = nullptr;
	UPROPERTY() TObjectPtr<UMaterialInterface> FixtureBodyMaterialOverride = nullptr;
	UPROPERTY() TObjectPtr<UMaterialInterface> FixtureLensMaterialOverride = nullptr;
	UPROPERTY() TObjectPtr<class UMaterialInstanceDynamic> FixtureBodyMID = nullptr;
	UPROPERTY() TObjectPtr<class UMaterialInstanceDynamic> FixtureLensMID = nullptr;

	// v1.0.93 -- cook-safe hard refs to the two new Python-authored masters (built by
	// `build_rebus_base_level.py::ensure_*` on startup):
	//   * `GoboLightFunctionMaterial` -- `M_RebusGoboLightFunction`, the cookie LF with
	//     `bUsedWithVolumetricFog=true`. The smoking-gun flag missing from Epic's stock
	//     MI_Light: the per-material gate that lets the LF reach the volumetric integrator.
	//   * `InternalBeamShaftMaterial` -- `M_RebusInternalBeamShaft`, unlit additive cone-mesh
	//     shader that samples the per-fixture GoboRT directly so the cookie pattern reads in
	//     the visible volumetric beam shaft (the user-reported "gobo not in the volumetric
	//     beam" fix). Domain=Surface, ShadingModel=Unlit, BlendMode=Additive, two-sided.
	// Both refs are resolved by the constructor (FObjectFinder) so the cooker packages them.
	// If a path is missing (e.g. the Python builder hasn't run yet on a fresh checkout) the
	// ref stays null and the runtime falls back to the v1.0.92 per-light scattering path --
	// a benign Warning is logged in `EnsureFixtureInternalBeamMIDs` so the operator can see
	// they need to run `build_rebus_base_level.py` (or restart the editor so the startup
	// hook runs).
	UPROPERTY() TObjectPtr<UMaterialInterface> GoboLightFunctionMaterial = nullptr;
	UPROPERTY() TObjectPtr<UMaterialInterface> InternalBeamShaftMaterial = nullptr;
	// v1.0.93 per-fixture MIDs built off the masters above. `GoboLightFunctionMID` carries
	// the per-fixture `GoboRT` texture parameter (so each fixture's cookie is independent)
	// and is assigned as the SpotLight's `LightFunctionMaterial` while InternalBeam mode +
	// a gobo are both active. `InternalBeamShaftMID` carries `Color` / `Intensity` / `GoboRT`
	// + the spatial params (`BeamOrigin` / `BeamDir` / `BeamLength` / `LensRadius` /
	// `FarRadius`) and is applied to `InternalBeamShaft` (the cone-mesh component below).
	UPROPERTY() TObjectPtr<class UMaterialInstanceDynamic> GoboLightFunctionMID = nullptr;
	UPROPERTY() TObjectPtr<class UMaterialInstanceDynamic> InternalBeamShaftMID = nullptr;
	// v1.0.93 -- the procedural cone mesh that IS the visible volumetric shaft while
	// InternalBeam-cookie-cone mode is on. Geometry mirrors `BeamCone` (truncated frustum,
	// base = LensRadius at the lens plane, far = BeamLength * tan(outerHalf)) but attaches
	// at the LENS plane (NOT inside the head where the SpotLight sits after the v1.0.87
	// back-offset), so the shaft cannot be visible inside the head body. Material is
	// `InternalBeamShaftMID`, which samples the per-fixture GoboRT so the cookie pattern
	// appears IN the shaft. CastShadow = false (purely emissive). Tagged with
	// `RebusInternalBeamShaft` so the shadow walker can skip it.
	UPROPERTY() TObjectPtr<UProceduralMeshComponent> InternalBeamShaft = nullptr;
	float InternalBeamShaftLastFarRadius = -1.f;   // rebuild gate (same trick as BeamConeLastFarRadius)
	UPROPERTY() TArray<TObjectPtr<UMaterialInterface>> OriginalMeshMaterials; // index-aligned to MeshComponents
	// Non-UPROPERTY because TMap with TWeakObjectPtr key is not reliably UHT-supported; value is
	// weak too so a GC'd material (or destroyed comp on re-import) makes the restore a no-op
	// rather than crashing. The original materials we cache for Orbit comps are GLB-imported
	// assets owned by the OrbitConnector plugin's import root, so in practice they outlive the
	// override -- the weak-weak storage is purely defensive.
	TMap<TWeakObjectPtr<USceneComponent>, TWeakObjectPtr<UMaterialInterface>> OriginalOrbitMaterials;
	bool bOverrideFixtureMaterials = true; // default on; flippable via the console command

	// Build (once) the per-actor body + lens MIDs from FixtureMatParent. No-op if the parent is
	// missing OR the user-override variants are present (in which case we use those verbatim).
	void EnsureFixtureMIDs();

	// v1.0.93 -- build (once) the per-actor InternalBeam gobo LF MID + shaft MID off the
	// Python-baked masters (`GoboLightFunctionMaterial` + `InternalBeamShaftMaterial`).
	// Logs a one-shot Warning per missing master (the Python builder hasn't baked it yet on
	// a fresh checkout, OR an older project drop predates v1.0.93) and returns null in that
	// slot so the caller falls back to whatever the pre-v1.0.93 path did.
	void EnsureFixtureInternalBeamMIDs();
	// v1.0.93 -- (re)build the cone-mesh shaft geometry from the live throw / lens radius /
	// outer cone half-angle. Same rebuild-gate trick as `UpdateBeamConeGeometry` so live
	// zoom fades don't allocate every frame. Also pushes the spatial params (`BeamOrigin` /
	// `BeamDir` / `BeamLength` / `LensRadius` / `FarRadius`) to `InternalBeamShaftMID`.
	void UpdateInternalBeamShaftGeometry();
	// v1.0.93 -- push the live color + dimmer*shutter-gate*candela onto the shaft MID so the
	// shaft tracks the SpotLight intensity in lockstep (it's the visible cone now, the
	// SpotLight only lights surfaces). Called from `RefreshIntensity` alongside the existing
	// `RefreshBeamEmissive`.
	void RefreshInternalBeamShaftEmissive();
	// v1.0.93 -- ride the shaft cone off the SpotLight's live world transform (lens-plane
	// origin -- the SpotLight is back-offset INSIDE the head, so we subtract LiveFwd * back-
	// offset to recover the lens plane; the shaft starts AT the lens, not inside the head).
	// Mirrors `DriveBeamConeFromSpotLight` but anchors at the lens plane rather than the
	// SpotLight location.
	void DriveInternalBeamShaftFromSpotLight();
	// v1.0.93 -- push the per-fixture `GoboRT` Texture2D parameter onto the LF MID + the
	// shaft MID (or revert to the white default if no gobo is active). Idempotent / cheap;
	// safe to call from the cookie path (`ApplyCurrentGoboToLightFn`) without checking the
	// InternalBeam mode state -- the param just lands on whichever MID is bound.
	void PushGoboRTToInternalBeamMaterials();
	// Apply the override to a single mesh component. Captures the original material into
	// OutOriginal* the first time, so the override can be reverted cleanly. bIsOrbitComp picks
	// which cache to use. Returns true when a material was applied.
	bool ApplyFixtureMaterialTo(class UPrimitiveComponent* Comp, const FString& MeshName,
		const FString& GeomName, bool bIsOrbitComp);
	// True when any of the supplied tokens (case-insensitive) contains a lens-shaped substring
	// -- "lens" / "glass" / "crystal" / "optic" / "front" (last covers many GDTF naming
	// conventions for the front optic).
	static bool IsLensToken(const FString& Token);

	// Hybrid volumetric beam (Phase 1, §8.4a): a procedural TRUNCATED-CONE (frustum) mesh + an
	// additive faux-volumetric MID (M_RebusBeam), sized to the IES distribution (base = the lens
	// radius from ResolveLensDiameterMeters, far = Length*tan(fieldHalfAngle), length = the
	// SpotLight throw) and parented under FixtureRoot so it rides the head exactly like the
	// SpotLight + lens disc (BeamConeRest * Head). It coexists with the SpotLight (which keeps
	// surface lighting + IES + soft shadows); the SpotLight's fog VolumetricScatteringIntensity is
	// forced to 0 while the mesh beam is on. BeamMaterial is a hard CDO ref so the cooker packages
	// the master (the runtime LoadObject-by-path is not a cook dependency on its own).
	UPROPERTY() TObjectPtr<UProceduralMeshComponent> BeamCone = nullptr;
	UPROPERTY() TObjectPtr<class UMaterialInstanceDynamic> BeamMID = nullptr;
	UPROPERTY() TObjectPtr<UMaterialInterface> BeamMaterial = nullptr;

	// Epic DMX official beam (v1.0.43). Hard CDO refs (so the cooker packages them) to Epic's REAL
	// beam canvas mesh (SM_Beam_RM) + beam material (MI_Beam / M_Beam_Master) from the installed DMX
	// Fixtures plugin content (/DMXFixtures/LightFixtures/...). Null when the DMX content isn't
	// installed, in which case BuildBeamCone keeps the procedural cone + M_RebusBeam fallback.
	// bUsingEpicBeam latches which path is live; EpicBeamComp/EpicBeamMID are the per-fixture canvas
	// component + MID; EpicBeamLocalFwd is the mesh-local length axis aligned to the emission dir.
	UPROPERTY() TObjectPtr<UStaticMesh> EpicBeamMesh = nullptr;
	UPROPERTY() TObjectPtr<UMaterialInterface> EpicBeamMaterial = nullptr;
	UPROPERTY() TObjectPtr<class UStaticMeshComponent> EpicBeamComp = nullptr;
	UPROPERTY() TObjectPtr<class UMaterialInstanceDynamic> EpicBeamMID = nullptr;
	bool bUsingEpicBeam = false;
	// Last canvas emission axis we logged the Epic-beam alignment proof for (throttles the per-tick
	// dot/apex log to meaningful aim changes). SM_Beam_RM's local emission axis is a fixed -Z.
	FVector EpicLastLoggedFwd = FVector::ZeroVector;

	// Cone-beam geometry/state. BeamConeRest is the rest transform (mesh +Z -> beam forward, at the
	// beam origin) composed with the head motion each RefreshMotion. LastFarRadius lets zoom ticks
	// skip a rebuild when the far radius is ~unchanged.
	FTransform BeamConeRest = FTransform::Identity;
	// Last beam forward (= SpotLight world emission) we emitted an alignment log for; throttles the
	// per-update beam-align proof to meaningful aim changes (dot < 0.999) instead of every tick.
	FVector LastLoggedBeamFwd = FVector::ZeroVector;
	float BeamBaseRadiusUnreal = 2.f;    // cone base radius (lens radius), UE cm
	float BeamLengthUnreal = 6000.f;     // cone length (= SpotLight AttenuationRadius), UE cm
	float BeamConeLastFarRadius = -1.f;  // last-built far radius (rebuild gate), UE cm

	// bMeshBeams runtime toggle (default true = mesh beam on, fog scattering suppressed). When the
	// portal pushes bMeshBeams=false the cone hides and FogScatteringIntensity is restored on the
	// SpotLight. MeshBeamUserScale is the SetFixtureBeamVolumetrics intensity multiplier on the
	// mesh BeamIntensity; bWantsVolumetricShadow is the parsed castVolumetricShadow flag that (Phase
	// 2) drives the native VSM fog volumetric-shadow hybrid for hero beams (RefreshBeamShadowMode).
	bool bMeshBeamEnabled = true;
	float MeshBeamUserScale = 1.f;
	float FogScatteringIntensity = 2.5f;
	bool bWantsVolumetricShadow = false;

	// v1.0.87 InternalBeam A/B mode state. bInternalBeamEnabled gates the whole pose change so
	// RefreshBeamShadowMode / RefreshMotion / ApplyZoom can branch without re-deriving it. The
	// cached fields below are the byte-exact pre-toggle state used by RestoreInternalBeamPose --
	// they are NOT touched again after InternalBeam goes ON, so a second OFF -> ON -> OFF cycle
	// still lands the fixture on the same construction-time values.
	//
	// MaxZoomFullDeg defaults to a SAFE 45 degrees (a common stage moving-head MAX zoom). When
	// Profile.Zoom.bValid and Profile.Zoom.MaxDeg > 0, ApplyInternalBeamPose uses the profile's
	// value verbatim -- so an MVR / GDTF push with a real zoom range always wins. The 45-deg
	// fallback only fires for profile-less data-channel pushes (which is rare in production).
	UPROPERTY()
	float InternalBeamMaxZoomFullDeg = 45.f;
	bool bInternalBeamEnabled = false;
	// The back-offset is NOT applied by mutating BeamRestTransform: that would lock the offset
	// to the fixture-local rest direction in the no-rig synthetic-pan/tilt fallback (where the
	// spotlight aim follows a runtime Dir, not BeamForwardLocal). Instead RefreshMotion applies
	// the offset as a POST-STEP after SetRelativeTransform: AddRelativeLocation along the
	// spotlight's CURRENT FixtureRoot-space forward (read off the relative rotation), so the
	// rig path AND the synthetic path both push the spotlight back along its live aim. The
	// "byte-exact restore" guarantee then comes for free -- the next RefreshMotion after the
	// flag flips back to false re-sets the SpotLight relative transform from BeamRestTransform
	// * Head (the construction-time recipe), so no separate cached location is needed.
	// Cached SpotLight volumetric state from construction (so the InternalBeam push doesn't
	// permanently override a non-default scatter that the portal pushed via the hero-shadow path).
	float InternalBeamSpotVolScatterOrig = 0.f;
	bool bInternalBeamSpotCastVolShadowOrig = false;
	// Cached Epic beam / procedural cone visibility flags so toggling OFF returns whichever was
	// the visible shaft pre-InternalBeam (the bMeshBeamEnabled wire path drives both visibilities).
	bool bInternalBeamEpicCompVisOrig = true;
	bool bInternalBeamConeVisOrig = true;
	bool bInternalBeamPoseCached = false;
	// v1.0.92 -- cached per-fixture `bAllowMegaLights` from BEFORE the InternalBeam push so the
	// OFF transition restores it byte-exact. v1.0.92 forces `bAllowMegaLights = 0` on every
	// fixture in InternalBeam mode (regardless of gobo-active state) so the SpotLight's
	// `IESTexture` AND `LightFunctionMaterial` BOTH modulate the volumetric beam shaft. UE 5.7
	// MegaLights bypasses both the IES + light-function paths in the volumetric integrator (the
	// many-lights-per-pixel sampling path doesn't sample either), so a MegaLight spotlight has
	// a uniformly-bright cone in fog regardless of its IES profile or its cookie. Routing the
	// SpotLight through the legacy clustered/deferred path on InternalBeam ON means it loses
	// MegaLights' clustering perf for the duration -- documented perf trade-off (README v1.0.92).
	// The opt-out is gated by the `Rebus.InternalBeamForceLegacy` CVar (default 1); when 0 the
	// legacy MegaLights value is left untouched so deployments that don't need volumetric LF/IES
	// on the shaft can keep MegaLights' efficiency. `bInternalBeamMegaLightsOrigCached` latches
	// the snapshot so a 0->1->0->1 toggle of `Rebus.InternalBeamForceLegacy` (re-entrant push)
	// can't overwrite the cached value with the value we just installed (mirrors the
	// PushLightFunctionAtlasForInternalBeam idempotency latch on the subsystem).
	bool bInternalBeamAllowMegaLightsOrig = true;
	bool bInternalBeamMegaLightsOrigCached = false;

	// v1.0.93 -- cached per-fixture SpotLight LightFunctionMaterial pointer captured the
	// FIRST time the cookie-cone path installs `MID_RebusGoboLightFunction` so the OFF edge
	// can byte-exactly restore whatever the SpotLight had as its light function before
	// InternalBeam took over (typically null, or `MID_GoboLightFn` from the v1.0.49 cookie
	// path if a gobo went active before InternalBeam was toggled on). Separate latch from the
	// v1.0.92 `bInternalBeamMegaLightsOrigCached` so the operator can flip
	// `Rebus.InternalBeamCookieCone` independently of `Rebus.InternalBeamForceLegacy` without
	// the two caches stomping each other. WeakObjectPtr is enough -- if the cached MID went
	// dead before restore (e.g. operator hot-reloaded the cookie pipeline), the restore is
	// a null assignment which is still a defensible end state ("no light function").
	TWeakObjectPtr<class UMaterialInterface> InternalBeamPriorLightFunction;
	bool bInternalBeamPriorLightFunctionCached = false;

	// Per-primitive shadow cache captured the first time InternalBeam is enabled on this actor.
	// Each entry holds a weak ref + the original CastShadow flag set so RestoreInternalBeamPose
	// can push them back byte-exact. Weak refs tolerate component destruction (rebuild between
	// ON / OFF), in which case the restore is a no-op for that entry rather than a crash.
	struct FInternalBeamShadowEntry
	{
		TWeakObjectPtr<UPrimitiveComponent> Comp;
		uint8 bCastShadow : 1;
		uint8 bCastDynamicShadow : 1;
		uint8 bCastHiddenShadow : 1;
		uint8 bCastShadowAsTwoSided : 1;
	};
	TArray<FInternalBeamShadowEntry> InternalBeamShadowCache;

	// Apply the InternalBeam pose: hide Epic / cone, push the SpotLight back by the computed
	// back-offset, force volumetrics + volumetric shadow on, opt the body meshes out of shadow
	// casting. Snapshots the pre-toggle state into the InternalBeam* fields above on first call.
	void ApplyInternalBeamPose();
	// Reverse of ApplyInternalBeamPose: restore the cached SpotLight pose / volumetrics, the
	// Epic-beam / cone visibility flags, and every cached primitive's CastShadow flags.
	void RestoreInternalBeamPose();
	// Compute the spotlight back-offset (UE cm) so the cone at the lens plane is the lens
	// diameter at MaxZoom: offset = lensRadius / tan(MaxZoomHalfAngleRadians). Clamped >= 0.
	float ComputeInternalBeamBackOffsetCm() const;
	// When bRestoreOriginal=false, walk this actor's UPrimitiveComponents (skipping the SpotLight,
	// the Epic beam canvas, the procedural cone, the lens disc and any other beam-functional comp)
	// and force CastShadow + bCastDynamicShadow + bCastHiddenShadow + bCastShadowAsTwoSided to
	// false, caching the original flags. When bRestoreOriginal=true, push the cached flags back.
	//
	// v1.0.89: the walker INCLUDES every component tagged `RebusIsBeamLens` (the v1.0.88 real
	// `<Beam>` lens meshes) -- they are body geometry as far as the SpotLight is concerned, and
	// must not shadow it. The synthetic LensDisc is still skipped (it is an emissive disc,
	// already non-shadowing). The per-beam emissive flares (`RebusIsBeamFlare`) are also already
	// non-shadowing by construction (BuildIsBeamLensFlares hard-sets CastShadow=false).
	// v1.0.93: the walker ALSO skips the new `InternalBeamShaft` cone-mesh component (it
	// already has CastShadow=false by construction; including it in the cache + restore loop
	// would corrupt the cache for the OFF restore).
	void SetBodyMeshesCastShadow(bool bRestoreOriginal);

	// v1.0.89: opt a single freshly-created primitive out of shadow casting AND cache its
	// original flags into the InternalBeamShadowCache so RestoreInternalBeamPose's restore-on-
	// disable still pushes byte-exact. Defensive entry-point for BuildMeshes when a mesh-bundle
	// lands AFTER `SetInternalBeamModeEnabled(true)` has already walked the actor (e.g. a
	// delayed `/meshes` push -- rare, but the v1.0.88 isBeam lens components arrive in BuildMeshes
	// and would otherwise default to CastShadow=true and shadow the spotlight). No-op when not
	// in InternalBeam mode, when the comp is null, or when the comp is one of the beam-functional
	// primitives that the walker normally skips.
	void OptPrimitiveOutOfInternalBeamShadow(class UPrimitiveComponent* Comp);
	// True once this fixture has been granted a hero volumetric-shadow slot (under the per-batch
	// RebusMaxShadowFogBeams budget). Latched so toggling shadow on/off doesn't re-consume budget.
	bool bGrantedShadowHero = false;

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

	// ---- Orbit-imported model binding state ----
	// When true, RefreshMotion also drives the bound Orbit components from the head motion (the
	// SAME cumulative pan/tilt solve that drives the control-channel head meshes -- not a parallel
	// recomputation -- so the Orbit mesh tracks the control mesh pose-for-pose). v1.0.65: default
	// flipped from false to true so a freshly-spawned fixture starts driving on the first frame
	// it gets bound by RebindOrbitModels, instead of waiting up to 1 s for the periodic rebind to
	// also call SetDriveOrbitModel(true) on it.
	bool bDriveOrbitModel = true;
	// Object id (Speckle node id) this fixture is bound to on the Orbit-import side.
	FString BoundOrbitObjectId;
	// The matched Orbit-imported components (weak so a re-import that destroys them is tolerated).
	TArray<TWeakObjectPtr<USceneComponent>> OrbitComponents;
	// Each bound component's imported (rest) world transform; restored when driving is disabled.
	TArray<FTransform> OrbitCompRestWorld;
	// Per-component constant: CompRestWorld * ActorWorld^-1 (captured at bind time when every
	// axis' RestCumulative is identity, so AxisWorldRest == ActorWorld for ALL axes -- one
	// formula serves every bucket). Driven world transform per component:
	//   NewWorld = OrbitBindBase[i] * (Cumulative[OrbitAxisBucket[i]] * ActorWorld)
	// which simplifies (Cumulative=Identity at rest) to CompRest -- so an axis the user isn't
	// driving still leaves the component on its imported pose.
	TArray<FTransform> OrbitBindBase;
	// v1.0.68 introduced, v1.0.69 ID-only: per-component motion-axis bucket, parallel to
	// OrbitComponents. INDEX_NONE means "base" (never moves); otherwise an index into
	// Profile.MotionRig.Axes that drives the component. Classified by BindOrbitComponents using
	// an ID/name strategy chain: tag-name match against AffectedGeometryNames -> comp-name match
	// -> keyword scan ('head'/'tilt' -> tilt, 'yoke'/'arm'/'pan' -> pan, 'base'/'body' ->
	// static) -> attach-hierarchy depth. Components for which NO strategy fires bucket to
	// INDEX_NONE (static rest) -- v1.0.69 removed v1.0.68's position fallback + default-head
	// safety net because the position heuristic put every mesh on the deepest (tilt) axis for
	// the user's GLBs (every component was geometrically near the head, so nearest-pivot ties
	// went to tilt), fully defeating the per-part split. The classifier now logs a per-fixture
	// warning naming exactly which conventions the importer can adopt to enable motion.
	TArray<int32> OrbitAxisBucket;
	// Head world transform at the rest (pan=tilt=0) pose, captured at bind time.
	FTransform OrbitHeadWorldRest = FTransform::Identity;
	// Last pan/tilt we emitted a drive-sync log for (throttles the per-update sync log).
	FVector2D LastOrbitLogPanTilt = FVector2D(FLT_MAX, FLT_MAX);

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

	// v1.0.91 -- peak candela parsed from the active IES file (FIESConverter::GetBrightness() *
	// GetMultiplier(), via RebusIes::BuildLightProfile's OutCandelaMax). Drives
	// SpotLight->Intensity when >= 0 (with units = Candelas, see BuildSpotLight): the formula
	// is `IesCandelaMax * Dimmer * Gate` -- so the .ies file's candela max is the BASE
	// intensity, and the live operator dimmer + shutter-gate are linear multipliers on top.
	// Sentinel < 0 (no IES, or IES build failed) falls back to the flux-derived BaseCandela.
	// Reset to -1 by the clear path in SelectIesForZoom (no inline + no URL -> synthetic cone),
	// re-captured on every successful BuildLightProfile (zoom-keyed selection refresh too).
	// `ActiveIesProfileId` is informational (the inline profileId or "url:<zoomDmx>" for the
	// URL path), surfaced by `Rebus.DumpFixtureIes` so the operator can see WHICH .ies file
	// is currently driving the SpotLight without re-reading the inline cache.
	float   IesCandelaMax = -1.f;
	FString ActiveIesProfileId;

	bool bAnimating = false;
};
