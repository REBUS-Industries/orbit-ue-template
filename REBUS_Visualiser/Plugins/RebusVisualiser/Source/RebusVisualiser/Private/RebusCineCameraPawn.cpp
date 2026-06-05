// Copyright REBUS Industries.
#include "RebusCineCameraPawn.h"
#include "RebusVisualiserLog.h"

#include "CineCameraComponent.h"
#include "Components/SphereComponent.h"
#include "GameFramework/DefaultPawn.h"

ARebusCineCameraPawn::ARebusCineCameraPawn(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// ADefaultPawn in UE 5.7 has no UCameraComponent (the engine default pawn drives its view
	// straight from the controller's control rotation). We just add a fresh UCineCameraComponent
	// attached to ADefaultPawn's collision sphere and rely on APawn's
	// bFindCameraComponentWhenViewTarget=true default to make the camera manager auto-pick it
	// as the view target when this pawn is possessed.
	CineCamera = CreateDefaultSubobject<UCineCameraComponent>(TEXT("CineCamera"));
	if (USphereComponent* Sphere = GetCollisionComponent())
	{
		CineCamera->SetupAttachment(Sphere);
	}
	// Mouse-look writes the controller's control rotation; this flag makes the camera follow
	// it for pitch + yaw, identical to what UCameraComponent does on the stock DefaultPawn.
	CineCamera->bUsePawnControlRotation = true;

	// v1.0.79 cine defaults: Super35 sensor + 35mm f/2.8 prime + focus@5m, manual exposure
	// 0 EV. Super35 is the cinema-camera-equivalent crop most modern film cameras use; 35mm
	// f/2.8 is a neutral "see the whole stage but not wide" reference that matches what the
	// portal's preview thumbnail expects. Focus@5m keeps a typical close-mid stage target in
	// the depth of field at f/2.8 without bokeh-blurring everything behind it.
	CineCamera->SetCurrentFocalLength(35.f);
	CineCamera->SetCurrentAperture(2.8f);
	CineCamera->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
	CineCamera->FocusSettings.ManualFocusDistance = 500.f; // cm

	CineCamera->Filmback.SensorWidth  = 24.89f;
	CineCamera->Filmback.SensorHeight = 18.66f;

	// Manual exposure (the key bit the user asked for). Without these overrides, UE auto-
	// exposure rebalances every time the lights cut on/off, which reads to the audience as
	// the CAMERA adapting rather than the SHOW changing -- destroys the dramatic effect of
	// a blackout, masks the user's actual EV-based grading on the portal, and visually
	// interacts with the v1.0.78 Lumen fast-response pack (the GI cuts instantly but the
	// camera then slowly re-exposes, so the audience sees a fade anyway).
	CineCamera->PostProcessSettings.bOverride_AutoExposureMethod = true;
	CineCamera->PostProcessSettings.AutoExposureMethod = AEM_Manual;
	CineCamera->PostProcessSettings.bOverride_AutoExposureBias = true;
	CineCamera->PostProcessSettings.AutoExposureBias = 0.f;
	// Clamp manual EV so portal slider extremes don't crush blacks or blow highlights to
	// pure white -- the cine camera respects these clamps internally.
	CineCamera->PostProcessSettings.bOverride_AutoExposureMinBrightness = true;
	CineCamera->PostProcessSettings.AutoExposureMinBrightness = 0.f;
	CineCamera->PostProcessSettings.bOverride_AutoExposureMaxBrightness = true;
	CineCamera->PostProcessSettings.AutoExposureMaxBrightness = 0.f;
}

void ARebusCineCameraPawn::SetFocalLengthMm(float Mm)
{
	if (!CineCamera) return;
	CineCamera->SetCurrentFocalLength(FMath::Clamp(Mm, 4.f, 1000.f));
}

void ARebusCineCameraPawn::SetAperture(float FStop)
{
	if (!CineCamera) return;
	CineCamera->SetCurrentAperture(FMath::Clamp(FStop, 1.f, 32.f));
}

void ARebusCineCameraPawn::SetFocusDistanceCm(float Cm)
{
	if (!CineCamera) return;
	CineCamera->FocusSettings.ManualFocusDistance = FMath::Max(1.f, Cm);
}

void ARebusCineCameraPawn::SetManualFocus(bool bManual)
{
	if (!CineCamera) return;
	CineCamera->FocusSettings.FocusMethod = bManual ? ECameraFocusMethod::Manual : ECameraFocusMethod::Tracking;
}

void ARebusCineCameraPawn::SetExposureBiasEv(float Ev)
{
	if (!CineCamera) return;
	CineCamera->PostProcessSettings.bOverride_AutoExposureBias = true;
	CineCamera->PostProcessSettings.AutoExposureBias = FMath::Clamp(Ev, -10.f, 10.f);
}

void ARebusCineCameraPawn::SetSensorSizeMm(float WidthMm, float HeightMm)
{
	if (!CineCamera) return;
	CineCamera->Filmback.SensorWidth  = FMath::Clamp(WidthMm, 1.f, 100.f);
	CineCamera->Filmback.SensorHeight = FMath::Clamp(HeightMm, 1.f, 100.f);
}

void ARebusCineCameraPawn::ApplyTransform(const FVector& Loc, const FRotator& Rot)
{
	SetActorLocationAndRotation(Loc, Rot);
	// Drive the controller's control rotation too -- ADefaultPawn binds mouse-look to it, so
	// the next mouse delta would otherwise yank the view back to the controller's stale yaw.
	if (AController* C = GetController())
	{
		C->SetControlRotation(Rot);
	}
}

FRebusCameraState ARebusCineCameraPawn::GetCameraState() const
{
	FRebusCameraState S;
	S.Location = GetActorLocation();
	S.Rotation = GetActorRotation();
	if (CineCamera)
	{
		S.FocalLengthMm = CineCamera->CurrentFocalLength;
		S.Aperture = CineCamera->CurrentAperture;
		S.FocusDistanceCm = CineCamera->FocusSettings.ManualFocusDistance;
		S.bManualFocus = (CineCamera->FocusSettings.FocusMethod == ECameraFocusMethod::Manual);
		S.ExposureBiasEv = CineCamera->PostProcessSettings.bOverride_AutoExposureBias
			? CineCamera->PostProcessSettings.AutoExposureBias : 0.f;
		S.SensorWidthMm = CineCamera->Filmback.SensorWidth;
		S.SensorHeightMm = CineCamera->Filmback.SensorHeight;
	}
	return S;
}

void ARebusCineCameraPawn::ResetToDefaults()
{
	if (!CineCamera) return;
	CineCamera->SetCurrentFocalLength(35.f);
	CineCamera->SetCurrentAperture(2.8f);
	CineCamera->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
	CineCamera->FocusSettings.ManualFocusDistance = 500.f;
	CineCamera->Filmback.SensorWidth = 24.89f;
	CineCamera->Filmback.SensorHeight = 18.66f;
	CineCamera->PostProcessSettings.bOverride_AutoExposureMethod = true;
	CineCamera->PostProcessSettings.AutoExposureMethod = AEM_Manual;
	CineCamera->PostProcessSettings.bOverride_AutoExposureBias = true;
	CineCamera->PostProcessSettings.AutoExposureBias = 0.f;
	UE_LOG(LogRebusVisualiser, Log, TEXT("RebusCineCameraPawn: reset to v1.0.79 defaults (35mm f/2.8 focus@5m Super35 manual EV0)."));
}
