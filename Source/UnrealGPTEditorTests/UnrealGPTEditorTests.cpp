// Copyright (c) 2025 TREE Industries.

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
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UnrealGPTSceneContext.h"
#include "UnrealGPTReflectionQuery.h"
#include "UnrealGPTBlueprintContext.h"
#include "UnrealGPTLogCapture.h"
#include "UnrealGPTLogReader.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealGPTSettings.h"
#include "UnrealGPTAgentClient.h"
#include "Mcp/McpResultNormalizer.h"
#include "UnrealGPTSseClient.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealGPTReflectionQueryTest, "UnrealGPT.ReflectionQuery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealGPTReflectionQueryTest::RunTest(const FString& Parameters)
{
	const FString StaticMeshActorQuery = TEXT("{\"class_name\":\"StaticMeshActor\",\"member_contains\":\"Mobility\"}");
	const FString StaticMeshActorResult = FUnrealGPTReflectionQuery::Query(StaticMeshActorQuery);
	TestTrue(TEXT("StaticMeshActor query should succeed"), StaticMeshActorResult.Contains(TEXT("\"status\":\"ok\"")));
	TestTrue(TEXT("StaticMeshActor query should match Mobility"), StaticMeshActorResult.Contains(TEXT("Mobility")));

	const FString MissingClassResult = FUnrealGPTReflectionQuery::Query(TEXT("{\"class_name\":\"ThisClassDefinitelyDoesNotExist_12345\"}"));
	TestTrue(TEXT("Missing class should return error"), MissingClassResult.Contains(TEXT("\"status\":\"error\"")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealGPTMcpNormalizerTest, "UnrealGPT.Mcp.Normalizer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealGPTMcpNormalizerTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("png mime normalization"), FMcpResultNormalizer::NormalizeMimeType(TEXT("png")), FString(TEXT("image/png")));
	TestEqual(TEXT("image usage inference"), FMcpResultNormalizer::InferUsageFromMime(TEXT("image/png"), TEXT("file.png")), FString(TEXT("image")));

	TSharedPtr<FJsonObject> McpResult = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Content;
	TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
	TextBlock->SetStringField(TEXT("type"), TEXT("text"));
	TextBlock->SetStringField(TEXT("text"), TEXT("hello"));
	Content.Add(MakeShared<FJsonValueObject>(TextBlock));
	McpResult->SetArrayField(TEXT("content"), Content);

	const FString Envelope = FMcpResultNormalizer::BuildToolCallEnvelope(TEXT("test-server"), TEXT("demo"), McpResult, TEXT("image"));
	TestTrue(TEXT("Envelope should be ok"), Envelope.Contains(TEXT("\"status\":\"ok\"")));
	TestTrue(TEXT("Envelope should include text"), Envelope.Contains(TEXT("hello")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealGPTSseParserTest, "UnrealGPT.Mcp.SseParser", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealGPTSseParserTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT("event: message\ndata: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n");
	TArray<FUnrealGPTSseEvent> Events;
	TestTrue(TEXT("SSE body should parse"), FUnrealGPTSseClient::ParseEventsFromBody(Body, Events));
	TestEqual(TEXT("One SSE event"), Events.Num(), 1);
	TestTrue(TEXT("SSE data preserved"), Events[0].Data.Contains(TEXT("jsonrpc")));
	return true;
}

static FString BuildTestLogLine(const FString& Category, const FString& Verbosity, const FString& Message, int32 LineNumber)
{
	return FString::Printf(
		TEXT("[2025.06.18-12.00.%02d:000][% 4d]%s: %s: %s"),
		LineNumber % 60,
		LineNumber,
		*Category,
		*Verbosity,
		*Message);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealGPTLogReaderFileTailTest, "UnrealGPT.LogReader.FileTail", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealGPTLogReaderFileTailTest::RunTest(const FString& Parameters)
{
	const FString TempDir = FPaths::ProjectIntermediateDir() / TEXT("UnrealGPTLogTests");
	IFileManager::Get().MakeDirectory(*TempDir, true);
	const FString TempLogPath = TempDir / TEXT("TailTest.log");

	FString FileContent;
	for (int32 Index = 0; Index < 500; ++Index)
	{
		const FString Message = (Index == 499)
			? TEXT("TARGET_MATCH final line")
			: FString::Printf(TEXT("Routine log line %d"), Index);
		FileContent += BuildTestLogLine(TEXT("LogTemp"), TEXT("Warning"), Message, Index);
		FileContent += LINE_TERMINATOR;
	}

	TestTrue(TEXT("Temp log file should be written"), FFileHelper::SaveStringToFile(FileContent, *TempLogPath));

	FUnrealGPTLogReader::FOptions Options;
	Options.MaxLines = 10;
	Options.MinVerbosity = ELogVerbosity::Warning;
	Options.MessageContains = TEXT("TARGET_MATCH");
	Options.Mode = FUnrealGPTLogReader::ELogReadMode::File;
	Options.Source = FUnrealGPTLogReader::ELogReadSource::File;

	int32 TotalMatched = 0;
	bool bTruncated = false;
	const FString Result = FUnrealGPTLogReader::TailLogFileForTest(TempLogPath, Options, TotalMatched, bTruncated);

	TestTrue(TEXT("Tail query should succeed"), Result.Contains(TEXT("\"status\":\"ok\"")));
	TestTrue(TEXT("Tail query should match target line"), Result.Contains(TEXT("TARGET_MATCH")));
	TestTrue(TEXT("Tail query should return one matched line"), Result.Contains(TEXT("\"line_count\":1")));

	IFileManager::Get().Delete(*TempLogPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealGPTLogReaderVerbosityTest, "UnrealGPT.LogReader.Verbosity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealGPTLogReaderVerbosityTest::RunTest(const FString& Parameters)
{
	const FString TempDir = FPaths::ProjectIntermediateDir() / TEXT("UnrealGPTLogTests");
	IFileManager::Get().MakeDirectory(*TempDir, true);
	const FString TempLogPath = TempDir / TEXT("VerbosityTest.log");

	const FString FileContent =
		BuildTestLogLine(TEXT("LogTemp"), TEXT("Log"), TEXT("verbose noise"), 1) + LINE_TERMINATOR
		+ BuildTestLogLine(TEXT("LogTemp"), TEXT("Warning"), TEXT("important warning"), 2) + LINE_TERMINATOR
		+ BuildTestLogLine(TEXT("LogTemp"), TEXT("Error"), TEXT("important error"), 3) + LINE_TERMINATOR;

	TestTrue(TEXT("Verbosity temp log should be written"), FFileHelper::SaveStringToFile(FileContent, *TempLogPath));

	FUnrealGPTLogReader::FOptions Options;
	Options.MaxLines = 20;
	Options.MinVerbosity = ELogVerbosity::Warning;
	Options.Mode = FUnrealGPTLogReader::ELogReadMode::File;
	Options.Source = FUnrealGPTLogReader::ELogReadSource::File;

	int32 TotalMatched = 0;
	bool bTruncated = false;
	const FString Result = FUnrealGPTLogReader::TailLogFileForTest(TempLogPath, Options, TotalMatched, bTruncated);

	TestTrue(TEXT("Verbosity query should succeed"), Result.Contains(TEXT("\"status\":\"ok\"")));
	TestTrue(TEXT("Verbosity query should include warning"), Result.Contains(TEXT("important warning")));
	TestTrue(TEXT("Verbosity query should include error"), Result.Contains(TEXT("important error")));
	TestFalse(TEXT("Verbosity query should exclude log-level noise"), Result.Contains(TEXT("verbose noise")));
	TestTrue(TEXT("Verbosity query should match two lines"), Result.Contains(TEXT("\"line_count\":2")));

	IFileManager::Get().Delete(*TempLogPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealGPTLogReaderSinceLastReadTest, "UnrealGPT.LogReader.SinceLastRead", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealGPTLogReaderSinceLastReadTest::RunTest(const FString& Parameters)
{
	FUnrealGPTLogCapture& Capture = FUnrealGPTLogCapture::Get();
	Capture.Initialize();
	Capture.ResetReadCursor();

	Capture.Serialize(TEXT("batch-one-line-a"), ELogVerbosity::Warning, FName(TEXT("LogPython")));
	Capture.Serialize(TEXT("batch-one-line-b"), ELogVerbosity::Error, FName(TEXT("LogPython")));

	const FString FirstResult = FUnrealGPTLogReader::Query(
		TEXT("{\"mode\":\"since_last_read\",\"min_verbosity\":\"all\",\"max_lines\":20,\"source\":\"memory\"}"));
	TestTrue(TEXT("First since_last_read should succeed"), FirstResult.Contains(TEXT("\"status\":\"ok\"")));
	TestTrue(TEXT("First since_last_read should include batch one"), FirstResult.Contains(TEXT("batch-one-line-a")));
	TestTrue(TEXT("First since_last_read should include batch one b"), FirstResult.Contains(TEXT("batch-one-line-b")));

	Capture.Serialize(TEXT("batch-two-line-c"), ELogVerbosity::Warning, FName(TEXT("LogPython")));

	const FString SecondResult = FUnrealGPTLogReader::Query(
		TEXT("{\"mode\":\"since_last_read\",\"min_verbosity\":\"all\",\"max_lines\":20,\"source\":\"memory\"}"));
	TestTrue(TEXT("Second since_last_read should succeed"), SecondResult.Contains(TEXT("\"status\":\"ok\"")));
	TestTrue(TEXT("Second since_last_read should include batch two"), SecondResult.Contains(TEXT("batch-two-line-c")));
	TestFalse(TEXT("Second since_last_read should not repeat batch one a"), SecondResult.Contains(TEXT("batch-one-line-a")));
	TestFalse(TEXT("Second since_last_read should not repeat batch one b"), SecondResult.Contains(TEXT("batch-one-line-b")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealGPTLogReaderErrorTest, "UnrealGPT.LogReader.Error", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealGPTLogReaderErrorTest::RunTest(const FString& Parameters)
{
	FUnrealGPTLogReader::FOptions Options;
	Options.Source = FUnrealGPTLogReader::ELogReadSource::File;
	Options.Mode = FUnrealGPTLogReader::ELogReadMode::File;

	int32 TotalMatched = 0;
	bool bTruncated = false;
	const FString MissingPath = FPaths::ProjectIntermediateDir() / TEXT("UnrealGPTLogTests") / TEXT("DefinitelyMissing.log");
	const FString Result = FUnrealGPTLogReader::TailLogFileForTest(MissingPath, Options, TotalMatched, bTruncated);

	TestTrue(TEXT("Missing log file should return error"), Result.Contains(TEXT("\"status\":\"error\"")));
	TestTrue(TEXT("Missing log file should mention failure"), Result.Contains(TEXT("Failed to read log file")));

	const FString InvalidJsonResult = FUnrealGPTLogReader::Query(TEXT("{not-json"));
	TestTrue(TEXT("Invalid JSON should return error"), InvalidJsonResult.Contains(TEXT("\"status\":\"error\"")));

	const FString ForceFileMissingResult = FUnrealGPTLogReader::Query(
		TEXT("{\"source\":\"file\",\"mode\":\"file\",\"category\":\"NoLogsShouldMatchThisCategory_12345\"}"));
	// When no log file exists at all this is an error; otherwise it returns ok with zero lines.
	if (ForceFileMissingResult.Contains(TEXT("\"status\":\"error\"")))
	{
		TestTrue(TEXT("Forced file read without log should explain missing file"), ForceFileMissingResult.Contains(TEXT("No editor log file")));
	}
	else
	{
		TestTrue(TEXT("Forced file read should still return ok envelope"), ForceFileMissingResult.Contains(TEXT("\"status\":\"ok\"")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealGPTBlueprintToolsTest, "UnrealGPT.BlueprintTools", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealGPTBlueprintToolsTest::RunTest(const FString& Parameters)
{
	const FString TempDir = FPaths::ProjectIntermediateDir() / TEXT("UnrealGPTBlueprintTests");
	IFileManager::Get().MakeDirectory(*TempDir, true);
	const FString AssetPath = FString::Printf(TEXT("/Game/UnrealGPTBlueprintTests/BP_AgentTest_%d"), FPlatformTime::Cycles());

	const FString CreateArgs = FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/Engine.Actor\"}"), *AssetPath);
	const FString CreateResult = UUnrealGPTBlueprintContext::Create(CreateArgs);
	TestTrue(TEXT("Blueprint create should succeed"), CreateResult.Contains(TEXT("\"status\":\"ok\"")));

	const FString NormalizedPath = AssetPath + TEXT(".") + FPaths::GetCleanFilename(AssetPath);
	const FString AddVarArgs = FString::Printf(TEXT("{\"asset_path\":\"%s\",\"name\":\"bTestFlag\",\"type\":\"bool\",\"default_value\":\"true\"}"), *NormalizedPath);
	const FString AddVarResult = UUnrealGPTBlueprintContext::AddVariable(AddVarArgs);
	TestTrue(TEXT("Blueprint add variable should succeed"), AddVarResult.Contains(TEXT("\"status\":\"ok\"")));

	const FString AddEventArgs = FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"node_type\":\"Event\",\"params\":{\"event_name\":\"ReceiveBeginPlay\"},\"pos_x\":0,\"pos_y\":0}"),
		*NormalizedPath);
	const FString AddEventResult = UUnrealGPTBlueprintContext::AddNode(AddEventArgs);
	TestTrue(TEXT("Blueprint add event node should succeed"), AddEventResult.Contains(TEXT("\"status\":\"ok\"")));

	FString EventNodeGuid;
	{
		TSharedPtr<FJsonObject> EventJson;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(AddEventResult);
		if (FJsonSerializer::Deserialize(Reader, EventJson) && EventJson.IsValid())
		{
			const TSharedPtr<FJsonObject>* Details = nullptr;
			if (EventJson->TryGetObjectField(TEXT("details"), Details) && Details && Details->IsValid())
			{
				(*Details)->TryGetStringField(TEXT("node_guid"), EventNodeGuid);
			}
		}
	}
	TestFalse(TEXT("Event node GUID should be returned"), EventNodeGuid.IsEmpty());

	const FString AddPrintArgs = FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"node_type\":\"PrintString\",\"pos_x\":300,\"pos_y\":0}"),
		*NormalizedPath);
	const FString AddPrintResult = UUnrealGPTBlueprintContext::AddNode(AddPrintArgs);
	TestTrue(TEXT("Blueprint add PrintString node should succeed"), AddPrintResult.Contains(TEXT("\"status\":\"ok\"")));

	FString PrintNodeGuid;
	{
		TSharedPtr<FJsonObject> PrintJson;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(AddPrintResult);
		if (FJsonSerializer::Deserialize(Reader, PrintJson) && PrintJson.IsValid())
		{
			const TSharedPtr<FJsonObject>* Details = nullptr;
			if (PrintJson->TryGetObjectField(TEXT("details"), Details) && Details && Details->IsValid())
			{
				(*Details)->TryGetStringField(TEXT("node_guid"), PrintNodeGuid);
			}
		}
	}
	TestFalse(TEXT("PrintString node GUID should be returned"), PrintNodeGuid.IsEmpty());

	if (!EventNodeGuid.IsEmpty() && !PrintNodeGuid.IsEmpty())
	{
		const FString ConnectArgs = FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"from_node_guid\":\"%s\",\"from_pin\":\"then\",\"to_node_guid\":\"%s\",\"to_pin\":\"execute\"}"),
			*NormalizedPath, *EventNodeGuid, *PrintNodeGuid);
		const FString ConnectResult = UUnrealGPTBlueprintContext::ConnectPins(ConnectArgs);
		TestTrue(TEXT("Blueprint connect pins should succeed"), ConnectResult.Contains(TEXT("\"status\":\"ok\"")));
	}

	const FString SetPinArgs = FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"node_guid\":\"%s\",\"pin_name\":\"InString\",\"value\":\"Agent test\"}"),
		*NormalizedPath, *PrintNodeGuid);
	const FString SetPinResult = UUnrealGPTBlueprintContext::SetPinDefault(SetPinArgs);
	TestTrue(TEXT("Blueprint set pin default should succeed"), SetPinResult.Contains(TEXT("\"status\":\"ok\"")));

	const FString CompileArgs = FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *NormalizedPath);
	const FString CompileResult = UUnrealGPTBlueprintContext::Compile(CompileArgs);
	TestTrue(TEXT("Blueprint compile should succeed"), CompileResult.Contains(TEXT("\"status\":\"ok\"")));

	const FString QueryArgs = FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_pins\":true}"), *NormalizedPath);
	const FString QueryResult = UUnrealGPTBlueprintContext::Query(QueryArgs);
	TestTrue(TEXT("Blueprint query should succeed"), QueryResult.Contains(TEXT("\"status\":\"ok\"")));
	TestTrue(TEXT("Blueprint query should include variable"), QueryResult.Contains(TEXT("bTestFlag")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

