// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"

/** Sentinel option id for the auto-appended "Other" choice. */
inline const FString ClarifyOtherOptionId = TEXT("__other__");

struct FClarifyOption
{
	FString Id;
	FString Label;
};

struct FClarifyQuestion
{
	FString Id;
	FString Prompt;
	TArray<FClarifyOption> Options;
	bool bAllowMultiple = false;
};

struct FClarifyRequest
{
	FString Title;
	TArray<FClarifyQuestion> Questions;
	FString ParseError;
};

class UNREALGPTEDITOR_API FUnrealGPTClarifyTypes
{
public:
	static bool ParseRequest(const FString& ArgumentsJson, FClarifyRequest& OutRequest);

	static FString BuildAnsweredResult(const TMap<FString, TPair<TArray<FString>, FString>>& AnswersByQuestionId);
	static FString BuildCancelledResult();
	static FString BuildErrorResult(const FString& Message);

	/** Human-readable summary for chat UI after submission. */
	static FString FormatAnswerSummary(const FString& ResultJson, const FClarifyRequest& Request);
};
