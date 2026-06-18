// Copyright (c) 2025 TREE Industries.

#include "UnrealGPTLogReader.h"
#include "Dom/JsonObject.h"
#include "GenericPlatform/GenericPlatformOutputDevices.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UnrealGPTLogReaderPrivate
{
	static constexpr int32 MaxLinesHardCap = 150;
	static constexpr int32 DefaultMaxLines = 40;
	static constexpr int32 DefaultMaxChars = 8000;
	static constexpr int64 FileTailChunkBytes = 256 * 1024;

	static FString SerializeJson(const TSharedPtr<FJsonObject>& Root)
	{
		FString OutJson;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return OutJson;
	}

	static TSharedPtr<FJsonObject> MakeError(const FString& Message)
	{
		TSharedPtr<FJsonObject> ErrorObj = MakeShareable(new FJsonObject);
		ErrorObj->SetStringField(TEXT("status"), TEXT("error"));
		ErrorObj->SetStringField(TEXT("message"), Message);
		return ErrorObj;
	}

	static bool ParseLogLine(const FString& RawLine, FUnrealGPTLogLine& OutLine)
	{
		OutLine.Message = RawLine;
		OutLine.Verbosity = ELogVerbosity::Log;
		OutLine.Category = TEXT("Log");

		FString Remainder = RawLine;
		int32 BracketEnd = INDEX_NONE;
		int32 SearchStart = 0;
		for (int32 BracketIndex = 0; BracketIndex < 2; ++BracketIndex)
		{
			const int32 Found = Remainder.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchStart);
			if (Found == INDEX_NONE)
			{
				break;
			}
			BracketEnd = Found;
			SearchStart = Found + 1;
		}

		if (BracketEnd != INDEX_NONE && BracketEnd + 1 < RawLine.Len())
		{
			Remainder = RawLine.Mid(BracketEnd + 1).TrimStart();
		}

		int32 ColonIndex = INDEX_NONE;
		if (!Remainder.FindChar(TEXT(':'), ColonIndex))
		{
			return false;
		}

		OutLine.Category = Remainder.Left(ColonIndex);
		FString AfterCategory = Remainder.Mid(ColonIndex + 1).TrimStart();

		if (AfterCategory.StartsWith(TEXT("Error:"), ESearchCase::IgnoreCase))
		{
			OutLine.Verbosity = ELogVerbosity::Error;
			OutLine.Message = AfterCategory.Mid(6).TrimStart();
		}
		else if (AfterCategory.StartsWith(TEXT("Warning:"), ESearchCase::IgnoreCase))
		{
			OutLine.Verbosity = ELogVerbosity::Warning;
			OutLine.Message = AfterCategory.Mid(8).TrimStart();
		}
		else if (AfterCategory.StartsWith(TEXT("Display:"), ESearchCase::IgnoreCase))
		{
			OutLine.Verbosity = ELogVerbosity::Display;
			OutLine.Message = AfterCategory.Mid(8).TrimStart();
		}
		else
		{
			OutLine.Message = AfterCategory;
		}

		return true;
	}

	static bool PassesFilters(const FUnrealGPTLogLine& Line, const FUnrealGPTLogReader::FOptions& Options)
	{
		if (Line.Verbosity > Options.MinVerbosity)
		{
			return false;
		}

		if (!Options.CategoryContains.IsEmpty()
			&& !Line.Category.Contains(Options.CategoryContains, ESearchCase::IgnoreCase))
		{
			return false;
		}

		if (!Options.MessageContains.IsEmpty()
			&& !Line.Message.Contains(Options.MessageContains, ESearchCase::IgnoreCase))
		{
			return false;
		}

		return true;
	}
}

FString FUnrealGPTLogReader::VerbosityToString(ELogVerbosity::Type Verbosity)
{
	switch (Verbosity)
	{
	case ELogVerbosity::Fatal:
		return TEXT("Fatal");
	case ELogVerbosity::Error:
		return TEXT("Error");
	case ELogVerbosity::Warning:
		return TEXT("Warning");
	case ELogVerbosity::Display:
		return TEXT("Display");
	case ELogVerbosity::Log:
		return TEXT("Log");
	case ELogVerbosity::Verbose:
		return TEXT("Verbose");
	case ELogVerbosity::VeryVerbose:
		return TEXT("VeryVerbose");
	default:
		return TEXT("Log");
	}
}

