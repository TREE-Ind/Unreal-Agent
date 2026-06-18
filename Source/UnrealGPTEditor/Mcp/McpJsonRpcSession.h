// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"
#include "IMcpTransport.h"
#include "Dom/JsonObject.h"

/**
 * MCP JSON-RPC 2.0 session over any IMcpTransport.
 * Handles initialize handshake, request/response correlation, and notifications.
 */
class FMcpJsonRpcSession
{
public:
	explicit FMcpJsonRpcSession(TUniquePtr<IMcpTransport> InTransport);

	bool ConnectAndInitialize(float TimeoutSeconds, FString& OutError);
	void Disconnect();
	bool IsInitialized() const { return bInitialized; }

	bool SendRequest(
		const FString& Method,
		const TSharedPtr<FJsonObject>& Params,
		float TimeoutSeconds,
		TSharedPtr<FJsonObject>& OutResult,
		FString& OutError);

	bool SendNotification(
		const FString& Method,
		const TSharedPtr<FJsonObject>& Params,
		FString& OutError);

private:
	bool SendRawJson(const TSharedPtr<FJsonObject>& Message, FString& OutError);
	bool ReadJsonRpcResponse(int32 ExpectedId, float TimeoutSeconds, TSharedPtr<FJsonObject>& OutResult, FString& OutError);
	bool PerformInitialize(float TimeoutSeconds, FString& OutError);
	void DrainNotifications(float TimeoutSeconds);

	TUniquePtr<IMcpTransport> Transport;
	bool bInitialized = false;
	int32 NextRequestId = 1;
	FCriticalSection SessionLock;
};
