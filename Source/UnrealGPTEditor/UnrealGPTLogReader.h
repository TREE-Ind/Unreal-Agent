// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"
#include "UnrealGPTLogCapture.h"

/**
 * read_log agent tool implementation.
 * Hybrid in-memory capture + efficient file-tail fallback.
 */
class UNREALGPTEDITOR_API FUnrealGPTLogReader
{
public:
	enum class ELogReadMode : uint8
	{
		Tail,
		SinceLastRead,
		File
	};

	enum class ELogReadSource : uint8
	{
		Auto,
		Memory,
		File
	};

	struct FOptions
	{
		int32 MaxLines = 40;
		int32 MaxChars = 8000;
		ELogVerbosity::Type MinVerbosity = ELogVerbosity::Warning;
		FString CategoryContains;
		FString MessageContains;
		ELogReadMode Mode = ELogReadMode::Tail;
		ELogReadSource Source = ELogReadSource::Auto;
	};

	/** Execute read_log from a JSON arguments object. */
	static FString Query(const FString& ArgumentsJson);

#if WITH_DEV_AUTOMATION_TESTS
	/** Test hook: tail a specific log file path. */
	static FString TailLogFileForTest(
		const FString& FilePath,
		const FOptions& Options,
		int32& OutTotalMatched,
		bool& OutTruncated);
#endif

private:
	static bool ParseOptions(const FString& ArgumentsJson, FOptions& OutOptions, FString& OutError);
	static ELogVerbosity::Type ParseMinVerbosity(const FString& Value);
	static FString VerbosityToString(ELogVerbosity::Type Verbosity);
	static FString ResolveLogFilePath();
	static bool TailLogFile(
		const FString& FilePath,
		const FOptions& Options,
		TArray<FUnrealGPTLogLine>& OutLines,
		int32& OutTotalMatched);
	static FString BuildResponseJson(
		const FString& Status,
		const FString& Message,
		const FString& Source,
		const FString& LogPath,
		const TArray<FUnrealGPTLogLine>& Lines,
		int32 TotalMatched,
		bool bTruncated);
	static FString ApplyCharBudget(TArray<FUnrealGPTLogLine>& Lines, int32 MaxChars, bool& OutTruncated);
};
