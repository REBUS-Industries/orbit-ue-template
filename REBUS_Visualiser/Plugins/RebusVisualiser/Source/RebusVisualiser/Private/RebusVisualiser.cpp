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

DEFINE_LOG_CATEGORY(LogRebusVisualiser);

#define LOCTEXT_NAMESPACE "FRebusVisualiserModule"

namespace
{
	IConsoleCommand* GDriveOrbitModelsCommand = nullptr;
	IConsoleCommand* GMeshBeamsCommand = nullptr;
	IConsoleCommand* GDumpFixtureLightsCommand = nullptr;

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

	GDriveOrbitModelsCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Rebus.DriveOrbitModels"),
		TEXT("Drive Orbit-imported fixture models from fixture motion (Phase 1 A/B sync test). "
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

	UE_LOG(LogRebusVisualiser, Log, TEXT("RebusVisualiser module shut down."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FRebusVisualiserModule, RebusVisualiser)
