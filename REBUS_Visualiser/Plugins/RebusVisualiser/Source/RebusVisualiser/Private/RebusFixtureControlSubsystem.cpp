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
	// v1.0.61: log fixture-match outcome. Previously the call silently no-oped when the portal
	// sent an unknown id, leaving the user with only the "Descriptor type 'SetFixtureShutter'"
	// log line and no clue why nothing happened. Now: warn if id doesn't match anything in the
	// Fixtures map (with a snapshot of known ids for debugging), info if it matched.
	ARebusFixtureActor* F = FindFixture(Id);
	if (!F)
	{
		TArray<FString> Known;
		Fixtures.GetKeys(Known);
		const FString KnownList = Known.Num() > 0
			? FString::Join(Known, TEXT(", "))
			: FString(TEXT("(none registered)"));
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("SetFixtureShutter id='%s' mode=%d rate=%.2fHz: NO FIXTURE FOUND. Known ids: [%s]."),
			*Id, Mode, RateHz, *KnownList);
		return;
	}
	UE_LOG(LogRebusVisualiser, Log,
		TEXT("SetFixtureShutter dispatch: id='%s' fixture=%s mode=%d rate=%.2fHz."),
		*Id, *GetNameSafe(F), Mode, RateHz);
	F->ApplyShutter(static_cast<ERebusShutterMode>(FMath::Clamp(Mode, 0, 2)), RateHz);
}
void URebusFixtureControlSubsystem::SetFixtureGoboRotation(const FString& Id, float Speed, int32 WheelIndex)
{
	if (ARebusFixtureActor* F = FindFixture(Id)) F->ApplyGoboRotation(Speed, WheelIndex);
}
void URebusFixtureControlSubsystem::SetFixtureAnimationRotation(const FString& Id, float Speed)
{
	if (ARebusFixtureActor* F = FindFixture(Id)) F->ApplyAnimationWheelRotation(Speed);
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
		// v1.0.61: accept multiple field/value conventions and log every parse step. The user
		// reported logs showed "Descriptor type 'SetFixtureShutter'" arriving but no shutter
		// effect; previously we parsed only `mode` (numeric) and `rateHz` (numeric) and silently
		// no-oped if the portal used a string for mode ("open"/"closed"/"strobe") or used a
		// different field name for the rate (rate / frequency / hz / freq). Now: try numeric
		// first, then string, then alternate rate names. Log raw + resolved values + fixture
		// match outcome so the portal team can immediately see exactly what's arriving.
		int32 ModeInt = 0;
		FString ModeStr;
		bool bModeAsNumber = false;
		bool bModeAsString = false;
		double ModeNum = 0.0;
		if (RebusJson::TryGetNumber(Msg, TEXT("mode"), ModeNum))
		{
			bModeAsNumber = true;
			ModeInt = FMath::Clamp((int32)ModeNum, 0, 2);
		}
		else if (RebusJson::TryGetString(Msg, TEXT("mode"), ModeStr))
		{
			bModeAsString = true;
			const FString Lower = ModeStr.ToLower().TrimStartAndEnd();
			if (Lower == TEXT("open") || Lower == TEXT("on") || Lower == TEXT("1") || Lower == TEXT("true"))
			{
				ModeInt = 0;
			}
			else if (Lower == TEXT("closed") || Lower == TEXT("close") || Lower == TEXT("off") || Lower == TEXT("0") || Lower == TEXT("false"))
			{
				ModeInt = 1;
			}
			else if (Lower == TEXT("strobe") || Lower == TEXT("strobing") || Lower == TEXT("flash") || Lower == TEXT("pulse") || Lower == TEXT("2"))
			{
				ModeInt = 2;
			}
			else
			{
				UE_LOG(LogRebusVisualiser, Warning,
					TEXT("SetFixtureShutter id='%s': unrecognised mode string '%s' (expected open/closed/strobe or 0/1/2); falling back to Open."),
					*Id, *ModeStr);
				ModeInt = 0;
			}
		}

		// Rate: rateHz (canonical) -> rate -> frequency -> hz -> freq. Whichever lands first wins.
		double Rate = 0.0;
		const TCHAR* RateKey = TEXT("(none)");
		if      (RebusJson::TryGetNumber(Msg, TEXT("rateHz"),    Rate)) { RateKey = TEXT("rateHz"); }
		else if (RebusJson::TryGetNumber(Msg, TEXT("rate"),      Rate)) { RateKey = TEXT("rate"); }
		else if (RebusJson::TryGetNumber(Msg, TEXT("frequency"), Rate)) { RateKey = TEXT("frequency"); }
		else if (RebusJson::TryGetNumber(Msg, TEXT("hz"),        Rate)) { RateKey = TEXT("hz"); }
		else if (RebusJson::TryGetNumber(Msg, TEXT("freq"),      Rate)) { RateKey = TEXT("freq"); }

		const TCHAR* ModeName = (ModeInt == 0) ? TEXT("Open") : (ModeInt == 1 ? TEXT("Closed") : TEXT("Strobe"));
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("SetFixtureShutter parsed: id='%s' mode=%d(%s) [source=%s raw=%s] rate=%.2fHz [source=%s] fadeIgnored=%.2f"),
			*Id, ModeInt, ModeName,
			bModeAsNumber ? TEXT("number") : (bModeAsString ? TEXT("string") : TEXT("(missing -> Open)")),
			bModeAsNumber ? *FString::Printf(TEXT("%.2f"), ModeNum) : (bModeAsString ? *ModeStr : TEXT("<absent>")),
			Rate, RateKey, Fade);

		SetFixtureShutter(Id, ModeInt, (float)Rate);
		return true;
	}
	if (Type == TEXT("SetFixtureGoboRotation"))
	{
		// v1.0.50: 'speed' is a signed normalised rotation speed in [-1..1]; the visualiser clamps.
		// Sign = direction (+ CW looking down the beam, - CCW), 0 = stop. Optional 'wheelIndex'
		// (0-based into the full wheels[]) lets the portal disambiguate multi-wheel fixtures;
		// today the actor pushes one rotation to Epic's single DMX Gobo Disk Rotation Speed.
		double Speed = 0.0; RebusJson::TryGetNumber(Msg, TEXT("speed"), Speed);
		int32 WheelIndex = INDEX_NONE;
		const TSharedPtr<FJsonValue> WIdx = Msg->TryGetField(TEXT("wheelIndex"));
		if (WIdx.IsValid() && WIdx->Type == EJson::Number) WheelIndex = (int32)WIdx->AsNumber();
		SetFixtureGoboRotation(Id, (float)Speed, WheelIndex);
		return true;
	}
	if (Type == TEXT("SetFixtureAnimationRotation"))
	{
		// v1.0.50: animation-wheel rotation, signed normalised [-1..1]; same sign+stop convention
		// as SetFixtureGoboRotation. Epic's reference materials only model one rotating disc, so
		// the visualiser folds this into the gobo MID's rotation as a best-effort fallback (logged
		// once as a Warning per fixture on first non-zero apply).
		double Speed = 0.0; RebusJson::TryGetNumber(Msg, TEXT("speed"), Speed);
		SetFixtureAnimationRotation(Id, (float)Speed);
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
