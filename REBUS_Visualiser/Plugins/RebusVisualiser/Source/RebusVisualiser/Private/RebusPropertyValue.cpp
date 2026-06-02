// Copyright REBUS Industries.
#include "RebusPropertyValue.h"
#include "RebusJson.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"

FRebusPropertyValue FRebusPropertyValue::FromJson(const TSharedPtr<FJsonValue>& Value)
{
	FRebusPropertyValue Out;
	if (!Value.IsValid()) return Out;

	switch (Value->Type)
	{
	case EJson::Number:
		return MakeNumber(Value->AsNumber());
	case EJson::Boolean:
		return MakeBool(Value->AsBool());
	case EJson::String:
		return MakeString(Value->AsString());
	case EJson::Object:
	{
		const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
		double R = 0, G = 0, B = 0, A = 1;
		const bool bHasR = RebusJson::TryGetNumber(Obj, TEXT("r"), R);
		const bool bHasG = RebusJson::TryGetNumber(Obj, TEXT("g"), G);
		const bool bHasB = RebusJson::TryGetNumber(Obj, TEXT("b"), B);
		RebusJson::TryGetNumber(Obj, TEXT("a"), A);
		if (bHasR && bHasG && bHasB)
		{
			return MakeColor(FLinearColor((float)R, (float)G, (float)B, (float)A));
		}
		break;
	}
	default:
		break;
	}
	return Out;
}

TSharedPtr<FJsonValue> FRebusPropertyValue::ToJson() const
{
	switch (Kind)
	{
	case ERebusValueKind::Number:
		return MakeShared<FJsonValueNumber>(Number);
	case ERebusValueKind::Bool:
		return MakeShared<FJsonValueBoolean>(bBool);
	case ERebusValueKind::String:
		return MakeShared<FJsonValueString>(String);
	case ERebusValueKind::Color:
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("r"), Color.R);
		Obj->SetNumberField(TEXT("g"), Color.G);
		Obj->SetNumberField(TEXT("b"), Color.B);
		Obj->SetNumberField(TEXT("a"), Color.A);
		return MakeShared<FJsonValueObject>(Obj);
	}
	default:
		return MakeShared<FJsonValueNull>();
	}
}
