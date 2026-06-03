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
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ImageUtils.h"
#include "UObject/ConstructorHelpers.h"

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

void ARebusFixtureActor::ResetVolumetricShadowBudget()
{
	VolumetricShadowBeamCount = 0;
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

	// Beam-visible default scattering for haze (§8.4). The fixture scatters into the level's
	// volumetric height fog regardless of the global MegaLights mode.
	SpotLight->SetVolumetricScatteringIntensity(2.5f);

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
		}
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
		SpotLight->SetRelativeTransform(BeamRestTransform * Head);

		// Lens disc rides the SAME head transform (LensDiscRest * Head), so it stays co-located
		// with the beam origin and perpendicular to the v1.0.21 beam direction through pan/tilt.
		if (LensDisc)
		{
			LensDisc->SetRelativeTransform(LensDiscRest * Head);
		}
	}
}

void ARebusFixtureActor::RecomputeConeAngles()
{
	if (!SpotLight) return;

	float OuterHalf = ZoomDeg.Current;

	// Clamp to the fixture's zoom range (half-angles) when known (§8.1).
	if (Profile.Zoom.bValid)
	{
		OuterHalf = FMath::Clamp(OuterHalf, (float)(Profile.Zoom.MinDeg * 0.5), (float)(Profile.Zoom.MaxDeg * 0.5));
	}
	OuterHalf = FMath::Clamp(OuterHalf, 0.5f, 80.f);

	// Iris pinches the outer cone (iris 1 = open).
	const float IrisScale = FMath::Lerp(0.4f, 1.f, FMath::Clamp(Iris.Current, 0.f, 1.f));
	OuterHalf *= IrisScale;

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

void ARebusFixtureActor::ApplyGoboRotation(float Speed)
{
	GoboRotationSpeed = FMath::Clamp(Speed, -1.f, 1.f);
	if (!FMath::IsNearlyZero(GoboRotationSpeed)) bAnimating = true;
}

void ARebusFixtureActor::ApplyPrism(int32 Facets, float RotationDeg)
{
	// Stored + logged; visual deferred on the reference plugin (§5.2).
	UE_LOG(LogRebusVisualiser, Verbose, TEXT("Fixture %s prism facets=%d rot=%.1f"), *FixtureId, Facets, RotationDeg);
}

void ARebusFixtureActor::ApplyBeamVolumetrics(float Intensity, bool bCastVolumetricShadow)
{
	if (!SpotLight) return;
	SpotLight->SetVolumetricScatteringIntensity(FMath::Clamp(Intensity, 0.f, 10.f));
	SpotLight->bCastVolumetricShadow = bCastVolumetricShadow;
	SpotLight->MarkRenderStateDirty();
}

void ARebusFixtureActor::ApplyGobo(int32 GoboIndex, bool bHasIndex, int32 WheelIndex, const FString& Wheel, float /*FadeSeconds*/)
{
	// Discrete: switch the slot immediately (a wheel slot can't be half-selected, §11).
	if (!bHasIndex)
	{
		CurrentGoboIndex = INDEX_NONE;
		CurrentGoboWheelIndex = WheelIndex;
		CurrentGoboWheel = Wheel;
		if (SpotLight) SpotLight->SetLightFunctionMaterial(nullptr);
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
	if (!SpotLight || Bytes.Num() == 0) return false;

	// Decode the bytes to a transient UTexture2D (auto-detects PNG/JPEG/etc) and feed it into
	// the light-function MID -- the SAME assignment path the URL fetch uses. Projecting it as a
	// true light-function needs a light-function material in content (M_RebusGobo with a
	// Texture2D param); see README "Gobo decode" for the wiring.
	UTexture2D* Tex = FImageUtils::ImportBufferAsTexture2D(Bytes);
	if (Tex && GoboMID)
	{
		GoboMID->SetTextureParameterValue(TEXT("GoboTexture"), Tex);
		SpotLight->SetLightFunctionMaterial(GoboMID);
		return true;
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
		if (Inline->Bytes.Num() > 0 && ApplyGoboTextureFromBytes(Inline->Bytes))
		{
			UE_LOG(LogRebusVisualiser, Verbose,
				TEXT("Fixture %s gobo assigned (source=inline, reqWheelIdx=%d -> wheelIndex=%d wheel='%s', slot=%d, %d bytes)."),
				*FixtureId, WheelIndex, Inline->WheelIndex, *Inline->Wheel, Inline->Slot, Inline->Bytes.Num());
			return;
		}
		// 2) Inline entry carries only a signed url fallback (or its bytes failed to decode).
		if (!Inline->ImageUrl.IsEmpty())
		{
			UE_LOG(LogRebusVisualiser, Verbose,
				TEXT("Fixture %s gobo assigned (source=inline-url, reqWheelIdx=%d -> wheelIndex=%d wheel='%s', slot=%d)."),
				*FixtureId, WheelIndex, Inline->WheelIndex, *Inline->Wheel, Inline->Slot);
			FetchAndAssignGoboFromUrl(Inline->ImageUrl);
			return;
		}
	}

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
		UE_LOG(LogRebusVisualiser, Verbose, TEXT("Fixture %s gobo: no inline image and no profile wheel (source=none)."), *FixtureId);
		return;
	}
	const FRebusWheel& Wheel = Profile.Wheels[WheelIdx];
	if (!Wheel.Slots.IsValidIndex(GoboIndex) || Wheel.Slots[GoboIndex].ImageUrl.IsEmpty())
	{
		SpotLight->SetLightFunctionMaterial(nullptr); // no media for this slot = clear
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
			UE_LOG(LogRebusVisualiser, Verbose, TEXT("Fixture %s gobo image fetched (%d bytes, applied=%d)."),
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

	// Gobo spin (visual only when a light-function material is present).
	if (!FMath::IsNearlyZero(GoboRotationSpeed))
	{
		GoboAngle += DeltaSeconds * GoboRotationSpeed * 360.f;
		bStillAnimating = true;
	}

	bAnimating = bStillAnimating;
}
