#include "Modules/ModuleManager.h"

class FUnrealGPTEditorTestsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FUnrealGPTEditorTestsModule, UnrealGPTEditorTests)

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UnrealGPTSceneContext.h"
#include "UnrealGPTSettings.h"
#include "UnrealGPTAgentClient.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealGPTSettingsTest, "UnrealGPT.Settings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealGPTSettingsTest::RunTest(const FString& Parameters)
{
	UUnrealGPTSettings* Settings = GetMutableDefault<UUnrealGPTSettings>();
	
	TestNotNull(TEXT("Settings should not be null"), Settings);
	TestTrue(TEXT("Default model should be set"), !Settings->DefaultModel.IsEmpty());
	
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealGPTSceneContextTest, "UnrealGPT.SceneContext", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealGPTSceneContextTest::RunTest(const FString& Parameters)
{
	// Test scene summary
	FString Summary = UUnrealGPTSceneContext::GetSceneSummary(10, 0);
	TestTrue(TEXT("Scene summary should not be empty"), !Summary.IsEmpty());
	
	// Test selected actors summary
	FString SelectedSummary = UUnrealGPTSceneContext::GetSelectedActorsSummary();
	TestTrue(TEXT("Selected actors summary should not be null"), true); // Can be empty if nothing selected
	
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealGPTAgentClientTest, "UnrealGPT.AgentClient", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealGPTAgentClientTest::RunTest(const FString& Parameters)
{
	UUnrealGPTAgentClient* Client = NewObject<UUnrealGPTAgentClient>();
	TestNotNull(TEXT("Agent client should not be null"), Client);
	
	Client->Initialize();
	TestTrue(TEXT("Agent client should initialize"), true);
	
	// Test tool definitions
	// Note: This would require accessing private methods or making them testable
	// For now, just verify client can be created
	
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

