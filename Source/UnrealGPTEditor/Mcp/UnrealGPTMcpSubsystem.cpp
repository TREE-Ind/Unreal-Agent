// Copyright (c) 2025 TREE Industries.

#include "UnrealGPTMcpSubsystem.h"

#include "McpResultNormalizer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealGPTSettings.h"

void UUnrealGPTMcpSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ReloadServers();
}

void UUnrealGPTMcpSubsystem::Deinitialize()
{
	FScopeLock Lock(&ManagerLock);
	ServerManager.DisconnectAll();
	Super::Deinitialize();
}

void UUnrealGPTMcpSubsystem::ReloadServers()
{
	FScopeLock Lock(&ManagerLock);
	ServerManager.ReloadFromSettings();
}

float UUnrealGPTMcpSubsystem::GetTimeoutSeconds() const
{
	const UUnrealGPTSettings* Settings = GetDefault<UUnrealGPTSettings>();
	return Settings ? Settings->ExecutionTimeoutSeconds : 90.0f;
}

FString UUnrealGPTMcpSubsystem::ExecuteMcpListTools(const FString& ArgumentsJson) const
{
	FScopeLock Lock(&ManagerLock);

	TSharedPtr<FJsonObject> ArgsObj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!ArgumentsJson.IsEmpty() && !(FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid()))
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Failed to parse mcp_list_tools arguments\"}");
	}

	FString ServerFilter;
	if (ArgsObj.IsValid())
	{
		ArgsObj->TryGetStringField(TEXT("server"), ServerFilter);
	}

	TArray<TSharedPtr<FJsonValue>> ServersArray;
	for (const FMcpServerStatus& Status : ServerManager.GetStatuses())
	{
		if (!ServerFilter.IsEmpty() && !Status.ServerName.Equals(ServerFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TSharedPtr<FJsonObject> ServerObj = MakeShared<FJsonObject>();
		ServerObj->SetStringField(TEXT("server"), Status.ServerName);
		ServerObj->SetBoolField(TEXT("connected"), Status.bConnected);

		TArray<TSharedPtr<FJsonValue>> ToolsArray;
		for (const FMcpToolInfo& Tool : Status.Tools)
		{
			TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
			ToolObj->SetStringField(TEXT("name"), Tool.Name);
			ToolObj->SetStringField(TEXT("description"), Tool.Description);
			if (Tool.InputSchema.IsValid())
			{
				ToolObj->SetObjectField(TEXT("inputSchema"), Tool.InputSchema);
			}
			ToolsArray.Add(MakeShared<FJsonValueObject>(ToolObj));
		}
		ServerObj->SetArrayField(TEXT("tools"), ToolsArray);
		ServersArray.Add(MakeShared<FJsonValueObject>(ServerObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetArrayField(TEXT("servers"), ServersArray);

	FString OutJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
	return OutJson;
}

FString UUnrealGPTMcpSubsystem::ExecuteMcpCall(const FString& ArgumentsJson) const
{
	FScopeLock Lock(&ManagerLock);

	TSharedPtr<FJsonObject> ArgsObj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!(FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid()))
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Failed to parse mcp_call arguments\"}");
	}

	FString ServerName;
	FString ToolName;
	if (!ArgsObj->TryGetStringField(TEXT("server"), ServerName) || ServerName.IsEmpty())
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Missing required field: server\"}");
	}
	if (!ArgsObj->TryGetStringField(TEXT("tool"), ToolName) || ToolName.IsEmpty())
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Missing required field: tool\"}");
	}

	const TSharedPtr<FJsonObject>* Arguments = nullptr;
	TSharedPtr<FJsonObject> ToolArgs = MakeShared<FJsonObject>();
	if (ArgsObj->TryGetObjectField(TEXT("arguments"), Arguments) && Arguments && Arguments->IsValid())
	{
		ToolArgs = *Arguments;
	}

	FString KindHint = TEXT("misc");
	ArgsObj->TryGetStringField(TEXT("output_kind"), KindHint);

	FString Error;
	if (!const_cast<FMcpServerManager&>(ServerManager).EnsureConnected(ServerName, GetTimeoutSeconds(), Error))
	{
		return FString::Printf(TEXT("{\"status\":\"error\",\"message\":\"%s\"}"), *Error);
	}

	FMcpServerConnection* Connection = const_cast<FMcpServerManager&>(ServerManager).FindServer(ServerName);
	if (!Connection)
	{
		return TEXT("{\"status\":\"error\",\"message\":\"MCP server not found\"}");
	}

	TSharedPtr<FJsonObject> McpResult;
	if (!Connection->CallTool(ToolName, ToolArgs, GetTimeoutSeconds(), McpResult, Error))
	{
		return FString::Printf(TEXT("{\"status\":\"error\",\"message\":\"%s\"}"), *Error);
	}

	return FMcpResultNormalizer::BuildToolCallEnvelope(ServerName, ToolName, McpResult, KindHint);
}

