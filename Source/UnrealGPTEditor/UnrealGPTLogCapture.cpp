// Copyright (c) 2025 TREE Industries.

#include "UnrealGPTLogCapture.h"
#include "Misc/DateTime.h"

FUnrealGPTLogCapture& FUnrealGPTLogCapture::Get()
{
	static FUnrealGPTLogCapture Instance;
	return Instance;
}

void FUnrealGPTLogCapture::Initialize()
{
	FScopeLock Lock(&BufferLock);
	if (!bRegistered)
	{
		GLog->AddOutputDevice(this);
		bRegistered = true;
	}
}

void FUnrealGPTLogCapture::Shutdown()
{
	FScopeLock Lock(&BufferLock);
	if (bRegistered)
	{
		GLog->RemoveOutputDevice(this);
		bRegistered = false;
	}
}

void FUnrealGPTLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	if (!V)
	{
		return;
	}

	const FString Message(V);
	if (Message.IsEmpty())
	{
		return;
	}

	AppendLine(Verbosity, Category.ToString(), Message);
}

void FUnrealGPTLogCapture::AppendLine(ELogVerbosity::Type Verbosity, const FString& Category, const FString& Message)
{
	FScopeLock Lock(&BufferLock);

	FUnrealGPTLogLine Line;
	Line.TimestampMs = FDateTime::UtcNow().ToUnixTimestamp() * 1000LL + FDateTime::UtcNow().GetMillisecond();
	Line.Verbosity = Verbosity;
	Line.Category = Category;
	Line.Message = Message;

	Lines.Add(MoveTemp(Line));
	if (Lines.Num() > MaxBufferLines)
	{
		const int32 Overflow = Lines.Num() - MaxBufferLines;
		Lines.RemoveAt(0, Overflow, EAllowShrinking::No);
		if (ReadCursor > 0)
		{
			ReadCursor = FMath::Max(0, ReadCursor - Overflow);
		}
	}
}

bool FUnrealGPTLogCapture::PassesFilters(const FUnrealGPTLogLine& Line, const FUnrealGPTLogQueryFilters& Filters) const
{
	if (Line.Verbosity > Filters.MinVerbosity)
	{
		return false;
	}

	if (!Filters.CategoryContains.IsEmpty()
		&& !Line.Category.Contains(Filters.CategoryContains, ESearchCase::IgnoreCase))
	{
		return false;
	}

	if (!Filters.MessageContains.IsEmpty()
		&& !Line.Message.Contains(Filters.MessageContains, ESearchCase::IgnoreCase))
	{
		return false;
	}

	return true;
}

FUnrealGPTLogQueryResult FUnrealGPTLogCapture::QueryLines(const FUnrealGPTLogQueryFilters& Filters) const
{
	FScopeLock Lock(&BufferLock);

	FUnrealGPTLogQueryResult Result;
	Result.NextReadIndex = Filters.StartIndex;

	if (Filters.bTailFromEnd)
	{
		TArray<FUnrealGPTLogLine> Matched;
		Matched.Reserve(Filters.MaxLines);

		for (int32 Index = Lines.Num() - 1; Index >= 0; --Index)
		{
			const FUnrealGPTLogLine& Line = Lines[Index];
			if (!PassesFilters(Line, Filters))
			{
				continue;
			}

			++Result.TotalMatched;
			if (Matched.Num() < Filters.MaxLines)
			{
				Matched.Add(Line);
			}
		}

		Algo::Reverse(Matched);
		Result.Lines = MoveTemp(Matched);
		Result.NextReadIndex = Lines.Num();
	}
	else
	{
		const int32 Start = FMath::Clamp(Filters.StartIndex, 0, Lines.Num());
		for (int32 Index = Start; Index < Lines.Num(); ++Index)
		{
			const FUnrealGPTLogLine& Line = Lines[Index];
			if (!PassesFilters(Line, Filters))
			{
				continue;
			}

			++Result.TotalMatched;
			Result.Lines.Add(Line);
			if (Result.Lines.Num() >= Filters.MaxLines)
			{
				Result.NextReadIndex = Index + 1;
				return Result;
			}
		}

		Result.NextReadIndex = Lines.Num();
	}

	return Result;
}

int32 FUnrealGPTLogCapture::GetLineCount() const
{
	FScopeLock Lock(&BufferLock);
	return Lines.Num();
}

void FUnrealGPTLogCapture::ResetReadCursor()
{
	FScopeLock Lock(&BufferLock);
	ReadCursor = Lines.Num();
}
