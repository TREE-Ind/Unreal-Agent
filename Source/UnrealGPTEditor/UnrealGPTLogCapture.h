// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

/** Single captured log line from the UE logging system. */
struct FUnrealGPTLogLine
{
	int64 TimestampMs = 0;
	ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
	FString Category;
	FString Message;
};

/** Filters applied when querying the in-memory log ring buffer. */
struct FUnrealGPTLogQueryFilters
{
	ELogVerbosity::Type MinVerbosity = ELogVerbosity::Warning;
	FString CategoryContains;
	FString MessageContains;
	int32 MaxLines = 40;
	int32 StartIndex = 0;
	bool bTailFromEnd = true;
};

/** Query result from the in-memory log ring buffer. */
struct FUnrealGPTLogQueryResult
{
	TArray<FUnrealGPTLogLine> Lines;
	int32 TotalMatched = 0;
	int32 NextReadIndex = 0;
};

/**
 * Thread-safe ring buffer that captures UE log output via FOutputDevice.
 * Registered on GLog at module startup.
 */
class UNREALGPTEDITOR_API FUnrealGPTLogCapture : public FOutputDevice
{
public:
	static constexpr int32 MaxBufferLines = 2000;

	static FUnrealGPTLogCapture& Get();

	void Initialize();
	void Shutdown();

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;

	FUnrealGPTLogQueryResult QueryLines(const FUnrealGPTLogQueryFilters& Filters) const;
	int32 GetLineCount() const;
	void ResetReadCursor();
	int32 GetReadCursor() const { return ReadCursor; }
	void SetReadCursor(int32 NewCursor) { ReadCursor = NewCursor; }

private:
	FUnrealGPTLogCapture() = default;

	void AppendLine(ELogVerbosity::Type Verbosity, const FString& Category, const FString& Message);
	bool PassesFilters(const FUnrealGPTLogLine& Line, const FUnrealGPTLogQueryFilters& Filters) const;

	mutable FCriticalSection BufferLock;
	TArray<FUnrealGPTLogLine> Lines;
	int32 ReadCursor = 0;
	bool bRegistered = false;
};
