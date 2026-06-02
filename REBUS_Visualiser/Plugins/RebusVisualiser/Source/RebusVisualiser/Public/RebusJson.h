// Copyright REBUS Industries.
//
// JSON helpers + REST payload parsers. Tolerant by design (ue-plugin-build-guide.md §4 / §5):
// unknown fields ignored, missing optionals left unset, malformed input rejected gracefully.
#pragma once

#include "CoreMinimal.h"
#include "RebusSceneTypes.h"

class FJsonObject;
class FJsonValue;

namespace RebusJson
{
	// Parse a JSON document string into an object. Returns null on malformed input.
	TSharedPtr<FJsonObject> ParseObject(const FString& Json);

	// Optional getters that only write Out when the field exists and is the right kind.
	bool TryGetString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, FString& Out);
	bool TryGetNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, double& Out);
	bool TryGetBool(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, bool& Out);
	bool TryGetOptionalNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, TOptional<double>& Out);

	// Top-level payload parsers.
	bool ParseScene(const FString& Json, FRebusScene& Out);
	bool ParseFixtureProfile(const FString& Json, FRebusFixtureProfile& Out);
	bool ParseMeshBundle(const FString& Json, FRebusMeshBundle& Out);
}
