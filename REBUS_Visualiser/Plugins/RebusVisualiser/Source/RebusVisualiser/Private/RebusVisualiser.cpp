// Copyright REBUS Industries. RebusVisualiser module implementation.
#include "RebusVisualiser.h"
#include "RebusVisualiserLog.h"
#include "RebusFixtureControlSubsystem.h"
#include "RebusFixtureActor.h"
#include "RebusSceneSettingsSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Components/LightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Interfaces/IPluginManager.h"
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

	// CVar definitions for the anti-ghost pack. Each entry has a name and an "on" value; some
	// are int, some are float (kept as float here so the int-snapshot path is just the int
	// truncation of the prior float -- restore-via-Set on a float CVar accepts a stringified
	// value with no precision drama for our 1-decimal settings). v1.0.74 expands the v1.0.73
	// pack with three additions; see comments alongside each entry below.
	struct FAntiGhostCVarDef { const TCHAR* Name; float OnValue; };
	const FAntiGhostCVarDef GAntiGhostCVars[] = {
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
		if (bEnable == GGoboAntiGhostEnabled) return; // no-op if already in target state

		if (bEnable)
		{
			GGoboAntiGhostPrior.Reset();
			for (const FAntiGhostCVarDef& Def : GAntiGhostCVars)
			{
				IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Def.Name);
				FCVarSnapshot Snap;
				Snap.Name = Def.Name;
				if (CVar)
				{
					// Snapshot via int (truncation is fine for restore since we only push 1
					// fractional digit at most -- 0.6 etc. -- and the int round-trip preserves
					// any prior int CVars exactly).
					Snap.PriorInt = CVar->GetInt();
					Snap.bValid = true;
					CVar->Set(*FString::SanitizeFloat(Def.OnValue), ECVF_SetByGameOverride);
					UE_LOG(LogRebusVisualiser, Log,
						TEXT("GoboAntiGhost ON [%s]: %s was=%d now=%s"),
						Phase, Def.Name, Snap.PriorInt, *CVar->GetString());
				}
				else
				{
					UE_LOG(LogRebusVisualiser, Log,
						TEXT("GoboAntiGhost ON [%s]: %s NOT REGISTERED (renderer module not loaded yet, or CVar renamed in this engine version -- benign, the other CVars still apply)."),
						Phase, Def.Name);
				}
				GGoboAntiGhostPrior.Add(Snap);
			}
		}
		else
		{
			for (const FCVarSnapshot& Snap : GGoboAntiGhostPrior)
			{
				if (!Snap.bValid) continue;
				IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Snap.Name);
				if (!CVar) continue;
				const FString BeforeRestore = CVar->GetString();
				CVar->Set(Snap.PriorInt, ECVF_SetByGameOverride);
				UE_LOG(LogRebusVisualiser, Log,
					TEXT("GoboAntiGhost OFF [%s]: %s was=%s restored=%s"),
					Phase, *Snap.Name, *BeforeRestore, *CVar->GetString());
			}
			GGoboAntiGhostPrior.Reset();
		}
		GGoboAntiGhostEnabled = bEnable;
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
	// (TSR flicker-aware shading rejection + full-res light functions). Default ON since
	// v1.0.73 (auto-applied on PostEngineInit). Disable for A/B comparison or if a specific
	// scene needs raw TSR back.
	void HandleGoboAntiGhostCommand(const TArray<FString>& Args)
	{
		const bool bEnable = ParseBoolArg(Args, true);
		ApplyGoboAntiGhost(bEnable, TEXT("ConsoleCommand"));
		UE_LOG(LogRebusVisualiser, Log,
			TEXT("Rebus.GoboAntiGhost %d -> live state %s."),
			bEnable ? 1 : 0, GGoboAntiGhostEnabled ? TEXT("ON") : TEXT("OFF"));
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

		// v1.0.73: rotating-gobo ghosting mitigation, auto-applied at PostEngineInit so the
		// renderer module's TSR CVars are guaranteed registered. Default ON; disable per
		// session with `Rebus.GoboAntiGhost 0` for A/B against the prior look.
		ApplyGoboAntiGhost(true, TEXT("PostEngineInit"));
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
	// v1.0.73: restore the anti-ghost CVars to their snapshotted values so a hot-reload of
	// the module doesn't leak a permanent override into the engine session.
	ApplyGoboAntiGhost(false, TEXT("ShutdownModule"));

	UE_LOG(LogRebusVisualiser, Log, TEXT("RebusVisualiser module shut down."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FRebusVisualiserModule, RebusVisualiser)
