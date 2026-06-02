// Copyright REBUS Industries.
#include "RebusMotionSolver.h"
#include "RebusCoordinates.h"

namespace RebusMotion
{
	// Build an affine that rotates `Quat` about `PivotUnreal`: T(p) . R . T(-p).
	static FTransform RotateAboutPivot(const FQuat& Quat, const FVector& PivotUnreal)
	{
		FTransform Affine;
		Affine.SetRotation(Quat);
		Affine.SetTranslation(PivotUnreal - Quat.RotateVector(PivotUnreal));
		Affine.SetScale3D(FVector::OneVector);
		return Affine;
	}

	static double ValueForKind(ERebusAxisKind Kind, double PanDeg, double TiltDeg, double DefaultDeg)
	{
		switch (Kind)
		{
		case ERebusAxisKind::Pan:  return PanDeg;
		case ERebusAxisKind::Tilt: return TiltDeg;
		default:                   return DefaultDeg;
		}
	}

	void Solve(const FRebusMotionRig& Rig, double PanDeg, double TiltDeg, TArray<FTransform>& OutCumulative)
	{
		const int32 Num = Rig.Axes.Num();
		OutCumulative.SetNum(Num);

		for (int32 i = 0; i < Num; ++i)
		{
			const FRebusMotionAxis& Axis = Rig.Axes[i];

			// (1) Resolve + clamp the angle for this axis (§7.4 step 2).
			const double Raw = ValueForKind(Axis.Kind, PanDeg, TiltDeg, Axis.DefaultDeg);
			const double AngleDeg = FMath::Clamp(Raw, Axis.MinDeg, Axis.MaxDeg);

			// (2) Pivot into fixture-local space: subtract pivotOffset (Y-up) first (§7.4 step 3).
			FVector PivotYUp = Axis.Pivot;
			if (Rig.bHasPivotOffset)
			{
				PivotYUp -= Rig.PivotOffset;
			}
			const FVector PivotUnreal = RebusCoords::PointYUpMetersToUnreal(PivotYUp);

			// (3) Axis direction with the +90 tilt-under-pan compensation (§7.4 step 4),
			//     performed in Y-up using the parent pan axis BEFORE the Unreal conversion.
			FVector BaseDirYUp = Axis.Axis.GetSafeNormal();
			if (Axis.Kind == ERebusAxisKind::Tilt && Axis.ParentAxisIndex != INDEX_NONE)
			{
				const FRebusMotionAxis& Parent = Rig.Axes[Axis.ParentAxisIndex];
				if (Parent.Kind == ERebusAxisKind::Pan)
				{
					const FVector ParentAxisYUp = Parent.Axis.GetSafeNormal();
					const FQuat Comp(ParentAxisYUp, HALF_PI); // +90 about parent pan axis
					BaseDirYUp = Comp.RotateVector(BaseDirYUp).GetSafeNormal();
				}
			}
			const FVector AxisDirUnreal = RebusCoords::DirectionYUpToUnreal(BaseDirYUp);

			// (4) This axis's own affine, and compose parent-first (§7.4 step 5):
			//     Cum[child] = childAffine * Cum[parent]  (UE applies the left operand first).
			const FQuat Quat(AxisDirUnreal, FMath::DegreesToRadians(AngleDeg));
			const FTransform Self = RotateAboutPivot(Quat, PivotUnreal);

			if (Axis.ParentAxisIndex != INDEX_NONE && Rig.Axes.IsValidIndex(Axis.ParentAxisIndex))
			{
				OutCumulative[i] = Self * OutCumulative[Axis.ParentAxisIndex];
			}
			else
			{
				OutCumulative[i] = Self;
			}
		}
	}

	int32 DeepestAxisIndex(const FRebusMotionRig& Rig)
	{
		// Axes are sorted parent-first, so the deepest descendant is the one with the longest
		// ancestor chain. Compute depth per axis and return the max.
		int32 Best = INDEX_NONE;
		int32 BestDepth = -1;
		for (int32 i = 0; i < Rig.Axes.Num(); ++i)
		{
			int32 Depth = 0;
			int32 P = Rig.Axes[i].ParentAxisIndex;
			while (P != INDEX_NONE && Rig.Axes.IsValidIndex(P))
			{
				++Depth;
				P = Rig.Axes[P].ParentAxisIndex;
			}
			if (Depth > BestDepth)
			{
				BestDepth = Depth;
				Best = i;
			}
		}
		return Best;
	}

	int32 ResolveAxisForMesh(const FRebusMotionRig& Rig, const FString& GeometryName, const FString& ModelName)
	{
		const FString G = GeometryName.ToLower();
		const FString M = ModelName.ToLower();

		int32 Best = INDEX_NONE;
		int32 BestDepth = -1;
		for (int32 i = 0; i < Rig.Axes.Num(); ++i)
		{
			const FRebusMotionAxis& Axis = Rig.Axes[i];
			const bool bMatch = (!G.IsEmpty() && Axis.AffectedGeometryNames.Contains(G))
				|| (!M.IsEmpty() && Axis.AffectedGeometryNames.Contains(M));
			if (!bMatch) continue;

			int32 Depth = 0;
			int32 P = Axis.ParentAxisIndex;
			while (P != INDEX_NONE && Rig.Axes.IsValidIndex(P)) { ++Depth; P = Rig.Axes[P].ParentAxisIndex; }
			if (Depth > BestDepth)
			{
				BestDepth = Depth;
				Best = i;
			}
		}
		return Best;
	}
}
