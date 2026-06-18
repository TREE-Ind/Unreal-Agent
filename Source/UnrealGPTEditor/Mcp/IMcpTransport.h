// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"

/** Low-level MCP wire transport (stdio pipes or HTTP/SSE). */
class IMcpTransport
{
public:
	virtual ~IMcpTransport() = default;

	virtual bool Connect(FString& OutError) = 0;
	virtual void Disconnect() = 0;
	virtual bool IsConnected() const = 0;

	/** Send one JSON-RPC message (single line for stdio). */
	virtual bool SendMessage(const FString& JsonMessage, FString& OutError) = 0;

	/** Read one JSON-RPC message. Blocks up to TimeoutSeconds. */
	virtual bool ReadMessage(FString& OutMessage, float TimeoutSeconds, FString& OutError) = 0;
};
