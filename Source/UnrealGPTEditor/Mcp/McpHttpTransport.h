// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"
#include "IMcpTransport.h"
#include "McpTypes.h"

/** MCP transport over HTTP POST with JSON or SSE response bodies. */
class FMcpHttpTransport : public IMcpTransport
{
public:
	explicit FMcpHttpTransport(const FMcpServerConfig& InConfig);
	virtual ~FMcpHttpTransport() override = default;

	virtual bool Connect(FString& OutError) override;
	virtual void Disconnect() override;
	virtual bool IsConnected() const override;

	virtual bool SendMessage(const FString& JsonMessage, FString& OutError) override;
	virtual bool ReadMessage(FString& OutMessage, float TimeoutSeconds, FString& OutError) override;

private:
	bool PerformHttpExchange(const FString& JsonMessage, float TimeoutSeconds, FString& OutResponseBody, FString& OutError);
	bool ExtractJsonFromSse(const FString& SseBody, FString& OutJson) const;

	FMcpServerConfig Config;
	bool bConnected = false;
	FString PendingResponse;
};