ELogVerbosity::Type FUnrealGPTLogReader::ParseMinVerbosity(const FString& Value)
{
	const FString Normalized = Value.ToLower();
	if (Normalized == TEXT("error"))
	{
		return ELogVerbosity::Error;
	}
	if (Normalized == TEXT("warning"))
	{
		return ELogVerbosity::Warning;
	}
	if (Normalized == TEXT("display"))
	{
		return ELogVerbosity::Display;
	}
	if (Normalized == TEXT("log"))
	{
		return ELogVerbosity::Log;
	}
	if (Normalized == TEXT("all") || Normalized == TEXT("verbose"))
	{
		return ELogVerbosity::VeryVerbose;
	}
	return ELogVerbosity::Warning;
}

bool FUnrealGPTLogReader::ParseOptions(const FString& ArgumentsJson, FOptions& OutOptions, FString& OutError)
{
	OutOptions = FOptions();

	if (ArgumentsJson.IsEmpty())
	{
		return true;
	}

	TSharedPtr<FJsonObject> ArgsObj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!FJsonSerializer::Deserialize(Reader, ArgsObj) || !ArgsObj.IsValid())
	{
		OutError = TEXT("Invalid JSON arguments for read_log");
		return false;
	}

	double MaxLinesValue = OutOptions.MaxLines;
	if (ArgsObj->TryGetNumberField(TEXT("max_lines"), MaxLinesValue))
	{
		OutOptions.MaxLines = FMath::Clamp(static_cast<int32>(MaxLinesValue), 1, UnrealGPTLogReaderPrivate::MaxLinesHardCap);
	}

	double MaxCharsValue = OutOptions.MaxChars;
	if (ArgsObj->TryGetNumberField(TEXT("max_chars"), MaxCharsValue))
	{
		OutOptions.MaxChars = FMath::Max(256, static_cast<int32>(MaxCharsValue));
	}

	FString MinVerbosityValue;
	if (ArgsObj->TryGetStringField(TEXT("min_verbosity"), MinVerbosityValue))
	{
		OutOptions.MinVerbosity = ParseMinVerbosity(MinVerbosityValue);
	}

	ArgsObj->TryGetStringField(TEXT("category"), OutOptions.CategoryContains);
	ArgsObj->TryGetStringField(TEXT("contains"), OutOptions.MessageContains);

	FString ModeValue;
	if (ArgsObj->TryGetStringField(TEXT("mode"), ModeValue))
	{
		const FString ModeLower = ModeValue.ToLower();
		if (ModeLower == TEXT("since_last_read"))
		{
			OutOptions.Mode = ELogReadMode::SinceLastRead;
		}
		else if (ModeLower == TEXT("file"))
		{
			OutOptions.Mode = ELogReadMode::File;
		}
		else
		{
			OutOptions.Mode = ELogReadMode::Tail;
		}
	}

	FString SourceValue;
	if (ArgsObj->TryGetStringField(TEXT("source"), SourceValue))
	{
		const FString SourceLower = SourceValue.ToLower();
		if (SourceLower == TEXT("memory"))
		{
			OutOptions.Source = ELogReadSource::Memory;
		}
		else if (SourceLower == TEXT("file"))
		{
			OutOptions.Source = ELogReadSource::File;
		}
		else
		{
			OutOptions.Source = ELogReadSource::Auto;
		}
	}

	return true;
}

FString FUnrealGPTLogReader::ResolveLogFilePath()
{
	const FString AbsolutePath = FGenericPlatformOutputDevices::GetAbsoluteLogFilename();
	if (!AbsolutePath.IsEmpty() && FPaths::FileExists(AbsolutePath))
	{
		return AbsolutePath;
	}

	const FString LogDir = FPaths::ProjectLogDir();
	TArray<FString> LogFiles;
	IFileManager::Get().FindFiles(LogFiles, *LogDir, TEXT("*.log"));
	if (LogFiles.Num() == 0)
	{
		return FString();
	}

	LogFiles.Sort([](const FString& A, const FString& B)
	{
		return A > B;
	});

	for (const FString& FileName : LogFiles)
	{
		const FString FullPath = FPaths::Combine(LogDir, FileName);
		if (FPaths::FileExists(FullPath))
		{
			return FullPath;
		}
	}

	return FString();
}

