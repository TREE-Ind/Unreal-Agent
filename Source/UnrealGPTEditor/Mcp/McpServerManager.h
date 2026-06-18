// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"
#include "McpJsonRpcSession.h"
#include "McpTypes.h"

class FMcpServerConnection
{
public:
	explicit FMcpServerConnection(const FMcpServerConfig& InConfig);

	bool Connect(float TimeoutSeconds, FString& OutError);
	void Disconnect();
	bool IsConnected() const;

	const FMcpServerConfig& GetConfig() const { return Config; }
	const FMcpServerStatus& GetStatus() const { return Status; }

	bool RefreshCapabilities(float TimeoutSeconds, FString& OutError);
	bool CallTool(
		const FString& ToolName,
		const TSharedPtr<FJsonObject>& Arguments,
		float TimeoutSeconds,
		TSharedPtr<FJsonObject>& OutResult,
		FString& OutError);
	bool ReadResource(
		const FString& Uri,
		float TimeoutSeconds,
		TSharedPtr<FJsonObject>& OutResult,
		FString& OutError);
	bool GetPrompt(
		const FString& PromptName,
		const TSharedPtr<FJsonObject>& Arguments,
		float TimeoutSeconds,
		TSharedPtr<FJsonObject>& OutResult,
		FString& OutError);

private:
	static TUniquePtr<IMcpTransport> CreateTransport(const FMcpServerConfig& Config);
	bool ParseToolsList(const TSharedPtr<FJsonObject>& Result);
	bool ParseResourcesList(const TSharedPtr<FJsonObject>& Result);
	bool ParsePromptsList(const TSharedPtr<FJsonObject>& Result);

	FMcpServerConfig Config;
	FMcpServerStatus Status;
	TUniquePtr<FMcpJsonRpcSession> Session;
};

class FMcpServerManager
{
public:
	void ReloadFromSettings();
	void DisconnectAll();

	bool HasEnabledServers() const;
	int32 GetConnectedCount() const;
	TArray<FMcpServerStatus> GetStatuses() const;

	FMcpServerConnection* FindServer(const FString& ServerName);
	const FMcpServerConnection* FindServer(const FString& ServerName) const;

	bool EnsureConnected(const FString& ServerName, float TimeoutSeconds, FString& OutError);

private:
	TArray<TUniquePtr<FMcpServerConnection>> Connections;
};
