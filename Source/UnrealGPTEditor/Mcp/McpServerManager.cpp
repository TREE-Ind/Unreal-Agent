// Copyright (c) 2025 TREE Industries.

#include "McpServerManager.h"

#include "McpHttpTransport.h"
#include "McpStdioTransport.h"
#include "UnrealGPTSettings.h"

TUniquePtr<IMcpTransport> FMcpServerConnection::CreateTransport(const FMcpServerConfig& Config)
{
	if (Config.Transport == EMcpTransportType::HttpSse)
	{
		return MakeUnique<FMcpHttpTransport>(Config);
	}
	return MakeUnique<FMcpStdioTransport>(Config);
}

FMcpServerConnection::FMcpServerConnection(const FMcpServerConfig& InConfig)
	: Config(InConfig)
{
	Status.ServerName = Config.Name;
}

bool FMcpServerConnection::Connect(float TimeoutSeconds, FString& OutError)
{
	Disconnect();

	Session = MakeUnique<FMcpJsonRpcSession>(CreateTransport(Config));
	if (!Session->ConnectAndInitialize(TimeoutSeconds, OutError))
	{
		Status.bConnected = false;
		Status.bInitialized = false;
		Status.LastError = OutError;
		Session.Reset();
		return false;
	}

	Status.bConnected = true;
	Status.bInitialized = true;
	Status.LastError.Empty();

	if (!RefreshCapabilities(TimeoutSeconds, OutError))
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT MCP: Connected to '%s' but capability refresh failed: %s"), *Config.Name, *OutError);
	}

	return true;
}

void FMcpServerConnection::Disconnect()
{
	if (Session.IsValid())
	{
		Session->Disconnect();
		Session.Reset();
	}
	Status.bConnected = false;
	Status.bInitialized = false;
}

bool FMcpServerConnection::IsConnected() const
{
	return Session.IsValid() && Session->IsInitialized();
}

bool FMcpServerConnection::RefreshCapabilities(float TimeoutSeconds, FString& OutError)
{
	if (!Session.IsValid())
	{
		OutError = TEXT("MCP session is not active");
		return false;
	}

	TSharedPtr<FJsonObject> ToolsResult;
	if (Session->SendRequest(TEXT("tools/list"), MakeShared<FJsonObject>(), TimeoutSeconds, ToolsResult, OutError))
	{
		ParseToolsList(ToolsResult);
	}

	TSharedPtr<FJsonObject> ResourcesResult;
	FString ResourceError;
	if (Session->SendRequest(TEXT("resources/list"), MakeShared<FJsonObject>(), TimeoutSeconds, ResourcesResult, ResourceError))
	{
		ParseResourcesList(ResourcesResult);
	}

	TSharedPtr<FJsonObject> PromptsResult;
	FString PromptError;
	if (Session->SendRequest(TEXT("prompts/list"), MakeShared<FJsonObject>(), TimeoutSeconds, PromptsResult, PromptError))
	{
		ParsePromptsList(PromptsResult);
	}

	return true;
}

bool FMcpServerConnection::CallTool(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Arguments,
	float TimeoutSeconds,
	TSharedPtr<FJsonObject>& OutResult,
	FString& OutError)
{
	if (!Session.IsValid())
	{
		OutError = TEXT("MCP session is not active");
		return false;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), ToolName);
	Params->SetObjectField(TEXT("arguments"), Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>());
	return Session->SendRequest(TEXT("tools/call"), Params, TimeoutSeconds, OutResult, OutError);
}

bool FMcpServerConnection::ReadResource(
	const FString& Uri,
	float TimeoutSeconds,
	TSharedPtr<FJsonObject>& OutResult,
	FString& OutError)
{
	if (!Session.IsValid())
	{
		OutError = TEXT("MCP session is not active");
		return false;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("uri"), Uri);
	return Session->SendRequest(TEXT("resources/read"), Params, TimeoutSeconds, OutResult, OutError);
}

bool FMcpServerConnection::GetPrompt(
	const FString& PromptName,
	const TSharedPtr<FJsonObject>& Arguments,
	float TimeoutSeconds,
	TSharedPtr<FJsonObject>& OutResult,
	FString& OutError)
{
	if (!Session.IsValid())
	{
		OutError = TEXT("MCP session is not active");
		return false;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), PromptName);
	if (Arguments.IsValid())
	{
		Params->SetObjectField(TEXT("arguments"), Arguments);
	}
	return Session->SendRequest(TEXT("prompts/get"), Params, TimeoutSeconds, OutResult, OutError);
}

bool FMcpServerConnection::ParseToolsList(const TSharedPtr<FJsonObject>& Result)
{
	Status.Tools.Reset();
	if (!Result.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ToolsArray = nullptr;
	if (!Result->TryGetArrayField(TEXT("tools"), ToolsArray) || !ToolsArray)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Value : *ToolsArray)
	{
		const TSharedPtr<FJsonObject> ToolObj = Value->AsObject();
		if (!ToolObj.IsValid())
		{
			continue;
		}

		FMcpToolInfo Info;
		ToolObj->TryGetStringField(TEXT("name"), Info.Name);
		ToolObj->TryGetStringField(TEXT("description"), Info.Description);
		const TSharedPtr<FJsonObject>* SchemaObj = nullptr;
		if (ToolObj->TryGetObjectField(TEXT("inputSchema"), SchemaObj) && SchemaObj && SchemaObj->IsValid())
		{
			Info.InputSchema = *SchemaObj;
		}
		Status.Tools.Add(Info);
	}

	return true;
}