bool FUnrealGPTLogReader::TailLogFile(
	const FString& FilePath,
	const FOptions& Options,
	TArray<FUnrealGPTLogLine>& OutLines,
	int32& OutTotalMatched)
{
	OutLines.Reset();
	OutTotalMatched = 0;

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.FileExists(*FilePath))
	{
		return false;
	}

	TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*FilePath));
	if (!Handle.IsValid())
	{
		return false;
	}

	const int64 FileSize = Handle->Size();
	if (FileSize <= 0)
	{
		return true;
	}

	const int64 ReadSize = FMath::Min<int64>(FileSize, UnrealGPTLogReaderPrivate::FileTailChunkBytes);
	const int64 Offset = FileSize - ReadSize;
	Handle->Seek(Offset);

	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(static_cast<int32>(ReadSize));
	if (!Handle->Read(Buffer.GetData(), ReadSize))
	{
		return false;
	}

	FUTF8ToTCHAR Convert(reinterpret_cast<const ANSICHAR*>(Buffer.GetData()), Buffer.Num());
	FString Chunk(Convert.Length(), Convert.Get());
	if (Offset > 0)
	{
		int32 FirstNewline = INDEX_NONE;
		if (Chunk.FindChar(TEXT('\n'), FirstNewline))
		{
			Chunk = Chunk.Mid(FirstNewline + 1);
		}
		else
		{
			Chunk.Reset();
		}
	}

	TArray<FString> RawLines;
	Chunk.ParseIntoArray(RawLines, TEXT("\n"), true);

	TArray<FUnrealGPTLogLine> Matched;
	Matched.Reserve(Options.MaxLines);

	for (int32 Index = RawLines.Num() - 1; Index >= 0; --Index)
	{
		const FString& RawLine = RawLines[Index];
		if (RawLine.IsEmpty())
		{
			continue;
		}

		FUnrealGPTLogLine Line;
		if (!UnrealGPTLogReaderPrivate::ParseLogLine(RawLine, Line))
		{
			Line.Message = RawLine;
			Line.Category = TEXT("Log");
			Line.Verbosity = ELogVerbosity::Log;
		}

		if (!UnrealGPTLogReaderPrivate::PassesFilters(Line, Options))
		{
			continue;
		}

		++OutTotalMatched;
		if (Matched.Num() < Options.MaxLines)
		{
			Matched.Add(MoveTemp(Line));
		}
	}

	Algo::Reverse(Matched);
	OutLines = MoveTemp(Matched);
	return true;
}

FString FUnrealGPTLogReader::ApplyCharBudget(TArray<FUnrealGPTLogLine>& Lines, int32 MaxChars, bool& OutTruncated)
{
	OutTruncated = false;
	if (MaxChars <= 0 || Lines.Num() == 0)
	{
		return FString();
	}

	auto EstimateLineChars = [](const FUnrealGPTLogLine& Line)
	{
		return Line.Category.Len() + Line.Message.Len() + 32;
	};

	int32 TotalChars = 0;
	for (const FUnrealGPTLogLine& Line : Lines)
	{
		TotalChars += EstimateLineChars(Line);
	}

	if (TotalChars <= MaxChars)
	{
		return FString();
	}

	OutTruncated = true;
	while (!Lines.IsEmpty() && TotalChars > MaxChars)
	{
		TotalChars -= EstimateLineChars(Lines[0]);
		Lines.RemoveAt(0);
	}

	return FString();
}

FString FUnrealGPTLogReader::BuildResponseJson(
	const FString& Status,
	const FString& Message,
	const FString& Source,
	const FString& EditorLogFilePath,
	const TArray<FUnrealGPTLogLine>& Lines,
	int32 TotalMatched,
	bool bTruncated)
{
	TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
	Root->SetStringField(TEXT("status"), Status);
	if (!Message.IsEmpty())
	{
		Root->SetStringField(TEXT("message"), Message);
	}
	if (!Source.IsEmpty())
	{
		Root->SetStringField(TEXT("source"), Source);
	}
	if (!EditorLogFilePath.IsEmpty())
	{
		Root->SetStringField(TEXT("log_path"), EditorLogFilePath);
	}
	Root->SetNumberField(TEXT("line_count"), Lines.Num());
	Root->SetNumberField(TEXT("total_matched"), TotalMatched);
	Root->SetBoolField(TEXT("truncated"), bTruncated);

	TArray<TSharedPtr<FJsonValue>> LineValues;
	LineValues.Reserve(Lines.Num());
	for (const FUnrealGPTLogLine& Line : Lines)
	{
		TSharedPtr<FJsonObject> LineObj = MakeShareable(new FJsonObject);
		LineObj->SetStringField(TEXT("v"), VerbosityToString(Line.Verbosity));
		LineObj->SetStringField(TEXT("cat"), Line.Category);
		LineObj->SetStringField(TEXT("msg"), Line.Message);
		LineValues.Add(MakeShareable(new FJsonValueObject(LineObj)));
	}
	Root->SetArrayField(TEXT("lines"), LineValues);

	return UnrealGPTLogReaderPrivate::SerializeJson(Root);
}

