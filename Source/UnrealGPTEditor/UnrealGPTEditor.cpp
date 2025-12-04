#include "UnrealGPTEditor.h"
#include "ISettingsModule.h"
#include "UnrealGPTSettings.h"
#include "LevelEditor.h"
#include "ToolMenus.h"
#include "UnrealGPTWidget.h"
#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "FUnrealGPTEditorModule"

static const FName UnrealGPTTabName("UnrealGPT");

void FUnrealGPTEditorModule::StartupModule()
{
	RegisterMenus();
}

void FUnrealGPTEditorModule::ShutdownModule()
{
}

void FUnrealGPTEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);
	
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
	FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
	Section.AddMenuEntry(
		NAME_None,
		LOCTEXT("UnrealGPTMenuEntryTitle", "UnrealGPT"),
		LOCTEXT("UnrealGPTMenuEntryTooltip", "Open the UnrealGPT AI Assistant"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			FGlobalTabmanager::Get()->TryInvokeTab(UnrealGPTTabName);
		}))
	);
	
	// Register tab spawner
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(UnrealGPTTabName, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SUnrealGPTWidget)
			];
	}))
	.SetDisplayName(LOCTEXT("FUnrealGPTTabTitle", "UnrealGPT"))
	.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void FUnrealGPTEditorModule::RegisterSettings()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		// Register the settings
		SettingsModule->RegisterSettings("Project", "Plugins", "UnrealGPT",
			LOCTEXT("RuntimeGeneralSettingsName", "UnrealGPT"),
			LOCTEXT("RuntimeGeneralSettingsDescription", "Configure UnrealGPT AI Agent"),
			GetMutableDefault<UUnrealGPTSettings>()
		);
	}
}

void FUnrealGPTEditorModule::UnregisterSettings()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->UnregisterSettings("Project", "Plugins", "UnrealGPT");
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUnrealGPTEditorModule, UnrealGPTEditor)

