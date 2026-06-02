// Copyright REBUS Industries.
//
// Coordinate-system conversions between the portal's wire conventions and Unreal.
//
// See ue-plugin-build-guide.md §4.1 / §7.2 / §7.3. The two source spaces that matter:
//
//   * RH Z-up metres (column-major) -- fixture *placement* matrices from /api/ue/scene
//     ("matrixZUpMeters"). When matrixSource == "transform-row" the matrix is the Speckle
//     row-major fallback and must be transposed first.
//
//   * RH Y-up metres (fixture-local) -- mesh vertices, FixturePart.worldMatrixMeters,
//     beam aim vectors, AND the motionRig pivot / axis / pivotOffset vectors. This is the
//     single most common mistake: motion-rig vectors are Y-up, NOT Z-up.
//
// Unreal is LH Z-up, centimetres. We scale metres -> centimetres with METERS_TO_UNREAL.
//
// The viewer's intermediate Y-up -> Z-up step (verbatim from the guide §7.2):
//     yUpFixturePointToZUpLocal(p)     = { x: p.x, y: -p.z, z: p.y }
//     yUpFixtureDirectionToZUpLocal(v) = normalize({ x: v.x, y: -v.z, z: v.y })
// We fold that into the Y-up helpers below so callers never double-convert.
#pragma once

#include "CoreMinimal.h"

namespace RebusCoords
{
	static constexpr double METERS_TO_UNREAL = 100.0;

	// ---- RH Z-up metres (placement) -> Unreal LH Z-up centimetres ----------------------

	// Point: negate Y for RH->LH handedness, scale to centimetres.
	FORCEINLINE FVector PointZUpMetersToUnreal(const FVector& P)
	{
		return FVector(P.X, -P.Y, P.Z) * METERS_TO_UNREAL;
	}

	// Direction: handedness flip only (no scale), renormalised by the caller if needed.
	FORCEINLINE FVector DirectionZUpToUnreal(const FVector& V)
	{
		return FVector(V.X, -V.Y, V.Z);
	}

	// ---- RH Y-up metres (fixture-local) -> Unreal LH Z-up centimetres ------------------
	//
	// Y-up -> Z-up (RH): (x, y, z) -> (x, -z, y).  Then Z-up(RH) -> Unreal: negate Y, scale.
	// Composing: (x, -z, y) --negate Y--> (x, z, y), so the net point map is (x, z, y)*100.
	FORCEINLINE FVector PointYUpMetersToUnreal(const FVector& P)
	{
		return FVector(P.X, P.Z, P.Y) * METERS_TO_UNREAL;
	}

	FORCEINLINE FVector DirectionYUpToUnreal(const FVector& V)
	{
		const FVector D(V.X, V.Z, V.Y);
		return D.GetSafeNormal();
	}

	// Y-up -> RH Z-up (no Unreal handedness flip / no scale). Used internally by the motion
	// math when it wants to stay in the viewer's Z-up intermediate space.
	FORCEINLINE FVector PointYUpToZUp(const FVector& P)
	{
		return FVector(P.X, -P.Z, P.Y);
	}

	FORCEINLINE FVector DirectionYUpToZUp(const FVector& V)
	{
		return FVector(V.X, -V.Z, V.Y).GetSafeNormal();
	}

	// ---- Matrix conversions ------------------------------------------------------------

	// Convert a 16-element matrix into an Unreal FMatrix.
	//
	//  Data layout: by default the 16 doubles are column-major (column c, row r at index
	//  c*4 + r), which is the matrixZUpMeters / glTF convention. Pass bRowMajor = true for
	//  the Speckle "transform-row" fallback (matrixSource == "transform-row").
	//
	//  bYUp selects the source basis: false = RH Z-up metres (placement), true = RH Y-up
	//  metres (FixturePart.worldMatrixMeters / beam node).
	FMatrix MatrixToUnreal(const double M[16], bool bRowMajor, bool bYUp);

	// Convenience: read 16 numbers (already ordered as the wire delivered them) into the
	// double array. Returns false if Source does not have exactly 16 finite numbers.
	bool ReadMatrix16(const TArray<double>& Source, double OutM[16]);
}
