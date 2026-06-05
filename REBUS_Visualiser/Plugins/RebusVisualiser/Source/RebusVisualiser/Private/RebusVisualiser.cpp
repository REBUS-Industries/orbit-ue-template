// Copyright REBUS Industries. RebusVisualiser module implementation.
#include "RebusVisualiser.h"
#include "RebusVisualiserLog.h"
#include "RebusFixtureControlSubsystem.h"
#include "RebusFixtureActor.h"
#include "RebusSceneSettingsSubsystem.h"
#include "RebusVisualiserSubsystem.h"
#include "RebusCineCameraPawn.h"

#include "CineCameraComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Components/LightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"
#include "Misc/CoreDelegates.h"

DEFINE_LOG_CATEGORY(LogRebusVisualiser);

#define LOCTEXT_NAMESPACE "FRebusVisualiserModule"

namespace
{
	IConsoleCommand* GDriveOrbitModelsCommand = nullptr;
	IConsoleCommand* GMeshBeamsCommand = nullptr;
	IConsoleCommand* GDumpFixtureLightsCommand = nullptr;
	IConsoleCommand* GShowOrbitFixturesCommand = nullptr;
	IConsoleCommand* GShowOrbitCommand = nullptr;
	IConsoleCommand* GOverrideFixtureMaterialsCommand = nullptr;
	IConsoleCommand* GGoboAntiGhostCommand = nullptr;
	IConsoleCommand* GDumpGoboStateCommand = nullptr;
	IConsoleCommand* GGoboRTSizeCommand = nullptr;
	IConsoleCommand* GDLSSCommand = nullptr;
	IConsoleCommand* GLumenFastResponseCommand = nullptr;
	IConsoleCommand* GCameraSnapshotCommand = nullptr;
	IConsoleCommand* GCameraResetCommand = nullptr;
	IConsoleCommand* GCameraStreamStatusCommand = nullptr;
	IConsoleCommand* GSendCameraStateCommand = nullptr;
	IConsoleCommand* GAAModeCommand = nullptr;
	IConsoleCommand* GOverrideTrussMaterialCommand = nullptr;
	IConsoleCommand* GSetGroundTilingCommand = nullptr;
	IConsoleCommand* GDumpFixtureIesCommand = nullptr;
	IConsoleCommand* GBeamShadowCommand = nullptr;
	IConsoleCommand* GDumpBeamShadowCommand = nullptr;
	IConsoleCommand* GRebuildBeamMaterialCommand = nullptr; // v1.0.103 -- editor-only runtime regen
	IConsoleCommand* GOrbitCastShadowsCommand = nullptr;
	IConsoleCommand* GOrbitDoubleSidedCommand = nullptr; // v1.0.104 -- imported-primitive double-sided normalisation
	IConsoleCommand* GNaniteOrbitImportsCommand = nullptr; // v1.0.105 -- imported-mesh Nanite enable
	IConsoleCommand* GDumpOrbitNaniteCommand = nullptr;    // v1.0.105 -- per-mesh Nanite diagnostic dump
	// v1.0.101 -- per-fixture zoom / cone-mesh / SpotLight outer-cone runtime dump.
	IConsoleCommand* GDumpFixtureZoomCommand = nullptr;
	// v1.0.107 -- top-centre version watermark overlay (UDebugDrawService("Foreground")
	// canvas draw). Two commands: the visibility toggle (with a `status` arg that
	// prints the live state + cached version string + Y-margin) and the top-edge
	// margin tunable. See the doc-comment on URebusVisualiserSubsystem::
	// SetVersionWatermarkEnabled for the rationale.
	IConsoleCommand* GShowVersionCommand = nullptr;
	IConsoleCommand* GVersionWatermarkYCommand = nullptr;

	// Forward decl -- defined further down with the other arg parsers; the v1.0.73 anti-ghost
	// handler below uses it before its in-file definition.
	bool ParseBoolArg(const TArray<FString>& Args, bool bDefault);

	// v1.0.73: rotating-gobo ghosting mitigation.
	//
	// Symptom (user): "When the gobo is rotating fast we are getting ghosting. Is it a
	// historyweight?". Yes, exactly that. TSR (UE's default upscaler since 5.2) is a temporal
	// accumulator that relies on motion vectors to know "this pixel moved by N pixels last
	// frame, so go fetch the value from there in history". Animated light functions (our
	// rotating gobo, projected onto opaque floor/set geometry) violate the assumption: the
	// LIGHTING moves but the SURFACE does not, so the motion vector for the lit floor pixel
	// is zero. TSR sees "same pixel, different colour every frame" -> reads as flickering
	// shading -> the history rejection trails the new value as the pattern rotates. Result:
	// a smear/trail behind the rotating cookie.
	//
	// UE5 added a TSR rejection mode specifically for this -- introduced for club / disco /
	// stage lighting in 5.3 and refined through 5.5. The CVars:
	//   r.TSR.ShadingRejection.Flickering 1        // enable the flicker-aware rejection path
	//   r.TSR.ShadingRejection.Flickering.AdjustToFrameRate 1
	//                                              // scale the rejection threshold with fps so
	//                                              // a 30fps scene doesn't blur a real moving
	//                                              // shadow as "flicker"
	//   r.LightFunctionQuality 2                   // full-res light function (smoother
	//                                              // per-frame projection -> less for TSR to
	//                                              // reject in the first place)
	//
	// We push these on PostEngineInit (renderer module is loaded by then -- pushing earlier
	// just logs "CVar not registered") with priority ECVF_SetByGameOverride so per-scene
	// scalability overrides still win for diagnostics. We snapshot prior values into
	// GGoboAntiGhostPrior so the toggle can restore byte-exact. Default ON since v1.0.73.
	struct FCVarSnapshot { FString Name; int32 PriorInt = 0; bool bValid = false; };
	TArray<FCVarSnapshot> GGoboAntiGhostPrior;
	bool GGoboAntiGhostEnabled = false; // tracks the LIVE state, not the requested state

	// v1.0.78: shared snapshot-restore machinery for any "Rebus.*FastResponse" / antighost-style
	// CVar pack. ApplyCVarPack runs the pack defined by `Defs[]`, writes each to OnValue with
	// ECVF_SetByGameOverride priority, captures the prior string value into `PriorState[]` (one
	// snapshot entry per def, index-aligned), and on the OFF path restores byte-exact from the
	// snapshot. Stringified set/restore handles both int and float CVars uniformly.
	struct FCVarPackDef { const TCHAR* Name; float OnValue; };

