// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "McpServerManager.h"
#include "UnrealGPTMcpSubsystem.generated.h"

UCLASS()
class UNREALGPTEDITOR_API UUnrealGPTMcpSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	void ReloadServers();
	FMcpServerManager& GetManager() { return ServerManager; }
	const FMcpServerManager& GetManager() const { return ServerManager; }

	int32 GetConnectedServerCount() const { return ServerManager.GetConnectedCount(); }
	TArray<FMcpServerStatus> GetServerStatuses() const { return ServerManager.GetStatuses(); }
	bool HasEnabledServers() const { return ServerManager.HasEnabledServers(); }

	FString ExecuteMcpListTools(const FString& ArgumentsJson) const;
	FString ExecuteMcpCall(const FString& ArgumentsJson) const;
	FString ExecuteMcpReadResource(const FString& ArgumentsJson) const;
	FString ExecuteMcpGetPrompt(const FString& ArgumentsJson) const;

private:
	float GetTimeoutSeconds() const;

	mutable FCriticalSection ManagerLock;
	FMcpServerManager ServerManager;
};
