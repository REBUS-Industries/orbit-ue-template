// Copyright REBUS Industries. RebusVisualiser module implementation.
#include "RebusVisualiser.h"
#include "RebusVisualiserLog.h"
#include "RebusFixtureControlSubsystem.h"
#include "RebusSceneSettingsSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY(LogRebusVisualiser);

#define LOCTEXT_NAMESPACE "FRebusVisualiserModule"

namespace
{
	IConsoleCommand* GDriveOrbitModelsCommand = nullptr;
	IConsoleCommand* GMeshBeamsCommand = nullptr;

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

	UE_LOG(LogRebusVisualiser, Log, TEXT("RebusVisualiser module shut down."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FRebusVisualiserModule, RebusVisualiser)
