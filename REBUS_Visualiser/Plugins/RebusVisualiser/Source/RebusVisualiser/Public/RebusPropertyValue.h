// Copyright REBUS Industries.
//
// Variant for scene-property values on the wire (ue-plugin-build-guide.md §5.4 / §6.3):
//   number | bool | string(<=64) | { r, g, b, a }  (FLinearColor)
#pragma once

#include "CoreMinimal.h"

class FJsonValue;
class FJsonObject;

enum class ERebusValueKind : uint8
{
	None,
	Number,
	Bool,
	String,
	Color
};

struct FRebusPropertyValue
{
	ERebusValueKind Kind = ERebusValueKind::None;
	double Number = 0.0;
	bool bBool = false;
	FString String;
	FLinearColor Color = FLinearColor::White;

	static FRebusPropertyValue MakeNumber(double N) { FRebusPropertyValue V; V.Kind = ERebusValueKind::Number; V.Number = N; return V; }
	static FRebusPropertyValue MakeBool(bool B) { FRebusPropertyValue V; V.Kind = ERebusValueKind::Bool; V.bBool = B; return V; }
	static FRebusPropertyValue MakeString(const FString& S) { FRebusPropertyValue V; V.Kind = ERebusValueKind::String; V.String = S; return V; }
	static FRebusPropertyValue MakeColor(const FLinearColor& C) { FRebusPropertyValue V; V.Kind = ERebusValueKind::Color; V.Color = C; return V; }

	bool IsSet() const { return Kind != ERebusValueKind::None; }

	int32 AsInt() const { return (int32)FMath::RoundToInt(Number); }
	float AsFloat() const { return (float)Number; }

	// Parse from a JSON value (number/bool/string/{r,g,b,a}); returns false for null/unknown.
	static FRebusPropertyValue FromJson(const TSharedPtr<FJsonValue>& Value);

	// Serialise into a JSON value for SceneState/FixtureState read-back.
	TSharedPtr<FJsonValue> ToJson() const;
};
