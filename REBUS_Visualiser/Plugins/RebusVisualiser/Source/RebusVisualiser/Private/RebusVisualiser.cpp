// Copyright REBUS Industries. RebusVisualiser module implementation.
#include "RebusVisualiser.h"
#include "RebusVisualiserLog.h"

DEFINE_LOG_CATEGORY(LogRebusVisualiser);

#define LOCTEXT_NAMESPACE "FRebusVisualiserModule"

void FRebusVisualiserModule::StartupModule()
{
	UE_LOG(LogRebusVisualiser, Log, TEXT("RebusVisualiser module started."));
}

void FRebusVisualiserModule::ShutdownModule()
{
	UE_LOG(LogRebusVisualiser, Log, TEXT("RebusVisualiser module shut down."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FRebusVisualiserModule, RebusVisualiser)
