// Copyright (c) 2025 TREE Industries.

#include "SUnrealGPTClarifyWidget.h"
#include "UnrealGPTAgentClient.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"

namespace UnrealGPTClarifyUI
{
	static constexpr float SpaceXs = 8.f;
	static constexpr float SpaceSm = 12.f;
	static constexpr float SpaceMd = 15.f;

	static const FLinearColor AccentBlue = FLinearColor(0.0f, 0.6f, 1.0f, 1.0f);
	static const FLinearColor Surface1 = FLinearColor(0.078f, 0.078f, 0.078f, 1.0f);
	static const FLinearColor Surface2 = FLinearColor(0.11f, 0.11f, 0.11f, 1.0f);

	static FSlateFontInfo BodyFont()
	{
		return FAppStyle::GetFontStyle("NormalFont");
	}

	static FSlateFontInfo BodyBoldFont()
	{
		return FAppStyle::GetFontStyle("NormalFontBold");
	}

	static FSlateFontInfo CaptionFont()
	{
		return FAppStyle::GetFontStyle("SmallFont");
	}
}

void SUnrealGPTClarifyWidget::Construct(const FArguments& InArgs)
{
	ToolCallId = InArgs._ToolCallId;
	AgentClient = InArgs._AgentClient;

	if (!FUnrealGPTClarifyTypes::ParseRequest(InArgs._ArgumentsJson, Request))
	{
		ChildSlot
		[
			SNew(STextBlock)
			.Text(FText::FromString(Request.ParseError))
			.Font(UnrealGPTClarifyUI::BodyFont())
			.ColorAndOpacity(FStyleColors::Error)
			.AutoWrapText(true)
		];
		return;
	}

	TSharedRef<SVerticalBox> RootBox = SNew(SVerticalBox);

	if (!Request.Title.IsEmpty())
	{
		RootBox->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, UnrealGPTClarifyUI::SpaceSm)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Request.Title))
			.Font(UnrealGPTClarifyUI::BodyBoldFont())
			.ColorAndOpacity(FStyleColors::Foreground)
			.AutoWrapText(true)
		];
	}

	for (const FClarifyQuestion& Question : Request.Questions)
	{
		TSharedPtr<FQuestionUiState> QuestionState = MakeShared<FQuestionUiState>();
		QuestionState->Question = Question;
		QuestionStates.Add(QuestionState);

		TSharedRef<SWrapBox> OptionsWrap = SNew(SWrapBox)
			.UseAllottedSize(true)
			.InnerSlotPadding(FVector2D(UnrealGPTClarifyUI::SpaceXs, UnrealGPTClarifyUI::SpaceXs));

		auto AddOptionButton = [&](const FString& OptionId, const FString& Label)
		{
			TSharedPtr<SButton> OptionButton;
			const FString QuestionId = Question.Id;
			const bool bAllowMultiple = Question.bAllowMultiple;

			OptionsWrap->AddSlot()
			[
				SAssignNew(OptionButton, SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.OnClicked_Lambda([this, QuestionId, OptionId, bAllowMultiple]()
				{
					return OnOptionClicked(QuestionId, OptionId, bAllowMultiple);
				})
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("Brushes.White"))
					.BorderBackgroundColor_Lambda([this, QuestionId, OptionId]()
					{
						for (const TSharedPtr<FQuestionUiState>& State : QuestionStates)
						{
							if (State->Question.Id == QuestionId && State->SelectedOptionIds.Contains(OptionId))
							{
								return UnrealGPTClarifyUI::AccentBlue;
							}
						}
						return UnrealGPTClarifyUI::Surface2;
					})
					.Padding(FMargin(12.0f, 8.0f))
					[
						SNew(STextBlock)
						.Text(FText::FromString(Label))
						.Font(UnrealGPTClarifyUI::BodyFont())
						.ColorAndOpacity_Lambda([this, QuestionId, OptionId]()
						{
							for (const TSharedPtr<FQuestionUiState>& State : QuestionStates)
							{
								if (State->Question.Id == QuestionId && State->SelectedOptionIds.Contains(OptionId))
								{
									return FSlateColor(FLinearColor::Black);
								}
							}
							return FSlateColor(FStyleColors::Foreground);
						})
					]
				]
			];

			QuestionState->OptionButtons.Add(OptionButton);
		};

		for (const FClarifyOption& Option : Question.Options)
		{
			AddOptionButton(Option.Id, Option.Label);
		}

		AddOptionButton(ClarifyOtherOptionId, TEXT("Other"));

		RootBox->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, UnrealGPTClarifyUI::SpaceMd)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, UnrealGPTClarifyUI::SpaceXs)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Question.Prompt))
				.Font(UnrealGPTClarifyUI::BodyBoldFont())
				.ColorAndOpacity(FStyleColors::Foreground)
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				OptionsWrap
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, UnrealGPTClarifyUI::SpaceXs, 0.0f, 0.0f)
			[
				SAssignNew(QuestionState->OtherTextBox, SEditableTextBox)
				.Visibility_Lambda([QuestionState]()
				{
					return QuestionState->SelectedOptionIds.Contains(ClarifyOtherOptionId)
						? EVisibility::Visible
						: EVisibility::Collapsed;
				})
				.HintText(NSLOCTEXT("UnrealGPT", "ClarifyOtherHint", "Please specify..."))
				.OnTextChanged_Lambda([this, QuestionState](const FText& NewText)
				{
					QuestionState->OtherText = NewText.ToString();
					if (SubmitButton.IsValid())
					{
						SubmitButton->SetEnabled(CanSubmit());
					}
				})
			]
		];
	}

	RootBox->AddSlot()
	.AutoHeight()
	.Padding(0.0f, UnrealGPTClarifyUI::SpaceXs, 0.0f, 0.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, 0.0f, UnrealGPTClarifyUI::SpaceSm, 0.0f)
		[
			SAssignNew(SubmitButton, SButton)
			.Text(NSLOCTEXT("UnrealGPT", "ClarifySubmit", "Submit"))
			.IsEnabled(false)
			.OnClicked(this, &SUnrealGPTClarifyWidget::OnSubmitClicked)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SAssignNew(SkipButton, SButton)
			.Text(NSLOCTEXT("UnrealGPT", "ClarifySkip", "Skip"))
			.OnClicked(this, &SUnrealGPTClarifyWidget::OnSkipClicked)
		]
	];

	ChildSlot
	[
		RootBox
	];
}

