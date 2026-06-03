// Copyright REBUS Industries.
#include "RebusFixtureControlSubsystem.h"
#include "RebusFixtureActor.h"
#include "RebusJson.h"
#include "RebusVisualiserLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"

void URebusFixtureControlSubsystem::RegisterFixture(const FString& NodeId, ARebusFixtureActor* Actor)
{
	if (NodeId.IsEmpty() || !Actor) return;
	Fixtures.Add(NodeId, Actor);
}

void URebusFixtureControlSubsystem::UnregisterFixture(const FString& NodeId)
{
	Fixtures.Remove(NodeId);
}

void URebusFixtureControlSubsystem::Reset()
{
	Fixtures.Empty();
	CurrentSelection.Empty();
	PrimarySelection.Empty();
}

ARebusFixtureActor* URebusFixtureControlSubsystem::FindFixture(const FString& NodeId) const
{
	const TObjectPtr<ARebusFixtureActor>* Found = Fixtures.Find(NodeId);
	return Found ? Found->Get() : nullptr;
}

TArray<FString> URebusFixtureControlSubsystem::GetFixtureIds() const
{
	TArray<FString> Ids;
	Fixtures.GetKeys(Ids);
	return Ids;
}

// ---- Typed control --------------------------------------------------------------------

void URebusFixtureControlSubsystem::SetFixtureDimmer(const FString& Id, float Intensity01, float FadeSeconds)
{
	if (ARebusFixtureActor* F = FindFixture(Id)) F->ApplyDimmer(Intensity01, FadeSeconds);
}
void URebusFixtureControlSubsystem::SetFixtureColor(const FString& Id, const FLinearColor& Srgb, float FadeSeconds)
{
	if (ARebusFixtureActor* F = FindFixture(Id)) F->ApplyColor(Srgb, FadeSeconds);
}
void URebusFixtureControlSubsystem::SetFixturePanTilt(const FString& Id, float PanDeg, float TiltDeg, float FadeSeconds)
{
	if (ARebusFixtureActor* F = FindFixture(Id)) F->ApplyPanTilt(PanDeg, TiltDeg, FadeSeconds);
}
void URebusFixtureControlSubsystem::SetFixtureZoom(const FString& Id, float ZoomDeg, float FadeSeconds)
{
	if (ARebusFixtureActor* F = FindFixture(Id)) F->ApplyZoom(ZoomDeg, FadeSeconds);
}
void URebusFixtureControlSubsystem::SetFixtureGobo(const FString& Id, int32 GoboIndex, bool bHasIndex, int32 WheelIndex, const FString& Wheel, float FadeSeconds)
{
	if (ARebusFixtureActor* F = FindFixture(Id)) F->ApplyGobo(GoboIndex, bHasIndex, WheelIndex, Wheel, FadeSeconds);
}
void URebusFixtureControlSubsystem::SetFixtureIris(const FString& Id, float Iris01, float FadeSeconds)
{
	if (ARebusFixtureActor* F = FindFixture(Id)) F->ApplyIris(Iris01, FadeSeconds);
}
void URebusFixtureControlSubsystem::SetFixtureFocus(const FString& Id, float Focus01, float FadeSeconds)
{
	if (ARebusFixtureActor* F = FindFixture(Id)) F->ApplyFocus(Focus01, FadeSeconds);
}
void URebusFixtureControlSubsystem::SetFixtureFrost(const FString& Id, float Frost01, float FadeSeconds)
{
	if (ARebusFixtureActor* F = FindFixture(Id)) F->ApplyFrost(Frost01, FadeSeconds);
}
void URebusFixtureControlSubsystem::SetFixtureColorTemp(const FString& Id, float Kelvin)
{
	if (ARebusFixtureActor* F = FindFixture(Id)) F->ApplyColorTemp(Kelvin);
}
void URebusFixtureControlSubsystem::SetFixtureShutter(const FString& Id, int32 Mode, float RateHz)
{
	if (ARebusFixtureActor* F = FindFixture(Id))
	{
		F->ApplyShutter(static_cast<ERebusShutterMode>(FMath::Clamp(Mode, 0, 2)), RateHz);
	}
}
void URebusFixtureControlSubsystem::SetFixtureGoboRotation(const FString& Id, float Speed)
{
	if (ARebusFixtureActor* F = FindFixture(Id)) F->ApplyGoboRotation(Speed);
}
void URebusFixtureControlSubsystem::SetFixturePrism(const FString& Id, int32 Facets, float RotationDeg)
{
	if (ARebusFixtureActor* F = FindFixture(Id)) F->ApplyPrism(Facets, RotationDeg);
}
void URebusFixtureControlSubsystem::SetFixtureBeamVolumetrics(const FString& Id, float Intensity, bool bCastVolumetricShadow)
{
	if (ARebusFixtureActor* F = FindFixture(Id)) F->ApplyBeamVolumetrics(Intensity, bCastVolumetricShadow);
}

