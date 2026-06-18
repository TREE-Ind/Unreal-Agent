// Copyright (c) 2025 TREE Industries.

#include "McpStdioTransport.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

FMcpStdioTransport::FMcpStdioTransport(const FMcpServerConfig& InConfig)
	: Config(InConfig)
{
}

FMcpStdioTransport::~FMcpStdioTransport()
{
	Disconnect();
}

bool FMcpStdioTransport::Connect(FString& OutError)
{
	if (Config.Command.IsEmpty())
	{
		OutError = TEXT("MCP stdio server command is empty");
		return false;
	}

	return EnsureProcessRunning(OutError);
}

void FMcpStdioTransport::Disconnect()
{
	if (ReadPipe || WritePipe)
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		ReadPipe = nullptr;
		WritePipe = nullptr;
	}

	if (ProcessHandle.IsValid())
	{
		FPlatformProcess::TerminateProc(ProcessHandle, true);
		FPlatformProcess::CloseProc(ProcessHandle);
		ProcessHandle.Reset();
	}

	ReadBuffer.Empty();
	bConnected = false;
}

bool FMcpStdioTransport::IsConnected() const
{
	if (!bConnected || !ProcessHandle.IsValid())
	{
		return false;
	}

	FProcHandle MutableHandle = ProcessHandle;
	return FPlatformProcess::IsProcRunning(MutableHandle);
}

bool FMcpStdioTransport::SendMessage(const FString& JsonMessage, FString& OutError)
{
	if (!EnsureProcessRunning(OutError))
	{
		return false;
	}

	FString Line = JsonMessage;
	Line.ReplaceInline(TEXT("\r"), TEXT(""));
	Line.ReplaceInline(TEXT("\n"), TEXT(""));
	Line.AppendChar(TEXT('\n'));

	if (!FPlatformProcess::WritePipe(WritePipe, Line, nullptr))
	{
		OutError = TEXT("Failed to write to MCP stdio process");
		bConnected = false;
		return false;
	}

	return true;
}

bool FMcpStdioTransport::ReadMessage(FString& OutMessage, float TimeoutSeconds, FString& OutError)
{
	OutMessage.Empty();

	if (!EnsureProcessRunning(OutError))
	{
		return false;
	}

	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (FPlatformTime::Seconds() < Deadline)
	{
		if (TryExtractLine(OutMessage))
		{
			return true;
		}

		if (!ReadPipe)
		{
			break;
		}

		const FString Chunk = FPlatformProcess::ReadPipe(ReadPipe);
		if (!Chunk.IsEmpty())
		{
			ReadBuffer += Chunk;
			if (TryExtractLine(OutMessage))
			{
				return true;
			}
		}
		else
		{
			FProcHandle MutableHandle = ProcessHandle;
			if (!FPlatformProcess::IsProcRunning(MutableHandle))
			{
				OutError = TEXT("MCP stdio process exited unexpectedly");
				bConnected = false;
				return false;
			}
		}

		FPlatformProcess::Sleep(0.01f);
	}

	OutError = TEXT("Timed out reading MCP stdio message");
	return false;
}

bool FMcpStdioTransport::EnsureProcessRunning(FString& OutError)
{
	if (IsConnected())
	{
		return true;
	}

	Disconnect();

	FString WorkingDirectory = FPaths::ProjectDir();

	uint32 ProcessId = 0;
	ProcessHandle = FPlatformProcess::CreateProc(
		*Config.Command,
		*Config.Arguments,
		true,
		false,
		true,
		&ProcessId,
		0,
		*WorkingDirectory,
		&WritePipe,
		&ReadPipe,
		nullptr);

	if (!ProcessHandle.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to start MCP stdio process: %s %s"), *Config.Command, *Config.Arguments);
		return false;
	}

	bConnected = true;
	return true;
}

bool FMcpStdioTransport::TryExtractLine(FString& OutLine)
{
	int32 NewlineIndex = INDEX_NONE;
	if (!ReadBuffer.FindChar(TEXT('\n'), NewlineIndex))
	{
		return false;
	}

	OutLine = ReadBuffer.Left(NewlineIndex);
	ReadBuffer = ReadBuffer.Mid(NewlineIndex + 1);
	OutLine.TrimStartAndEndInline();
	return !OutLine.IsEmpty();
}