	void ApplyCVarPack(
		bool bEnable,
		const TCHAR* Phase,
		const TCHAR* PackLabel,
		bool& bLiveStateRef,
		TArray<FCVarSnapshot>& PriorState,
		const FCVarPackDef* Defs,
		int32 NumDefs)
	{
		if (bEnable == bLiveStateRef) return;

		if (bEnable)
		{
			PriorState.Reset();
			for (int32 i = 0; i < NumDefs; ++i)
			{
				const FCVarPackDef& Def = Defs[i];
				IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Def.Name);
				FCVarSnapshot Snap;
				Snap.Name = Def.Name;
				if (CVar)
				{
					Snap.PriorInt = CVar->GetInt(); // int truncation is fine for the labels we push (0/1/2 + 0.6 etc.)
					Snap.bValid = true;
					CVar->Set(*FString::SanitizeFloat(Def.OnValue), ECVF_SetByGameOverride);
					UE_LOG(LogRebusVisualiser, Log,
						TEXT("%s ON [%s]: %s was=%d now=%s"),
						PackLabel, Phase, Def.Name, Snap.PriorInt, *CVar->GetString());
				}
				else
				{
					UE_LOG(LogRebusVisualiser, Log,
						TEXT("%s ON [%s]: %s NOT REGISTERED (renderer module not loaded yet, or CVar renamed in this engine version -- benign, the other CVars still apply)."),
						PackLabel, Phase, Def.Name);
				}
				PriorState.Add(Snap);
			}
		}
		else
		{
			for (const FCVarSnapshot& Snap : PriorState)
			{
				if (!Snap.bValid) continue;
				IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Snap.Name);
				if (!CVar) continue;
				const FString BeforeRestore = CVar->GetString();
				CVar->Set(Snap.PriorInt, ECVF_SetByGameOverride);
				UE_LOG(LogRebusVisualiser, Log,
					TEXT("%s OFF [%s]: %s was=%s restored=%s"),
					PackLabel, Phase, *Snap.Name, *BeforeRestore, *CVar->GetString());
			}
			PriorState.Reset();
		}
		bLiveStateRef = bEnable;
	}

	// CVar definitions for the anti-ghost pack. Each entry has a name and an "on" value; some
	// are int, some are float (kept as float here so the int-snapshot path is just the int
	// truncation of the prior float -- restore-via-Set on a float CVar accepts a stringified
	// value with no precision drama for our 1-decimal settings). v1.0.74 expands the v1.0.73
	// pack with three additions; see comments alongside each entry below.
	const FCVarPackDef GAntiGhostCVars[] = {
		// v1.0.73 baseline: TSR flicker-rejection mode + full-res light functions. Necessary
		// but not sufficient -- biases TSR to reject flickering pixels, but the underlying
		// temporal history weight is still high enough to leave a trail behind a fast cookie.
		{ TEXT("r.TSR.ShadingRejection.Flickering"),                  1.f },
		{ TEXT("r.TSR.ShadingRejection.Flickering.AdjustToFrameRate"),1.f },
		{ TEXT("r.LightFunctionQuality"),                             2.f },
		// v1.0.74 ADD: increase TSR history update rate so new frames stand more on their own
		// (less weight to historical samples). Default in UE 5.4-5.7 is 0.4 (history weighted
		// ~60% in the blend). 0.6 keeps enough history for clean static AA but cuts the trail
		// length on a fast-rotating cookie noticeably. Higher than 0.7 starts to introduce
		// sub-pixel shimmer on static high-frequency detail (truss meshes, set pieces).
		{ TEXT("r.TSR.History.UpdateRate"),                           0.6f },
		// v1.0.74 ADD: disable the LightFunctionAtlas global path. UE 5.5+ caches LF samples
		// into a single atlas to reduce shader permutations and improve perf -- great for
		// STATIC light functions, but for a dynamic per-frame-changing cookie (our rotating
		// GoboRT) the atlas refresh path has caused stale-sample artefacts that read as the
		// gobo "lagging" its true rotation. The comment in RebusFixtureActor.cpp:3063 notes
		// M_Light_Master isn't atlas-compatible AND the LF is forced through the legacy
		// deferred path when a gobo is active (bAllowMegaLights=0), but disabling the atlas
		// globally removes any chance the engine ever decides to route the LF through it for
		// some future-version atlas-compat heuristic. Cost: per-pixel LF eval cost for any
		// other (non-fixture) LF in the scene is slightly higher; acceptable, we don't have
		// other LFs in a stage scene.
		{ TEXT("r.LightFunctionAtlas.Enabled"),                       0.f },
	};

	void ApplyGoboAntiGhost(bool bEnable, const TCHAR* Phase)
	{
		ApplyCVarPack(bEnable, Phase, TEXT("GoboAntiGhost"),
			GGoboAntiGhostEnabled, GGoboAntiGhostPrior,
			GAntiGhostCVars, UE_ARRAY_COUNT(GAntiGhostCVars));
	}

	// v1.0.78: Lumen GI fast-response pack. User reported: "the ghosting is to do with Global
	// illumination and the camera. When we turn lights on and off there is a fade off of GI,
	// we want this instant." -- correct diagnosis. The fade and the residual gobo trail on
	// the floor are both Lumen temporal accumulation symptoms:
	//
	//   * Lights toggle: direct lighting flips instantly, but Lumen's screen-probe gather +
	//     radiosity history hold onto the previous lit state for many frames -> the floor
	//     glow fades out instead of cutting.
	//   * Rotating gobo: each frame Lumen samples the lit floor sparsely for indirect bounce;
	//     prior-frame samples linger in history -> ghost trail in the GI on TOP of any TSR
	//     smear (v1.0.73/74 fixed the TSR layer; this is the layer underneath).
	//
	// The fix is to disable Lumen's temporal filters so direct light changes propagate to GI
	// at full strength on the very next frame. Cost: noisier GI (the temporal accumulator was
	// hiding sparse-sampling noise) -- but for a stage lighting visualiser, instant response
	// trumps smoothness. The eye reads the slight grain as natural surface texture; the eye
	// reads a slow GI fade-off as "the lights aren't actually responding".
	//
	// CVars (UE 5.7 names):
	//   r.Lumen.ScreenProbeGather.Temporal 0    -- primary screen-probe temporal filter off
	//   r.Lumen.Reflections.Temporal 0          -- reflection temporal filter off (catches
	//                                              the gobo bounce reflected off shiny surfaces)
	//   r.Lumen.Radiosity.Temporal 0            -- radiosity (final-gather bounced light)
	//                                              temporal off
	//   r.LumenScene.SurfaceCache.RecaptureLightingPerFrame 1
	//                                           -- force per-frame surface cache lighting
	//                                              recapture (otherwise the surface cache
	//                                              ALSO holds stale direct-lighting samples
	//                                              for several frames -> compounds the fade)
	bool GLumenFastResponseEnabled = false;
	TArray<FCVarSnapshot> GLumenFastResponsePrior;
	const FCVarPackDef GLumenFastResponseCVars[] = {
		{ TEXT("r.Lumen.ScreenProbeGather.Temporal"),                       0.f },
		{ TEXT("r.Lumen.Reflections.Temporal"),                             0.f },
		{ TEXT("r.Lumen.Radiosity.Temporal"),                               0.f },
		{ TEXT("r.LumenScene.SurfaceCache.RecaptureLightingPerFrame"),      1.f },
	};

	void ApplyLumenFastResponse(bool bEnable, const TCHAR* Phase)
	{
		ApplyCVarPack(bEnable, Phase, TEXT("LumenFastResponse"),
			GLumenFastResponseEnabled, GLumenFastResponsePrior,
			GLumenFastResponseCVars, UE_ARRAY_COUNT(GLumenFastResponseCVars));
	}

	// v1.0.78: `Rebus.LumenFastResponse [0|1]` -- toggle the Lumen GI temporal-disable pack.
	// Default ON since v1.0.78 (auto-applied at PostEngineInit). Disable for A/B comparison
	// or for cinematic scenes where smooth GI matters more than instant light response.
	void HandleLumenFastResponseCommand(const TArray<FString>& Args)
	{
		const bool bEnable = ParseBoolArg(Args, true);
		ApplyLumenFastResponse(bEnable, TEXT("ConsoleCommand"));
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.LumenFastResponse %d -> live state %s."),
			bEnable ? 1 : 0, GLumenFastResponseEnabled ? TEXT("ON") : TEXT("OFF"));
	}

	// v1.0.74: `Rebus.DumpGoboState` -- per-fixture gobo runtime dump. Useful to confirm:
	//   * the GoboRT exists and bShouldClearRenderTargetOnReceiveUpdate is on
	//   * CombinedSpin matches the requested rotation speed (proves Tick is running)
	//   * SpotLight.bAllowMegaLights is 0 while the gobo is up (proves the LF flows through
	//     the legacy non-MegaLights path -- if 1, MegaLights' temporal denoiser is in the
	//     loop and would cause exactly the ghost trail symptom)
	void HandleDumpGoboStateCommand(const TArray<FString>& /*Args*/)
	{
		if (!GEngine) return;
		int32 Total = 0;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE && Ctx.WorldType != EWorldType::Editor)) continue;
			for (TActorIterator<ARebusFixtureActor> It(World); It; ++It)
			{
				if (const ARebusFixtureActor* F = *It)
				{
					F->DumpGoboStateForDebug();
					++Total;
				}
			}
		}
		UE_LOG(LogRebusVisualiser, Log, TEXT("Rebus.DumpGoboState: dumped %d fixture(s)."), Total);
	}

	// v1.0.91: `Rebus.DumpFixtureIes [fixtureId]` -- per-fixture IES runtime dump. Confirms
	// the full chain landed for the v1.0.91 "use the IES profile + IES intensity for the
	// SpotLight" change:
	//   * IES source (inline iesText vs URL vs synthetic-cone fallback) + profileId
	//   * zoomDmx that selected the active entry (paired with the live ZoomDeg.Current half-angle)
	//   * the live UTextureLightProfile UObject name (proves SpotLight->SetIESTexture landed)
	//   * the PEAK CANDELA parsed from the .ies file -- the BASE value that drives Intensity
	//   * SpotLight->IntensityUnits (must be `2` = Candelas for the cd values to be physically
	//     meaningful) + the live SpotLight->Intensity, plus the expected `base*dim*gate` value
	//     so the operator can tell at a glance whether dimmer / shutter-gate are zeroing the
	//     beam vs. the IES being missing
	// With NO fixtureId arg, dumps EVERY fixture in every Game/PIE/Editor world. With an
	// optional fixtureId arg (the Speckle node id -- the same key SetFixture* uses), dumps
	// just the matching fixture; logs a warning if not found.
	void HandleDumpFixtureIesCommand(const TArray<FString>& Args)
	{
		if (!GEngine) return;
		const FString Filter = (Args.Num() > 0) ? Args[0] : FString();
		int32 Total = 0, Matched = 0;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE && Ctx.WorldType != EWorldType::Editor)) continue;
			for (TActorIterator<ARebusFixtureActor> It(World); It; ++It)
			{
				const ARebusFixtureActor* F = *It;
				if (!F) continue;
				++Total;
				if (!Filter.IsEmpty() && !F->GetFixtureId().Equals(Filter, ESearchCase::IgnoreCase)) continue;
				F->DumpIesStateForDebug();
				++Matched;
			}
		}
		if (Filter.IsEmpty())
		{
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Rebus.DumpFixtureIes: dumped %d fixture(s) (no filter)."), Matched);
		}
		else if (Matched == 0)
		{
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("Rebus.DumpFixtureIes '%s': NOT FOUND (scanned %d fixture(s) -- check the Speckle node id, same key SetFixture* uses)."),
				*Filter, Total);
		}
		else
		{
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Rebus.DumpFixtureIes '%s': dumped %d matching fixture(s)."), *Filter, Matched);
		}
	}

	// v1.0.101: `Rebus.DumpFixtureZoom [fixtureId]` -- per-fixture zoom / cone-mesh /
	// SpotLight outer-cone runtime dump. Mirrors `Rebus.DumpFixtureIes`'s shape:
	//   * No arg -> dumps every Rebus fixture in every Game/PIE/Editor world.
	//   * Optional fixtureId arg (Speckle node id, the same key SetFixture* uses) ->
	//     dumps just the matching fixture; logs a warning if not found.
	// Each line carries the live ZoomDeg target, the GDTF zoom range from the profile,
	// the resolved canonical half-angle (ResolveZoomHalfDeg -- the SINGLE source of
	// truth shared by SpotLight outer cone + cone-mesh radius), the SpotLight's live
	// Outer/Inner cone angles + their ratio (the linear-taper light model that makes
	// the visible bright disc smaller than the geometric cone -- the v1.0.101 root
	// cause), the procedural cone-mesh BeamLength + last-built far-radius + expected
	// far-radius for the live state, the per-fixture BeamConeRadiusScale, the BeamMID's
	// live FarRadius scalar param (read back from the MID), and bUsingEpicBeam /
	// bMeshBeamEnabled / bGoboActive / iris status flags. Use this to verify the
	// `Rebus.BeamConeRadiusScale` knob actually landed on every fixture (not just the
	// CVar global) and that no portal push is fighting the per-fixture state.
	void HandleDumpFixtureZoomCommand(const TArray<FString>& Args)
	{
		if (!GEngine) return;
		const FString Filter = (Args.Num() > 0) ? Args[0] : FString();

		// One-line CVar header so the operator has the global value alongside the
		// per-fixture lines for diff. Mirrors `Rebus.DumpBeamShadow`'s pattern.
		IConsoleVariable* ScaleCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Rebus.BeamConeRadiusScale"));
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("DumpFixtureZoom CVars: Rebus.BeamConeRadiusScale=%s"),
			ScaleCVar ? *ScaleCVar->GetString() : TEXT("<unregistered>"));

		int32 Total = 0, Matched = 0;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE && Ctx.WorldType != EWorldType::Editor)) continue;
			for (TActorIterator<ARebusFixtureActor> It(World); It; ++It)
			{
				const ARebusFixtureActor* F = *It;
				if (!F) continue;
				++Total;
				if (!Filter.IsEmpty() && !F->GetFixtureId().Equals(Filter, ESearchCase::IgnoreCase)) continue;
				F->DumpFixtureZoomStateForDebug();
				++Matched;
			}
		}
		if (Filter.IsEmpty())
		{
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Rebus.DumpFixtureZoom: dumped %d fixture(s) (no filter)."), Matched);
		}
		else if (Matched == 0)
		{
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("Rebus.DumpFixtureZoom '%s': NOT FOUND (scanned %d fixture(s) -- check the Speckle node id, same key SetFixture* uses)."),
				*Filter, Total);
		}
		else
		{
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Rebus.DumpFixtureZoom '%s': dumped %d matching fixture(s)."), *Filter, Matched);
		}
	}

	// v1.0.79 helpers: pluck the live cinematic camera pawn from any game/PIE world. There's
	// only ever one (the visualiser subsystem spawns + possesses it in TryPositionPlayerView)
	// but iterating worlds makes the console commands work the same in PIE and packaged.
	ARebusCineCameraPawn* FindLiveCineCameraPawn()
	{
		if (!GEngine) return nullptr;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE)) continue;
			for (TActorIterator<ARebusCineCameraPawn> It(World); It; ++It)
			{
				if (ARebusCineCameraPawn* Cam = *It) return Cam;
			}
		}
		return nullptr;
	}

	// v1.0.79 `Rebus.CameraSnapshot` -- log the live cine camera state in one line. Useful
	// when the portal stream is choked (CameraState events not arriving) and the operator
	// wants a quick "where am I and what is the lens" sanity check without firing up a
	// proper debugger overlay.
	void HandleCameraSnapshotCommand(const TArray<FString>& /*Args*/)
	{
		ARebusCineCameraPawn* Cam = FindLiveCineCameraPawn();
		if (!Cam)
		{
			UE_LOG(LogRebusVisualiser, Warning, TEXT("Rebus.CameraSnapshot: no live RebusCineCameraPawn found (subsystem hasn't possessed yet?)."));
			return;
		}
		const FRebusCameraState S = Cam->GetCameraState();
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.CameraSnapshot: loc=(%.1f,%.1f,%.1f)cm rot=(P%.2f,Y%.2f,R%.2f)deg "
				 "focal=%.1fmm fStop=%.2f focus=%.1fcm (%s) EV=%.2f sensor=%.2fx%.2fmm"),
			S.Location.X, S.Location.Y, S.Location.Z,
			S.Rotation.Pitch, S.Rotation.Yaw, S.Rotation.Roll,
			S.FocalLengthMm, S.Aperture, S.FocusDistanceCm,
			S.bManualFocus ? TEXT("manual") : TEXT("auto"),
			S.ExposureBiasEv,
			S.SensorWidthMm, S.SensorHeightMm);
	}

	// v1.0.79 `Rebus.CameraReset` -- restore the cine pawn to factory defaults. Doesn't move
	// the camera (operators don't want to lose their framing); only re-applies the lens +
	// exposure defaults. To reset the framing too the operator can send SetCameraTransform
	// from the portal (or relaunch the stream).
	void HandleCameraResetCommand(const TArray<FString>& /*Args*/)
	{
		ARebusCineCameraPawn* Cam = FindLiveCineCameraPawn();
		if (!Cam)
		{
			UE_LOG(LogRebusVisualiser, Warning, TEXT("Rebus.CameraReset: no live RebusCineCameraPawn found."));
			return;
		}
		Cam->ResetToDefaults();
	}

	// v1.0.82 helper: pluck the live URebusVisualiserSubsystem from any active world. There
	// can only be one game-instance subsystem, but we iterate worlds so it works identically
	// in PIE + packaged.
	URebusVisualiserSubsystem* FindLiveVisualiserSubsystem()
	{
		if (!GEngine) return nullptr;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE)) continue;
			if (UGameInstance* GI = World->GetGameInstance())
			{
				if (URebusVisualiserSubsystem* Viz = GI->GetSubsystem<URebusVisualiserSubsystem>())
				{
					return Viz;
				}
			}
		}
		return nullptr;
	}

	// v1.0.82 `Rebus.CameraStreamStatus` -- dump the entire camera-stream chain in one log
	// line so the operator can diagnose "portal isn't receiving CameraState":
	//   1. Visualiser subsystem alive?     -> if no, no scene was ever started
	//   2. Cine pawn alive?                 -> if no, TryPositionPlayerView hasn't run yet
	//   3. Live snapshot of the pawn        -> proves what state WOULD ship out right now
	// Pair this with the existing data-channel "Sending 'CameraState' (Response, N players)"
	// log to confirm the message reached the wire AND that there's >=1 connected viewer.
	void HandleCameraStreamStatusCommand(const TArray<FString>& /*Args*/)
	{
		URebusVisualiserSubsystem* Viz = FindLiveVisualiserSubsystem();
		ARebusCineCameraPawn* Cam = Viz ? Viz->GetCineCameraPawn() : nullptr;
		if (!Cam) Cam = FindLiveCineCameraPawn(); // fallback in case Viz isn't ready

		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.CameraStreamStatus: subsystem=%s cinePawn=%s%s"),
			Viz ? TEXT("ALIVE") : TEXT("NULL (no game world / subsystem)"),
			Cam ? *Cam->GetName() : TEXT("NULL (TryPositionPlayerView hasn't spawned it yet)"),
			Cam ? TEXT("") : TEXT(" -- portal will receive zero CameraState until a PlayerController exists."));
		if (Cam)
		{
			const FRebusCameraState S = Cam->GetCameraState();
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Rebus.CameraStreamStatus: live snapshot loc=(%.1f,%.1f,%.1f) rot=(P%.2f,Y%.2f,R%.2f) "
					 "focal=%.1fmm fStop=%.2f focus=%.0fcm ev=%.2f sensor=%.2fx%.2fmm"),
				S.Location.X, S.Location.Y, S.Location.Z,
				S.Rotation.Pitch, S.Rotation.Yaw, S.Rotation.Roll,
				S.FocalLengthMm, S.Aperture, S.FocusDistanceCm, S.ExposureBiasEv,
				S.SensorWidthMm, S.SensorHeightMm);
		}
	}

	// v1.0.82 `Rebus.SendCameraState` -- force one CameraState onto the wire NOW (same as
	// the portal-side RequestCameraState descriptor). Useful for verifying the data channel
	// is delivering AT ALL without needing portal cooperation -- if this runs but no one
	// receives it, the failure is on the PS2 transport (no connected viewer, streamer-id
	// mismatch, frontend not listening for type:"CameraState"), not the broadcast layer.
	void HandleSendCameraStateCommand(const TArray<FString>& /*Args*/)
	{
		URebusVisualiserSubsystem* Viz = FindLiveVisualiserSubsystem();
		if (!Viz)
		{
			UE_LOG(LogRebusVisualiser, Warning, TEXT("Rebus.SendCameraState: visualiser subsystem null -- can't send."));
			return;
		}
		// Synthesise a RequestCameraState descriptor and route it through the existing handler,
		// so the diagnostic exactly matches what the portal would trigger.
		TSharedPtr<FJsonObject> Empty = MakeShared<FJsonObject>();
		const bool bHandled = Viz->HandleCameraDescriptor(TEXT("RequestCameraState"), Empty);
		UE_LOG(LogRebusVisualiser, Log, TEXT("Rebus.SendCameraState: dispatched (handled=%d) -- check the next 'Sending CameraState (Response, N players)' line in the log."), (int32)bHandled);
	}

	// v1.0.75: `Rebus.GoboRTSize <pixels>` -- rebuild every fixture's gobo render target at a
	// new square pixel size + enable mipmaps + trilinear filtering. Default since v1.0.75 is
	// 1024 (was 512). User asked for "increase the resolution of the gobos, they look
	// pixelated" -- this is the knob. Useful sizes:
	//   * 512  -- old default; lowest VRAM, alias on close throws / small fixtures
	//   * 1024 -- new default; 4x area, crisp at typical throws, ~6 MiB VRAM/fixture (RGBA8 + mips)
	//   * 2048 -- hero shows; ~25 MiB/fixture, virtually no aliasing even up close
	//   * 4096 -- max useful; ~100 MiB/fixture, mip-pyramid bandwidth becomes noticeable
	// Sizes are clamped to [128, 8192] and rounded up to the next pow2 (mip-chain alignment).
	void HandleGoboRTSizeCommand(const TArray<FString>& Args)
	{
		if (!GEngine) return;
		int32 Requested = 1024;
		if (Args.Num() > 0)
		{
			Requested = FCString::Atoi(*Args[0]);
		}
		int32 Total = 0, Resolved = 0;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE)) continue;
			for (TActorIterator<ARebusFixtureActor> It(World); It; ++It)
			{
				if (ARebusFixtureActor* F = *It)
				{
					const int32 R = F->RebuildGoboRTAtSize(Requested);
					if (R > 0) { Resolved = R; ++Total; }
				}
			}
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.GoboRTSize %d -> resolved=%d (pow2 clamp [128, 8192]); rebuilt %d fixture(s)."),
			Requested, Resolved, Total);
	}

	// v1.0.75: `Rebus.DLSS [off|quality|balanced|performance|ultraperformance|dlaa]` -- enable
	// the NVIDIA DLSS upscaler if the DLSS Streamline plugin is installed in this project.
	// Detection is by CVar presence (r.NGX.DLSS.Enable) -- if the plugin's module isn't
	// loaded, the CVar isn't registered and we log a friendly "DLSS plugin not installed"
	// note with install instructions. When present, the requested quality preset maps to
	// r.NGX.DLSS.Quality:
	//   off              -- r.NGX.DLSS.Enable 0 (falls back to TSR)
	//   quality          -- 2  (67% internal render scale; visually closest to native)
	//   balanced         -- 1  (58% scale)
	//   performance      -- 0  (50% scale)
	//   ultraperformance -- 3  (33% scale; aggressive, prefer for 4K+ output only)
	//   dlaa             -- enables DLAA (Deep Learning AA at native resolution, no upscaling)
	//                       via r.NGX.DLAA.Enable=1 instead of r.NGX.DLSS.Enable.
	// WARNING: DLSS uses temporal accumulation (same family as TSR), so the rotating-gobo
	// ghost-trail symptom v1.0.73/74 fixed for TSR can re-appear under DLSS. The GoboAntiGhost
	// pack stays on, but its r.TSR.* CVars don't affect DLSS's internal accumulator. If gobo
	// ghosting returns under DLSS, the practical mitigation is to drop DLAA (no upscale, less
	// accumulation) or fall back to TSR for gobo-heavy scenes.
	void HandleDLSSCommand(const TArray<FString>& Args)
	{
		const FString Preset = (Args.Num() > 0) ? Args[0].ToLower() : FString(TEXT("quality"));

		IConsoleVariable* DLSSEnable    = IConsoleManager::Get().FindConsoleVariable(TEXT("r.NGX.DLSS.Enable"));
		IConsoleVariable* DLSSQuality   = IConsoleManager::Get().FindConsoleVariable(TEXT("r.NGX.DLSS.Quality"));
		IConsoleVariable* DLAAEnable    = IConsoleManager::Get().FindConsoleVariable(TEXT("r.NGX.DLAA.Enable"));
		IConsoleVariable* NGXEnable     = IConsoleManager::Get().FindConsoleVariable(TEXT("r.NGX.Enable"));

		if (!DLSSEnable && !DLAAEnable)
		{
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("Rebus.DLSS '%s' -- DLSS plugin not installed (r.NGX.* CVars not registered). "
					 "To install: 1) Download the 'NVIDIA DLSS' plugin from https://developer.nvidia.com/rtx/dlss/get-started "
					 "(or the UE Marketplace 'DLSS' listing), 2) extract to REBUS_Visualiser/Plugins/DLSS, "
					 "3) add { \"Name\": \"DLSS\", \"Enabled\": true } to REBUS_Visualiser.uproject's Plugins array, "
					 "4) restart UE, 5) re-run this command. Requires NVIDIA RTX hardware (RTX 20-series or newer)."),
				*Preset);
			return;
		}

		if (NGXEnable) NGXEnable->Set(1, ECVF_SetByGameOverride); // master gate

		if (Preset == TEXT("off") || Preset == TEXT("0") || Preset == TEXT("disable"))
		{
			if (DLSSEnable) DLSSEnable->Set(0, ECVF_SetByGameOverride);
			if (DLAAEnable) DLAAEnable->Set(0, ECVF_SetByGameOverride);
			UE_LOG(LogRebusVisualiser, Log, TEXT("Rebus.DLSS off -- DLSS/DLAA disabled, falling back to engine upscaler (TSR if r.AntiAliasingMethod=4)."));
			return;
		}

		if (Preset == TEXT("dlaa"))
		{
			if (DLSSEnable) DLSSEnable->Set(0, ECVF_SetByGameOverride);
			if (DLAAEnable) DLAAEnable->Set(1, ECVF_SetByGameOverride);
			UE_LOG(LogRebusVisualiser, Log, TEXT("Rebus.DLSS dlaa -- DLAA enabled (deep-learning AA at native resolution, no upscale)."));
			return;
		}

		int32 QualityVal = 2; // default to "quality" mapping
		if      (Preset == TEXT("performance")      || Preset == TEXT("perf"))   QualityVal = 0;
		else if (Preset == TEXT("balanced")         || Preset == TEXT("bal"))    QualityVal = 1;
		else if (Preset == TEXT("quality")          || Preset == TEXT("qual"))   QualityVal = 2;
		else if (Preset == TEXT("ultraperformance") || Preset == TEXT("ultra")
			  || Preset == TEXT("ultra-performance")|| Preset == TEXT("up"))     QualityVal = 3;
		else
		{
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("Rebus.DLSS: unknown preset '%s'. Valid: off|quality|balanced|performance|ultraperformance|dlaa."),
				*Preset);
			return;
		}

		if (DLSSEnable)  DLSSEnable->Set(1, ECVF_SetByGameOverride);
		if (DLAAEnable)  DLAAEnable->Set(0, ECVF_SetByGameOverride);
		if (DLSSQuality) DLSSQuality->Set(QualityVal, ECVF_SetByGameOverride);

		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.DLSS '%s' -- DLSS enabled (r.NGX.DLSS.Quality=%d). NOTE: DLSS uses temporal accumulation; if rotating-gobo ghosting returns try Rebus.DLSS dlaa (no upscale) or Rebus.DLSS off (TSR fallback, where the GoboAntiGhost pack applies)."),
			*Preset, QualityVal);
	}

	// v1.0.73: `Rebus.GoboAntiGhost [0|1]` -- toggle the rotating-gobo ghosting mitigation
	// (TSR flicker-aware shading rejection + full-res light functions). Default OFF since
	// v1.0.83 (the auto-apply on PostEngineInit was removed -- this pack added more flicker
	// than it removed and didn't actually fix the underlying TSR pathology). Keep the
	// command for A/B testing.
	void HandleGoboAntiGhostCommand(const TArray<FString>& Args)
	{
		const bool bEnable = ParseBoolArg(Args, true);
		ApplyGoboAntiGhost(bEnable, TEXT("ConsoleCommand"));
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.GoboAntiGhost %d -> live state %s."),
			bEnable ? 1 : 0, GGoboAntiGhostEnabled ? TEXT("ON") : TEXT("OFF"));
	}

	// v1.0.83: `Rebus.AAMode [tsr|taa|fxaa|msaa|off]` -- pick the screen-space anti-aliasing
	// method. The fresh approach to the rotating-gobo ghost problem.
	//
	// Background: TSR (the default since UE 5.2) is a temporal upscaler that REPROJECTS the
	// previous frame's image via per-pixel motion vectors and blends it with the current
	// frame. Geometry that doesn't move has motion-vector = 0, so TSR fetches "the same pixel
	// from the last frame" as history. When an ANIMATED LIGHT FUNCTION (our rotating gobo
	// cookie) projects onto that stationary geometry, the lit colour changes per frame but the
	// geometry doesn't, so TSR keeps blending in stale lit colour from the trailing rotation
	// angle -> ghost trail. The user's diagnostic observation "if we move the camera, it
	// temporarily stops ghosting" is the proof: camera motion makes the floor's motion vector
	// non-zero, TSR reprojects to a DIFFERENT pixel in history (which had different lit
	// content), the colour-match check fails, history is rejected, ghost gone.
	//
	// TSR's shading-rejection heuristic (r.TSR.ShadingRejection.*) tries to detect this case
	// but can't distinguish "rotating gobo" from "noisy pixel" without lowering thresholds
	// globally -- which is exactly what the v1.0.73/74 GoboAntiGhost pack did, and exactly
	// what produced the noise/flickering side effect the user reported. TSR fundamentally
	// can't fix this case in software without surface-classification metadata it doesn't have.
	//
	// Workarounds, in order of preference for a stage visualiser:
	//   * TAA  (r.AntiAliasingMethod=2): older temporal AA, simpler history, more aggressive
	//          rejection. Trails MUCH less behind animated lights at the cost of slightly
	//          softer static AA. RECOMMENDED for shows where rotating gobos are featured.
	//   * TSR  (r.AntiAliasingMethod=4): UE5 default. Sharpest, best static AA, ghosts on
	//          rotating gobos. Use for shows without animated cookies.
	//   * FXAA (r.AntiAliasingMethod=1): per-frame edge filter, no history at all -> zero
	//          ghosting. Costs cookie sharpness across frames (no temporal accumulation
	//          benefit) and is jaggier on geometry edges. Acceptable on high-res output.
	//   * MSAA (r.AntiAliasingMethod=3): forward shading only -- DOES NOT WORK in this
	//          project's deferred renderer; we accept the argument and log that it falls
	//          back to no-AA. Listed for completeness.
	//   * off  (r.AntiAliasingMethod=0): no AA. Maximum ghost-freedom; visible aliasing.
	//
	// Values are pushed with ECVF_SetByGameOverride so per-scene scalability still wins.
	// Snapshot/restore: not needed; flipping AA mode is a single CVar write the engine
	// already handles via runtime scalability.
	void HandleAAModeCommand(const TArray<FString>& Args)
	{
		const FString Mode = (Args.Num() > 0) ? Args[0].ToLower() : FString(TEXT("status"));
		IConsoleVariable* AA = IConsoleManager::Get().FindConsoleVariable(TEXT("r.AntiAliasingMethod"));
		if (!AA)
		{
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("Rebus.AAMode: r.AntiAliasingMethod CVar not registered (renderer module not loaded?)."));
			return;
		}

		if (Mode == TEXT("status") || Mode == TEXT(""))
		{
			static const TCHAR* Names[] = { TEXT("off"), TEXT("fxaa"), TEXT("taa"), TEXT("msaa"), TEXT("tsr") };
			const int32 V = AA->GetInt();
			const TCHAR* Name = (V >= 0 && V < UE_ARRAY_COUNT(Names)) ? Names[V] : TEXT("?");
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Rebus.AAMode status: r.AntiAliasingMethod=%d (%s). Set with: Rebus.AAMode tsr|taa|fxaa|msaa|off"),
				V, Name);
			return;
		}

		int32 Target = -1;
		if      (Mode == TEXT("off") || Mode == TEXT("none") || Mode == TEXT("0"))            Target = 0;
		else if (Mode == TEXT("fxaa") || Mode == TEXT("1"))                                    Target = 1;
		else if (Mode == TEXT("taa") || Mode == TEXT("temporal") || Mode == TEXT("2"))         Target = 2;
		else if (Mode == TEXT("msaa") || Mode == TEXT("3"))                                    Target = 3;
		else if (Mode == TEXT("tsr") || Mode == TEXT("4"))                                     Target = 4;
		else
		{
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("Rebus.AAMode: unknown mode '%s'. Valid: tsr|taa|fxaa|msaa|off|status."), *Mode);
			return;
		}

		const FString Before = AA->GetString();
		AA->Set(Target, ECVF_SetByGameOverride);
		const FString After = AA->GetString();
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.AAMode '%s' -> r.AntiAliasingMethod %s -> %s. %s"),
			*Mode, *Before, *After,
			(Target == 2)
				? TEXT("(TAA selected -- recommended for rotating gobos; slightly softer static AA than TSR but no ghost trail.)")
				: (Target == 4)
					? TEXT("(TSR selected -- best static AA; rotating gobos on stationary geometry will trail because of TSR's temporal reprojection.)")
					: (Target == 1)
						? TEXT("(FXAA -- per-frame edge filter, no temporal history, zero ghosting, jaggier geometry.)")
						: (Target == 0)
							? TEXT("(AA off -- maximum ghost-freedom, visible aliasing.)")
							: TEXT("(MSAA selected; this project uses deferred shading so MSAA effectively maps to no-AA.)"));
	}

	// v1.0.55: Pixel Streaming console-command gate. v1.0.54 set only the PS1 CVar
	// ("PixelStreaming.AllowPixelStreamingCommands") and had no effect because this project enables
	// the **PS2** plugin (verified: REBUS_Visualiser.uproject -> "PixelStreaming2": enabled). PS2
	// uses a completely different CVar ("PixelStreaming2.AllowPixelStreamingCommands") that gates
	// BOTH the streamer's Command/ConsoleCommand handler AND the InitialSettings JSON the streamer
	// sends to the frontend (the frontend ALSO refuses to send commands when its mirror of the gate
	// is false). v1.0.55 layers three defences:
	//   1. .ini config (DefaultEngine.ini [SystemSettings] + [ConsoleVariables],
	//      DefaultGame.ini [/Script/PixelStreaming2Settings.PixelStreaming2PluginSettings]).
	//   2. Force the CVar to 1 at RebusVisualiser StartupModule (and re-force on
	//      FCoreDelegates::OnPostEngineInit so the PS2 plugin is definitely loaded).
	//   3. Diagnostic log line so the user can grep LogRebusVisualiser for "PixelStreaming console
	//      gate status:" and see the final resolved value plus which plugin is active.
	// We poke BOTH the PS1 and PS2 CVars defensively (whichever plugin is actually loaded picks up
	// the right one; the other CVar simply won't exist and we log a benign "not registered" note).
	const TCHAR* GPixelStreamingGateCVars[] = {
		TEXT("PixelStreaming2.AllowPixelStreamingCommands"), // PS2 - active in this project
		TEXT("PixelStreaming.AllowPixelStreamingCommands"),  // PS1 - defensive
	};

	void TryForcePixelStreamingGate(const TCHAR* Phase)
	{
		for (const TCHAR* Name : GPixelStreamingGateCVars)
		{
			IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name);
			if (!CVar)
			{
				UE_LOG(LogRebusVisualiser, Log,
					TEXT("Pixel Streaming CVar %s NOT REGISTERED at phase=%s (plugin module not loaded -- benign if the other PS plugin is active)."),
					Name, Phase);
				continue;
			}
			const int32 Before = CVar->GetInt();
			CVar->Set(1, ECVF_SetByProjectSetting);
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Forced %s=1 at phase=%s (was=%d, now=%d)"),
				Name, Phase, Before, CVar->GetInt());
		}
	}

	void LogPixelStreamingGateStatus()
	{
		const bool bPS1 = IPluginManager::Get().FindPlugin(TEXT("PixelStreaming")).IsValid() && IPluginManager::Get().FindPlugin(TEXT("PixelStreaming"))->IsEnabled();
		const bool bPS2 = IPluginManager::Get().FindPlugin(TEXT("PixelStreaming2")).IsValid() && IPluginManager::Get().FindPlugin(TEXT("PixelStreaming2"))->IsEnabled();
		const TCHAR* PluginStr = (bPS1 && bPS2) ? TEXT("both")
			: bPS2 ? TEXT("PS2")
			: bPS1 ? TEXT("PS1")
			: TEXT("none");

		auto ReadCVar = [](const TCHAR* Name) -> FString
		{
			IConsoleVariable* C = IConsoleManager::Get().FindConsoleVariable(Name);
			return C ? FString::Printf(TEXT("%d"), C->GetInt()) : FString(TEXT("<unregistered>"));
		};

		UE_LOG(LogRebusVisualiser, Log,
			TEXT("PixelStreaming console gate status: PixelStreaming.AllowPixelStreamingCommands=%s PixelStreaming2.AllowPixelStreamingCommands=%s plugin=%s -- expected =1 for the active plugin so the portal's console pane works (run 'stat fps' from the portal console to verify)."),
			*ReadCVar(TEXT("PixelStreaming.AllowPixelStreamingCommands")),
			*ReadCVar(TEXT("PixelStreaming2.AllowPixelStreamingCommands")),
			PluginStr);
	}

	// v1.0.51: `Rebus.DumpFixtureLights` -- enumerate every fixture's SpotLight and ANY sibling
	// light components for cookie debugging. Also reports world-level competing lights (sky /
	// directional) and the relevant LightFunction CVars so the user can paste one block and we
	// can tell why a projected gobo cookie isn't visible on the lit floor pool.
	void HandleDumpFixtureLightsCommand(const TArray<FString>& /*Args*/)
	{
		if (!GEngine) return;

		auto ReadIntCVar = [](const TCHAR* Name) -> int32
		{
			IConsoleVariable* C = IConsoleManager::Get().FindConsoleVariable(Name);
			return C ? C->GetInt() : -1;
		};
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("DumpFixtureLights CVars: r.SupportLightFunctions=%d r.LightFunctionAtlas.Enabled=%d r.MegaLights.Allow=%d r.MegaLights.LightFunctions=%d r.MegaLights.Volume=%d"),
			ReadIntCVar(TEXT("r.SupportLightFunctions")),
			ReadIntCVar(TEXT("r.LightFunctionAtlas.Enabled")),
			ReadIntCVar(TEXT("r.MegaLights.Allow")),
			ReadIntCVar(TEXT("r.MegaLights.LightFunctions")),
			ReadIntCVar(TEXT("r.MegaLights.Volume")));

		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE && Ctx.WorldType != EWorldType::Editor)) continue;

			// World-level competing lights: any ULightComponent that is NOT owned by a fixture is
			// a candidate cookie wash-out source. A bright skylight or directional light will dilute
			// the projected gobo's dark cells -- not a duplication, but a contrast killer.
			int32 OtherLights = 0;
			int32 SkyLights = 0;
			int32 DirLights = 0;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* A = *It;
				if (!A || A->IsA(ARebusFixtureActor::StaticClass())) continue;
				TArray<ULightComponent*> Lights;
				A->GetComponents<ULightComponent>(Lights);
				for (const ULightComponent* L : Lights)
				{
					if (!L || !L->IsVisible() || L->Intensity <= 0.f) continue;
					++OtherLights;
					if (L->GetClass()->GetName().Contains(TEXT("DirectionalLight"))) ++DirLights;
					if (L->GetClass()->GetName().Contains(TEXT("SkyLight"))) ++SkyLights;
				}
			}
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("DumpFixtureLights world '%s': competing scene lights (not fixtures) = %d (sky=%d directional=%d). Bright skylight/dirlight dilutes projected gobo contrast on the lit pool."),
				*World->GetName(), OtherLights, SkyLights, DirLights);

			// Per-fixture dump.
			int32 FixtureCount = 0;
			for (TActorIterator<ARebusFixtureActor> It(World); It; ++It)
			{
				if (const ARebusFixtureActor* F = *It)
				{
					F->DumpLightStateForDebug();
					++FixtureCount;
				}
			}
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("DumpFixtureLights world '%s': dumped %d fixture(s)."),
				*World->GetName(), FixtureCount);
		}
	}

	// v1.0.96 -- `Rebus.BeamShadow [0|1]` master toggle for the M_RebusBeam screen-space shadow
	// trace. Mirrors the operator's mental model of "is the shaft self-shadowing on or off?"
	// without losing the tuned strength: OFF saves the live `Rebus.BeamShadowStrength` value
	// into a file-static prior and forces strength = 0 (which the shader's `[branch] if (...)`
	// gate takes the whole trace out of the per-pixel cost for); ON restores the saved prior
	// (default 1.0 on a fresh launch where the toggle's never been touched). Both transitions
	// route through the existing `Rebus.BeamShadowStrength` CVar so the refresh sink walks
	// every fixture exactly once.
	//
	// Why a CONSOLE COMMAND (not a CVar): the binary semantics (save/restore the underlying
	// float) need a custom handler. Pure-CVar bools can't store the prior value, and a
	// CVar-on-CVar dependency would race with portal-driven strength pushes. The command
	// pattern matches v1.0.47 `Rebus.MeshBeams` and keeps the state self-contained.
	float GBeamShadowPriorStrength = 1.f; // last non-zero strength captured by `Rebus.BeamShadow 0`
	void HandleBeamShadowCommand(const TArray<FString>& Args)
	{
		IConsoleVariable* StrengthCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Rebus.BeamShadowStrength"));
		if (!StrengthCVar)
		{
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("Rebus.BeamShadow: Rebus.BeamShadowStrength CVar not registered yet (fixture module not loaded?); no-op."));
			return;
		}

		// No arg / "status" -> diagnostic only, never mutates state. Returns the live strength
		// + the saved prior so the operator can see at a glance which one would land on the
		// next `Rebus.BeamShadow 1`.
		const bool bArgGiven = Args.Num() > 0;
		const float LiveStrength = StrengthCVar->GetFloat();
		if (!bArgGiven || (Args.Num() > 0 && Args[0].Equals(TEXT("status"), ESearchCase::IgnoreCase)))
		{
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Rebus.BeamShadow status: live Rebus.BeamShadowStrength=%.3f (savedPrior=%.3f). "
					 "Use `Rebus.BeamShadow 0` to disable (strength -> 0; saves prior). "
					 "Use `Rebus.BeamShadow 1` to enable (restores prior, default 1.0)."),
				LiveStrength, GBeamShadowPriorStrength);
			return;
		}

		const bool bEnable = ParseBoolArg(Args, true);
		if (!bEnable)
		{
			// OFF -- save the live strength (only when it's non-zero so a double-off doesn't
			// clobber the prior with 0) then force strength to 0. The CVar's refresh sink
			// walks every fixture and re-pushes the new value through `RefreshBeamShadowParams`.
			if (LiveStrength > 0.001f)
			{
				GBeamShadowPriorStrength = LiveStrength;
			}
			StrengthCVar->Set(TEXT("0.0"), ECVF_SetByConsole);
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Rebus.BeamShadow 0: master OFF -- Rebus.BeamShadowStrength %.3f -> 0.0 (saved prior=%.3f for next ON)."),
				LiveStrength, GBeamShadowPriorStrength);
		}
		else
		{
			// ON -- restore the saved prior. If the operator never touched the toggle the
			// prior is the v1.0.96 default (1.0), so a first-time ON lands a sensible value.
			const float Restore = (GBeamShadowPriorStrength > 0.001f) ? GBeamShadowPriorStrength : 1.0f;
			StrengthCVar->Set(*FString::SanitizeFloat(Restore), ECVF_SetByConsole);
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Rebus.BeamShadow 1: master ON -- Rebus.BeamShadowStrength %.3f -> %.3f (restored prior)."),
				LiveStrength, Restore);
		}
	}

	// v1.0.99 -- `Rebus.DumpBeamShadow` console command. Walks every Rebus fixture in every
	// Game/PIE/Editor world and dumps the live MID + CVar values for the screen-space shadow
	// trace, plus a one-line per-fixture "shadowing enabled, debug mode N" diagnostic. Use
	// this when the operator reports "I'm not seeing the shadow trace work" -- the dump
	// proves whether the values landed on the BeamMID, whether the MID scalar contract is
	// the v1.0.99 shape (Bias + Debug present), and whether the master toggle is in force.
	void HandleDumpBeamShadowCommand(const TArray<FString>& /*Args*/)
	{
		if (!GEngine) return;
		IConsoleVariable* StepsCVar    = IConsoleManager::Get().FindConsoleVariable(TEXT("Rebus.BeamShadowSteps"));
		IConsoleVariable* StrengthCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Rebus.BeamShadowStrength"));
		IConsoleVariable* BiasCVar     = IConsoleManager::Get().FindConsoleVariable(TEXT("Rebus.BeamShadowBias"));
		IConsoleVariable* DebugCVar    = IConsoleManager::Get().FindConsoleVariable(TEXT("Rebus.BeamShadowDebug"));
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("DumpBeamShadow CVars: Rebus.BeamShadowSteps=%s Rebus.BeamShadowStrength=%s Rebus.BeamShadowBias=%s Rebus.BeamShadowDebug=%s"),
			StepsCVar    ? *StepsCVar->GetString()    : TEXT("<unregistered>"),
			StrengthCVar ? *StrengthCVar->GetString() : TEXT("<unregistered>"),
			BiasCVar     ? *BiasCVar->GetString()     : TEXT("<unregistered>"),
			DebugCVar    ? *DebugCVar->GetString()    : TEXT("<unregistered>"));

		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE && Ctx.WorldType != EWorldType::Editor)) continue;
			int32 FixtureCount = 0;
			for (TActorIterator<ARebusFixtureActor> It(World); It; ++It)
			{
				if (const ARebusFixtureActor* F = *It)
				{
					F->DumpBeamShadowStateForDebug();
					++FixtureCount;
				}
			}
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("DumpBeamShadow world '%s': dumped %d fixture(s)."),
				*World->GetName(), FixtureCount);
		}
	}

	// v1.0.103 -- `Rebus.RebuildBeamMaterial` editor-only runtime regen of `M_RebusBeam`.
	//
	// Why this exists: v1.0.99 / v1.0.100 / v1.0.101 / v1.0.102 each shipped Custom-HLSL
	// changes inside `_BEAM_RAYMARCH_HLSL` (the LWC PreViewTranslation projection fix in
	// v1.0.99 is the smoking gun for the user's "beams still going straight through
	// objects" report against v1.0.102). The corrected HLSL only commits to the on-disk
	// `M_RebusBeam.uasset` when `build_rebus_base_level.py::ensure_beam_material()` runs --
	// which previously required either Tools > Execute Python Script in the editor (manual
	// operator action) or a fresh `-run=pythonscript` headless invocation. An operator who
	// `git pull`ed v1.0.99+ and opened the editor without re-running the Python script kept
	// the v1.0.96 cooked master with the LWC projection bug; the C++ side then dutifully
	// pushed BeamShadowStrength=1 onto a MID whose master never declared it,
	// `SetScalarParameterValue` silently no-op'd, and the trace concluded "always
	// unoccluded" (the user-visible symptom).
	//
	// `Rebus.RebuildBeamMaterial` invokes the SAME Python entry point at runtime via the
	// `PythonScriptPlugin`'s built-in `py` console command, gated behind `WITH_EDITOR` so
	// it's a no-op in packaged builds (PythonScriptPlugin is editor-only -- see
	// `REBUS_Visualiser.uproject` `TargetAllowList: ["Editor"]`). After the regen the
	// existing per-fixture `BeamMID`s still reference the OLD UMaterial (the Python side
	// deletes + recreates the asset, which leaves dangling MID parents); the operator
	// must follow up with ClearScene + LoadScene from the portal (the data-channel flow
	// that respawns every fixture, rebuilding each `BeamMID` off the freshly-loaded
	// `M_RebusBeam`) OR restart the editor. We log the next-step prompt explicitly so the
	// operator never has to guess.
	//
	// Defensive layout: we route through `GEngine->Exec` with the `py` command rather than
	// linking against `IPythonScriptPlugin` directly so the call site stays insulated from
	// any UE 5.7 API renames (the `py` console command is the documented engine-stable
	// entry point and has been around since the PythonScriptPlugin first shipped). We
	// still check `FModuleManager::IsModuleLoaded(TEXT("PythonScriptPlugin"))` first to
	// produce a clean Warning when a `WITH_EDITOR` build was launched without the editor
	// (e.g. an editor-target headless run with `-noplugins`) instead of a silent "py:
	// unknown command" log line.
	void HandleRebuildBeamMaterialCommand(const TArray<FString>& /*Args*/)
	{
#if WITH_EDITOR
		if (!GEngine)
		{
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("Rebus.RebuildBeamMaterial: GEngine null -- module not fully initialised; no-op."));
			return;
		}

		if (!FModuleManager::Get().IsModuleLoaded(TEXT("PythonScriptPlugin")))
		{
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("Rebus.RebuildBeamMaterial: PythonScriptPlugin not loaded -- this command is "
					 "editor-only (the regen logic lives in `build_rebus_base_level.py::"
					 "ensure_beam_material(force=True)`, exposed via the engine's `py` console "
					 "command which only registers when PythonScriptPlugin is loaded). "
					 "Workaround for headless / non-editor sessions: in editor, run Tools > "
					 "Execute Python Script > `build_rebus_base_level.ensure_beam_material(force=True)` "
					 "and then ClearScene+LoadScene OR restart the editor."));
			return;
		}

		// Pick a world to scope the Exec call (Editor wins over Game/PIE so the regen
		// runs against the asset-edit world, mirroring how Tools > Execute Python Script
		// hosts the script).
		UWorld* World = nullptr;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* W = Ctx.World();
			if (!W) continue;
			if (Ctx.WorldType == EWorldType::Editor) { World = W; break; }
			if (Ctx.WorldType == EWorldType::Game || Ctx.WorldType == EWorldType::PIE)
			{
				if (!World) World = W; // fallback if no Editor world (PIE-only run)
			}
		}

		// `py <expr>` is the engine-stable entry point. The expression matches the v1.0.99
		// self-heal flow exactly (force-regen + write-back the .uasset), so the on-disk
		// master picks up whatever Custom HLSL the current `_BEAM_RAYMARCH_HLSL` source
		// declares -- v1.0.99 LWC fix on v1.0.99+ pulls, plus any later shader work.
		const TCHAR* PyCmd = TEXT("py import build_rebus_base_level; build_rebus_base_level.ensure_beam_material(force=True)");
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.RebuildBeamMaterial: invoking `%s` via the engine's `py` console "
				 "command (editor-only runtime regen of M_RebusBeam). The Python script "
				 "deletes + recreates `/Game/REBUS/Materials/M_RebusBeam.uasset` with the "
				 "current `_BEAM_RAYMARCH_HLSL` source -- expect a `RebusBaseLevel: ...` "
				 "log block on the next line(s) confirming the regen landed."), PyCmd);

		const bool bExecOk = GEngine->Exec(World, PyCmd, *GLog);
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.RebuildBeamMaterial: `py` Exec returned %s. "
				 "NEXT-STEP -- existing per-fixture BeamMIDs still reference the OLD master "
				 "(the Python side regenerates the .uasset in place; UMaterialInstanceDynamic "
				 "parent pointers don't refresh automatically). Run ClearScene + LoadScene "
				 "from the portal to respawn every fixture (which calls BuildBeamCone, which "
				 "LoadObject's the freshly-regenerated master) -- OR restart the editor. "
				 "Verify with `Rebus.DumpBeamShadow` (every MID scalar should show EXISTS) "
				 "and `Rebus.BeamShadowDebug 1` (a cube placed between fixture + floor "
				 "should appear RED inside the beam)."),
			bExecOk ? TEXT("OK") : TEXT("FAILED"));
