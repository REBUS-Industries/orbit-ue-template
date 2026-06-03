// Copyright REBUS Industries.
//
// Maps a FixtureId (Speckle node id) -> spawned ARebusFixtureActor and applies the inbound
// per-fixture control descriptors (ue-plugin-build-guide.md §2/§5.2/§5.3). There is no id
// translation: the registry key is exactly the id every descriptor carries.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "RebusFixtureControlSubsystem.generated.h"

class ARebusFixtureActor;
class FJsonObject;

UCLASS()
class REBUSVISUALISER_API URebusFixtureControlSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// Registry.
	void RegisterFixture(const FString& NodeId, ARebusFixtureActor* Actor);
	void UnregisterFixture(const FString& NodeId);
	void Reset();
	ARebusFixtureActor* FindFixture(const FString& NodeId) const;
	int32 NumFixtures() const { return Fixtures.Num(); }
	TArray<FString> GetFixtureIds() const;

	// Typed control (the wire field for the first is "intensity"; the function is "Dimmer").
	void SetFixtureDimmer(const FString& Id, float Intensity01, float FadeSeconds = 0.f);
	void SetFixtureColor(const FString& Id, const FLinearColor& Srgb, float FadeSeconds = 0.f);
	void SetFixturePanTilt(const FString& Id, float PanDeg, float TiltDeg, float FadeSeconds = 0.f);
	void SetFixtureZoom(const FString& Id, float ZoomDeg, float FadeSeconds = 0.f);
	void SetFixtureGobo(const FString& Id, int32 GoboIndex, bool bHasIndex, int32 WheelIndex = INDEX_NONE, const FString& Wheel = FString(), float FadeSeconds = 0.f);
	void SetFixtureIris(const FString& Id, float Iris01, float FadeSeconds = 0.f);
	void SetFixtureFocus(const FString& Id, float Focus01, float FadeSeconds = 0.f);
	void SetFixtureFrost(const FString& Id, float Frost01, float FadeSeconds = 0.f);
	void SetFixtureColorTemp(const FString& Id, float Kelvin);
	void SetFixtureShutter(const FString& Id, int32 Mode, float RateHz);
	void SetFixtureGoboRotation(const FString& Id, float Speed);
	void SetFixturePrism(const FString& Id, int32 Facets, float RotationDeg);
	void SetFixtureBeamVolumetrics(const FString& Id, float Intensity, bool bCastVolumetricShadow);

	// Selection highlight; empty Ids clears all (§5.3).
	void SelectFixtures(const TArray<FString>& Ids, const FString& PrimaryId);
	const TArray<FString>& GetCurrentSelection() const { return CurrentSelection; }
	const FString& GetPrimarySelection() const { return PrimarySelection; }

	// Parse + route a per-fixture or selection descriptor. Returns true if the type was one
	// this subsystem owns (so the caller can fall through to scene-property handling).
	bool HandleControlDescriptor(const FString& Type, const TSharedPtr<FJsonObject>& Msg);

private:
	UPROPERTY() TMap<FString, TObjectPtr<ARebusFixtureActor>> Fixtures;
	TArray<FString> CurrentSelection;
	FString PrimarySelection;
};
