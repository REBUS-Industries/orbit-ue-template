// Copyright REBUS Industries.
//
// Runtime IES (IESNA LM-63) -> UTextureLightProfile loader (ue-plugin-build-guide.md §8.2).
//
// We keep the portal's brightness authority for the IES SPATIAL DISTRIBUTION: the returned
// profile is meant to be assigned with bUseIESBrightness = false and IESBrightnessScale = 1.0
// so the texture only reshapes the spotlight's spatial falloff. The PEAK CANDELA parsed from
// the .ies file (`OutCandelaMax`, v1.0.91) is surfaced separately so the caller can fold it
// into `SpotLight->Intensity` (units = Candelas) as the BASE candela value -- multiplied by
// the live operator dimmer + shutter-gate. Together the two paths give us "the IES file's
// photometrics define BOTH the shape (texture) and the absolute peak brightness (intensity)"
// without UE's `bUseIESBrightness` interpretation -- which would also bake the candela max
// into the texture sample at the brightest point, losing the dimmer-as-multiplier story.
//
// NOTE: Runtime IES construction relies on FIESConverter (Engine module, IESConverter.h),
// which is the same loader the editor import path uses. If a future engine drop changes that
// surface, this is the one spot to adjust.
#pragma once

#include "CoreMinimal.h"

class UTextureLightProfile;

namespace RebusIes
{
	// Build a UTextureLightProfile from raw .ies bytes. Returns nullptr on failure.
	// Outer should be a long-lived UObject (e.g. the owning subsystem) so the profile isn't
	// GC'd while assigned to a light.
	//
	// v1.0.91 -- when `OutCandelaMax` is non-null, the parsed PEAK CANDELA from the .ies
	// file (`FIESConverter::GetBrightness() * GetMultiplier()`) is written to it on success.
	// Sentinel < 0 means parse failed / no IES; the caller treats that as "fall back to the
	// flux-derived BaseCandela". The multiplier is folded in because UE's importer applies
	// it to `UTextureLightProfile::Brightness` for the editor-asset path (so the peak the
	// fixture should reach with dimmer=1 is `Brightness * Multiplier`), keeping the runtime
	// path numerically identical to the cooked / -game-with-editor-data path.
	UTextureLightProfile* BuildLightProfile(UObject* Outer, const TArray<uint8>& IesBytes,
		float* OutCandelaMax = nullptr);
}
