// Copyright REBUS Industries.
//
// Runtime IES (IESNA LM-63) -> UTextureLightProfile loader (ue-plugin-build-guide.md §8.2).
//
// We keep the portal's brightness authority: the returned profile is meant to be assigned
// with bUseIESBrightness = false and IESBrightnessScale = 1.0 so it only reshapes the spatial
// falloff -- the flux-derived intensity + operator dimmer stay in charge of overall output.
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
	UTextureLightProfile* BuildLightProfile(UObject* Outer, const TArray<uint8>& IesBytes);
}
