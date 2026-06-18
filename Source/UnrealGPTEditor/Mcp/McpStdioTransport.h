// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"
#include "IMcpTransport.h"
#include "McpTypes.h"

/** MCP transport over a child process stdin/stdout (newline-delimited JSON-RPC). */
class FMcpStdioTransport : public IMcpTransport
{
public:
	explicit FMcpStdioTransport(const FMcpServerConfig& InConfig);
	virtual ~FMcpStdioTransport() override;

	virtual bool Connect(FString& OutError) override;
	virtual void Disconnect() override;
	virtual bool IsConnected() const override;

	virtual bool SendMessage(const FString& JsonMessage, FString& OutError) override;
	virtual bool ReadMessage(FString& OutMessage, float TimeoutSeconds, FString& OutError) override;

private:
	bool EnsureProcessRunning(FString& OutError);
	bool TryExtractLine(FString& OutLine);

	FMcpServerConfig Config;
	FProcHandle ProcessHandle;
	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	FString ReadBuffer;
	bool bConnected = false;
};