FString FUnrealGPTLogReader::Query(const FString& ArgumentsJson)
{
	FOptions Options;
	FString ParseError;
	if (!ParseOptions(ArgumentsJson, Options, ParseError))
	{
		return UnrealGPTLogReaderPrivate::SerializeJson(UnrealGPTLogReaderPrivate::MakeError(ParseError));
	}

	const FString EditorLogFilePath = ResolveLogFilePath();
	const bool bForceFile = (Options.Mode == ELogReadMode::File) || (Options.Source == ELogReadSource::File);
	const bool bPreferMemory = !bForceFile && (Options.Source == ELogReadSource::Auto || Options.Source == ELogReadSource::Memory);

	TArray<FUnrealGPTLogLine> Lines;
	int32 TotalMatched = 0;
	FString SourceUsed;
	bool bTruncated = false;

	if (bPreferMemory)
	{
		FUnrealGPTLogQueryFilters Filters;
		Filters.MinVerbosity = Options.MinVerbosity;
		Filters.CategoryContains = Options.CategoryContains;
		Filters.MessageContains = Options.MessageContains;
		Filters.MaxLines = Options.MaxLines;
		Filters.bTailFromEnd = (Options.Mode != ELogReadMode::SinceLastRead);

		FUnrealGPTLogCapture& Capture = FUnrealGPTLogCapture::Get();
		if (Options.Mode == ELogReadMode::SinceLastRead)
		{
			Filters.StartIndex = Capture.GetReadCursor();
		}

		const FUnrealGPTLogQueryResult MemoryResult = Capture.QueryLines(Filters);
		Lines = MemoryResult.Lines;
		TotalMatched = MemoryResult.TotalMatched;

		if (Options.Mode == ELogReadMode::SinceLastRead)
		{
			Capture.SetReadCursor(MemoryResult.NextReadIndex);
		}

		if (Lines.Num() > 0 || Options.Source == ELogReadSource::Memory)
		{
			SourceUsed = TEXT("memory");
		}
	}

	if (SourceUsed.IsEmpty())
	{
		if (EditorLogFilePath.IsEmpty())
		{
			return UnrealGPTLogReaderPrivate::SerializeJson(
				UnrealGPTLogReaderPrivate::MakeError(TEXT("No editor log file found under Saved/Logs")));
		}

		if (!TailLogFile(EditorLogFilePath, Options, Lines, TotalMatched))
		{
			return UnrealGPTLogReaderPrivate::SerializeJson(
				UnrealGPTLogReaderPrivate::MakeError(FString::Printf(TEXT("Failed to read log file: %s"), *EditorLogFilePath)));
		}

		SourceUsed = TEXT("file");
	}

	ApplyCharBudget(Lines, Options.MaxChars, bTruncated);
	if (TotalMatched > Lines.Num())
	{
		bTruncated = true;
	}

	return BuildResponseJson(TEXT("ok"), FString(), SourceUsed, EditorLogFilePath, Lines, TotalMatched, bTruncated);
}

#if WITH_DEV_AUTOMATION_TESTS
FString FUnrealGPTLogReader::TailLogFileForTest(
	const FString& FilePath,
	const FOptions& Options,
	int32& OutTotalMatched,
	bool& OutTruncated)
{
	TArray<FUnrealGPTLogLine> Lines;
	if (!TailLogFile(FilePath, Options, Lines, OutTotalMatched))
	{
		return UnrealGPTLogReaderPrivate::SerializeJson(
			UnrealGPTLogReaderPrivate::MakeError(FString::Printf(TEXT("Failed to read log file: %s"), *FilePath)));
	}

	ApplyCharBudget(Lines, Options.MaxChars, OutTruncated);
	if (OutTotalMatched > Lines.Num())
	{
		OutTruncated = true;
	}

	return BuildResponseJson(TEXT("ok"), FString(), TEXT("file"), FilePath, Lines, OutTotalMatched, OutTruncated);
}
#endif