FString UUnrealGPTMcpSubsystem::ExecuteMcpReadResource(const FString& ArgumentsJson) const
{
	FScopeLock Lock(&ManagerLock);

	TSharedPtr<FJsonObject> ArgsObj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!(FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid()))
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Failed to parse mcp_read_resource arguments\"}");
	}

	FString ServerName;
	FString Uri;
	if (!ArgsObj->TryGetStringField(TEXT("server"), ServerName) || ServerName.IsEmpty())
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Missing required field: server\"}");
	}
	if (!ArgsObj->TryGetStringField(TEXT("uri"), Uri) || Uri.IsEmpty())
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Missing required field: uri\"}");
	}

	FString Error;
	if (!const_cast<FMcpServerManager&>(ServerManager).EnsureConnected(ServerName, GetTimeoutSeconds(), Error))
	{
		return FString::Printf(TEXT("{\"status\":\"error\",\"message\":\"%s\"}"), *Error);
	}

	FMcpServerConnection* Connection = const_cast<FMcpServerManager&>(ServerManager).FindServer(ServerName);
	if (!Connection)
	{
		return TEXT("{\"status\":\"error\",\"message\":\"MCP server not found\"}");
	}

	TSharedPtr<FJsonObject> McpResult;
	if (!Connection->ReadResource(Uri, GetTimeoutSeconds(), McpResult, Error))
	{
		return FString::Printf(TEXT("{\"status\":\"error\",\"message\":\"%s\"}"), *Error);
	}

	return FMcpResultNormalizer::BuildToolCallEnvelope(ServerName, TEXT("resources/read"), McpResult, TEXT("misc"));
}

FString UUnrealGPTMcpSubsystem::ExecuteMcpGetPrompt(const FString& ArgumentsJson) const
{
	FScopeLock Lock(&ManagerLock);

	TSharedPtr<FJsonObject> ArgsObj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!(FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid()))
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Failed to parse mcp_get_prompt arguments\"}");
	}

	FString ServerName;
	FString PromptName;
	if (!ArgsObj->TryGetStringField(TEXT("server"), ServerName) || ServerName.IsEmpty())
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Missing required field: server\"}");
	}
	if (!ArgsObj->TryGetStringField(TEXT("name"), PromptName) || PromptName.IsEmpty())
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Missing required field: name\"}");
	}

	const TSharedPtr<FJsonObject>* Arguments = nullptr;
	TSharedPtr<FJsonObject> PromptArgs;
	if (ArgsObj->TryGetObjectField(TEXT("arguments"), Arguments) && Arguments && Arguments->IsValid())
	{
		PromptArgs = *Arguments;
	}

	FString Error;
	if (!const_cast<FMcpServerManager&>(ServerManager).EnsureConnected(ServerName, GetTimeoutSeconds(), Error))
	{
		return FString::Printf(TEXT("{\"status\":\"error\",\"message\":\"%s\"}"), *Error);
	}

	FMcpServerConnection* Connection = const_cast<FMcpServerManager&>(ServerManager).FindServer(ServerName);
	if (!Connection)
	{
		return TEXT("{\"status\":\"error\",\"message\":\"MCP server not found\"}");
	}

	TSharedPtr<FJsonObject> McpResult;
	if (!Connection->GetPrompt(PromptName, PromptArgs, GetTimeoutSeconds(), McpResult, Error))
	{
		return FString::Printf(TEXT("{\"status\":\"error\",\"message\":\"%s\"}"), *Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetObjectField(TEXT("details"), McpResult);
	FString OutJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
	return OutJson;
}