#else
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("Rebus.RebuildBeamMaterial: not available in non-editor builds "
				 "(PythonScriptPlugin is editor-only per the v1.0.103 design -- "
				 "regenerate the master in the editor and repackage)."));
#endif
	}

	// v1.0.47: `Rebus.MeshBeams [0|1]` -- live toggle for the visible Epic beam canvas, so you can
	// A/B against the SpotLight's VSM-shadowed fog beam (the source of the truss-gap shafts inside
	// the cone). Routes through URebusSceneSettingsSubsystem::SetMeshBeamsEnabled, which mirrors
	// the existing `SetSceneProperty bMeshBeams` wire path (one call per fixture -> SetMeshBeamEnabled).
	void HandleMeshBeamsCommand(const TArray<FString>& Args)
	{
		bool bEnable = true;
		if (Args.Num() > 0)
		{
			const FString& A = Args[0];
			bEnable = A == TEXT("1") || A.Equals(TEXT("on"), ESearchCase::IgnoreCase) || A.Equals(TEXT("true"), ESearchCase::IgnoreCase);
		}
		if (!GEngine) return;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE)) continue;
			if (URebusSceneSettingsSubsystem* Sce = World->GetSubsystem<URebusSceneSettingsSubsystem>())
			{
				Sce->SetMeshBeamsEnabled(bEnable);
			}
		}
	}

	// v1.0.70 shared helper -- parse a single bool-ish arg ("1"/"on"/"true" -> true, anything
	// else -> false; no arg -> true) for the show/hide console commands. Matches the existing
	// MeshBeams / DriveOrbitModels parser to keep the wire shape uniform.
	bool ParseBoolArg(const TArray<FString>& Args, bool bDefault = true)
	{
		if (Args.Num() == 0) return bDefault;
		const FString& A = Args[0];
		if (A == TEXT("1") || A.Equals(TEXT("on"), ESearchCase::IgnoreCase) || A.Equals(TEXT("true"), ESearchCase::IgnoreCase)) return true;
		if (A == TEXT("0") || A.Equals(TEXT("off"), ESearchCase::IgnoreCase) || A.Equals(TEXT("false"), ESearchCase::IgnoreCase)) return false;
		return bDefault;
	}

	// v1.0.70: `Rebus.ShowOrbitFixtures [0|1]` -- show/hide the Orbit-imported fixture geometry
	// bound to each ARebusFixtureActor (the components matched + bound by RebindOrbitModels).
	// Anything else in the OrbitImportRoot (trusses, set pieces, layout meshes that aren't
	// fixtures) stays visible. Use this when you want to A/B between the control-channel mesh
	// proxies and the orbit-imported fixture bodies, or to "delete" duplicate fixture geometry
	// without losing the rest of the Orbit scene. Routes via ARebusFixtureActor::SetOrbitVisibility
	// per fixture in every Game/PIE world.
	void HandleShowOrbitFixturesCommand(const TArray<FString>& Args)
	{
		const bool bShow = ParseBoolArg(Args, true);
		if (!GEngine) return;
		int32 TotalFixtures = 0, TotalAffected = 0;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE)) continue;
			for (TActorIterator<ARebusFixtureActor> It(World); It; ++It)
			{
				if (ARebusFixtureActor* F = *It)
				{
					TotalAffected += F->SetOrbitVisibility(bShow);
					++TotalFixtures;
				}
			}
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.ShowOrbitFixtures %d: %d fixture(s), %d Orbit component(s) %s."),
			bShow ? 1 : 0, TotalFixtures, TotalAffected, bShow ? TEXT("shown") : TEXT("hidden"));
	}

	// v1.0.107: `Rebus.ShowVersion [0|1|status]` -- toggle the always-on-top version
	// watermark (drawn at the top centre of every rendered viewport via
	// UDebugDrawService("Foreground"), captured into PixelStreaming2 stream frames).
	// Default ON (the watermark is the operator's at-a-glance confirmation that the
	// running binary matches the expected release on every rendered frame). The
	// `status` arg prints the LIVE flag + the cached `v<VersionName>` display string
	// + the live Y-margin so an operator can verify in one log line that the
	// watermark is wired and what it'll print -- mirrors the Rebus.AAMode "status"
	// surface introduced in v1.0.83. Routes through every Game/PIE world's
	// URebusVisualiserSubsystem::SetVersionWatermarkEnabled (single chokepoint
	// shared with the bShowVersionWatermark scene property).
	void HandleShowVersionCommand(const TArray<FString>& Args)
	{
		if (!GEngine) return;
		const bool bWantStatus = (Args.Num() > 0)
			&& Args[0].Equals(TEXT("status"), ESearchCase::IgnoreCase);

		if (!bWantStatus)
		{
			const bool bEnable = ParseBoolArg(Args, true);
			int32 Subsystems = 0;
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				UWorld* World = Ctx.World();
				if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE)) continue;
				UGameInstance* GI = World->GetGameInstance();
				if (!GI) continue;
				if (URebusVisualiserSubsystem* Viz = GI->GetSubsystem<URebusVisualiserSubsystem>())
				{
					Viz->SetVersionWatermarkEnabled(bEnable);
					++Subsystems;
				}
			}
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Rebus.ShowVersion %d: subsystems=%d -- watermark %s on every "
					 "rendered viewport (and every PixelStreaming2 stream frame that "
					 "captures the FCanvas overlay)."),
				bEnable ? 1 : 0, Subsystems,
				bEnable ? TEXT("ENABLED") : TEXT("DISABLED"));
		}

		// Status path always logs the live state + cached version + Y-margin (lifted
		// from any subsystem -- they all share file-scope flags + each owns the same
		// CachedVersionDisplay derived from the SAME plugin descriptor).
		FString DisplayStr;
		bool bLiveEnabled = false;
		bool bFoundSubsystem = false;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE)) continue;
			UGameInstance* GI = World->GetGameInstance();
			if (!GI) continue;
			if (URebusVisualiserSubsystem* Viz = GI->GetSubsystem<URebusVisualiserSubsystem>())
			{
				DisplayStr = Viz->GetCachedVersionDisplay();
				bLiveEnabled = Viz->IsVersionWatermarkEnabled();
				bFoundSubsystem = true;
				break;
			}
		}
		const float YPx = URebusVisualiserSubsystem::GetVersionWatermarkTopMarginPx();
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.ShowVersion status: enabled=%s, display='%s', y-margin=%.1fpx, "
				 "subsystem-found=%s. Source-of-truth: IPluginManager::FindPlugin("
				 "\"RebusVisualiser\")->GetDescriptor().VersionName, cached at "
				 "subsystem Initialize()."),
			bLiveEnabled ? TEXT("true") : TEXT("false"),
			DisplayStr.IsEmpty() ? TEXT("<unset>") : *DisplayStr,
			YPx,
			bFoundSubsystem ? TEXT("yes") : TEXT("no -- launch a PIE/Game world first"));
	}

	// v1.0.107: `Rebus.VersionWatermarkY <px>` -- set the top-edge margin in pixels
	// for the version watermark (default 12px). Operators with a HUD element near
	// the top centre can drop the watermark below it without disabling it. Negative
	// values are clamped to 0 in the setter. With no arg, logs the current margin.
	void HandleVersionWatermarkYCommand(const TArray<FString>& Args)
	{
		if (Args.Num() == 0)
		{
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Rebus.VersionWatermarkY (status): y-margin=%.1fpx (default 12)."),
				URebusVisualiserSubsystem::GetVersionWatermarkTopMarginPx());
			return;
		}
		const float Px = FCString::Atof(*Args[0]);
		URebusVisualiserSubsystem::SetVersionWatermarkTopMarginPx(Px);
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.VersionWatermarkY %.1fpx (clamped from %s)."),
			URebusVisualiserSubsystem::GetVersionWatermarkTopMarginPx(),
			*Args[0]);
	}

	// v1.0.70: `Rebus.ShowOrbit [0|1]` -- broad sledgehammer that hides EVERY actor of class
	// OrbitImportRoot in every Game/PIE world via SetActorHiddenInGame. This kills the whole
	// Orbit import (fixtures AND trusses / set pieces / layout meshes); use ShowOrbitFixtures
	// when you only want to hide the fixture bodies. The class match is by name string (no
	// compile dependency on the separately-owned OrbitConnector plugin, matching how
	// RebindOrbitModels finds the root).
	void HandleShowOrbitCommand(const TArray<FString>& Args)
	{
		const bool bShow = ParseBoolArg(Args, true);
		if (!GEngine) return;
		int32 RootsAffected = 0;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE)) continue;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* A = *It;
				if (!A || A->GetClass()->GetName() != TEXT("OrbitImportRoot")) continue;
				A->SetActorHiddenInGame(!bShow);
				++RootsAffected;
			}
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.ShowOrbit %d: %d OrbitImportRoot actor(s) %s."),
			bShow ? 1 : 0, RootsAffected, bShow ? TEXT("shown") : TEXT("hidden"));
	}

	// v1.0.86: `Rebus.SetGroundTiling <metres>` -- set the floor texture's physical tile size.
	// With the v1.0.86 ground master, 1.0 (default) = one texture repeat per 1 m of world
	// space regardless of the floor mesh's actor scale (the BaseLevel floor is a 100 cm engine
	// plane scaled 2000x = 2 km square, so without world-driven UVs every texture was being
	// stretched 2000x). Lower => finer tiling (0.5 = 2 repeats/m). Routes through every
	// running Game/PIE world's URebusSceneSettingsSubsystem::SetGroundTilingMeters (and
	// updates the SceneState 'GroundTilingMeters' value so the portal sees the new tiling on
	// its next read-back).
	void HandleSetGroundTilingCommand(const TArray<FString>& Args)
	{
		if (!GEngine) return;
		const float Metres = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 1.0f;
		int32 Subsystems = 0;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE)) continue;
			URebusSceneSettingsSubsystem* Sce = World->GetSubsystem<URebusSceneSettingsSubsystem>();
			if (!Sce) continue;
			++Subsystems;
			// Route via the catalogue path so SceneState gets the new value (instead of just
			// pushing to the MID directly -- that would render correctly but a SceneState
			// read-back would still report the old tiling).
			Sce->ApplySceneProperty(TEXT("GroundTilingMeters"), FRebusPropertyValue::MakeNumber(Metres));
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.SetGroundTiling %.3f: pushed to %d scene-settings subsystem(s)."), Metres, Subsystems);
	}

	// v1.0.85: `Rebus.OverrideTrussMaterial [0|1]` -- enable / disable the powdercoat material
	// override on every Orbit-imported primitive NOT bound to a fixture. ON applies the truss
	// material (loaded from /Game/REBUS/Materials/M_RebusTruss.M_RebusTruss if present, else a
	// runtime MID built off BasicShapeMaterial with PBR params tuned for matte black powder-
	// coat) to every material slot of every unbound primitive on every OrbitImportRoot in
	// every Game/PIE world. OFF restores each component's original slot materials from the
	// per-subsystem cache captured the first time the override was applied. Default ON since
	// v1.0.85 (the override fires automatically on the same 1Hz cadence as RebindOrbitModels).
	void HandleOverrideTrussMaterialCommand(const TArray<FString>& Args)
	{
		const bool bEnable = ParseBoolArg(Args, true);
		if (!GEngine) return;
		int32 Subsystems = 0;
		int32 TotalComponents = 0, TotalTouched = 0, TotalSkippedBound = 0, TotalRestored = 0;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE)) continue;
			UGameInstance* GI = World->GetGameInstance();
			if (!GI) continue;
			URebusVisualiserSubsystem* Viz = GI->GetSubsystem<URebusVisualiserSubsystem>();
			if (!Viz) continue;
			++Subsystems;
			const URebusVisualiserSubsystem::FTrussMaterialApplyCount C = Viz->SetTrussMaterialOverrideEnabled(bEnable);
			TotalComponents   += C.Components;
			TotalTouched      += C.Touched;
			TotalSkippedBound += C.SkippedBound;
			TotalRestored     += C.Restored;
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.OverrideTrussMaterial %d: subsystems=%d -- %s."),
			bEnable ? 1 : 0, Subsystems,
			bEnable
				? *FString::Printf(TEXT("scanned=%d touched=%d skippedFixtureBound=%d (unbound Orbit comps repainted; bound fixture geometry untouched)"),
					TotalComponents, TotalTouched, TotalSkippedBound)
				: *FString::Printf(TEXT("restored=%d original Orbit material(s)"), TotalRestored));
	}

	// v1.0.71: `Rebus.OverrideFixtureMaterials [0|1]` -- enable / disable the body+lens material
	// override on every ARebusFixtureActor in every Game/PIE world. ON applies the black satin
	// plastic body material to every non-lens mesh (control-channel procedural meshes +
	// Orbit-imported components bound to the fixture) and the mirrored-glass lens material to
	// every mesh whose name/tag contains a lens keyword ("lens"/"glass"/"crystal"/"optic"/
	// "front"). OFF restores each component's pre-override material from the per-actor cache
	// captured the first time the override was applied. Default ON since v1.0.71 (the
	// override fires automatically at fixture build time + Orbit bind time).
	void HandleOverrideFixtureMaterialsCommand(const TArray<FString>& Args)
	{
		const bool bEnable = ParseBoolArg(Args, true);
		if (!GEngine) return;
		int32 TotalFixtures = 0;
		int32 TotalBody = 0, TotalLens = 0, TotalRestored = 0;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE)) continue;
			for (TActorIterator<ARebusFixtureActor> It(World); It; ++It)
			{
				if (ARebusFixtureActor* F = *It)
				{
					const ARebusFixtureActor::FFixtureMaterialApplyCount C = F->SetFixtureMaterialOverrideEnabled(bEnable);
					TotalBody += C.Body; TotalLens += C.Lens; TotalRestored += C.Restored;
					++TotalFixtures;
				}
			}
		}
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.OverrideFixtureMaterials %d: %d fixture(s) -- %s."),
			bEnable ? 1 : 0, TotalFixtures,
			bEnable
				? *FString::Printf(TEXT("body=%d lens=%d meshes overridden"), TotalBody, TotalLens)
				: *FString::Printf(TEXT("restored=%d original material(s)"), TotalRestored));
	}

	// `Rebus.DriveOrbitModels [0|1]` -- live toggle for the Phase-1 Orbit-model sync test (mirrors
	// the bDriveOrbitModels scene property). No arg / "1"/"on"/"true" enables; "0"/"off"/"false"
	// disables. Routes to the fixture control subsystem of each running Game/PIE world.
	void HandleDriveOrbitModelsCommand(const TArray<FString>& Args)
	{
		bool bEnable = true;
		if (Args.Num() > 0)
		{
			const FString& A = Args[0];
			bEnable = A == TEXT("1") || A.Equals(TEXT("on"), ESearchCase::IgnoreCase) || A.Equals(TEXT("true"), ESearchCase::IgnoreCase);
		}
		if (!GEngine) return;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE)) continue;
			if (UGameInstance* GI = World->GetGameInstance())
			{
				if (URebusFixtureControlSubsystem* Ctl = GI->GetSubsystem<URebusFixtureControlSubsystem>())
				{
					Ctl->SetDriveOrbitModels(bEnable);
				}
			}
		}
	}
}