bool SUnrealGPTClarifyWidget::CanSubmit() const
{
	if (bSubmitted)
	{
		return false;
	}

	for (const TSharedPtr<FQuestionUiState>& QuestionState : QuestionStates)
	{
		if (!QuestionState.IsValid())
		{
			continue;
		}

		if (QuestionState->SelectedOptionIds.Num() == 0)
		{
			return false;
		}

		if (QuestionState->SelectedOptionIds.Contains(ClarifyOtherOptionId)
			&& QuestionState->OtherText.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}
	}

	return QuestionStates.Num() > 0;
}

void SUnrealGPTClarifyWidget::RefreshOptionButtons(const TSharedPtr<FQuestionUiState>& QuestionState)
{
	if (!QuestionState.IsValid())
	{
		return;
	}

	for (const TSharedPtr<SButton>& Button : QuestionState->OptionButtons)
	{
		if (Button.IsValid())
		{
			Button->Invalidate(EInvalidateWidget::LayoutAndVolatility);
		}
	}

	if (QuestionState->OtherTextBox.IsValid())
	{
		QuestionState->OtherTextBox->Invalidate(EInvalidateWidget::LayoutAndVolatility);
	}
}

FReply SUnrealGPTClarifyWidget::OnOptionClicked(FString QuestionId, FString OptionId, bool bAllowMultiple)
{
	if (bSubmitted)
	{
		return FReply::Handled();
	}

	for (const TSharedPtr<FQuestionUiState>& QuestionState : QuestionStates)
	{
		if (!QuestionState.IsValid() || QuestionState->Question.Id != QuestionId)
		{
			continue;
		}

		if (bAllowMultiple)
		{
			if (QuestionState->SelectedOptionIds.Contains(OptionId))
			{
				QuestionState->SelectedOptionIds.Remove(OptionId);
				if (OptionId == ClarifyOtherOptionId)
				{
					QuestionState->OtherText.Empty();
				}
			}
			else
			{
				QuestionState->SelectedOptionIds.Add(OptionId);
			}
		}
		else
		{
			const bool bWasSelected = QuestionState->SelectedOptionIds.Contains(OptionId);
			QuestionState->SelectedOptionIds.Empty();
			if (!bWasSelected)
			{
				QuestionState->SelectedOptionIds.Add(OptionId);
			}

			if (!QuestionState->SelectedOptionIds.Contains(ClarifyOtherOptionId))
			{
				QuestionState->OtherText.Empty();
			}
		}

		RefreshOptionButtons(QuestionState);
		break;
	}

	if (SubmitButton.IsValid())
	{
		SubmitButton->SetEnabled(CanSubmit());
	}

	return FReply::Handled();
}

void SUnrealGPTClarifyWidget::SubmitAnswers(bool bCancelled)
{
	if (bSubmitted)
	{
		return;
	}

	bSubmitted = true;

	if (SubmitButton.IsValid())
	{
		SubmitButton->SetEnabled(false);
	}
	if (SkipButton.IsValid())
	{
		SkipButton->SetEnabled(false);
	}

	UUnrealGPTAgentClient* Client = AgentClient.Get();
	if (!Client)
	{
		return;
	}

	if (bCancelled)
	{
		Client->SubmitClarifyResponse(ToolCallId, FUnrealGPTClarifyTypes::BuildCancelledResult());
		return;
	}

	TMap<FString, TPair<TArray<FString>, FString>> AnswersByQuestionId;
	for (const TSharedPtr<FQuestionUiState>& QuestionState : QuestionStates)
	{
		if (!QuestionState.IsValid())
		{
			continue;
		}

		TArray<FString> SelectedIds = QuestionState->SelectedOptionIds.Array();
		FString OtherText;
		if (QuestionState->SelectedOptionIds.Contains(ClarifyOtherOptionId))
		{
			OtherText = QuestionState->OtherText.TrimStartAndEnd();
		}

		AnswersByQuestionId.Add(QuestionState->Question.Id, TPair<TArray<FString>, FString>(SelectedIds, OtherText));
	}

	Client->SubmitClarifyResponse(ToolCallId, FUnrealGPTClarifyTypes::BuildAnsweredResult(AnswersByQuestionId));
}

FReply SUnrealGPTClarifyWidget::OnSubmitClicked()
{
	if (CanSubmit())
	{
		SubmitAnswers(false);
	}
	return FReply::Handled();
}

FReply SUnrealGPTClarifyWidget::OnSkipClicked()
{
	SubmitAnswers(true);
	return FReply::Handled();
}
