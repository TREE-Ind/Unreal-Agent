// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "McpTypes.generated.h"

UENUM()
enum class EMcpTransportType : uint8
{
	Stdio UMETA(DisplayName = "stdio (local process)"),
	HttpSse UMETA(DisplayName = "HTTP / SSE (remote)")
};

USTRUCT()
struct FMcpEnvVar
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "MCP")
	FString Key;

	UPROPERTY(EditAnywhere, Category = "MCP")
	FString Value;
};

USTRUCT()
struct FMcpHeader
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "MCP")
	FString Key;

	UPROPERTY(EditAnywhere, Category = "MCP")
	FString Value;
};

USTRUCT()
struct FMcpServerConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "MCP", meta = (DisplayName = "Server Name"))
	FString Name;

	UPROPERTY(EditAnywhere, Category = "MCP")
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, Category = "MCP")
	EMcpTransportType Transport = EMcpTransportType::Stdio;

	UPROPERTY(EditAnywhere, Category = "MCP", meta = (DisplayName = "Command", EditCondition = "Transport == EMcpTransportType::Stdio", EditConditionHides))
	FString Command;

	UPROPERTY(EditAnywhere, Category = "MCP", meta = (DisplayName = "Arguments", EditCondition = "Transport == EMcpTransportType::Stdio", EditConditionHides))
	FString Arguments;

	UPROPERTY(EditAnywhere, Category = "MCP", meta = (EditCondition = "Transport == EMcpTransportType::Stdio", EditConditionHides))
	TArray<FMcpEnvVar> Environment;

	UPROPERTY(EditAnywhere, Category = "MCP", meta = (DisplayName = "URL", EditCondition = "Transport == EMcpTransportType::HttpSse", EditConditionHides))
	FString Url;

	UPROPERTY(EditAnywhere, Category = "MCP", meta = (EditCondition = "Transport == EMcpTransportType::HttpSse", EditConditionHides))
	TArray<FMcpHeader> Headers;
};

struct FMcpToolInfo
{
	FString Name;
	FString Description;
	TSharedPtr<FJsonObject> InputSchema;
};

struct FMcpResourceInfo
{
	FString Uri;
	FString Name;
	FString Description;
	FString MimeType;
};

struct FMcpPromptInfo
{
	FString Name;
	FString Description;
	TArray<FString> ArgumentNames;
};

struct FMcpServerStatus
{
	FString ServerName;
	bool bConnected = false;
	bool bInitialized = false;
	FString LastError;
	TArray<FMcpToolInfo> Tools;
	TArray<FMcpResourceInfo> Resources;
	TArray<FMcpPromptInfo> Prompts;
};