void FRebusVisualiserModule::StartupModule()
{
	UE_LOG(LogRebusVisualiser, Log, TEXT("RebusVisualiser module started."));

	// v1.0.55: force the Pixel Streaming console-command gate. First attempt at StartupModule
	// (may be too early -- if the PS module hasn't loaded yet the CVar isn't registered and the
	// "not registered" log fires; that's fine). Second attempt on OnPostEngineInit after every
	// plugin is up, which always wins, followed by the gate-status diagnostic so the user can
	// confirm from one grep ("PixelStreaming console gate status:") whether the gate is now 1.
	TryForcePixelStreamingGate(TEXT("StartupModule"));
	FCoreDelegates::OnPostEngineInit.AddLambda([]()
	{
		TryForcePixelStreamingGate(TEXT("PostEngineInit"));
		LogPixelStreamingGateStatus();

		// v1.0.83: REVERTED the v1.0.73/74/78 auto-apply. The GoboAntiGhost + LumenFastResponse
		// packs traded ghosting for global noise/flickering AND didn't actually fix the
		// underlying TSR pathology. Root cause (proved by the user's "moving the camera
		// temporarily stops ghosting" observation): TSR + animated light functions on
		// stationary geometry. Motion vectors are zero on the floor, so TSR reprojects from
		// the SAME pixel last frame -- which had the cookie pattern at a slightly different
		// rotation. TSR's shading-rejection heuristic can't tell "rotating gobo" apart from
		// "noise" without lowering thresholds globally, which is what created the noise side
		// effect. The packs remain available behind their console commands for A/B; the
		// fresh approach (v1.0.83) tackles the actual layers:
		//   1. Lumen ghost layer  -> per-spotlight bAffectDynamicIndirectLighting=false while
		//                            a gobo is active (in ARebusFixtureActor::ApplyGobo). Kills
		//                            the GI trail cleanly WITHOUT the global Lumen.Temporal=0
		//                            nuke that added noise everywhere.
		//   2. TSR ghost layer    -> Rebus.AAMode console + SetAAMode portal descriptor.
		//                            Operator picks TAA (handles animated lights without
		//                            ghost trails because its history rejection is simpler/
		//                            more aggressive) when a show features rotating gobos.
		//                            TSR stays default for shows without animated cookies.
	});

	GDriveOrbitModelsCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.DriveOrbitModels"),
		TEXT("Drive Orbit-imported fixture models from fixture motion (default ON since v1.0.65). "
			 "Usage: Rebus.DriveOrbitModels [0|1]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleDriveOrbitModelsCommand),
		ECVF_Default);

	GMeshBeamsCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.MeshBeams"),
		TEXT("Toggle the visible Epic beam canvas on all fixtures (A/B against the SpotLight's "
			 "VSM-shadowed fog beam, which is what carves the truss-gap shafts). "
			 "Usage: Rebus.MeshBeams [0|1]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleMeshBeamsCommand),
		ECVF_Default);

	// v1.0.96 -- screen-space shadow trace master toggle. Paired with the `Rebus.BeamShadowSteps`
	// + `Rebus.BeamShadowStrength` CVars defined in RebusFixtureActor.cpp; the toggle saves/
	// restores the strength so flicking the master off doesn't lose the operator's tuned value.
	// See the HandleBeamShadowCommand doc-comment for the save/restore semantics.
	GBeamShadowCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.BeamShadow"),
		TEXT("v1.0.96 -- master toggle for the M_RebusBeam screen-space shadow trace. "
			 "`Rebus.BeamShadow 0` forces Rebus.BeamShadowStrength to 0 (the shader's "
			 "`[branch] if (BeamShadowStrength > 0.001)` gate then takes the whole trace OUT of "
			 "the per-pixel cost) and saves the prior strength. `Rebus.BeamShadow 1` restores "
			 "the saved prior (default 1.0 on a fresh launch). No arg / `status` logs the live "
			 "strength + saved prior without mutating either. Pair with `Rebus.BeamShadowSteps "
			 "<n>` (default 8, clamp [1,16]) and `Rebus.BeamShadowStrength <0..1>` (default 1.0) "
			 "for tuning. See the README v1.0.96 release block for the algorithm + limitations. "
			 "Usage: Rebus.BeamShadow [0|1|status]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleBeamShadowCommand),
		ECVF_Default);

	// v1.0.99 imported-primitive shadow-cast normalisation toggle.
	GOrbitCastShadowsCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.OrbitCastShadows"),
		TEXT("v1.0.99 -- toggle the imported-Orbit-primitive shadow-cast normalisation "
			 "(default ON). When ON the visualiser subsystem walks every OrbitImportRoot's "
			 "primitive components on the same 1 Hz cadence as RebindOrbitModels and forces "
			 "CastShadow=true / bCastDynamicShadow=true / bCastHiddenShadow=false / "
			 "bCastFarShadow=true on each, so the SpotLight's own shadow casting catches "
			 "every imported truss / set-piece / fixture body. OFF walks the same tracked "
			 "set and forces CastShadow=false (so the operator can A/B against the "
			 "no-shadow baseline). Mirrors the `bOrbitCastShadows` scene property -- both "
			 "drive the same SetOrbitCastShadowsEnabled chokepoint. See the v1.0.99 README "
			 "release block for the full diagnosis (the user report \"Can we check that all "
			 "imported objects cast shadows as default\" was Part B of the v1.0.99 work)."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const bool bEnable = ParseBoolArg(Args, true);
			if (!GEngine) return;
			int32 Subsystems = 0;
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				UWorld* World = Ctx.World();
				if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE)) continue;
				UGameInstance* GI = World->GetGameInstance();
				if (!GI) continue;
				if (URebusVisualiserSubsystem* Viz = GI->GetSubsystem<URebusVisualiserSubsystem>())
				{
					Viz->SetOrbitCastShadowsEnabled(bEnable);
					++Subsystems;
				}
			}
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Rebus.OrbitCastShadows %d: applied to %d subsystem(s)."),
				bEnable ? 1 : 0, Subsystems);
		}),
		ECVF_Default);

	// v1.0.104 imported-primitive double-sided normalisation toggle. Mirrors the v1.0.99
	// Rebus.OrbitCastShadows shape byte-for-byte (same lambda body, same per-world Game/
	// PIE walk, same GameInstance-subsystem chokepoint), just routed through the v1.0.104
	// SetOrbitDoubleSidedEnabled chokepoint. The scene-property `bOrbitDoubleSided` mirror
	// lives in URebusSceneSettingsSubsystem::ApplySceneProperty -- both paths drive the
	// same chokepoint so console + portal can never diverge. See the v1.0.104 README
	// release block for the user report ("Orbit-imported materials are still single-
	// sided in many cases, so thin geometry disappears when viewed from the back").
	GOrbitDoubleSidedCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.OrbitDoubleSided"),
		TEXT("v1.0.104 -- toggle the imported-Orbit-primitive double-sided normalisation "
			 "(default ON). When ON the visualiser subsystem walks every OrbitImportRoot's "
			 "primitive components on the same 1 Hz cadence as RebindOrbitModels + "
			 "EnsureImportedShadowsCast and forces bCastShadowAsTwoSided=true on each "
			 "comp, wraps every non-MID material slot in a UMaterialInstanceDynamic, and "
			 "pushes a `bTwoSidedScalar` (0/1) parameter onto every MID -- silently no-ops "
			 "on the scalar push when the parent master doesn't expose the param (e.g. "
			 "glTFRuntime / engine masters whose top-level two_sided flag is hard-baked "
			 "at cook time and can't be flipped at runtime), so the operator-visible win "
			 "on those imports is the shadow-side bCastShadowAsTwoSided fix; the full "
			 "double-sided RENDER win lands on every Rebus-authored master (M_RebusGround, "
			 "M_RebusFixtureLens, M_RebusOrbitImported, ...) that the operator has re-"
			 "parented Orbit assets to. OFF walks the same tracked set and restores the "
			 "single-sided baseline (so the operator can A/B against pre-v1.0.104 "
			 "behaviour). Mirrors the `bOrbitDoubleSided` scene property -- both drive "
			 "the same SetOrbitDoubleSidedEnabled chokepoint. See the v1.0.104 README "
			 "release block for the full diagnosis + the ~5-15% base-pass perf caveat "
			 "for two-sided opaque (user report: \"can you set all Orbit textures to be "
			 "double sided on import\"). Usage: Rebus.OrbitDoubleSided [0|1]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const bool bEnable = ParseBoolArg(Args, true);
			if (!GEngine) return;
			int32 Subsystems = 0;
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				UWorld* World = Ctx.World();
				if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE)) continue;
				UGameInstance* GI = World->GetGameInstance();
				if (!GI) continue;
				if (URebusVisualiserSubsystem* Viz = GI->GetSubsystem<URebusVisualiserSubsystem>())
				{
					Viz->SetOrbitDoubleSidedEnabled(bEnable);
					++Subsystems;
				}
			}
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Rebus.OrbitDoubleSided %d: applied to %d subsystem(s)."),
				bEnable ? 1 : 0, Subsystems);
		}),
		ECVF_Default);

	// v1.0.105 imported-mesh Nanite enable toggle. Mirrors the v1.0.99 / v1.0.104
	// Rebus.Orbit* shape (same lambda body, same per-world Game/PIE walk, same
	// GameInstance-subsystem chokepoint), routed through the v1.0.105
	// SetNaniteOrbitImportsEnabled chokepoint. The scene-property `bNaniteOrbitImports`
	// mirror lives in URebusSceneSettingsSubsystem::ApplySceneProperty -- both paths
	// drive the same chokepoint so console + portal can never diverge. See the v1.0.105
	// README release block for the user request ("can all imported objects from orbit
	// be converted to nanite post import to improve performance"), the editor-only
	// constraint (UStaticMesh::Build is `#if WITH_EDITOR` in UE 5.7), and the rebuild-
	// storm caveat on the OFF path (every previously-Nanite-enabled mesh gets Build()
	// re-invoked which can take seconds per mesh and blocks the game thread).
	GNaniteOrbitImportsCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.NaniteOrbitImports"),
		TEXT("v1.0.105 -- toggle the imported-Orbit-mesh Nanite enable pass (default ON). "
			 "When ON the visualiser subsystem walks every OrbitImportRoot's UStaticMesh "
			 "on the same 1 Hz cadence as RebindOrbitModels + EnsureImportedShadowsCast + "
			 "EnsureImportedDoubleSided. Per unique UStaticMesh: skip if state already "
			 "matches and we've visited the mesh before; skip with a one-shot operator-fix "
			 "Warning if the mesh has no source MeshDescription (glTFRuntime needs "
			 "bGenerateStaticMeshDescription=true on the OrbitConnector import config); "
			 "otherwise set NaniteSettings.bEnabled=true (+ conservative defaults: "
			 "PositionPrecision=auto, FallbackPercentTriangles=1.0 to preserve the "
			 "v1.0.97/v1.0.104 two-sided fallback proxy, TrimRelativeError=0.0), call "
			 "UStaticMesh::Build, MarkRenderStateDirty on every component referencing the "
			 "mesh. OFF walks the same tracked set and DISABLES Nanite + Build()s -- emits "
			 "a single Warning naming the rebuild-storm cost (Build() can take seconds "
			 "per mesh and blocks the game thread; preferred is to A/B between scenes / "
			 "shows, not during a live cue). Editor-only -- in packaged builds the toggle "
			 "is still settable for SceneState parity but the conversion is no-effect "
			 "(UStaticMesh::Build + INaniteBuilderModule are `#if WITH_EDITOR` in UE 5.7); "
			 "pre-cook the Orbit GLBs to UStaticMesh .uasset(s) with NaniteSettings."
			 "bEnabled=true in editor before packaging. Mirrors the `bNaniteOrbitImports` "
			 "scene property -- both drive the same SetNaniteOrbitImportsEnabled "
			 "chokepoint. Verify with `Rebus.DumpOrbitNanite` (every entry should report "
			 "Nanite=ON once the walker has run). See the v1.0.105 README release block "
			 "for the full diagnosis, the cooked-Nanite path for packaged builds, and the "
			 "expected ~5-50x draw-call cost reduction on imported trusses + set pieces. "
			 "Usage: Rebus.NaniteOrbitImports [0|1]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const bool bEnable = ParseBoolArg(Args, true);
			if (!GEngine) return;
			int32 Subsystems = 0;
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				UWorld* World = Ctx.World();
				if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE)) continue;
				UGameInstance* GI = World->GetGameInstance();
				if (!GI) continue;
				if (URebusVisualiserSubsystem* Viz = GI->GetSubsystem<URebusVisualiserSubsystem>())
				{
					Viz->SetNaniteOrbitImportsEnabled(bEnable);
					++Subsystems;
				}
			}
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Rebus.NaniteOrbitImports %d: applied to %d subsystem(s)."),
				bEnable ? 1 : 0, Subsystems);
		}),
		ECVF_Default);

	// v1.0.105 per-mesh Nanite diagnostic dump. Walks every Orbit StaticMesh in every
	// Game/PIE/Editor world and emits one log line per UNIQUE mesh (grouped by
	// UStaticMesh* so multiple components sharing the same imported asset collapse to
	// ONE entry with a ref count). Mirrors the `Rebus.DumpBeamShadow` style for
	// consistency with the v1.0.99 / v1.0.103 diagnostic surfaces. Operator
	// verification: every entry should report `Nanite=ON` once the v1.0.105 walker
	// has run successfully -- if any entry shows `Nanite=OFF`, look one log line up
	// for the matching `[Rebus] Nanite skip on '<Mesh>': no source MeshDescription`
	// Warning (= the OrbitConnector import config needs bGenerateStaticMeshDescription=
	// true) OR the operator may have toggled `Rebus.NaniteOrbitImports 0` recently.
	GDumpOrbitNaniteCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.DumpOrbitNanite"),
		TEXT("v1.0.105 -- dump every Orbit-imported UStaticMesh's Nanite state in one "
			 "line each: Mesh='<n>' refs=<r> tris=<t> Nanite=ON|OFF FallbackTris=<ft> "
			 "dsSlots=<a/b> (a = material slots reporting bTwoSidedScalar=1.0, b = "
			 "total slots; surfaces the v1.0.104 double-sided pipeline alongside the "
			 "v1.0.105 Nanite state so the operator can see at a glance which assets "
			 "are getting which optimisations). Grouped by unique UStaticMesh* -- "
			 "components sharing the same imported asset collapse to one entry with a "
			 "ref count, so a busy Orbit import doesn't flood the log with hundreds of "
			 "redundant lines. Use this as the canonical operator verification step "
			 "after `Rebus.NaniteOrbitImports 1` or after a fresh PRISM session boot: "
			 "every entry should report Nanite=ON. Any Nanite=OFF entry indicates "
			 "either (a) a missing source MeshDescription (look one log line up for the "
			 "matching `[Rebus] Nanite skip on '<Mesh>': no source MeshDescription` "
			 "Warning -- the OrbitConnector import config needs bGenerateStaticMesh-"
			 "Description=true), (b) a packaged build (Nanite conversion is editor-"
			 "only -- pre-cook the Orbit GLBs in editor before packaging), OR (c) the "
			 "operator recently sent `Rebus.NaniteOrbitImports 0`. Usage: "
			 "Rebus.DumpOrbitNanite"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& /*Args*/)
		{
			if (!GEngine) return;
			int32 Worlds = 0;
			int32 Entries = 0;
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				UWorld* World = Ctx.World();
				if (!World || (Ctx.WorldType != EWorldType::Game && Ctx.WorldType != EWorldType::PIE && Ctx.WorldType != EWorldType::Editor)) continue;
				UGameInstance* GI = World->GetGameInstance();
				if (!GI) continue;
				URebusVisualiserSubsystem* Viz = GI->GetSubsystem<URebusVisualiserSubsystem>();
				if (!Viz) continue;
				++Worlds;
				const TArray<URebusVisualiserSubsystem::FOrbitNaniteDumpEntry> Dump = Viz->DumpOrbitNanite();
				for (const URebusVisualiserSubsystem::FOrbitNaniteDumpEntry& E : Dump)
				{
					UE_LOG(LogRebusVisualiser, Log,
						TEXT("DumpOrbitNanite world='%s' Mesh='%s' refs=%d tris=%d Nanite=%s FallbackTris=%d dsSlots=%d/%d"),
						*World->GetName(), *E.MeshName, E.ComponentRefs, E.Faces,
						E.bNaniteEnabled ? TEXT("ON") : TEXT("OFF"),
						E.FallbackTris, E.SlotsTwoSided, E.SlotsTotal);
					++Entries;
				}
			}
			UE_LOG(LogRebusVisualiser, Log,
				TEXT("Rebus.DumpOrbitNanite: dumped %d unique Orbit mesh(es) across %d world(s)."),
				Entries, Worlds);
		}),
		ECVF_Default);

	// v1.0.99 per-fixture screen-space-shadow-trace runtime dump.
	// v1.0.103 -- the dump is now the primary diagnostic for "v1.0.99 fix didn't materialise"
	// because the per-scalar MID column reports EXISTS/MISSING explicitly (was a -999
	// sentinel pre-v1.0.103). MISSING on any scalar = stale master = run the new
	// `Rebus.RebuildBeamMaterial` editor-only runtime regen.
	GDumpBeamShadowCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.DumpBeamShadow"),
		TEXT("v1.0.99 -- dump every Rebus fixture's M_RebusBeam screen-space shadow trace "
			 "state in one line each: live MID values (Steps/Strength/Bias/Debug as actually "
			 "read by the per-pixel shader) + global CVar values + a 'shadowing enabled, "
			 "debug mode N' diagnostic. Use when the operator reports the v1.0.96 shadow "
			 "trace doesn't appear to work -- the dump proves whether RefreshBeamShadowParams "
			 "is winning the push race, whether the master is the v1.0.99 shape (Bias+Debug "
			 "present), and whether the master `Rebus.BeamShadow` toggle is OFF. v1.0.103: "
			 "MID column now reports EXISTS/MISSING per scalar (was a -999 sentinel) so "
			 "stale-master is unmistakable -- MISSING means run `Rebus.RebuildBeamMaterial`. "
			 "Usage: Rebus.DumpBeamShadow"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleDumpBeamShadowCommand),
		ECVF_Default);

	// v1.0.103 -- editor-only runtime regen of `M_RebusBeam` via PythonScriptPlugin's
	// `py` console command (no module link required). Fixes the user-reported regression
	// where v1.0.99..v1.0.102 shipped Custom-HLSL changes but the on-disk master only
	// picks them up when the operator manually runs `build_rebus_base_level.py` -- this
	// command runs the Python entry point at runtime so an editor restart isn't required.
	// See the `HandleRebuildBeamMaterialCommand` doc-comment for the post-regen
	// fixture-respawn step (the existing per-fixture BeamMIDs are still parented to the
	// old UMaterial -- ClearScene + LoadScene OR editor restart is the chaser).
	GRebuildBeamMaterialCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.RebuildBeamMaterial"),
		TEXT("v1.0.103 -- regenerate `/Game/REBUS/Materials/M_RebusBeam.uasset` AT RUNTIME "
			 "by invoking `build_rebus_base_level.ensure_beam_material(force=True)` via the "
			 "engine's `py` console command (PythonScriptPlugin). Editor-only -- no-op in "
			 "packaged builds. Use when `Rebus.DumpBeamShadow` reports MISSING on any MID "
			 "scalar (= the operator pulled v1.0.99+ but the on-disk master is the stale "
			 "v1.0.96 cooked version with the LWC projection bug). After this command: "
			 "ClearScene + LoadScene from the portal (or restart the editor) so each "
			 "fixture respawns + rebuilds its BeamMID off the freshly-regenerated master, "
			 "then verify with `Rebus.DumpBeamShadow` (every scalar should show EXISTS) "
			 "and `Rebus.BeamShadowDebug 1` (a cube placed between fixture + floor should "
			 "appear RED inside the beam). See the v1.0.103 README release block for the "
			 "full operator checklist. Usage: Rebus.RebuildBeamMaterial"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleRebuildBeamMaterialCommand),
		ECVF_Default);

	GDumpFixtureLightsCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.DumpFixtureLights"),
		TEXT("Dump every fixture's SpotLight + any sibling/aux lights to LogRebusVisualiser, plus "
			 "world-level competing lights (sky/directional) and the relevant LightFunction CVars. "
			 "Use this when the projected gobo cookie isn't visible on the lit floor pool to see "
			 "whether the proxy state is what we set (bAllowMegaLights, LightFunctionMaterial, "
			 "CastShadows) and whether a competing/aux light is washing it out."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleDumpFixtureLightsCommand),
		ECVF_Default);

	// v1.0.70 visibility toggles for the Orbit import. ShowOrbitFixtures is the targeted one
	// (hides JUST the fixture bodies bound to ARebusFixtureActors -- trusses / set / non-fixture
	// orbit imports stay visible). ShowOrbit is the broad sledgehammer (hides every
	// OrbitImportRoot actor). Both default to showing when invoked with no arg, matching the
	// other Rebus.* toggles.
	GShowOrbitFixturesCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.ShowOrbitFixtures"),
		TEXT("Show/hide the Orbit-imported fixture geometry bound to each control-channel fixture "
			 "(leaves non-fixture orbit imports like trusses and set pieces visible). "
			 "Usage: Rebus.ShowOrbitFixtures [0|1]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleShowOrbitFixturesCommand),
		ECVF_Default);

	GShowOrbitCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.ShowOrbit"),
		TEXT("Show/hide EVERY actor of class OrbitImportRoot in every Game/PIE world (kills the "
			 "whole Orbit import -- fixtures AND trusses / set / layout). Use Rebus.ShowOrbitFixtures "
			 "instead if you only want to hide the fixture bodies. "
			 "Usage: Rebus.ShowOrbit [0|1]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleShowOrbitCommand),
		ECVF_Default);

	// v1.0.107 top-centre version watermark overlay (UDebugDrawService("Foreground")
	// canvas draw, captured into PixelStreaming2 stream frames). Default ON; routes
	// through URebusVisualiserSubsystem::SetVersionWatermarkEnabled (the same
	// chokepoint the bShowVersionWatermark scene property uses) so the console +
	// scene-property paths can never diverge.
	GShowVersionCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.ShowVersion"),
		TEXT("v1.0.107 -- toggle the always-on-top version watermark drawn at the top "
			 "centre of every rendered viewport (and every PixelStreaming2 stream "
			 "frame that captures the FCanvas overlay). The displayed string is "
			 "`v<RebusVisualiser plugin VersionName>` (e.g. `v1.0.107`), sourced once "
			 "at subsystem Initialize() via IPluginManager::FindPlugin->GetDescriptor"
			 "().VersionName so it always reflects the running binary's plugin "
			 "descriptor (you can trust what you see). Default ON. The `status` arg "
			 "logs the live flag + cached display string + Y-margin in one line. "
			 "Pair with `Rebus.VersionWatermarkY <px>` to tune the top-edge margin. "
			 "Usage: Rebus.ShowVersion [0|1|status]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleShowVersionCommand),
		ECVF_Default);

	GVersionWatermarkYCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.VersionWatermarkY"),
		TEXT("v1.0.107 -- set the version watermark's top-edge margin in pixels "
			 "(default 12). Increase to drop the watermark below an overlapping HUD "
			 "element; decrease to push it closer to the top edge. With no arg, "
			 "logs the current margin. Usage: Rebus.VersionWatermarkY <px>"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleVersionWatermarkYCommand),
		ECVF_Default);

	// v1.0.71 fixture material override toggle.
	GOverrideFixtureMaterialsCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.OverrideFixtureMaterials"),
		TEXT("Apply / restore the v1.0.71 fixture material override on every fixture's body "
			 "meshes (control-channel + Orbit). ON = black satin plastic body + mirrored glass "
			 "lens (lens detection by name/tag: lens/glass/crystal/optic/front). OFF restores "
			 "each mesh's original material from the per-actor cache. Drop a /Game/REBUS/"
			 "Materials/M_RebusFixtureBody.uasset (and/or M_RebusFixtureLens.uasset) to use "
			 "your own materials instead of the runtime BasicShapeMaterial MIDs. "
			 "Usage: Rebus.OverrideFixtureMaterials [0|1]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleOverrideFixtureMaterialsCommand),
		ECVF_Default);

	// v1.0.73 rotating-gobo ghosting toggle (TSR flicker rejection + full-res light functions).
	// v1.0.74 expanded the pack with r.TSR.History.UpdateRate 0.6 + r.LightFunctionAtlas.Enabled 0.
	GGoboAntiGhostCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.GoboAntiGhost"),
		TEXT("Toggle the rotating-gobo ghosting mitigation. ON (default since v1.0.73, expanded "
			 "v1.0.74, auto-applied at PostEngineInit) pushes: "
			 "r.TSR.ShadingRejection.Flickering=1, "
			 "r.TSR.ShadingRejection.Flickering.AdjustToFrameRate=1, "
			 "r.LightFunctionQuality=2, "
			 "r.TSR.History.UpdateRate=0.6 (was 0.4 default -- less history weight, less trail), "
			 "r.LightFunctionAtlas.Enabled=0 (defensive -- bypass any stale-atlas path). "
			 "OFF restores each CVar to its pre-push value byte-exact. "
			 "Usage: Rebus.GoboAntiGhost [0|1]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleGoboAntiGhostCommand),
		ECVF_Default);

	// v1.0.74 per-fixture gobo runtime dump.
	GDumpGoboStateCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.DumpGoboState"),
		TEXT("Dump every fixture's gobo runtime state (bGoboActive, RT pointer + clear flag, "
			 "GoboAngle + spin speeds, SpotLight.bAllowMegaLights + LightFunctionMaterial) to "
			 "LogRebusVisualiser. Use when ghosting persists -- the dump proves whether the "
			 "RT is allocated, the clear-on-update flag is on (v1.0.74 explicit), the spin "
			 "rate is non-zero, the SpotLight is opted OUT of MegaLights, and the LF material "
			 "is bound. Any one of those failing produces the wrong symptom."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleDumpGoboStateCommand),
		ECVF_Default);

	// v1.0.75 gobo resolution + DLSS.
	GGoboRTSizeCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.GoboRTSize"),
		TEXT("Rebuild every fixture's gobo render target at the requested square pixel size. "
			 "Default since v1.0.75 is 1024 (was 512). Sizes are clamped to [128, 8192] and "
			 "rounded up to the next pow2. Cost: ~6 MiB/fixture at 1024, ~25 MiB at 2048, "
			 "~100 MiB at 4096 (RGBA8 + mip chain). Mipmaps and trilinear filtering are auto-"
			 "enabled so distant footprints don't alias. Usage: Rebus.GoboRTSize <pixels>"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleGoboRTSizeCommand),
		ECVF_Default);

	// v1.0.78 Lumen GI fast-response toggle.
	GLumenFastResponseCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.LumenFastResponse"),
		TEXT("Toggle the Lumen GI fast-response pack -- disables Lumen's temporal filters so "
			 "direct light changes propagate to GI instantly. Default ON since v1.0.78 (auto-"
			 "applied at PostEngineInit). Fixes: 'lights on/off shows a GI fade-off instead "
			 "of cutting' AND 'rotating gobo leaves a GI ghost trail on the floor underneath "
			 "the TSR-side trail'. Pushes: r.Lumen.ScreenProbeGather.Temporal=0, "
			 "r.Lumen.Reflections.Temporal=0, r.Lumen.Radiosity.Temporal=0, "
			 "r.LumenScene.SurfaceCache.RecaptureLightingPerFrame=1. OFF restores each CVar "
			 "byte-exact (snapshot taken on first ON). Cost: noisier GI -- the temporal "
			 "filter was hiding sparse-sampling noise. Trade-off favours stage shows where "
			 "instant response trumps smoothness. Usage: Rebus.LumenFastResponse [0|1]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleLumenFastResponseCommand),
		ECVF_Default);

	GDLSSCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.DLSS"),
		TEXT("Enable NVIDIA DLSS upscaling (requires the NVIDIA DLSS plugin installed in this "
			 "project; if not installed the command logs install instructions). "
			 "Presets: off|quality|balanced|performance|ultraperformance|dlaa. Default preset "
			 "if no arg is 'quality'. 'dlaa' is no-upscale deep-learning AA. WARNING: DLSS "
			 "uses temporal accumulation, so the rotating-gobo ghost-trail symptom can "
			 "re-appear under DLSS -- the GoboAntiGhost CVars don't affect DLSS's internal "
			 "accumulator. If ghosting returns, try 'dlaa' or 'off'."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleDLSSCommand),
		ECVF_Default);

	// v1.0.79 cinematic-camera console aids. The portal drives the camera over the data
	// channel (SetCamera*) but these are useful when the operator is in PIE or wants a
	// quick state dump without the portal in the loop.
	GCameraSnapshotCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.CameraSnapshot"),
		TEXT("Log the live cinematic camera state -- loc/rot/focal length/aperture/focus "
			 "distance/EV/sensor -- in one line. Use to verify the portal's SetCamera* "
			 "descriptors are landing (the same struct is what gets broadcast as CameraState). "
			 "Returns a warning if the RebusCineCameraPawn isn't possessed yet (happens "
			 "before the streamer has connected)."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleCameraSnapshotCommand),
		ECVF_Default);

	GCameraResetCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.CameraReset"),
		TEXT("Reset the live cinematic camera to v1.0.79 defaults: 35mm focal, f/2.8 aperture, "
			 "manual focus @ 5m, Super35 sensor (24.89x18.66mm), manual exposure +0 EV. "
			 "Does NOT move the camera -- only resets the lens + exposure. To re-frame, send "
			 "SetCameraTransform from the portal."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleCameraResetCommand),
		ECVF_Default);

	// v1.0.82 diagnostics for 'portal not receiving CameraState'.
	GCameraStreamStatusCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.CameraStreamStatus"),
		TEXT("Diagnose why the portal isn't receiving CameraState. Logs whether the "
			 "visualiser subsystem is alive, whether the cine pawn has been spawned by "
			 "TryPositionPlayerView, and (if it has) what state would ship on the next "
			 "broadcast. Pair with the data-channel 'Sending CameraState (Response, N "
			 "players)' log -- if that line says players=0, the issue is no connected "
			 "viewer, not the broadcast layer."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleCameraStreamStatusCommand),
		ECVF_Default);

	GSendCameraStateCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.SendCameraState"),
		TEXT("Force one CameraState onto the data channel right now (synthesises a portal-"
			 "side RequestCameraState). Use to test the wire end-to-end without portal "
			 "cooperation: if you see 'Sending CameraState (Response, N players)' in the "
			 "log but the portal doesn't render the update, the failure is at the portal's "
			 "event listener (event name mismatch, frontend not subscribed), not in UE."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleSendCameraStateCommand),
		ECVF_Default);

	// v1.0.86 floor texture tiling at world scale.
	GSetGroundTilingCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.SetGroundTiling"),
		TEXT("Set the floor texture's physical tile size in metres (1 texture repeat per N metres "
			 "of world space). Default 1.0 (matches the SceneState default). Pre-v1.0.86 ground "
			 "masters lack the TilingMeters parameter -- the push is a silent no-op there until "
			 "the master is regenerated via build_rebus_base_level.build() in the editor. "
			 "Usage: Rebus.SetGroundTiling <metres>"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleSetGroundTilingCommand),
		ECVF_Default);

	// v1.0.85 truss / set-piece powdercoat material override -- ON by default. See the
	// HandleOverrideTrussMaterialCommand comment header for the full rationale.
	GOverrideTrussMaterialCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.OverrideTrussMaterial"),
		TEXT("Toggle the powdercoat material override on every Orbit-imported primitive NOT "
			 "bound to a fixture (trusses, set pieces, layout meshes). ON loads "
			 "/Game/REBUS/Materials/M_RebusTruss.M_RebusTruss if present, else builds a runtime "
			 "MID from BasicShapeMaterial (color #040404, roughness 0.55, metallic 0). Default "
			 "ON since v1.0.85; the override re-applies on the same 1Hz cadence as the Orbit "
			 "rebind so newly-imported geometry inherits it without re-running the command. OFF "
			 "restores each Orbit primitive's original slot materials byte-exact from the per-"
			 "subsystem cache. Usage: Rebus.OverrideTrussMaterial [0|1]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleOverrideTrussMaterialCommand),
		ECVF_Default);

	// v1.0.83 fresh approach to rotating-gobo ghosting -- operator-picked AA method. See the
	// HandleAAModeCommand comment header for the full diagnosis of why TSR ghosts on animated
	// light functions.
	GAAModeCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.AAMode"),
		TEXT("Pick the screen-space AA method: tsr (UE default, ghosts on rotating gobos), "
			 "taa (older temporal AA, recommended for shows featuring rotating gobos -- "
			 "slightly softer static AA but no ghost trail), fxaa (no temporal history, zero "
			 "ghosting, jaggier geometry), msaa (deferred renderer falls back to no-AA), "
			 "off (no AA). No argument or 'status' logs the current setting. "
			 "Usage: Rebus.AAMode [tsr|taa|fxaa|msaa|off|status]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleAAModeCommand),
		ECVF_Default);

	// v1.0.91 per-fixture IES runtime dump. Useful to confirm the v1.0.91 chain landed:
	// .ies peak candela -> IesCandelaMax -> RefreshIntensity -> SpotLight->Intensity, with
	// IntensityUnits=Candelas. Pass a fixtureId (Speckle node id) to filter to one fixture.
	GDumpFixtureIesCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.DumpFixtureIes"),
		TEXT("Dump per-fixture IES runtime state in one line: IES source (inline/url/none), "
			 "active profileId, selected zoomDmx + live zoom half-angle, IESTexture object name, "
			 "parsed peak candela max, BaseCandela fallback, SpotLight IntensityUnits + live "
			 "Intensity + expected (=base*dim*gate), dimmer/shutter, inline+url IES counts, "
			 "and bUseIESBrightness/IESBrightnessScale. With no arg dumps every fixture; with "
			 "a fixtureId (Speckle node id, the same key SetFixture* uses) dumps just that one. "
			 "Use this to verify the .ies file's peak candela actually drives SpotLight->Intensity "
			 "in Epic-beam mode (the only beam mode since v1.0.95). "
			 "Usage: Rebus.DumpFixtureIes [fixtureId]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleDumpFixtureIesCommand),
		ECVF_Default);

	// v1.0.101 per-fixture zoom / cone-mesh / SpotLight outer-cone runtime dump.
	GDumpFixtureZoomCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.DumpFixtureZoom"),
		TEXT("v1.0.101 -- dump per-fixture zoom + cone-mesh + SpotLight outer-cone state in "
			 "one line: live ZoomDeg target (half-angle), GDTF zoom range from the profile, "
			 "resolved canonical half-angle (ResolveZoomHalfDeg -- single source of truth for "
			 "both the SpotLight outer cone and the visible cone-mesh radius), SpotLight live "
			 "Outer/Inner cone angles + their ratio (the linear-taper light model that makes "
			 "the visible bright disc smaller than the geometric cone -- the v1.0.101 root "
			 "cause), procedural cone-mesh BeamLength + last-built and expected far-radius, "
			 "per-fixture BeamConeRadiusScale, BeamMID's live FarRadius scalar param read back "
			 "from the MID, and bUsingEpicBeam / bMeshBeamEnabled / bGoboActive / iris flags. "
			 "Use this to verify `Rebus.BeamConeRadiusScale` actually landed on every fixture "
			 "and the visible shaft + lit footprint stay in sync with the GDTF zoom-range spec. "
			 "With no arg dumps every fixture; with a fixtureId (Speckle node id, the same key "
			 "SetFixture* uses) dumps just that one. Mirrors `Rebus.DumpFixtureIes`'s shape. "
			 "Usage: Rebus.DumpFixtureZoom [fixtureId]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleDumpFixtureZoomCommand),
		ECVF_Default);
}

