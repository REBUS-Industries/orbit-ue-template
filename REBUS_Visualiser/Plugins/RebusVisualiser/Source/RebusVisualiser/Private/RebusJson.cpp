// Copyright REBUS Industries.
#include "RebusJson.h"
#include "RebusVisualiserLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace RebusJson
{
	TSharedPtr<FJsonObject> ParseObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
		{
			return nullptr;
		}
		return Obj;
	}

	bool TryGetString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, FString& Out)
	{
		if (!Obj.IsValid()) return false;
		const TSharedPtr<FJsonValue> V = Obj->TryGetField(Field);
		if (!V.IsValid() || V->Type != EJson::String) return false;
		Out = V->AsString();
		return true;
	}

	bool TryGetNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, double& Out)
	{
		if (!Obj.IsValid()) return false;
		const TSharedPtr<FJsonValue> V = Obj->TryGetField(Field);
		if (!V.IsValid() || V->Type != EJson::Number) return false;
		Out = V->AsNumber();
		return true;
	}

	bool TryGetBool(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, bool& Out)
	{
		if (!Obj.IsValid()) return false;
		const TSharedPtr<FJsonValue> V = Obj->TryGetField(Field);
		if (!V.IsValid() || V->Type != EJson::Boolean) return false;
		Out = V->AsBool();
		return true;
	}

	bool TryGetOptionalNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, TOptional<double>& Out)
	{
		double N = 0.0;
		if (TryGetNumber(Obj, Field, N))
		{
			Out = N;
			return true;
		}
		return false;
	}

	static void ReadVector3(const TSharedPtr<FJsonObject>& Obj, FVector& Out)
	{
		double X = Out.X, Y = Out.Y, Z = Out.Z;
		TryGetNumber(Obj, TEXT("x"), X);
		TryGetNumber(Obj, TEXT("y"), Y);
		TryGetNumber(Obj, TEXT("z"), Z);
		Out = FVector(X, Y, Z);
	}

	static bool ReadMatrixField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, double Out[16])
	{
		if (!Obj.IsValid()) return false;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj->TryGetArrayField(Field, Arr) || !Arr || Arr->Num() != 16) return false;
		for (int32 i = 0; i < 16; ++i)
		{
			if ((*Arr)[i]->Type != EJson::Number) return false;
			Out[i] = (*Arr)[i]->AsNumber();
		}
		return true;
	}

	// ---- Scene ------------------------------------------------------------------------

	bool ParseScene(const FString& Json, FRebusScene& Out)
	{
		const TSharedPtr<FJsonObject> Root = ParseObject(Json);
		if (!Root.IsValid()) return false;

		TryGetString(Root, TEXT("schema"), Out.Schema);
		TryGetString(Root, TEXT("projectId"), Out.ProjectId);
		TryGetString(Root, TEXT("speckleProjectId"), Out.SpeckleProjectId);
		TryGetString(Root, TEXT("modelId"), Out.ModelId);
		TryGetString(Root, TEXT("versionId"), Out.VersionId);
		TryGetString(Root, TEXT("units"), Out.Units);
		TryGetBool(Root, TEXT("truncated"), Out.bTruncated);

		const TArray<TSharedPtr<FJsonValue>>* Fixtures = nullptr;
		if (Root->TryGetArrayField(TEXT("fixtures"), Fixtures) && Fixtures)
		{
			for (const TSharedPtr<FJsonValue>& FV : *Fixtures)
			{
				const TSharedPtr<FJsonObject>* FO = nullptr;
				if (!FV->TryGetObject(FO) || !FO) continue;

				FRebusSceneFixture F;
				TryGetString(*FO, TEXT("id"), F.Id);
				if (F.Id.IsEmpty()) continue; // the control key is mandatory
				TryGetString(*FO, TEXT("name"), F.Name);
				TryGetString(*FO, TEXT("fixtureId"), F.LibraryFixtureId);
				TryGetString(*FO, TEXT("gdtfKey"), F.GdtfKey);
				TryGetString(*FO, TEXT("mode"), F.Mode);
				TryGetString(*FO, TEXT("unitNumber"), F.UnitNumber);
				TryGetString(*FO, TEXT("universe"), F.Universe);
				TryGetString(*FO, TEXT("address"), F.Address);
				TryGetString(*FO, TEXT("fixtureType"), F.FixtureType);
				TryGetString(*FO, TEXT("layer"), F.Layer);

				F.bHasMatrix = ReadMatrixField(*FO, TEXT("matrixZUpMeters"), F.Matrix);

				FString Src;
				TryGetString(*FO, TEXT("matrixSource"), Src);
				if (Src == TEXT("zup-column")) F.MatrixSource = ERebusMatrixSource::ZUpColumn;
				else if (Src == TEXT("transform-row")) F.MatrixSource = ERebusMatrixSource::TransformRow;
				else F.MatrixSource = ERebusMatrixSource::Unknown;

				Out.Fixtures.Add(MoveTemp(F));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
		if (Root->TryGetArrayField(TEXT("profileIds"), Profiles) && Profiles)
		{
			for (const TSharedPtr<FJsonValue>& PV : *Profiles)
			{
				if (PV->Type == EJson::String) Out.ProfileIds.Add(PV->AsString());
			}
		}

		return true;
	}

	// ---- Fixture parts ----------------------------------------------------------------

	static void ParsePart(const TSharedPtr<FJsonObject>& PO, FRebusFixturePart& Part)
	{
		TryGetString(PO, TEXT("name"), Part.Name);
		TryGetString(PO, TEXT("geometryName"), Part.GeometryName);
		TryGetString(PO, TEXT("modelName"), Part.ModelName);

		Part.bHasLocalMatrix = ReadMatrixField(PO, TEXT("localMatrix"), Part.LocalMatrix);
		Part.bHasWorldMatrixMeters = ReadMatrixField(PO, TEXT("worldMatrixMeters"), Part.WorldMatrixMeters);

		// Beam detection: a node carrying beamDirectionWorld (or named "Beam") is the emitter.
		FString TypeStr;
		TryGetString(PO, TEXT("type"), TypeStr);
		const bool bNamedBeam = Part.Name.Equals(TEXT("Beam"), ESearchCase::IgnoreCase)
			|| Part.GeometryName.Equals(TEXT("Beam"), ESearchCase::IgnoreCase)
			|| TypeStr.Equals(TEXT("Beam"), ESearchCase::IgnoreCase);

		const TSharedPtr<FJsonObject>* Dir = nullptr;
		if (PO->TryGetObjectField(TEXT("beamDirectionWorld"), Dir) && Dir)
		{
			ReadVector3(*Dir, Part.BeamDirectionWorld);
			Part.bHasBeamDirection = true;
		}
		const TSharedPtr<FJsonObject>* Up = nullptr;
		if (PO->TryGetObjectField(TEXT("beamUpWorld"), Up) && Up)
		{
			ReadVector3(*Up, Part.BeamUpWorld);
			Part.bHasBeamUp = true;
		}
		Part.bIsBeam = bNamedBeam || Part.bHasBeamDirection;

		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (PO->TryGetArrayField(TEXT("children"), Children) && Children)
		{
			for (const TSharedPtr<FJsonValue>& CV : *Children)
			{
				const TSharedPtr<FJsonObject>* CO = nullptr;
				if (CV->TryGetObject(CO) && CO)
				{
					FRebusFixturePart Child;
					ParsePart(*CO, Child);
					Part.Children.Add(MoveTemp(Child));
				}
			}
		}
	}

	// ---- Motion rig -------------------------------------------------------------------

	static ERebusAxisKind ParseAxisKind(const FString& S)
	{
		if (S.Equals(TEXT("pan"), ESearchCase::IgnoreCase)) return ERebusAxisKind::Pan;
		if (S.Equals(TEXT("tilt"), ESearchCase::IgnoreCase)) return ERebusAxisKind::Tilt;
		return ERebusAxisKind::Other;
	}

	static void ParseMotionRig(const TSharedPtr<FJsonObject>& RO, FRebusMotionRig& Rig)
	{
		Rig.bValid = true;

		const TSharedPtr<FJsonObject>* Off = nullptr;
		if (RO->TryGetObjectField(TEXT("pivotOffset"), Off) && Off)
		{
			ReadVector3(*Off, Rig.PivotOffset);
			Rig.bHasPivotOffset = true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Axes = nullptr;
		if (RO->TryGetArrayField(TEXT("axes"), Axes) && Axes)
		{
			for (const TSharedPtr<FJsonValue>& AV : *Axes)
			{
				const TSharedPtr<FJsonObject>* AO = nullptr;
				if (!AV->TryGetObject(AO) || !AO) continue;

				FRebusMotionAxis Axis;
				FString KindStr;
				TryGetString(*AO, TEXT("kind"), KindStr);
				Axis.Kind = ParseAxisKind(KindStr);

				const TSharedPtr<FJsonObject>* Pivot = nullptr;
				if ((*AO)->TryGetObjectField(TEXT("pivot"), Pivot) && Pivot) ReadVector3(*Pivot, Axis.Pivot);
				const TSharedPtr<FJsonObject>* AxisVec = nullptr;
				if ((*AO)->TryGetObjectField(TEXT("axis"), AxisVec) && AxisVec) ReadVector3(*AxisVec, Axis.Axis);

				TryGetNumber(*AO, TEXT("minDeg"), Axis.MinDeg);
				TryGetNumber(*AO, TEXT("maxDeg"), Axis.MaxDeg);
				TryGetNumber(*AO, TEXT("defaultDeg"), Axis.DefaultDeg);
				// Per-axis identity: prefer the explicit nodeName/parentNodeName, but fall back to
				// the portal's geometryName/parentGeometryName aliases so parent-link resolution
				// (and the tilt-under-pan compensation that depends on ParentAxisIndex) works.
				if (!TryGetString(*AO, TEXT("nodeName"), Axis.NodeName))
				{
					TryGetString(*AO, TEXT("geometryName"), Axis.NodeName);
				}
				if (!TryGetString(*AO, TEXT("parentNodeName"), Axis.ParentNodeName))
				{
					TryGetString(*AO, TEXT("parentGeometryName"), Axis.ParentNodeName);
				}

				const TArray<TSharedPtr<FJsonValue>>* Affected = nullptr;
				if ((*AO)->TryGetArrayField(TEXT("affectedGeometryNames"), Affected) && Affected)
				{
					for (const TSharedPtr<FJsonValue>& GV : *Affected)
					{
						if (GV->Type == EJson::String)
						{
							Axis.AffectedGeometryNames.Add(GV->AsString().ToLower());
						}
					}
				}

				Rig.Axes.Add(MoveTemp(Axis));
			}
		}

		// Resolve parent links (by parentNodeName -> nodeName) and topologically sort
		// parent-first so a parent axis is always applied before its children (§7.4 step 1).
		for (int32 i = 0; i < Rig.Axes.Num(); ++i)
		{
			const FString& ParentName = Rig.Axes[i].ParentNodeName;
			if (ParentName.IsEmpty()) continue;
			for (int32 j = 0; j < Rig.Axes.Num(); ++j)
			{
				if (i != j && Rig.Axes[j].NodeName == ParentName)
				{
					Rig.Axes[i].ParentAxisIndex = j;
					break;
				}
			}
		}

		// Stable parent-first sort: an axis with a resolved parent must come after it.
		TArray<FRebusMotionAxis> Sorted;
		Sorted.Reserve(Rig.Axes.Num());
		TArray<bool> Emitted; Emitted.Init(false, Rig.Axes.Num());
		TFunction<void(int32)> Emit = [&](int32 Index)
		{
			if (Index == INDEX_NONE || Emitted[Index]) return;
			Emit(Rig.Axes[Index].ParentAxisIndex);
			Emitted[Index] = true;
			Sorted.Add(Rig.Axes[Index]);
		};
		for (int32 i = 0; i < Rig.Axes.Num(); ++i) Emit(i);
		// Fall back to the parsed order if the sort dropped anything (cycle / bad data).
		if (Sorted.Num() == Rig.Axes.Num())
		{
			Rig.Axes = MoveTemp(Sorted);
		}
	}

	// ---- Fixture profile --------------------------------------------------------------

	bool ParseFixtureProfile(const FString& Json, FRebusFixtureProfile& Out)
	{
		const TSharedPtr<FJsonObject> Root = ParseObject(Json);
		if (!Root.IsValid()) return false;

		Out.bValid = true;
		TryGetString(Root, TEXT("schema"), Out.Schema);
		TryGetString(Root, TEXT("id"), Out.Id);
		TryGetString(Root, TEXT("manufacturer"), Out.Manufacturer);
		TryGetString(Root, TEXT("fixtureName"), Out.FixtureName);
		TryGetString(Root, TEXT("meshesUrl"), Out.MeshesUrl);
		TryGetString(Root, TEXT("iesProfileUrl"), Out.IesProfileUrl);

		const TSharedPtr<FJsonObject>* Dims = nullptr;
		if (Root->TryGetObjectField(TEXT("dimensions"), Dims) && Dims)
		{
			ReadVector3(*Dims, Out.DimensionsMeters);
			Out.bHasDimensions = true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
		if (Root->TryGetArrayField(TEXT("fixtureParts"), Parts) && Parts)
		{
			for (const TSharedPtr<FJsonValue>& PV : *Parts)
			{
				const TSharedPtr<FJsonObject>* PO = nullptr;
				if (PV->TryGetObject(PO) && PO)
				{
					FRebusFixturePart Part;
					ParsePart(*PO, Part);
					Out.Parts.Add(MoveTemp(Part));
				}
			}
		}

		const TSharedPtr<FJsonObject>* Rig = nullptr;
		if (Root->TryGetObjectField(TEXT("motionRig"), Rig) && Rig)
		{
			ParseMotionRig(*Rig, Out.MotionRig);
		}

		const TSharedPtr<FJsonObject>* Photo = nullptr;
		if (Root->TryGetObjectField(TEXT("photometrics"), Photo) && Photo)
		{
			TryGetOptionalNumber(*Photo, TEXT("luminousFlux"), Out.Photometrics.LuminousFlux);
			TryGetOptionalNumber(*Photo, TEXT("beamAngle"), Out.Photometrics.BeamAngle);
			TryGetOptionalNumber(*Photo, TEXT("fieldAngle"), Out.Photometrics.FieldAngle);
			TryGetOptionalNumber(*Photo, TEXT("colorTemperature"), Out.Photometrics.ColorTemperature);
			TryGetOptionalNumber(*Photo, TEXT("cri"), Out.Photometrics.Cri);
			TryGetBool(*Photo, TEXT("hasIesProfile"), Out.Photometrics.bHasIesProfile);
		}

		const TSharedPtr<FJsonObject>* Zoom = nullptr;
		if (Root->TryGetObjectField(TEXT("zoom"), Zoom) && Zoom)
		{
			if (TryGetNumber(*Zoom, TEXT("minDeg"), Out.Zoom.MinDeg)
				&& TryGetNumber(*Zoom, TEXT("maxDeg"), Out.Zoom.MaxDeg))
			{
				Out.Zoom.bValid = true;
			}
		}

		const TSharedPtr<FJsonObject>* Src = nullptr;
		if (Root->TryGetObjectField(TEXT("source"), Src) && Src)
		{
			TryGetOptionalNumber(*Src, TEXT("radiusMeters"), Out.Source.RadiusMeters);
			TryGetOptionalNumber(*Src, TEXT("diameterMeters"), Out.Source.DiameterMeters);
		}

		const TArray<TSharedPtr<FJsonValue>>* Wheels = nullptr;
		if (Root->TryGetArrayField(TEXT("wheels"), Wheels) && Wheels)
		{
			for (const TSharedPtr<FJsonValue>& WV : *Wheels)
			{
				const TSharedPtr<FJsonObject>* WO = nullptr;
				if (!WV->TryGetObject(WO) || !WO) continue;

				FRebusWheel Wheel;
				TryGetString(*WO, TEXT("name"), Wheel.Name);
				TryGetString(*WO, TEXT("kind"), Wheel.Kind);

				const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
				if ((*WO)->TryGetArrayField(TEXT("slots"), Slots) && Slots)
				{
					for (const TSharedPtr<FJsonValue>& SV : *Slots)
					{
						const TSharedPtr<FJsonObject>* SO = nullptr;
						if (!SV->TryGetObject(SO) || !SO) continue;
						FRebusWheelSlot Slot;
						TryGetString(*SO, TEXT("name"), Slot.Name);
						TryGetString(*SO, TEXT("color"), Slot.Color);
						TryGetString(*SO, TEXT("imageUrl"), Slot.ImageUrl);
						Wheel.Slots.Add(MoveTemp(Slot));
					}
				}
				Out.Wheels.Add(MoveTemp(Wheel));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* IesProfiles = nullptr;
		if (Root->TryGetArrayField(TEXT("iesProfiles"), IesProfiles) && IesProfiles)
		{
			for (const TSharedPtr<FJsonValue>& IV : *IesProfiles)
			{
				const TSharedPtr<FJsonObject>* IO = nullptr;
				if (!IV->TryGetObject(IO) || !IO) continue;
				FRebusIesProfileRef Ref;
				double Dmx = 0.0;
				TryGetNumber(*IO, TEXT("zoomDmx"), Dmx);
				Ref.ZoomDmx = FMath::Clamp((int32)FMath::RoundToInt(Dmx), 0, 255);
				TryGetNumber(*IO, TEXT("zoomAngleDeg"), Ref.ZoomAngleDeg);
				TryGetNumber(*IO, TEXT("beamAngleDeg"), Ref.BeamAngleDeg);
				TryGetNumber(*IO, TEXT("fieldAngleDeg"), Ref.FieldAngleDeg);
				TryGetString(*IO, TEXT("iesUrl"), Ref.IesUrl);
				Out.IesProfiles.Add(MoveTemp(Ref));
			}
			Out.IesProfiles.Sort([](const FRebusIesProfileRef& A, const FRebusIesProfileRef& B)
			{
				return A.ZoomDmx < B.ZoomDmx;
			});
		}

		return true;
	}

	// ---- Mesh bundle ------------------------------------------------------------------

	static void ParseMeshObject(const TSharedPtr<FJsonObject>& MO, FRebusMesh& Mesh)
	{
		TryGetString(MO, TEXT("name"), Mesh.Name);
		TryGetString(MO, TEXT("geometryName"), Mesh.GeometryName);
		TryGetString(MO, TEXT("modelName"), Mesh.ModelName);

		const TArray<TSharedPtr<FJsonValue>>* Verts = nullptr;
		if (MO->TryGetArrayField(TEXT("vertices"), Verts) && Verts)
		{
			Mesh.Vertices.Reserve(Verts->Num());
			for (const TSharedPtr<FJsonValue>& VV : *Verts)
			{
				Mesh.Vertices.Add(VV->AsNumber());
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Faces = nullptr;
		if (MO->TryGetArrayField(TEXT("faces"), Faces) && Faces)
		{
			Mesh.Faces.Reserve(Faces->Num());
			for (const TSharedPtr<FJsonValue>& FV : *Faces)
			{
				Mesh.Faces.Add((int32)FV->AsNumber());
			}
		}
	}

	bool ParseMeshBundle(const FString& Json, FRebusMeshBundle& Out)
	{
		// Two accepted shapes (§4.4): { version, meshes:[...] } OR a bare top-level array.
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		TSharedPtr<FJsonValue> RootVal;
		if (!FJsonSerializer::Deserialize(Reader, RootVal) || !RootVal.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* MeshArray = nullptr;
		if (RootVal->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject>& Root = RootVal->AsObject();
			double Ver = 0.0;
			TryGetNumber(Root, TEXT("version"), Ver);
			Out.Version = (int32)Ver;
			Root->TryGetArrayField(TEXT("meshes"), MeshArray);
		}
		else if (RootVal->Type == EJson::Array)
		{
			MeshArray = &RootVal->AsArray();
		}

		if (!MeshArray) return false;

		for (const TSharedPtr<FJsonValue>& MV : *MeshArray)
		{
			const TSharedPtr<FJsonObject>* MO = nullptr;
			if (MV->TryGetObject(MO) && MO)
			{
				FRebusMesh Mesh;
				ParseMeshObject(*MO, Mesh);
				Out.Meshes.Add(MoveTemp(Mesh));
			}
		}
		return true;
	}
}
