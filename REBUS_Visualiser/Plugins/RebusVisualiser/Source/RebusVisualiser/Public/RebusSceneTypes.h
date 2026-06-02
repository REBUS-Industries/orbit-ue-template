// Copyright REBUS Industries.
//
// Parsed representations of the portal's REST payloads:
//   * /api/ue/scene                      -> FRebusScene + FRebusSceneFixture[]
//   * /api/ue/fixtures/{libraryId}       -> FRebusFixtureProfile (schema rebus-ue-fixture/v5)
//   * /api/ue/fixtures/{id}/meshes       -> FRebusMeshBundle
//
// All field names mirror the wire schema (ue-plugin-build-guide.md §4). Parsing is by field
// name and additive: unknown fields are ignored, missing optionals stay unset.
#pragma once

#include "CoreMinimal.h"

// ---- Scene (/api/ue/scene) -------------------------------------------------------------

enum class ERebusMatrixSource : uint8
{
	ZUpColumn,    // "zup-column"   -> RH Z-up metres, column-major (default)
	TransformRow, // "transform-row"-> Speckle row-major fallback, transpose first
	Unknown       // null / absent
};

struct FRebusSceneFixture
{
	FString Id;                 // Speckle node id = FixtureId (the control key)
	FString Name;
	FString LibraryFixtureId;   // "fixtureId" -> per-fixture profile route param (may be empty)
	FString GdtfKey;
	FString Mode;
	FString UnitNumber;
	FString Universe;
	FString Address;
	FString FixtureType;
	FString Layer;

	bool bHasMatrix = false;
	double Matrix[16] = { 0 };  // matrixZUpMeters, as delivered
	ERebusMatrixSource MatrixSource = ERebusMatrixSource::Unknown;
};

struct FRebusScene
{
	FString Schema;
	FString ProjectId;
	FString SpeckleProjectId;
	FString ModelId;
	FString VersionId;
	FString Units;
	TArray<FRebusSceneFixture> Fixtures;
	TArray<FString> ProfileIds;
	bool bTruncated = false;
};

// ---- Fixture profile (/api/ue/fixtures/{libraryId}) ------------------------------------

struct FRebusFixturePart
{
	FString Name;
	FString GeometryName;
	FString ModelName;

	bool bHasLocalMatrix = false;
	double LocalMatrix[16] = { 0 };       // RH Z-up, relative to parent

	bool bHasWorldMatrixMeters = false;
	double WorldMatrixMeters[16] = { 0 }; // engine Y-up metres

	bool bIsBeam = false;                 // GDTF <Beam> node
	bool bHasBeamDirection = false;
	FVector BeamDirectionWorld = FVector::ForwardVector; // engine Y-up, unit
	bool bHasBeamUp = false;
	FVector BeamUpWorld = FVector::UpVector;             // engine Y-up, unit

	int32 ParentIndex = INDEX_NONE;       // resolved during parse if the tree is flattened
	TArray<FRebusFixturePart> Children;   // nested form
};

enum class ERebusAxisKind : uint8
{
	Pan,
	Tilt,
	Other
};

struct FRebusMotionAxis
{
	ERebusAxisKind Kind = ERebusAxisKind::Other;
	FVector Pivot = FVector::ZeroVector;     // RH Y-up metres, fixture-local
	FVector Axis = FVector::UpVector;        // RH Y-up, direction
	double MinDeg = -360.0;
	double MaxDeg = 360.0;
	double DefaultDeg = 0.0;
	TArray<FString> AffectedGeometryNames;   // normalised lower-case
	int32 ParentAxisIndex = INDEX_NONE;      // resolved parent-first ordering
	FString NodeName;
	FString ParentNodeName;
};

struct FRebusMotionRig
{
	bool bValid = false;
	bool bHasPivotOffset = false;
	FVector PivotOffset = FVector::ZeroVector; // RH Y-up metres
	TArray<FRebusMotionAxis> Axes;             // topologically sorted parent-first after Parse
};

struct FRebusPhotometrics
{
	TOptional<double> LuminousFlux;     // lumens
	TOptional<double> BeamAngle;        // GDTF FULL angle, degrees
	TOptional<double> FieldAngle;       // GDTF FULL angle, degrees
	TOptional<double> ColorTemperature; // Kelvin
	TOptional<double> Cri;
	bool bHasIesProfile = false;
};

struct FRebusZoomRange
{
	bool bValid = false;
	double MinDeg = 0.0; // FULL angles
	double MaxDeg = 0.0;
};

struct FRebusSource
{
	TOptional<double> RadiusMeters;
	TOptional<double> DiameterMeters;
};

struct FRebusWheelSlot
{
	FString Name;
	FString Color;     // raw CIE xyY string, or empty
	FString ImageUrl;  // relative redirect, or empty
};

struct FRebusWheel
{
	FString Name;
	FString Kind;      // "color" | "gobo" | "effect" | "wheel" | ""
	TArray<FRebusWheelSlot> Slots;
};

struct FRebusIesProfileRef
{
	int32 ZoomDmx = 0;          // 0..255
	double ZoomAngleDeg = 0.0;
	double BeamAngleDeg = 0.0;
	double FieldAngleDeg = 0.0;
	FString IesUrl;
};

struct FRebusFixtureProfile
{
	bool bValid = false;
	FString Schema;
	FString Id;                 // libraryFixtureId
	FString Manufacturer;
	FString FixtureName;

	bool bHasDimensions = false;
	FVector DimensionsMeters = FVector::ZeroVector; // x=length, y=width, z=height

	TArray<FRebusFixturePart> Parts; // root-level parts (children nested)
	FRebusMotionRig MotionRig;
	FRebusPhotometrics Photometrics;
	FRebusZoomRange Zoom;
	FRebusSource Source;
	TArray<FRebusWheel> Wheels;

	FString MeshesUrl;
	FString IesProfileUrl;          // single default, or empty
	TArray<FRebusIesProfileRef> IesProfiles; // zoom-keyed, sorted ascending by ZoomDmx
};

// ---- Mesh bundle (/api/ue/fixtures/{id}/meshes) ----------------------------------------

struct FRebusMesh
{
	FString Name;
	FString GeometryName;
	FString ModelName;
	TArray<double> Vertices; // flat, 3 per vertex, metres, engine Y-up
	TArray<int32> Faces;     // Speckle face encoding (0 = tri, 1 = quad, then indices)
};

struct FRebusMeshBundle
{
	int32 Version = 0;
	TArray<FRebusMesh> Meshes;
};
