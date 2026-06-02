// Copyright REBUS Industries. RebusVisualiser module public header.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FRebusVisualiserModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
