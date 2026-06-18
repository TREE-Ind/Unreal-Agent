// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "UnrealGPTClarifyTypes.h"

class UUnrealGPTAgentClient;
class SEditableTextBox;
class SButton;

class SUnrealGPTClarifyWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUnrealGPTClarifyWidget) {}
		SLATE_ARGUMENT(FString, ToolCallId)
		SLATE_ARGUMENT(FString, ArgumentsJson)
		SLATE_ARGUMENT(UUnrealGPTAgentClient*, AgentClient)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	struct FQuestionUiState
	{
		FClarifyQuestion Question;
		TSet<FString> SelectedOptionIds;
		FString OtherText;
		TSharedPtr<SEditableTextBox> OtherTextBox;
		TArray<TSharedPtr<SButton>> OptionButtons;
	};

	bool bSubmitted = false;
	FString ToolCallId;
	TWeakObjectPtr<UUnrealGPTAgentClient> AgentClient;
	FClarifyRequest Request;

	TArray<TSharedPtr<FQuestionUiState>> QuestionStates;
	TSharedPtr<SButton> SubmitButton;
	TSharedPtr<SButton> SkipButton;

	bool CanSubmit() const;
	void RefreshOptionButtons(const TSharedPtr<FQuestionUiState>& QuestionState);
	void SubmitAnswers(bool bCancelled);
	FReply OnSubmitClicked();
	FReply OnSkipClicked();
	FReply OnOptionClicked(FString QuestionId, FString OptionId, bool bAllowMultiple);
};
