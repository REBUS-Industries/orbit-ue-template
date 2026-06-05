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

	// v1.0.79 cine defaults: 35mm f/2.8 prime + focus@5m, manual exposure 0 EV. 35mm
	// f/2.8 is a neutral "see the whole stage but not wide" reference that matches what
	// the portal's preview thumbnail expects. Focus@5m keeps a typical close-mid stage
	// target in the depth of field at f/2.8 without bokeh-blurring everything behind it.
	CineCamera->SetCurrentFocalLength(35.f);
	CineCamera->SetCurrentAperture(2.8f);
	CineCamera->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
	CineCamera->FocusSettings.ManualFocusDistance = 500.f; // cm

	// v1.0.98: default sensor flipped from Super35 (24.89mm x 18.66mm, ~4:3 cinematic) to
	// UE 5.7's "16:9 DSLR" preset (23.76mm x 13.365mm, exact 16:9 aspect = 1.778). Operator
	// requested the camera sensor match the live-streaming aspect by default so the previs
	// and the streamed surface frame the same content without operator-side correction.
	// 16:9 DSLR is preferred over the alternate "16:9 Digital Film" preset because it's the
	// more common live-streaming reference (DSLR/mirrorless bodies feeding studio multiviewers).
	// Portal still overrides per-shot via SetSensorSizeMm / the SetCameraSensor wire descriptor;
	// this is JUST the construction-time seed the operator sees before any portal push lands.
	CineCamera->Filmback.SensorWidth        = 23.76f;
	CineCamera->Filmback.SensorHeight       = 13.365f;
	// SensorAspectRatio is a stored field on FCameraFilmbackSettings in UE 5.7 (not derived
	// at read time). Set it explicitly so a SceneState read-back / portal query reports the
	// canonical 16:9 (1.778) value without waiting for a Width/Height edit to recompute it.
	CineCamera->Filmback.SensorAspectRatio  = 1.778f;

	// Manual exposure (the key bit the user asked for). Without these overrides, UE auto-
	// exposure rebalances every time the lights cut on/off, which reads to the audience as
	// the CAMERA adapting rather than the SHOW changing -- destroys the dramatic effect of
	// a blackout, masks the user's actual EV-based grading on the portal, and visually
	// interacts with the v1.0.78 Lumen fast-response pack (the GI cuts instantly but the
	// camera then slowly re-exposes, so the audience sees a fade anyway).
	//
	// v1.0.96: AutoExposureBias DEFAULT lifted from 0 EV to +10 EV. Rationale: live previs
	// in the pixel-streaming context runs unattended without an auto-exposure ramp, and dim
	// stage lights wash out on a flat exposure -- +10 EV is the operator-tested landing value
	// that keeps the visible IES pool bright on first spawn. The portal can still push any EV
	// via SetCameraExposure, and the SceneState read-back reflects the override; this is JUST
	// the construction-time seed the operator sees before any portal push lands.
	CineCamera->PostProcessSettings.bOverride_AutoExposureMethod = true;
	CineCamera->PostProcessSettings.AutoExposureMethod = AEM_Manual;
	CineCamera->PostProcessSettings.bOverride_AutoExposureBias = true;
	CineCamera->PostProcessSettings.AutoExposureBias = 10.f;
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
		// v1.0.96 -- when the override flag is off we report the construction-time default
		// (10 EV) instead of 0 so a SceneState read-back round-trips the new landing value
		// even before the operator pushes one explicitly. The ctor sets bOverride to true so
		// in normal usage this fallback never fires; it's only reached if some out-of-process
		// caller turned the override off (today: nobody does).
		S.ExposureBiasEv = CineCamera->PostProcessSettings.bOverride_AutoExposureBias
			? CineCamera->PostProcessSettings.AutoExposureBias : 10.f;
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
	// v1.0.98 -- ResetToDefaults lands on the 16:9 DSLR sensor (23.76mm x 13.365mm) so the
	// reset matches the construction-time default. See the ctor doc-comment for the rationale
	// (live-streaming aspect parity).
	CineCamera->Filmback.SensorWidth       = 23.76f;
	CineCamera->Filmback.SensorHeight      = 13.365f;
	CineCamera->Filmback.SensorAspectRatio = 1.778f;
	CineCamera->PostProcessSettings.bOverride_AutoExposureMethod = true;
	CineCamera->PostProcessSettings.AutoExposureMethod = AEM_Manual;
	CineCamera->PostProcessSettings.bOverride_AutoExposureBias = true;
	// v1.0.96 -- ResetToDefaults now lands on +10 EV to match the construction-time default.
	// See the ctor doc-comment for the rationale (live-previs pixel-streaming brightness).
	CineCamera->PostProcessSettings.AutoExposureBias = 10.f;

	// v1.0.100 -- also snap the ACTOR TRANSFORM back to the shared construction-time landing
	// pose (location (0,-20,2) m looking at (0,0,5) m, aim derived in
	// RebusCineCameraDefaults). Pre-v1.0.100 the reset only touched cine settings, so a
	// portal-side Rebus.CameraReset after the operator dollied the camera left it sitting
	// wherever they last parked it -- the reset matched the lens but not the framing, which
	// confused operators expecting "reset = back to spawn pose". We also drive the
	// controller's control rotation to the same yaw/pitch (mirrors ApplyTransform) so the
	// next mouse delta doesn't yank the view back to a stale yaw, and the next
	// BroadcastCameraStateIfChanged read-back ships the new pose to the portal.
	const FVector&  ResetLocation = RebusCineCameraDefaults::kDefaultCameraLocation_cm;
	const FRotator& ResetRotation = RebusCineCameraDefaults::kDefaultCameraRotation;
	SetActorLocationAndRotation(ResetLocation, ResetRotation);
	if (AController* C = GetController())
	{
		C->SetControlRotation(ResetRotation);
	}

	UE_LOG(LogRebusVisualiser, Log, TEXT("RebusCineCameraPawn: reset to v1.0.100 defaults (35mm f/2.8 focus@5m 16:9 DSLR manual EV+10, transform=%s facing %s)."),
		*ResetLocation.ToString(), *ResetRotation.ToString());
}