void URebusFixtureControlSubsystem::SelectFixtures(const TArray<FString>& Ids, const FString& PrimaryId)
{
	// Clear the old selection's highlight, then paint the new one.
	for (const FString& OldId : CurrentSelection)
	{
		if (!Ids.Contains(OldId))
		{
			if (ARebusFixtureActor* F = FindFixture(OldId)) F->SetSelected(false, false);
		}
	}
	for (const FString& Id : Ids)
	{
		if (ARebusFixtureActor* F = FindFixture(Id))
		{
			F->SetSelected(true, Id == PrimaryId);
		}
	}
	CurrentSelection = Ids;
	PrimarySelection = PrimaryId;
}

// ---- Descriptor routing ---------------------------------------------------------------

namespace
{
	// Optional fadeMs (float 0..60000) -> seconds. Absent => 0 (snap), §11.
	float ReadFadeSeconds(const TSharedPtr<FJsonObject>& Msg)
	{
		double Ms = 0.0;
		if (RebusJson::TryGetNumber(Msg, TEXT("fadeMs"), Ms))
		{
			return (float)(FMath::Clamp(Ms, 0.0, 60000.0) / 1000.0);
		}
		return 0.f;
	}
}

bool URebusFixtureControlSubsystem::HandleControlDescriptor(const FString& Type, const TSharedPtr<FJsonObject>& Msg)
{
	if (!Msg.IsValid()) return false;

	const float Fade = ReadFadeSeconds(Msg);
	FString Id;
	RebusJson::TryGetString(Msg, TEXT("fixtureId"), Id);

	if (Type == TEXT("SetFixtureIntensity"))
	{
		double Intensity = 0.0; RebusJson::TryGetNumber(Msg, TEXT("intensity"), Intensity);
		SetFixtureDimmer(Id, (float)Intensity, Fade);
		return true;
	}
	if (Type == TEXT("SetFixtureColor"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Rgb = nullptr;
		if (Msg->TryGetArrayField(TEXT("rgb"), Rgb) && Rgb && Rgb->Num() >= 3)
		{
			const FLinearColor C((float)(*Rgb)[0]->AsNumber(), (float)(*Rgb)[1]->AsNumber(), (float)(*Rgb)[2]->AsNumber(), 1.f);
			SetFixtureColor(Id, C, Fade);
		}
		return true;
	}
	if (Type == TEXT("SetFixturePanTilt"))
	{
		double Pan = 0.0, Tilt = 0.0;
		RebusJson::TryGetNumber(Msg, TEXT("panDeg"), Pan);
		RebusJson::TryGetNumber(Msg, TEXT("tiltDeg"), Tilt);
		SetFixturePanTilt(Id, (float)Pan, (float)Tilt, Fade);
		return true;
	}
	if (Type == TEXT("SetFixtureZoom"))
	{
		double Zoom = 0.0; RebusJson::TryGetNumber(Msg, TEXT("zoomDeg"), Zoom);
		SetFixtureZoom(Id, (float)Zoom, Fade);
		return true;
	}
	if (Type == TEXT("SetFixtureGobo"))
	{
		const TSharedPtr<FJsonValue> V = Msg->TryGetField(TEXT("goboIndex"));
		const bool bHasIndex = V.IsValid() && V->Type == EJson::Number;
		const int32 Index = bHasIndex ? (int32)V->AsNumber() : 0;
		// Optional wheel selectors to disambiguate multi-gobo-wheel fixtures. Precedence is
		// resolved in the actor: wheelIndex (0-based, selects the Nth gobo-kind wheel) wins over
		// the legacy wheel-name hint, which wins over the first gobo-kind wheel.
		int32 WheelIndex = INDEX_NONE;
		const TSharedPtr<FJsonValue> WIdx = Msg->TryGetField(TEXT("wheelIndex"));
		if (WIdx.IsValid() && WIdx->Type == EJson::Number) WheelIndex = (int32)WIdx->AsNumber();
		FString Wheel;
		RebusJson::TryGetString(Msg, TEXT("wheel"), Wheel);
		SetFixtureGobo(Id, Index, bHasIndex, WheelIndex, Wheel, Fade);
		return true;
	}
	if (Type == TEXT("SetFixtureIris"))
	{
		double Iris = 1.0; RebusJson::TryGetNumber(Msg, TEXT("iris"), Iris);
		SetFixtureIris(Id, (float)Iris, Fade);
		return true;
	}
	if (Type == TEXT("SetFixtureFocus"))
	{
		double Focus = 0.0; RebusJson::TryGetNumber(Msg, TEXT("focus"), Focus);
		SetFixtureFocus(Id, (float)Focus, Fade);
		return true;
	}
	if (Type == TEXT("SetFixtureFrost"))
	{
		double Frost = 0.0; RebusJson::TryGetNumber(Msg, TEXT("frost"), Frost);
		SetFixtureFrost(Id, (float)Frost, Fade);
		return true;
	}
	if (Type == TEXT("SetFixtureColorTemp"))
	{
		double K = 6500.0; RebusJson::TryGetNumber(Msg, TEXT("kelvin"), K);
		SetFixtureColorTemp(Id, (float)K);
		return true;
	}
	if (Type == TEXT("SetFixtureShutter"))
	{
		double Mode = 0.0, Rate = 0.0;
		RebusJson::TryGetNumber(Msg, TEXT("mode"), Mode);
		RebusJson::TryGetNumber(Msg, TEXT("rateHz"), Rate);
		SetFixtureShutter(Id, (int32)Mode, (float)Rate);
		return true;
	}
	if (Type == TEXT("SetFixtureGoboRotation"))
	{
		double Speed = 0.0; RebusJson::TryGetNumber(Msg, TEXT("speed"), Speed);
		SetFixtureGoboRotation(Id, (float)Speed);
		return true;
	}
	if (Type == TEXT("SetFixturePrism"))
	{
		double Facets = 0.0, Rot = 0.0;
		RebusJson::TryGetNumber(Msg, TEXT("facets"), Facets);
		RebusJson::TryGetNumber(Msg, TEXT("rotationDeg"), Rot);
		SetFixturePrism(Id, (int32)Facets, (float)Rot);
		return true;
	}
	if (Type == TEXT("SetFixtureBeamVolumetrics"))
	{
		double Intensity = 1.0; bool bShadow = true;
		RebusJson::TryGetNumber(Msg, TEXT("intensity"), Intensity);
		RebusJson::TryGetBool(Msg, TEXT("castVolumetricShadow"), bShadow);
		SetFixtureBeamVolumetrics(Id, (float)Intensity, bShadow);
		return true;
	}
	if (Type == TEXT("SelectFixtures"))
	{
		TArray<FString> Ids;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Msg->TryGetArrayField(TEXT("fixtureIds"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				if (V->Type == EJson::String) Ids.Add(V->AsString());
			}
		}
		FString Primary;
		RebusJson::TryGetString(Msg, TEXT("primaryFixtureId"), Primary);
		SelectFixtures(Ids, Primary);
		return true;
	}

	return false; // not a fixture/selection type -> let scene-property handling try.
}

// ---- Orbit-imported model binding (Phase 1 A/B sync test) -----------------------------

void URebusFixtureControlSubsystem::SetDriveOrbitModels(bool bEnabled)
{
	bDriveOrbitModels = bEnabled;
	LastOrbitMatchLogged = -1; // force a fresh match-summary log on the next rebind

	// Push the flag to every fixture first (so a disable restores even fixtures that no longer
	// match an import), then (re)bind so freshly-matched fixtures start driving immediately.
	for (const TPair<FString, TObjectPtr<ARebusFixtureActor>>& Pair : Fixtures)
	{
		if (ARebusFixtureActor* Actor = Pair.Value.Get())
		{
			Actor->SetDriveOrbitModel(bEnabled);
		}
	}

	UE_LOG(LogRebusVisualiser, Log, TEXT("bDriveOrbitModels=%d (fixtures=%d)."), bEnabled ? 1 : 0, Fixtures.Num());

	if (bEnabled)
	{
		RebindOrbitModels();
	}
}

void URebusFixtureControlSubsystem::RebindOrbitModels()
{
	if (!bDriveOrbitModels) return; // only spend cycles while the A/B test is on

	UWorld* World = nullptr;
	if (UGameInstance* GI = GetGameInstance())
	{
		World = GI->GetWorld();
	}
	if (!World) return;

	// Build objectId -> imported components by scanning any Orbit import root. The root is found
	// GENERICALLY by its class name so RebusVisualiser keeps no compile/link dependency on the
	// (separately-owned) OrbitConnector plugin; the imported mesh components are tagged with their
	// glb node-name ancestry (the object id lives at one of those levels).
	TMap<FString, TArray<USceneComponent*>> OrbitIndex;
	int32 RootCount = 0;
	int32 TaggedComps = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor->GetClass()->GetName() != TEXT("OrbitImportRoot"))
		{
			continue;
		}
		++RootCount;
		TArray<USceneComponent*> Comps;
		Actor->GetComponents(Comps);
		for (USceneComponent* Comp : Comps)
		{
			if (!Comp || Comp->ComponentTags.Num() == 0) continue;
			++TaggedComps;
			for (const FName& Tag : Comp->ComponentTags)
			{
				OrbitIndex.FindOrAdd(Tag.ToString()).Add(Comp);
			}
		}
	}

	if (RootCount == 0)
	{
		// No Orbit import in the world yet -> nothing to bind (the common case; late binding will
		// pick it up once an import arrives). Log once so the absence is visible without spamming.
		if (LastOrbitMatchLogged != 0)
		{
			LastOrbitMatchLogged = 0;
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Orbit bind: no OrbitImportRoot present yet (fixtures=%d). Will retry."), Fixtures.Num());
		}
		return;
	}

	int32 Matched = 0, Unmatched = 0;
	TArray<FString> UnmatchedIds;
	for (const TPair<FString, TObjectPtr<ARebusFixtureActor>>& Pair : Fixtures)
	{
		ARebusFixtureActor* Actor = Pair.Value.Get();
		if (!Actor) continue;
		const FString& FixtureId = Pair.Key;

		if (TArray<USceneComponent*>* Found = OrbitIndex.Find(FixtureId))
		{
			// (Re)bind only when not already bound to this id with live components -- re-binding
			// would otherwise re-capture the currently-driven pose as the new "rest" and drift.
			if (Actor->GetBoundOrbitObjectId() != FixtureId || !Actor->HasOrbitBinding())
			{
				Actor->BindOrbitComponents(*Found, FixtureId);
			}
			Actor->SetDriveOrbitModel(bDriveOrbitModels);
			++Matched;
		}
		else
		{
			++Unmatched;
			if (UnmatchedIds.Num() < 12) UnmatchedIds.Add(FixtureId);
		}
	}

	// Summary log, throttled to when the matched count changes (a fresh import / spawn), so a
	// 1 Hz rebind timer doesn't flood the log once steady.
	if (Matched != LastOrbitMatchLogged)
	{
		LastOrbitMatchLogged = Matched;
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Orbit bind: roots=%d taggedComps=%d distinctObjectIds=%d | fixtures matched=%d unmatched=%d%s%s"),
			RootCount, TaggedComps, OrbitIndex.Num(), Matched, Unmatched,
			UnmatchedIds.Num() > 0 ? TEXT(" unmatchedFixtureIds=") : TEXT(""),
			UnmatchedIds.Num() > 0 ? *FString::Join(UnmatchedIds, TEXT(",")) : TEXT(""));
	}
}
