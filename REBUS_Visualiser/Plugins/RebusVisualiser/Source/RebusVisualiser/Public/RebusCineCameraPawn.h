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
#include "Math/RotationMatrix.h"
#include "RebusCineCameraPawn.generated.h"

class UCineCameraComponent;

// v1.0.100 -- shared construction-time landing pose for the cinematic camera. Authored in
// metres (operator framing convention) and converted to UE world units (cm) by the literals
// below. Two files consume these (URebusVisualiserSubsystem::TryPositionPlayerView spawn +
// ARebusCineCameraPawn::ResetToDefaults), so they live in the header as `inline const` to
// keep one storage instance across TUs (C++17 inline variables; UE 5.7 compiles C++20).
//
// Aim is DERIVED at runtime from `(target - location).GetSafeNormal()` via
// FRotationMatrix::MakeFromX (UE's standard "build a rotation from a forward vector"
// helper) so any future tweak to the metres triples re-derives pitch/yaw/roll cleanly --
// nobody has to redo the trig by hand.
//
// Current values (v1.0.100): location (0, -20, 2) m = (0, -2000, 200) cm; target (0, 0, 5) m
// = (0, 0, 500) cm; derived rotator (pitch +8.53°, yaw 90°, roll 0°) -- a 20 m back-off
// looking gently up at the centre of stage 5 m above the deck.
//
// Portal control: this is JUST the construction-time / Rebus.CameraReset landing pose. The
// portal can still drive the live camera transform via SetCameraTransform at any time --
// the snapshot the portal reads back through SceneState always reflects the LIVE pawn pose,
// not these constants.
namespace RebusCineCameraDefaults
{
	// (0, -20, 2) m in cm -- 20 m back along -Y, 2 m up. Operator-tested neutral framing
	// position behind a centred stage.
	inline const FVector kDefaultCameraLocation_cm = FVector(0.f, -2000.f, 200.f);

	// (0, 0, 5) m in cm -- centre of stage, 5 m above the deck. Roughly the height of a
	// performer's head on a small riser; gives the derived aim a gentle look-up.
	inline const FVector kDefaultCameraTarget_cm = FVector(0.f, 0.f, 500.f);

	// Derived forward-vector rotation. `FRotationMatrix::MakeFromX` builds a basis whose +X
	// is the supplied forward (UE convention: actor +X = control-rotation forward), with a
	// sensible up. `.Rotator()` extracts pitch/yaw/roll in degrees. Today the math comes out
	// to (pitch +8.53°, yaw 90°, roll 0°); if the metres triples change above this updates
	// automatically on the next program start (function-local static, lazy-initialised on
	// first read; cheap -- one trig call ever).
	inline const FRotator kDefaultCameraRotation = FRotationMatrix::MakeFromX(
		(kDefaultCameraTarget_cm - kDefaultCameraLocation_cm).GetSafeNormal()).Rotator();
}

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
	UPROPERTY() float ExposureBiasEv = 10.f;                  // EV bias under manual exposure (v1.0.96 default +10 EV; see RebusCineCameraPawn.cpp ctor)
	UPROPERTY() float SensorWidthMm = 23.76f;                 // v1.0.98 default: 16:9 DSLR sensor (matches RebusCineCameraPawn ctor + ResetToDefaults)
	UPROPERTY() float SensorHeightMm = 13.365f;               // v1.0.98 default: 16:9 DSLR sensor (matches RebusCineCameraPawn ctor + ResetToDefaults)
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

	// Reset all cine settings to the construction-time defaults (35mm f/2.8 focus@5m,
	// 16:9 DSLR sensor [v1.0.98], manual exposure +10 EV [v1.0.96]). v1.0.100 also resets
	// the actor transform to the shared RebusCineCameraDefaults::kDefaultCameraLocation_cm
	// / kDefaultCameraRotation landing pose so the operator-side reset returns to the same
	// place TryPositionPlayerView spawned us at. Used by the Rebus.CameraReset console
	// command.
	void ResetToDefaults();

private:
	// The CineCamera is attached to ADefaultPawn's collision sphere in the ctor and marked
	// bUsePawnControlRotation = true, so mouse-look (which writes the PC's control rotation)
	// drives the camera's pitch + yaw. APawn::bFindCameraComponentWhenViewTarget defaults to
	// true, so the player camera manager picks this component as the view target without an
	// explicit SetViewTarget call.
	UPROPERTY() TObjectPtr<UCineCameraComponent> CineCamera = nullptr;
};