bool FMcpServerConnection::ParseResourcesList(const TSharedPtr<FJsonObject>& Result)
{
	Status.Resources.Reset();
	if (!Result.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ResourcesArray = nullptr;
	if (!Result->TryGetArrayField(TEXT("resources"), ResourcesArray) || !ResourcesArray)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Value : *ResourcesArray)
	{
		const TSharedPtr<FJsonObject> ResourceObj = Value->AsObject();
		if (!ResourceObj.IsValid())
		{
			continue;
		}

		FMcpResourceInfo Info;
		ResourceObj->TryGetStringField(TEXT("uri"), Info.Uri);
		ResourceObj->TryGetStringField(TEXT("name"), Info.Name);
		ResourceObj->TryGetStringField(TEXT("description"), Info.Description);
		ResourceObj->TryGetStringField(TEXT("mimeType"), Info.MimeType);
		Status.Resources.Add(Info);
	}

	return true;
}

bool FMcpServerConnection::ParsePromptsList(const TSharedPtr<FJsonObject>& Result)
{
	Status.Prompts.Reset();
	if (!Result.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* PromptsArray = nullptr;
	if (!Result->TryGetArrayField(TEXT("prompts"), PromptsArray) || !PromptsArray)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Value : *PromptsArray)
	{
		const TSharedPtr<FJsonObject> PromptObj = Value->AsObject();
		if (!PromptObj.IsValid())
		{
			continue;
		}

		FMcpPromptInfo Info;
		PromptObj->TryGetStringField(TEXT("name"), Info.Name);
		PromptObj->TryGetStringField(TEXT("description"), Info.Description);
		Status.Prompts.Add(Info);
	}

	return true;
}

void FMcpServerManager::ReloadFromSettings()
{
	DisconnectAll();

	const UUnrealGPTSettings* Settings = GetDefault<UUnrealGPTSettings>();
	if (!Settings || !Settings->bEnableMcpTool)
	{
		return;
	}

	for (const FMcpServerConfig& ServerConfig : Settings->McpServers)
	{
		if (!ServerConfig.bEnabled || ServerConfig.Name.IsEmpty())
		{
			continue;
		}

		TUniquePtr<FMcpServerConnection> Connection = MakeUnique<FMcpServerConnection>(ServerConfig);
		FString Error;
		if (!Connection->Connect(Settings->ExecutionTimeoutSeconds, Error))
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT MCP: Failed to connect server '%s': %s"), *ServerConfig.Name, *Error);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT MCP: Connected server '%s' with %d tool(s)"),
				*ServerConfig.Name, Connection->GetStatus().Tools.Num());
		}

		Connections.Add(MoveTemp(Connection));
	}
}

void FMcpServerManager::DisconnectAll()
{
	for (TUniquePtr<FMcpServerConnection>& Connection : Connections)
	{
		if (Connection.IsValid())
		{
			Connection->Disconnect();
		}
	}
	Connections.Reset();
}

bool FMcpServerManager::HasEnabledServers() const
{
	return Connections.Num() > 0;
}

int32 FMcpServerManager::GetConnectedCount() const
{
	int32 Count = 0;
	for (const TUniquePtr<FMcpServerConnection>& Connection : Connections)
	{
		if (Connection.IsValid() && Connection->IsConnected())
		{
			++Count;
		}
	}
	return Count;
}

TArray<FMcpServerStatus> FMcpServerManager::GetStatuses() const
{
	TArray<FMcpServerStatus> Statuses;
	for (const TUniquePtr<FMcpServerConnection>& Connection : Connections)
	{
		if (Connection.IsValid())
		{
			Statuses.Add(Connection->GetStatus());
		}
	}
	return Statuses;
}

FMcpServerConnection* FMcpServerManager::FindServer(const FString& ServerName)
{
	for (TUniquePtr<FMcpServerConnection>& Connection : Connections)
	{
		if (Connection.IsValid() && Connection->GetConfig().Name.Equals(ServerName, ESearchCase::IgnoreCase))
		{
			return Connection.Get();
		}
	}
	return nullptr;
}

const FMcpServerConnection* FMcpServerManager::FindServer(const FString& ServerName) const
{
	for (const TUniquePtr<FMcpServerConnection>& Connection : Connections)
	{
		if (Connection.IsValid() && Connection->GetConfig().Name.Equals(ServerName, ESearchCase::IgnoreCase))
		{
			return Connection.Get();
		}
	}
	return nullptr;
}

bool FMcpServerManager::EnsureConnected(const FString& ServerName, float TimeoutSeconds, FString& OutError)
{
	FMcpServerConnection* Connection = FindServer(ServerName);
	if (!Connection)
	{
		OutError = FString::Printf(TEXT("MCP server '%s' is not configured or enabled"), *ServerName);
		return false;
	}

	if (!Connection->IsConnected())
	{
		if (!Connection->Connect(TimeoutSeconds, OutError))
		{
			return false;
		}
	}

	return true;
}
