// Copyright REBUS Industries.
//
// Motion-rig parity solver (ue-plugin-build-guide.md §7). Reproduces the Speckle/Orbit
// viewer's pan/tilt math so UE lights move EXACTLY like the 3D viewer.
//
// Strategy: all geometry is baked into fixture-local Unreal space and parented under a
// fixture-root component that already carries the instance placement (matrixZUpMeters).
// Because of that parenting, applying a fixture-local affine to a mesh group automatically
// performs the "conjugate through the instance world matrix" step (§7.4 step 6) for free --
// we never have to do the L*R*L^-1 conjugation by hand.
//
// For each motion axis we build an affine "rotate `angle` about `axisDir` through `pivot`"
// (all in fixture-local Unreal cm) and compose them parent-first. A mesh tagged with a
// given axis gets that axis's cumulative transform; the beam/head inherits the deepest axis.
#pragma once

#include "CoreMinimal.h"
#include "RebusSceneTypes.h"

namespace RebusMotion
{
	// Resolve all per-axis cumulative fixture-local transforms for the given pan/tilt.
	// OutCumulative is index-aligned with Rig.Axes (already sorted parent-first). Identity
	// for the implicit static base. PanDeg/TiltDeg are the inbound control values (degrees).
	void Solve(const FRebusMotionRig& Rig, double PanDeg, double TiltDeg, TArray<FTransform>& OutCumulative);

	// The deepest axis index (the one the head/beam should track), or INDEX_NONE if static.
	int32 DeepestAxisIndex(const FRebusMotionRig& Rig);

	// Which axis (index into Rig.Axes) a mesh belongs to, by matching its names against each
	// axis's affectedGeometryNames. Returns the DEEPEST matching axis, or INDEX_NONE (= base/
	// static) when nothing matches. Names are compared case-insensitively.
	int32 ResolveAxisForMesh(const FRebusMotionRig& Rig, const FString& GeometryName, const FString& ModelName);
}
