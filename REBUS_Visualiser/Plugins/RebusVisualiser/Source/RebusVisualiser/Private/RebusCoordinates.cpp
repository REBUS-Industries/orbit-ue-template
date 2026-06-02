// Copyright REBUS Industries.
#include "RebusCoordinates.h"

namespace RebusCoords
{
	bool ReadMatrix16(const TArray<double>& Source, double OutM[16])
	{
		if (Source.Num() != 16)
		{
			return false;
		}
		for (int32 i = 0; i < 16; ++i)
		{
			if (!FMath::IsFinite(Source[i]))
			{
				return false;
			}
			OutM[i] = Source[i];
		}
		return true;
	}

	FMatrix MatrixToUnreal(const double M[16], bool bRowMajor, bool bYUp)
	{
		// Pull out the four columns as (basis x, basis y, basis z, translation) in the source
		// space. Column-major index = col*4 + row; row-major index = row*4 + col.
		auto Elem = [&](int32 Row, int32 Col) -> double
		{
			return bRowMajor ? M[Row * 4 + Col] : M[Col * 4 + Row];
		};

		const FVector C0(Elem(0, 0), Elem(1, 0), Elem(2, 0)); // image of source +X
		const FVector C1(Elem(0, 1), Elem(1, 1), Elem(2, 1)); // image of source +Y
		const FVector C2(Elem(0, 2), Elem(1, 2), Elem(2, 2)); // image of source +Z
		const FVector T (Elem(0, 3), Elem(1, 3), Elem(2, 3)); // translation (metres)

		FVector X, Y, Z, Origin;
		if (bYUp)
		{
			// Basis vectors carry scale, so convert without renormalising.
			X = FVector(C0.X, C0.Z, C0.Y);
			Y = FVector(C1.X, C1.Z, C1.Y);
			Z = FVector(C2.X, C2.Z, C2.Y);
			Origin = PointYUpMetersToUnreal(T);
		}
		else
		{
			X = FVector(C0.X, -C0.Y, C0.Z);
			Y = FVector(C1.X, -C1.Y, C1.Z);
			Z = FVector(C2.X, -C2.Y, C2.Z);
			Origin = PointZUpMetersToUnreal(T);
		}

		// FMatrix(X, Y, Z, Origin) treats each vector as the image of the corresponding unit
		// axis (rows of the matrix). The handedness flip above already makes the basis
		// left-handed, so this is a proper Unreal transform.
		return FMatrix(
			FPlane(X.X, X.Y, X.Z, 0.0),
			FPlane(Y.X, Y.Y, Y.Z, 0.0),
			FPlane(Z.X, Z.Y, Z.Z, 0.0),
			FPlane(Origin.X, Origin.Y, Origin.Z, 1.0));
	}
}
