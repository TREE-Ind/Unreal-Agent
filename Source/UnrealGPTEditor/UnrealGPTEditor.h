#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FUnrealGPTEditorModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();
	void RegisterSettings();
	void UnregisterSettings();
};