void FRebusVisualiserModule::ShutdownModule()
{
	if (GDriveOrbitModelsCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GDriveOrbitModelsCommand);
		GDriveOrbitModelsCommand = nullptr;
	}
	if (GMeshBeamsCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GMeshBeamsCommand);
		GMeshBeamsCommand = nullptr;
	}
	if (GDumpFixtureLightsCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GDumpFixtureLightsCommand);
		GDumpFixtureLightsCommand = nullptr;
	}
	if (GShowOrbitFixturesCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GShowOrbitFixturesCommand);
		GShowOrbitFixturesCommand = nullptr;
	}
	if (GShowOrbitCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GShowOrbitCommand);
		GShowOrbitCommand = nullptr;
	}
	if (GOverrideFixtureMaterialsCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GOverrideFixtureMaterialsCommand);
		GOverrideFixtureMaterialsCommand = nullptr;
	}
	if (GGoboAntiGhostCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GGoboAntiGhostCommand);
		GGoboAntiGhostCommand = nullptr;
	}
	if (GDumpGoboStateCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GDumpGoboStateCommand);
		GDumpGoboStateCommand = nullptr;
	}
	if (GGoboRTSizeCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GGoboRTSizeCommand);
		GGoboRTSizeCommand = nullptr;
	}
	if (GDLSSCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GDLSSCommand);
		GDLSSCommand = nullptr;
	}
	if (GLumenFastResponseCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GLumenFastResponseCommand);
		GLumenFastResponseCommand = nullptr;
	}
	if (GCameraSnapshotCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GCameraSnapshotCommand);
		GCameraSnapshotCommand = nullptr;
	}
	if (GCameraResetCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GCameraResetCommand);
		GCameraResetCommand = nullptr;
	}
	if (GCameraStreamStatusCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GCameraStreamStatusCommand);
		GCameraStreamStatusCommand = nullptr;
	}
	if (GSendCameraStateCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GSendCameraStateCommand);
		GSendCameraStateCommand = nullptr;
	}
	if (GAAModeCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GAAModeCommand);
		GAAModeCommand = nullptr;
	}
	if (GOverrideTrussMaterialCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GOverrideTrussMaterialCommand);
		GOverrideTrussMaterialCommand = nullptr;
	}
	if (GSetGroundTilingCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GSetGroundTilingCommand);
		GSetGroundTilingCommand = nullptr;
	}
	if (GDumpFixtureIesCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GDumpFixtureIesCommand);
		GDumpFixtureIesCommand = nullptr;
	}
	if (GBeamShadowCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GBeamShadowCommand);
		GBeamShadowCommand = nullptr;
	}
	if (GDumpBeamShadowCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GDumpBeamShadowCommand);
		GDumpBeamShadowCommand = nullptr;
	}
	if (GRebuildBeamMaterialCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GRebuildBeamMaterialCommand);
		GRebuildBeamMaterialCommand = nullptr;
	}
	if (GOrbitCastShadowsCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GOrbitCastShadowsCommand);
		GOrbitCastShadowsCommand = nullptr;
	}
	if (GOrbitDoubleSidedCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GOrbitDoubleSidedCommand);
		GOrbitDoubleSidedCommand = nullptr;
	}
	if (GNaniteOrbitImportsCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GNaniteOrbitImportsCommand);
		GNaniteOrbitImportsCommand = nullptr;
	}
	if (GDumpOrbitNaniteCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GDumpOrbitNaniteCommand);
		GDumpOrbitNaniteCommand = nullptr;
	}
	if (GDumpFixtureZoomCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GDumpFixtureZoomCommand);
		GDumpFixtureZoomCommand = nullptr;
	}
	if (GShowVersionCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GShowVersionCommand);
		GShowVersionCommand = nullptr;
	}
	if (GVersionWatermarkYCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GVersionWatermarkYCommand);
		GVersionWatermarkYCommand = nullptr;
	}
	// v1.0.73 / v1.0.78: restore both CVar packs to their snapshotted values so a hot-reload
	// of the module doesn't leak a permanent override into the engine session. Both packs
	// no-op if never enabled (v1.0.83 removed the auto-apply), so this is safe in the default
	// case where the operator never touched the toggles.
	ApplyGoboAntiGhost(false, TEXT("ShutdownModule"));
	ApplyLumenFastResponse(false, TEXT("ShutdownModule"));

	UE_LOG(LogRebusVisualiser, Log, TEXT("RebusVisualiser module shut down."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FRebusVisualiserModule, RebusVisualiser)
