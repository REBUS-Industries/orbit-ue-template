// Copyright REBUS Industries.
//
// v1.0.79 cinematic camera pawn -- replaces UE's default ADefaultPawn so the streamed view is
// a UCineCameraComponent (focal length, aperture, focus distance, sensor) instead of the
// stock UCameraComponent. Drives manual exposure by default (auto-exposure was rebalancing
// the scene every time lights toggled and made fade transitions look like the camera was
// "adjusting" rather than the show changing).
//
// Why subclass ADefaultPawn rather than build from scratch:
//   * ADefaultPawn ships a UFloatingPawnMovement + a USphereComponent and self-binds WASD +
//     mouse-look in SetupPlayerInputComponent. Subclassing keeps all of that for free.
//   * ADefaultPawn in UE 5.7 has NO UCameraComponent (the engine default pawn drives its
//     view straight from the controller's control rotation, without a component). So we just
//     create a UCineCameraComponent fresh and attach it to the collision sphere; APawn's
//     bFindCameraComponentWhenViewTarget = true default makes the camera manager auto-pick
//     our cine component when this pawn is the view target -- no SetViewTarget call needed.
//
// Portal control: URebusVisualiserSubsystem reads/writes the cine settings on the pawn via the
// Set/GetCineSettings + ApplyTransform/GetCameraState pairs below. Per-frame transform stream
// (CameraState) is sampled from GetCameraState every tick, throttled to ~30Hz, and only
// broadcast when something moved beyond a small dead zone (so a stationary camera doesn't
// flood the data channel).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/DefaultPawn.h"
#include "RebusCineCameraPawn.generated.h"

class UCineCameraComponent;

// Snapshot of every camera property the portal can read / write. Lives outside the actor so
// the subsystem can build it in one shot for outbound events without exposing the pawn
// internals (and so the threshold-gated periodic broadcast can hash the struct cheaply to
// detect "anything actually changed since the last send").
USTRUCT()
struct REBUSVISUALISER_API FRebusCameraState
{
	GENERATED_BODY()

	UPROPERTY() FVector Location = FVector::ZeroVector;       // cm, UE world space
	UPROPERTY() FRotator Rotation = FRotator::ZeroRotator;    // pitch / yaw / roll, degrees
	UPROPERTY() float FocalLengthMm = 35.f;                   // current lens focal length
	UPROPERTY() float Aperture = 2.8f;                        // f-stop (current iris)
	UPROPERTY() float FocusDistanceCm = 500.f;                // manual focus distance (cm)
	UPROPERTY() bool bManualFocus = true;                     // false -> auto-focus (tracking)
	UPROPERTY() float ExposureBiasEv = 0.f;                   // EV bias under manual exposure
	UPROPERTY() float SensorWidthMm = 24.89f;                 // Super35 default
	UPROPERTY() float SensorHeightMm = 18.66f;                // Super35 default
};

UCLASS()
class REBUSVISUALISER_API ARebusCineCameraPawn : public ADefaultPawn
{
	GENERATED_BODY()

public:
	ARebusCineCameraPawn(const FObjectInitializer& ObjectInitializer);

	UCineCameraComponent* GetCineCamera() const { return CineCamera; }

	// ---- Portal-facing setters (all checked against the cine component on apply) ----
	void SetFocalLengthMm(float Mm);
	void SetAperture(float FStop);
	void SetFocusDistanceCm(float Cm);
	void SetManualFocus(bool bManual);
	void SetExposureBiasEv(float Ev);
	void SetSensorSizeMm(float WidthMm, float HeightMm);
	void ApplyTransform(const FVector& Loc, const FRotator& Rot);

	// Snapshot the entire camera state. Cheap to call every frame -- just reads the cine
	// component's UPROPERTYs into FRebusCameraState.
	FRebusCameraState GetCameraState() const;

	// Reset all cine settings to the v1.0.79 defaults (Super35 35mm f/2.8 focus@5m, manual
	// exposure 0 EV). Used by the Rebus.CameraReset console command.
	void ResetToDefaults();

private:
	// The CineCamera is attached to ADefaultPawn's collision sphere in the ctor and marked
	// bUsePawnControlRotation = true, so mouse-look (which writes the PC's control rotation)
	// drives the camera's pitch + yaw. APawn::bFindCameraComponentWhenViewTarget defaults to
	// true, so the player camera manager picks this component as the view target without an
	// explicit SetViewTarget call.
	UPROPERTY() TObjectPtr<UCineCameraComponent> CineCamera = nullptr;
};
