// Copyright (c) 2025 TREE Industries.

#include "UnrealGPTWidget.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/SRichTextBlock.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "Styling/StyleColors.h"
#include "UnrealGPTSettings.h"
#include "ISettingsModule.h"
#include "Misc/Base64.h"
#include "UnrealGPTSceneContext.h"
#include "UnrealGPTWidgetDelegateHandler.h"
#include "Framework/Text/SlateTextRun.h"
#include "Framework/Text/SlateTextLayout.h"
#include "Widgets/Layout/SSpacer.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RenderingThread.h"
#include "Async/Async.h"
#include "ImageUtils.h"
#include "TextureResource.h"
#include "UnrealGPTVoiceInput.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SOverlay.h"

namespace UnrealGPTAgentUI
{
	// DESIGN.md spacing scale (xs/sm/md/lg) — Slate margins use whole pixels.
	static constexpr float SpaceXs = 8.f;
	static constexpr float SpaceSm = 12.f;
	static constexpr float SpaceMd = 15.f;
	static constexpr float SpaceLg = 20.f;
	static constexpr float SpaceXl = 30.f;
	static constexpr float ThreadBubbleMaxWidth = 640.f;
	static constexpr float ThreadSpotlightMaxWidth = 520.f;

	// DESIGN.md accent-blue (#0099ff) reserved for link-like / focus hints (not primary CTAs).
	static const FSlateColor AccentBlueHint = FSlateColor(FLinearColor(0.0f, 0.6f, 1.0f, 1.0f));

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

	static FSlateFontInfo CaptionItalicFont()
	{
		return FAppStyle::GetFontStyle("SmallFontItalic");
	}

	static FSlateFontInfo BodyItalicFont()
	{
		return FAppStyle::GetFontStyle("NormalFontItalic");
	}
}

namespace
{
	// Render a single chat line with very lightweight inline markdown support (**bold** only).
	TSharedRef<SWidget> CreateInlineMarkdownTextWidget(const FString& Line)
	{
		TSharedRef<SWrapBox> WrapBox =
			SNew(SWrapBox)
			.UseAllottedSize(true)
			.InnerSlotPadding(FVector2D::ZeroVector);

		auto AddRun = [&WrapBox](const FString& Text, bool bBold)
		{
			if (Text.IsEmpty())
			{
				return;
			}

			WrapBox->AddSlot()
			[
				SNew(STextBlock)
				.Text(FText::FromString(Text))
				.AutoWrapText(true)
				.Font(bBold ? UnrealGPTAgentUI::BodyBoldFont() : UnrealGPTAgentUI::BodyFont())
				.ColorAndOpacity(FStyleColors::Foreground)
			];
		};

		int32 Pos = 0;
		const int32 Length = Line.Len();

		while (Pos < Length)
		{
			const int32 Open = Line.Find(TEXT("**"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
			if (Open == INDEX_NONE)
			{
				// No more bold markers; add the rest as normal text.
				AddRun(Line.Mid(Pos), false);
				break;
			}

			// Add any normal text before the bold span.
			if (Open > Pos)
			{
				AddRun(Line.Mid(Pos, Open - Pos), false);
			}

			const int32 Close = Line.Find(TEXT("**"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open + 2);
			if (Close == INDEX_NONE)
			{
				// Unmatched '**' – treat the remainder as normal text including the markers.
				AddRun(Line.Mid(Open), false);
				break;
			}

			// Extract the bold span between the markers.
			const int32 BoldStart = Open + 2;
			const int32 BoldLen = Close - BoldStart;
			if (BoldLen > 0)
			{
				AddRun(Line.Mid(BoldStart, BoldLen), true);
			}

			Pos = Close + 2;
		}

		return WrapBox;
	}

	/** Constrain thread width and bias turns for a chat-style transcript (DESIGN rhythm + readability). */
	TSharedRef<SWidget> WrapThreadTurn(const TSharedRef<SWidget>& Inner, EHorizontalAlignment RowAlign)
	{
		const float MaxW = (RowAlign == HAlign_Center) ? UnrealGPTAgentUI::ThreadSpotlightMaxWidth : UnrealGPTAgentUI::ThreadBubbleMaxWidth;

		if (RowAlign == HAlign_Right)
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SSpacer)
					.Size(FVector2D(1.0f, 1.0f))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SBox)
					.MaxDesiredWidth(MaxW)
					[
						Inner
					]
				];
		}

		if (RowAlign == HAlign_Center)
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SSpacer)
					.Size(FVector2D(1.0f, 1.0f))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SBox)
					.MaxDesiredWidth(MaxW)
					[
						Inner
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SSpacer)
					.Size(FVector2D(1.0f, 1.0f))
				];
		}

		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SBox)
				.MaxDesiredWidth(MaxW)
				[
					Inner
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpacer)
				.Size(FVector2D(1.0f, 1.0f))
			];
	}

	static TSharedRef<SWidget> MakeRoleMetaRow(const FText& Eyebrow, const FText& TitleName, const FSlateColor& TitleColor)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Eyebrow)
				.Font(UnrealGPTAgentUI::CaptionFont())
				.ColorAndOpacity(FStyleColors::Foreground)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(NSLOCTEXT("UnrealGPT", "RoleMetaDot", "\xB7"))
				.Font(UnrealGPTAgentUI::CaptionFont())
				.ColorAndOpacity(FStyleColors::Foreground)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(TitleName)
				.Font(UnrealGPTAgentUI::BodyBoldFont())
				.ColorAndOpacity(TitleColor)
			];
	}
}

SUnrealGPTWidget::~SUnrealGPTWidget()
{
	ResetCodexLoginProcess();
}

void SUnrealGPTWidget::AddConversationTurnWidget(const TSharedRef<SWidget>& Content, const FMargin& SlotPadding)
{
	if (!ChatHistoryBox.IsValid())
	{
		return;
	}

	ChatHistoryBox->AddSlot()
		.Padding(SlotPadding)
		[ Content ];

	++ChatConversationBlockCount;
	SyncEmptyThreadVisibility();
	ChatHistoryBox->ScrollToEnd();
}

void SUnrealGPTWidget::SyncEmptyThreadVisibility()
{
	if (!EmptyThreadChrome.IsValid())
	{
		return;
	}

	EmptyThreadChrome->SetVisibility(ChatConversationBlockCount <= 0 ? EVisibility::Visible : EVisibility::Collapsed);
}

void SUnrealGPTWidget::Construct(const FArguments& InArgs)
{
	// Create agent client
	AgentClient = NewObject<UUnrealGPTAgentClient>();
	AgentClient->Initialize();

	// Create delegate handler for dynamic delegate bindings
	DelegateHandler = NewObject<UUnrealGPTWidgetDelegateHandler>();
	DelegateHandler->Initialize(this);

	// Bind delegates using UFunction bindings through the handler
	AgentClient->OnAgentMessage.AddDynamic(DelegateHandler, &UUnrealGPTWidgetDelegateHandler::OnAgentMessageReceived);
	AgentClient->OnAgentReasoning.AddDynamic(DelegateHandler, &UUnrealGPTWidgetDelegateHandler::OnAgentReasoningReceived);
	AgentClient->OnToolCall.AddDynamic(DelegateHandler, &UUnrealGPTWidgetDelegateHandler::OnToolCallReceived);
	AgentClient->OnToolResult.AddDynamic(DelegateHandler, &UUnrealGPTWidgetDelegateHandler::OnToolResultReceived);

	// Create voice input instance
	VoiceInput = NewObject<UUnrealGPTVoiceInput>();
	VoiceInput->Initialize();
	VoiceInput->OnTranscriptionComplete.AddDynamic(DelegateHandler, &UUnrealGPTWidgetDelegateHandler::OnTranscriptionCompleteReceived);
	VoiceInput->OnRecordingStarted.AddDynamic(DelegateHandler, &UUnrealGPTWidgetDelegateHandler::OnRecordingStartedReceived);
	VoiceInput->OnRecordingStopped.AddDynamic(DelegateHandler, &UUnrealGPTWidgetDelegateHandler::OnRecordingStoppedReceived);

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(0.0f)
		[
			SNew(SVerticalBox)

			// Editor-style header row + DESIGN.md single translucent atmosphere strip (violet anchor).
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
				.Padding(FMargin(UnrealGPTAgentUI::SpaceMd, UnrealGPTAgentUI::SpaceXs))
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot()
							.AutoHeight()
							[
								SNew(STextBlock)
								.Text(NSLOCTEXT("UnrealGPT", "HarnessTitle", "Unreal Agent"))
								.Font(FAppStyle::GetFontStyle("NormalFontBold"))
							]
							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
							[
								SNew(STextBlock)
								.Text(NSLOCTEXT("UnrealGPT", "HarnessSubtitle", "Scene-grounded harness — tools, reasoning, and directives in one thread."))
								.Font(UnrealGPTAgentUI::CaptionFont())
								.ColorAndOpacity(FStyleColors::Foreground)
								.AutoWrapText(true)
							]
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush("WhiteBorder"))
							.BorderBackgroundColor(FStyleColors::Input)
							.Padding(FMargin(12.0f, 6.0f))
							[
								SAssignNew(SessionStatusLabel, STextBlock)
								.Text(NSLOCTEXT("UnrealGPT", "SessionStatusIdle", "Ready"))
								.Font(FAppStyle::GetFontStyle("SmallFontBold"))
								.ColorAndOpacity(FStyleColors::Foreground)
							]
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.0f, UnrealGPTAgentUI::SpaceSm, 0.0f, 0.0f))
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SHorizontalBox)

							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0.0f, 0.0f, UnrealGPTAgentUI::SpaceXs, 0.0f)
							[
								SNew(SBox)
								.MinDesiredWidth(148.0f)
								[
									SAssignNew(RequestContextButton, SButton)
									.ButtonStyle(FAppStyle::Get(), "FlatButton.Default")
									.ForegroundColor(FSlateColor::UseForeground())
									.ContentPadding(FMargin(UnrealGPTAgentUI::SpaceMd, 10.0f))
									.OnClicked(this, &SUnrealGPTWidget::OnRequestContextClicked)
									[
										SNew(SHorizontalBox)
										+ SHorizontalBox::Slot()
										.AutoWidth()
										.VAlign(VAlign_Center)
										.Padding(0.0f, 0.0f, 6.0f, 0.0f)
										[
											SNew(STextBlock)
											.Font(FAppStyle::Get().GetFontStyle("FontAwesome.11"))
											.Text(FText::FromString(FString(TEXT("\xf030"))))
											.ColorAndOpacity(FStyleColors::Foreground)
										]
										+ SHorizontalBox::Slot()
										.VAlign(VAlign_Center)
										[
											SNew(STextBlock)
											.Text(NSLOCTEXT("UnrealGPT", "RequestContext", "Capture Context"))
											.Font(FAppStyle::GetFontStyle("SmallFont"))
										]
									]
								]
							]

							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0.0f, 0.0f, UnrealGPTAgentUI::SpaceXs, 0.0f)
							[
								SNew(SBox)
								.MinDesiredWidth(128.0f)
								[
									SAssignNew(ClearHistoryButton, SButton)
									.ButtonStyle(FAppStyle::Get(), "FlatButton.Default")
									.ForegroundColor(FSlateColor::UseForeground())
									.ContentPadding(FMargin(UnrealGPTAgentUI::SpaceMd, 10.0f))
									.OnClicked(this, &SUnrealGPTWidget::OnClearHistoryClicked)
									[
										SNew(SHorizontalBox)
										+ SHorizontalBox::Slot()
										.AutoWidth()
										.VAlign(VAlign_Center)
										.Padding(0.0f, 0.0f, 6.0f, 0.0f)
										[
											SNew(STextBlock)
											.Font(FAppStyle::Get().GetFontStyle("FontAwesome.11"))
											.Text(FText::FromString(FString(TEXT("\xf014"))))
											.ColorAndOpacity(FStyleColors::Foreground)
										]
										+ SHorizontalBox::Slot()
										.VAlign(VAlign_Center)
										[
											SNew(STextBlock)
											.Text(NSLOCTEXT("UnrealGPT", "ClearHistory", "Clear History"))
											.Font(FAppStyle::GetFontStyle("SmallFont"))
										]
									]
								]
							]
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						[
							SNullWidget::NullWidget
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, UnrealGPTAgentUI::SpaceXs, 0.0f)
						[
							SNew(SBox)
							.MinDesiredWidth(118.0f)
							[
								SAssignNew(CodexLoginButton, SButton)
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Default")
								.ForegroundColor(FSlateColor::UseForeground())
								.ContentPadding(FMargin(UnrealGPTAgentUI::SpaceMd, 10.0f))
								.OnClicked(this, &SUnrealGPTWidget::OnCodexLoginClicked)
								.IsEnabled(this, &SUnrealGPTWidget::IsCodexLoginButtonEnabled)
								.ToolTipText(this, &SUnrealGPTWidget::GetCodexLoginButtonTooltip)
								[
									SNew(SHorizontalBox)
									+ SHorizontalBox::Slot()
									.AutoWidth()
									.VAlign(VAlign_Center)
									.Padding(0.0f, 0.0f, 6.0f, 0.0f)
									[
										SNew(STextBlock)
										.Font(FAppStyle::Get().GetFontStyle("FontAwesome.11"))
										.Text_Lambda([this]()
										{
											return FText::FromString(IsCodexLoggedIn() ? FString(TEXT("\xf058")) : FString(TEXT("\xf084")));
										})
										.ColorAndOpacity_Lambda([this]()
										{
											return IsCodexLoggedIn() ? FStyleColors::Success : FStyleColors::Foreground;
										})
									]
									+ SHorizontalBox::Slot()
									.VAlign(VAlign_Center)
									[
										SNew(STextBlock)
										.Text(this, &SUnrealGPTWidget::GetCodexLoginButtonText)
										.Font(FAppStyle::GetFontStyle("SmallFont"))
									]
								]
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SBox)
							.MinDesiredWidth(108.0f)
							[
								SAssignNew(SettingsButton, SButton)
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Default")
								.ForegroundColor(FSlateColor::UseForeground())
								.ContentPadding(FMargin(UnrealGPTAgentUI::SpaceMd, 10.0f))
								.OnClicked(this, &SUnrealGPTWidget::OnSettingsClicked)
								[
									SNew(SHorizontalBox)
									+ SHorizontalBox::Slot()
									.AutoWidth()
									.VAlign(VAlign_Center)
									.Padding(0.0f, 0.0f, 6.0f, 0.0f)
									[
										SNew(STextBlock)
										.Font(FAppStyle::Get().GetFontStyle("FontAwesome.11"))
										.Text(FText::FromString(FString(TEXT("\xf013"))))
										.ColorAndOpacity(FStyleColors::Foreground)
									]
									+ SHorizontalBox::Slot()
									.VAlign(VAlign_Center)
									[
										SNew(STextBlock)
										.Text(NSLOCTEXT("UnrealGPT", "Settings", "Settings"))
										.Font(FAppStyle::GetFontStyle("SmallFont"))
									]
								]
							]
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.0f, UnrealGPTAgentUI::SpaceXs, 0.0f, 0.0f))
					[
						SNew(SBox)
						.HeightOverride(2.0f)
						[
							SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush("WhiteBorder"))
							.BorderBackgroundColor(FLinearColor(0.415f, 0.298f, 0.961f, 0.28f))
						]
					]
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SSeparator)
				.Thickness(1.0f)
			]

			// Screenshot preview with better styling
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(12.0f, 8.0f)
			[
				SAssignNew(ScreenshotPreview, SImage)
				.Visibility(EVisibility::Collapsed)
			]

			// Thumbnails for images attached to the *next* message
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(UnrealGPTAgentUI::SpaceSm, 0.0f, UnrealGPTAgentUI::SpaceSm, UnrealGPTAgentUI::SpaceXs))
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				.BorderBackgroundColor(FStyleColors::Panel)
				.Padding(FMargin(UnrealGPTAgentUI::SpaceXs, UnrealGPTAgentUI::SpaceXs))
				.Visibility_Lambda([this]() -> EVisibility
				{
					return PendingAttachedImages.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
				})
				[
					SAssignNew(AttachmentPreviewBox, SWrapBox)
					.UseAllottedSize(true)
					.InnerSlotPadding(FVector2D(UnrealGPTAgentUI::SpaceXs, UnrealGPTAgentUI::SpaceXs))
				]
			]

			// Thread surface + empty-state spotlight (DESIGN.md gradient card as single onboarding tile).
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				.BorderBackgroundColor(FStyleColors::Background)
				.Padding(0.0f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SAssignNew(ChatHistoryBox, SScrollBox)
						.Orientation(Orient_Vertical)
						.ScrollBarAlwaysVisible(false)
						.ConsumeMouseWheel(EConsumeMouseWheel::Always)
					]
					+ SOverlay::Slot()
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SAssignNew(EmptyThreadChrome, SBorder)
						.Visibility(EVisibility::Visible)
						.BorderImage(FAppStyle::GetBrush("Brushes.White"))
						.BorderBackgroundColor(FLinearColor(0.415f, 0.298f, 0.961f, 0.16f))
						.Padding(FMargin(UnrealGPTAgentUI::SpaceXl))
						[
							SNew(SBox)
							.MaxDesiredWidth(UnrealGPTAgentUI::ThreadSpotlightMaxWidth)
							[
								SNew(SVerticalBox)
								+ SVerticalBox::Slot()
								.AutoHeight()
								[
									SNew(STextBlock)
									.Text(NSLOCTEXT("UnrealGPT", "EmptyThreadTitle", "Start your agent thread"))
									.Font(FAppStyle::GetFontStyle("NormalFontBold"))
									.ColorAndOpacity(FStyleColors::Foreground)
								]
								+ SVerticalBox::Slot()
								.AutoHeight()
								.Padding(FMargin(0.0f, UnrealGPTAgentUI::SpaceXs, 0.0f, 0.0f))
								[
									SNew(STextBlock)
									.Text(NSLOCTEXT("UnrealGPT", "EmptyThreadBody", "Describe a goal, attach reference images, or capture the viewport as context. Quick starts use the same actions as the toolbar."))
									.Font(UnrealGPTAgentUI::BodyFont())
									.ColorAndOpacity(FStyleColors::Foreground)
									.AutoWrapText(true)
								]
								+ SVerticalBox::Slot()
								.AutoHeight()
								.Padding(FMargin(0.0f, UnrealGPTAgentUI::SpaceLg, 0.0f, 0.0f))
								[
									SNew(SWrapBox)
									.UseAllottedSize(true)
									.InnerSlotPadding(FVector2D(UnrealGPTAgentUI::SpaceXs, UnrealGPTAgentUI::SpaceXs))
									+ SWrapBox::Slot()
									[
										SNew(SButton)
										.ButtonStyle(FAppStyle::Get(), "FlatButton.Default")
										.ForegroundColor(FSlateColor::UseForeground())
										.ContentPadding(FMargin(UnrealGPTAgentUI::SpaceMd, 10.0f))
										.OnClicked(this, &SUnrealGPTWidget::OnRequestContextClicked)
										[
											SNew(STextBlock)
											.Text(NSLOCTEXT("UnrealGPT", "EmptyQuickCapture", "Capture context"))
											.Font(FAppStyle::GetFontStyle("SmallFont"))
										]
									]
									+ SWrapBox::Slot()
									[
										SNew(SButton)
										.ButtonStyle(FAppStyle::Get(), "FlatButton.Default")
										.ForegroundColor(FSlateColor::UseForeground())
										.ContentPadding(FMargin(UnrealGPTAgentUI::SpaceMd, 10.0f))
										.OnClicked(this, &SUnrealGPTWidget::OnAttachImageClicked)
										[
											SNew(STextBlock)
											.Text(NSLOCTEXT("UnrealGPT", "EmptyQuickAttach", "Attach image"))
											.Font(FAppStyle::GetFontStyle("SmallFont"))
										]
									]
									+ SWrapBox::Slot()
									[
										SNew(SButton)
										.ButtonStyle(FAppStyle::Get(), "FlatButton.Default")
										.ForegroundColor(FSlateColor::UseForeground())
										.ContentPadding(FMargin(UnrealGPTAgentUI::SpaceMd, 10.0f))
										.OnClicked(this, &SUnrealGPTWidget::OnSettingsClicked)
										[
											SNew(STextBlock)
											.Text(NSLOCTEXT("UnrealGPT", "EmptyQuickSettings", "Settings"))
											.Font(FAppStyle::GetFontStyle("SmallFont"))
										]
									]
									+ SWrapBox::Slot()
									[
										SNew(SButton)
										.ButtonStyle(FAppStyle::Get(), "FlatButton.Default")
										.ForegroundColor(FSlateColor::UseForeground())
										.ContentPadding(FMargin(UnrealGPTAgentUI::SpaceMd, 10.0f))
										.OnClicked(this, &SUnrealGPTWidget::OnCodexLoginClicked)
										.IsEnabled(this, &SUnrealGPTWidget::IsCodexLoginButtonEnabled)
										.ToolTipText(this, &SUnrealGPTWidget::GetCodexLoginButtonTooltip)
										[
											SNew(STextBlock)
											.Text(NSLOCTEXT("UnrealGPT", "EmptyQuickCodex", "Codex login"))
											.Font(FAppStyle::GetFontStyle("SmallFont"))
										]
									]
								]
							]
						]
					]
				]
			]

			// Reasoning status — hairline panel + DESIGN accent strip (signal-only blue).
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(UnrealGPTAgentUI::SpaceSm, UnrealGPTAgentUI::SpaceXs, UnrealGPTAgentUI::SpaceSm, UnrealGPTAgentUI::SpaceXs))
			[
				SAssignNew(ReasoningStatusBorder, SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				.BorderBackgroundColor(FStyleColors::Panel)
				.Padding(FMargin(0.0f))
				.Visibility(EVisibility::Collapsed)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("WhiteBorder"))
						.BorderBackgroundColor(UnrealGPTAgentUI::AccentBlueHint)
						.Padding(0.0f)
						[
							SNew(SBox)
							.WidthOverride(3.0f)
						]
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(UnrealGPTAgentUI::SpaceXs, UnrealGPTAgentUI::SpaceXs, 6.0f, UnrealGPTAgentUI::SpaceXs)
					[
						SNew(STextBlock)
						.Font(FAppStyle::Get().GetFontStyle("FontAwesome.10"))
						.Text(FText::FromString(FString(TEXT("\xf0eb"))))
						.ColorAndOpacity(FStyleColors::Foreground)
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					.Padding(0.0f, UnrealGPTAgentUI::SpaceXs, UnrealGPTAgentUI::SpaceSm, UnrealGPTAgentUI::SpaceXs)
					[
						SNew(SBox)
						.MaxDesiredHeight(76.0f)
						[
							SNew(SScrollBox)
							.Orientation(Orient_Vertical)
							+ SScrollBox::Slot()
							[
								SAssignNew(ReasoningSummaryText, STextBlock)
								.Text(NSLOCTEXT("UnrealGPT", "ReasoningPending", "Thinking..."))
								.Font(UnrealGPTAgentUI::CaptionFont())
								.AutoWrapText(true)
								.ColorAndOpacity(FStyleColors::Foreground)
							]
						]
					]
				]
			]

			// Composer — labeled stack + taller directive field (agent harness pattern).
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
				.Padding(FMargin(UnrealGPTAgentUI::SpaceSm, UnrealGPTAgentUI::SpaceSm))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(NSLOCTEXT("UnrealGPT", "ComposerLabel", "Next directive"))
							.Font(FAppStyle::GetFontStyle("SmallFontBold"))
							.ColorAndOpacity(FStyleColors::Foreground)
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(UnrealGPTAgentUI::SpaceSm, 0.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Visibility_Lambda([this]() -> EVisibility
							{
								return PendingAttachedImages.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
							})
							.Text_Lambda([this]() -> FText
							{
								const int32 Count = PendingAttachedImages.Num();
								if (Count == 1)
								{
									return NSLOCTEXT("UnrealGPT", "OneImageAttached", "1 image attached");
								}
								return FText::FromString(FString::Printf(TEXT("%d images attached"), Count));
							})
							.Font(FAppStyle::GetFontStyle("SmallFontItalic"))
							.ColorAndOpacity(UnrealGPTAgentUI::AccentBlueHint)
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.0f, UnrealGPTAgentUI::SpaceXs, 0.0f, 0.0f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.Padding(0.0f, 0.0f, UnrealGPTAgentUI::SpaceXs, 0.0f)
						[
							SNew(SBox)
							.MinDesiredHeight(100.0f)
							[
								SNew(SBorder)
								.BorderImage(FAppStyle::GetBrush("Brushes.White"))
								.BorderBackgroundColor(FStyleColors::Input)
								.Padding(FMargin(UnrealGPTAgentUI::SpaceSm, 12.0f))
								[
									SAssignNew(InputTextBox, SMultiLineEditableTextBox)
									.HintText(NSLOCTEXT("UnrealGPT", "InputHint", "Ask UnrealGPT anything... (Ctrl+Enter to send)"))
									.Font(UnrealGPTAgentUI::BodyFont())
									.Margin(FMargin(0.0f))
									.OnKeyDownHandler_Lambda([this](const FGeometry&, const FKeyEvent& KeyEvent) -> FReply
									{
										if (KeyEvent.GetKey() == EKeys::Enter && KeyEvent.IsControlDown())
										{
											OnSendOrStopClicked();
											return FReply::Handled();
										}
										return FReply::Unhandled();
									})
								]
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, UnrealGPTAgentUI::SpaceXs, 0.0f)
						[
							SNew(SBox)
							.MinDesiredWidth(44.0f)
							.MinDesiredHeight(44.0f)
							[
								SAssignNew(VoiceInputButton, SButton)
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Default")
								.ForegroundColor(FSlateColor::UseForeground())
								.ContentPadding(FMargin(10.0f, 0.0f))
								.OnClicked(this, &SUnrealGPTWidget::OnVoiceInputClicked)
								[
									SNew(STextBlock)
									.Font(FAppStyle::Get().GetFontStyle("FontAwesome.14"))
									.Text_Lambda([this]() -> FText
									{
										return FText::FromString(VoiceInput && VoiceInput->IsRecording()
											? FString(TEXT("\xf130"))
											: FString(TEXT("\xf130")));
									})
									.ColorAndOpacity_Lambda([this]() -> FSlateColor
									{
										return VoiceInput && VoiceInput->IsRecording()
											? FSlateColor(FStyleColors::Error)
											: FSlateColor(FStyleColors::Foreground);
									})
								]
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, UnrealGPTAgentUI::SpaceXs, 0.0f)
						[
							SNew(SBox)
							.MinDesiredWidth(44.0f)
							.MinDesiredHeight(44.0f)
							[
								SNew(SButton)
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Default")
								.ForegroundColor(FSlateColor::UseForeground())
								.ContentPadding(FMargin(10.0f, 0.0f))
								.OnClicked(this, &SUnrealGPTWidget::OnAttachImageClicked)
								[
									SNew(STextBlock)
									.Font(FAppStyle::Get().GetFontStyle("FontAwesome.14"))
									.Text(FText::FromString(FString(TEXT("\xf0c6"))))
									.ColorAndOpacity(FStyleColors::Foreground)
								]
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SBox)
							.MinDesiredWidth(108.0f)
							.MinDesiredHeight(44.0f)
							[
								SNew(SOverlay)
								+ SOverlay::Slot()
								[
									SAssignNew(SendStopButton, SButton)
									.Visibility_Lambda([this]() -> EVisibility
									{
										return bAgentIsRunning ? EVisibility::Collapsed : EVisibility::Visible;
									})
									.ButtonStyle(FAppStyle::Get(), "CalloutButton")
									.ForegroundColor(FSlateColor::UseForeground())
									.ContentPadding(FMargin(UnrealGPTAgentUI::SpaceLg, 10.0f))
									.OnClicked(this, &SUnrealGPTWidget::OnSendOrStopClicked)
									.IsEnabled(this, &SUnrealGPTWidget::IsSendOrStopEnabled)
									[
										SNew(SHorizontalBox)
										+ SHorizontalBox::Slot()
										.AutoWidth()
										.VAlign(VAlign_Center)
										.Padding(0.0f, 0.0f, 6.0f, 0.0f)
										[
											SNew(STextBlock)
											.Font(FAppStyle::Get().GetFontStyle("FontAwesome.12"))
											.Text(FText::FromString(FString(TEXT("\xf1d8"))))
											.ColorAndOpacity(FSlateColor::UseForeground())
										]
										+ SHorizontalBox::Slot()
										.VAlign(VAlign_Center)
										[
											SNew(STextBlock)
											.Text(NSLOCTEXT("UnrealGPT", "Send", "Send"))
											.Font(FAppStyle::GetFontStyle("NormalFontBold"))
											.ColorAndOpacity(FSlateColor::UseForeground())
										]
									]
								]
								+ SOverlay::Slot()
								[
									SNew(SButton)
									.Visibility_Lambda([this]() -> EVisibility
									{
										return bAgentIsRunning ? EVisibility::Visible : EVisibility::Collapsed;
									})
									.ButtonStyle(FAppStyle::Get(), "FlatButton.Danger")
									.ForegroundColor(FSlateColor::UseForeground())
									.ContentPadding(FMargin(UnrealGPTAgentUI::SpaceLg, 10.0f))
									.OnClicked(this, &SUnrealGPTWidget::OnSendOrStopClicked)
									[
										SNew(SHorizontalBox)
										+ SHorizontalBox::Slot()
										.AutoWidth()
										.VAlign(VAlign_Center)
										.Padding(0.0f, 0.0f, 6.0f, 0.0f)
										[
											SNew(STextBlock)
											.Font(FAppStyle::Get().GetFontStyle("FontAwesome.12"))
											.Text(FText::FromString(FString(TEXT("\xf04d"))))
											.ColorAndOpacity(FSlateColor::UseForeground())
										]
										+ SHorizontalBox::Slot()
										.VAlign(VAlign_Center)
										[
											SNew(STextBlock)
											.Text(NSLOCTEXT("UnrealGPT", "Stop", "Stop"))
											.Font(FAppStyle::GetFontStyle("NormalFontBold"))
											.ColorAndOpacity(FSlateColor::UseForeground())
										]
									]
								]
							]
						]
					]
				]
			]
		]
	];
	SyncEmptyThreadVisibility();
}

void SUnrealGPTWidget::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	if (bCodexDeviceAuthInProgress
		&& CodexLoginRequest.IsValid() == false
		&& CodexNextPollTime > 0.0
		&& InCurrentTime >= CodexNextPollTime)
	{
		PollCodexDeviceAuthToken();
	}
}

FSlateColor SUnrealGPTWidget::GetRoleColor(const FString& Role) const
{
	if (Role == TEXT("user"))
	{
		return FStyleColors::Primary;
	}
	if (Role == TEXT("assistant"))
	{
		return FStyleColors::Success;
	}
	if (Role == TEXT("system"))
	{
		return FStyleColors::Warning;
	}
	return FStyleColors::Foreground;
}

TSharedRef<SWidget> SUnrealGPTWidget::CreateMarkdownWidget(const FString& Content)
{
	// Lightweight markdown renderer that keeps formatting readable in the chat UI
	// while avoiding any heavy parsing or external dependencies.
	//
	// Supported (best-effort) features:
	// - Headings: lines starting with "# ", "## ", or "### "
	// - Bullet lists: lines starting with "- " or "* "
	// - Fenced code blocks: sections wrapped in ``` fences (language tag after ``` is ignored)
	// - Blank-line spacing

	TSharedRef<SVerticalBox> Container = SNew(SVerticalBox);

	TArray<FString> Lines;
	Content.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);

	bool bInCodeBlock = false;

	for (const FString& RawLine : Lines)
	{
		FString Line = RawLine;

		// Toggle fenced code blocks (```), discard the fence line itself
		if (Line.StartsWith(TEXT("```")))
		{
			bInCodeBlock = !bInCodeBlock;
			continue;
		}

		// Blank line → small vertical spacer
		if (Line.TrimStartAndEnd().IsEmpty())
		{
			Container->AddSlot()
				.AutoHeight()
				[
					SNew(SSpacer)
					.Size(FVector2D(1.0f, 4.0f))
				];
			continue;
		}

		// Lines inside fenced code block: monospace, no wrapping
		if (bInCodeBlock)
		{
			Container->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Line))
					.AutoWrapText(false)
					.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
					.ColorAndOpacity(FStyleColors::Foreground)
				];
			continue;
		}

		// Headings (# / ## / ###) – render slightly emphasized
		if (Line.StartsWith(TEXT("### ")))
		{
			const FString HeadingText = Line.Mid(4);
			Container->AddSlot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 2.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(HeadingText))
					.AutoWrapText(true)
					.Font(UnrealGPTAgentUI::BodyBoldFont())
					.ColorAndOpacity(FStyleColors::Foreground)
				];
			continue;
		}
		if (Line.StartsWith(TEXT("## ")))
		{
			const FString HeadingText = Line.Mid(3);
			Container->AddSlot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 2.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(HeadingText))
					.AutoWrapText(true)
					.Font(UnrealGPTAgentUI::BodyBoldFont())
					.ColorAndOpacity(FStyleColors::Foreground)
				];
			continue;
		}
		if (Line.StartsWith(TEXT("# ")))
		{
			const FString HeadingText = Line.Mid(2);
			Container->AddSlot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 4.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(HeadingText))
					.AutoWrapText(true)
					.Font(UnrealGPTAgentUI::BodyBoldFont())
					.ColorAndOpacity(FStyleColors::Foreground)
				];
			continue;
		}

		// Simple bullet list items: "- " or "* " at line start
		if (Line.StartsWith(TEXT("- ")) || Line.StartsWith(TEXT("* ")))
		{
			const FString ItemText = Line.Mid(2);

			Container->AddSlot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Top)
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("\u2022"))) // Bullet character
						.Font(UnrealGPTAgentUI::BodyFont())
						.ColorAndOpacity(FStyleColors::Foreground)
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Top)
					[
						CreateInlineMarkdownTextWidget(ItemText)
					]
				];

			continue;
		}

		// Default paragraph line (supports inline **bold** spans)
		Container->AddSlot()
			.AutoHeight()
			[
				CreateInlineMarkdownTextWidget(Line)
			];
	}

	return Container;
}

TSharedRef<SWidget> SUnrealGPTWidget::CreateMessageWidget(const FString& Role, const FString& Content)
{
	const bool bIsUser = Role == TEXT("user");
	const bool bIsSystem = Role == TEXT("system");
	const FSlateColor RoleColor = GetRoleColor(Role);

	FSlateColor BubbleBackground = FStyleColors::Background;
	if (bIsUser)
	{
		BubbleBackground = FStyleColors::Panel;
	}
	else if (bIsSystem)
	{
		BubbleBackground = FStyleColors::Dropdown;
	}

	FString RoleIcon;
	if (bIsUser)
	{
		RoleIcon = FString(TEXT("\xf007"));
	}
	else if (bIsSystem)
	{
		RoleIcon = FString(TEXT("\xf071"));
	}
	else
	{
		RoleIcon = FString(TEXT("\xf121"));
	}

	const FText Eyebrow = bIsUser
		? NSLOCTEXT("UnrealGPT", "EyebrowUser", "YOU")
		: (bIsSystem ? NSLOCTEXT("UnrealGPT", "EyebrowSystem", "SYSTEM") : NSLOCTEXT("UnrealGPT", "EyebrowAssistant", "ASSISTANT"));

	const FText TitleName = bIsUser
		? NSLOCTEXT("UnrealGPT", "TitleUser", "You")
		: (bIsSystem ? NSLOCTEXT("UnrealGPT", "TitleSystem", "Notice") : NSLOCTEXT("UnrealGPT", "TitleAssistant", "UnrealGPT"));

	const TSharedRef<SWidget> MetaRow = MakeRoleMetaRow(Eyebrow, TitleName, RoleColor);

	const TSharedRef<SWidget> BubbleInner = [&]() -> TSharedRef<SWidget>
	{
		if (bIsSystem)
		{
			return SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, UnrealGPTAgentUI::SpaceXs)
				[ MetaRow ]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					CreateMarkdownWidget(Content)
				];
		}

		const TSharedRef<SWidget> Avatar =
			SNew(SBox)
			.WidthOverride(40.0f)
			.HeightOverride(40.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.White"))
				.BorderBackgroundColor(RoleColor)
				.Padding(0.0f)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(FAppStyle::Get().GetFontStyle("FontAwesome.16"))
					.Text(FText::FromString(RoleIcon))
					.ColorAndOpacity(FStyleColors::ForegroundInverted)
					.Justification(ETextJustify::Center)
				]
			];

		const TSharedRef<SWidget> Body =
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, UnrealGPTAgentUI::SpaceXs)
			[ MetaRow ]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				CreateMarkdownWidget(Content)
			];

		if (bIsUser)
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					Body
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(UnrealGPTAgentUI::SpaceSm, 0.0f, 0.0f, 0.0f)
				[
					Avatar
				];
		}

		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, UnrealGPTAgentUI::SpaceSm, 0.0f)
			[
				Avatar
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				Body
			];
	}();

	const TSharedRef<SWidget> Bubble =
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.White"))
		.BorderBackgroundColor(BubbleBackground)
		.Padding(FMargin(UnrealGPTAgentUI::SpaceLg, UnrealGPTAgentUI::SpaceSm))
		[ BubbleInner ];

	const EHorizontalAlignment RowBias = bIsUser ? HAlign_Right : (bIsSystem ? HAlign_Center : HAlign_Left);
	return WrapThreadTurn(Bubble, RowBias);
}

const FSlateBrush* SUnrealGPTWidget::GetToolIcon(const FString& ToolName) const
{
	// Return appropriate icon brush for tool type
	return FAppStyle::GetBrush("Icons.Tool");
}

TSharedRef<SWidget> SUnrealGPTWidget::CreateToolSpecificWidget(const FString& ToolName, const FString& Arguments, const FString& Result)
{
	FLinearColor ToolColor;
	FString ToolIcon;
	FString ToolDisplayName;
	TSharedPtr<SWidget> ContentWidget;

	// Assign colors and icons based on tool type
	if (ToolName == TEXT("python_execute"))
	{
		ToolColor = FLinearColor(0.415f, 0.298f, 0.961f, 1.0f); // DESIGN gradient-violet
		ToolIcon = FString(TEXT("\xf121")); // Code icon
		ToolDisplayName = TEXT("Python Execution");
		
		// Parse JSON to extract code cleanly
		FString Code = Arguments;
		TSharedPtr<FJsonObject> ArgsObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Arguments);
		if (FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid())
		{
			ArgsObj->TryGetStringField(TEXT("code"), Code);
		}

		ContentWidget = SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(NSLOCTEXT("UnrealGPT", "PythonCode", "Script:"))
				.Font(UnrealGPTAgentUI::CaptionFont())
				.ColorAndOpacity(FStyleColors::Foreground)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				.Padding(FMargin(8.0f))
				.BorderBackgroundColor(FStyleColors::Input)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Code.TrimStartAndEnd()))
					.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
					.ColorAndOpacity(FStyleColors::Foreground)
					.AutoWrapText(true)
				]
			];
	}
	else if (ToolName == TEXT("scene_query"))
	{
		ToolColor = FLinearColor(0.831f, 0.302f, 0.941f, 1.0f); // DESIGN gradient-magenta
		ToolIcon = FString(TEXT("\xf002")); // Search icon
		ToolDisplayName = TEXT("Scene Query");
		
		// Parse filters to show them nicely
		FString FilterSummary;
		TSharedPtr<FJsonObject> ArgsObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Arguments);
		if (FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid())
		{
			for (const auto& Pair : ArgsObj->Values)
			{
				if (Pair.Key == TEXT("max_results")) continue; // Skip default/noise
				FString Val;
				if (Pair.Value->TryGetString(Val) && !Val.IsEmpty())
				{
					FilterSummary += FString::Printf(TEXT("• %s: \"%s\"\n"), *Pair.Key, *Val);
				}
			}
		}
		if (FilterSummary.IsEmpty())
		{
			FilterSummary = TEXT("No specific filters (searching all actors)");
		}

		ContentWidget = SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(NSLOCTEXT("UnrealGPT", "QueryFilters", "Filters:"))
				.Font(UnrealGPTAgentUI::CaptionFont())
				.ColorAndOpacity(FStyleColors::Foreground)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FilterSummary.TrimEnd()))
				.Font(UnrealGPTAgentUI::BodyFont())
				.ColorAndOpacity(FStyleColors::Foreground)
				.AutoWrapText(true)
			];
	}
	else if (ToolName == TEXT("viewport_screenshot"))
	{
		ToolColor = FLinearColor(1.0f, 0.478f, 0.239f, 1.0f); // DESIGN gradient-orange
		ToolIcon = FString(TEXT("\xf030")); // Camera icon
		ToolDisplayName = TEXT("Viewport Screenshot");
		
		ContentWidget = SNew(STextBlock)
			.Text(NSLOCTEXT("UnrealGPT", "ScreenshotMsg", "Capturing current viewport state..."))
			.Font(UnrealGPTAgentUI::CaptionItalicFont())
			.ColorAndOpacity(FStyleColors::Foreground);
	}
	// else if (ToolName == TEXT("computer_use"))
	// {
	// 	ToolColor = FLinearColor(0.9f, 0.5f, 0.3f, 1.0f);
	// 	ToolIcon = FString(TEXT("\xf108")); // Desktop icon
	// 	ToolDisplayName = TEXT("Computer Use");
	// 	
	// 	FString ActionType = TEXT("Unknown Action");
	// 	FString ActionDetails = Arguments;
	// 	
	// 	TSharedPtr<FJsonObject> ArgsObj;
	// 	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Arguments);
	// 	if (FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid())
	// 	{
	// 		// The tool arg is "action" which is a JSON string itself
	// 		FString ActionJsonString;
	// 		if (ArgsObj->TryGetStringField(TEXT("action"), ActionJsonString))
	// 		{
	// 			TSharedPtr<FJsonObject> InnerAction;
	// 			TSharedRef<TJsonReader<>> InnerReader = TJsonReaderFactory<>::Create(ActionJsonString);
	// 			if (FJsonSerializer::Deserialize(InnerReader, InnerAction) && InnerAction.IsValid())
	// 			{
	// 				InnerAction->TryGetStringField(TEXT("type"), ActionType);
	// 				
	// 				if (ActionType == TEXT("file_operation"))
	// 				{
	// 					FString Op, Path;
	// 					InnerAction->TryGetStringField(TEXT("operation"), Op);
	// 					InnerAction->TryGetStringField(TEXT("path"), Path);
	// 					ActionDetails = FString::Printf(TEXT("%s: %s"), *Op.ToUpper(), *Path);
	// 				}
	// 				else if (ActionType == TEXT("os_command"))
	// 				{
	// 					FString Cmd;
	// 					InnerAction->TryGetStringField(TEXT("command"), Cmd);
	// 					ActionDetails = FString::Printf(TEXT("Run: %s"), *Cmd);
	// 				}
	// 				else
	// 				{
	// 					ActionDetails = ActionJsonString;
	// 				}
	// 			}
	// 		}
	// 	}
	//
	// 	ContentWidget = SNew(SVerticalBox)
	// 		+ SVerticalBox::Slot()
	// 		.AutoHeight()
	// 		[
	// 			SNew(STextBlock)
	// 			.Text(FText::FromString(ActionType))
	// 			.Font(UnrealGPTAgentUI::CaptionFont())
	// 			.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f, 1.0f))
	// 		]
	// 		+ SVerticalBox::Slot()
	// 		.AutoHeight()
	// 		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
	// 		[
	// 			SNew(STextBlock)
	// 			.Text(FText::FromString(ActionDetails))
	// 			.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
	// 			.ColorAndOpacity(FLinearColor(0.9f, 0.9f, 0.9f, 1.0f))
	// 			.AutoWrapText(true)
	// 		];
	// }
	else if (ToolName == TEXT("web_search"))
	{
		ToolColor = FLinearColor(0.0f, 0.6f, 1.0f, 1.0f); // DESIGN accent-blue (tool chip signal only)
		ToolIcon = FString(TEXT("\xf0ac")); // Globe icon
		ToolDisplayName = TEXT("Web Search");

		// Parse the query from arguments
		FString Query;
		TSharedPtr<FJsonObject> ArgsObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Arguments);
		if (FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid())
		{
			ArgsObj->TryGetStringField(TEXT("query"), Query);
		}
		if (Query.IsEmpty())
		{
			Query = Arguments; // Fallback if not structured
		}

		ContentWidget = SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(NSLOCTEXT("UnrealGPT", "WebSearchQuery", "Searching for:"))
				.Font(UnrealGPTAgentUI::CaptionFont())
				.ColorAndOpacity(FStyleColors::Foreground)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Query))
				.Font(UnrealGPTAgentUI::BodyItalicFont())
					.ColorAndOpacity(FStyleColors::Foreground)
				.AutoWrapText(true)
			];
	}
	else if (ToolName == TEXT("file_search"))
	{
		ToolColor = FLinearColor(1.0f, 0.333f, 0.467f, 1.0f); // DESIGN gradient-coral
		ToolIcon = FString(TEXT("\xf02d")); // Book icon (documentation)
		ToolDisplayName = TEXT("Documentation Search"); // Friendlier name

		// Parse the query from arguments (usually just "query" string)
		FString Query;
		// Try simple string first if arguments is just a string (unlikely for tool args, but possible in some APIs)
		if (!Arguments.StartsWith(TEXT("{")))
		{
			Query = Arguments;
		}
		else
		{
			TSharedPtr<FJsonObject> ArgsObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Arguments);
			if (FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid())
			{
				ArgsObj->TryGetStringField(TEXT("query"), Query);
			}
		}

		ContentWidget = SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(NSLOCTEXT("UnrealGPT", "FileSearchQuery", "Searching local docs for:"))
				.Font(UnrealGPTAgentUI::CaptionFont())
				.ColorAndOpacity(FStyleColors::Foreground)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Query))
				.Font(UnrealGPTAgentUI::BodyItalicFont())
					.ColorAndOpacity(FStyleColors::Foreground)
				.AutoWrapText(true)
			];
	}
	else
	{
		ToolColor = FLinearColor(0.5f, 0.5f, 0.5f, 1.0f);
		ToolIcon = FString(TEXT("\xf085")); // Cog icon
		ToolDisplayName = ToolName;
		
		ContentWidget = SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(NSLOCTEXT("UnrealGPT", "Arguments", "Arguments:"))
				.Font(UnrealGPTAgentUI::CaptionFont())
				.ColorAndOpacity(FStyleColors::Foreground)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Arguments.Left(200) + (Arguments.Len() > 200 ? TEXT("...") : TEXT(""))))
				.AutoWrapText(true)
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
				.ColorAndOpacity(FStyleColors::Foreground)
			];
	}
	
	const TSharedRef<SWidget> ToolInner =
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.White"))
		.BorderBackgroundColor(FStyleColors::Panel)
		.Padding(FMargin(UnrealGPTAgentUI::SpaceMd, UnrealGPTAgentUI::SpaceSm))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, UnrealGPTAgentUI::SpaceSm, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(4.0f)
				.MinDesiredHeight(48.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("WhiteBorder"))
					.BorderBackgroundColor(ToolColor)
					.Padding(0.0f)
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, UnrealGPTAgentUI::SpaceXs)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SBox)
						.WidthOverride(24.0f)
						.HeightOverride(24.0f)
						[
							SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush("Brushes.White"))
							.BorderBackgroundColor(ToolColor)
							.Padding(0.0f)
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Font(FAppStyle::Get().GetFontStyle("FontAwesome.11"))
								.Text(FText::FromString(ToolIcon))
								.ColorAndOpacity(FStyleColors::ForegroundInverted)
							]
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(NSLOCTEXT("UnrealGPT", "ToolLaneEyebrow", "TOOL"))
						.Font(UnrealGPTAgentUI::CaptionFont())
						.ColorAndOpacity(FStyleColors::Foreground)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(10.0f, 0.0f, 10.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(ToolDisplayName))
						.Font(UnrealGPTAgentUI::BodyBoldFont())
						.ColorAndOpacity(ToolColor)
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(SSpacer)
						.Size(FVector2D(1.0f, 1.0f))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("Brushes.White"))
						.BorderBackgroundColor(FStyleColors::Input)
						.Padding(FMargin(10.0f, 4.0f))
						[
							SNew(STextBlock)
							.Text(NSLOCTEXT("UnrealGPT", "ToolExecuted", "Executed"))
							.Font(FAppStyle::GetFontStyle("SmallFont"))
							.ColorAndOpacity(FStyleColors::Foreground)
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(2.0f, 0.0f, 0.0f, 0.0f)
				[
					ContentWidget.ToSharedRef()
				]
			]
		];

	return WrapThreadTurn(ToolInner, HAlign_Left);
}

TSharedRef<SWidget> SUnrealGPTWidget::CreateToolCallWidget(const FString& ToolName, const FString& Arguments, const FString& Result)
{
	return CreateToolSpecificWidget(ToolName, Arguments, Result);
}

FReply SUnrealGPTWidget::OnSendOrStopClicked()
{
	if (!AgentClient)
	{
		return FReply::Handled();
	}

	// If agent is running, stop it
	if (bAgentIsRunning)
	{
		AgentClient->CancelRequest();
		SetAgentRunning(false);
		
		// Add a system message to chat indicating the agent was stopped
		if (ChatHistoryBox.IsValid())
		{
			AddConversationTurnWidget(
				CreateMessageWidget(TEXT("system"), TEXT("Agent stopped by user. Send a message to continue.")),
				FMargin(UnrealGPTAgentUI::SpaceXs));
		}
		
		// Hide reasoning indicator
		if (ReasoningStatusBorder.IsValid())
		{
			ReasoningStatusBorder->SetVisibility(EVisibility::Collapsed);
		}
		
		return FReply::Handled();
	}

	// Otherwise, send a message
	if (!InputTextBox.IsValid())
	{
		return FReply::Handled();
	}

	FString Message = InputTextBox->GetText().ToString();
	const bool bHasImages = PendingAttachedImages.Num() > 0;
	if (Message.IsEmpty() && !bHasImages)
	{
		return FReply::Handled();
	}

	// Add user message to chat
	if (ChatHistoryBox.IsValid())
	{
		FString DisplayMessage = Message;
		if (DisplayMessage.IsEmpty() && bHasImages)
		{
			DisplayMessage = TEXT("[Image attached]");
		}

		AddConversationTurnWidget(CreateMessageWidget(TEXT("user"), DisplayMessage), FMargin(UnrealGPTAgentUI::SpaceXs));
	}

	// Clear input
	InputTextBox->SetText(FText::GetEmpty());

	// Mark agent as running before sending
	SetAgentRunning(true);

	// Send to agent
	AgentClient->SendMessage(Message, PendingAttachedImages);

	// Show reasoning indicator while the agent is working
	if (ReasoningStatusBorder.IsValid())
	{
		ReasoningStatusBorder->SetVisibility(EVisibility::Visible);
	}
	if (ReasoningSummaryText.IsValid())
	{
		ReasoningSummaryText->SetText(NSLOCTEXT("UnrealGPT", "ReasoningPendingShort", "Thinking..."));
	}

	// Clear any pending images after sending
	PendingAttachedImages.Empty();
	AttachmentBrushes.Empty();
	if (AttachmentPreviewBox.IsValid())
	{
		AttachmentPreviewBox->ClearChildren();
	}

	return FReply::Handled();
}

FReply SUnrealGPTWidget::OnRequestContextClicked()
{
	if (!AgentClient)
	{
		return FReply::Handled();
	}

	// Capture screenshot
	FString ScreenshotBase64 = UUnrealGPTSceneContext::CaptureViewportScreenshot();
	
	// Get scene summary
	FString SceneSummary = UUnrealGPTSceneContext::GetSceneSummary(100);

	// Build context message
	FString ContextMessage = FString::Printf(
		TEXT("Please analyze the current scene context:\n\nScene Summary:\n%s\n\nI've also included a screenshot of the viewport."),
		*SceneSummary
	);

	TArray<FString> Images;
	if (!ScreenshotBase64.IsEmpty())
	{
		Images.Add(ScreenshotBase64);
		
		// Show screenshot preview
		if (ScreenshotPreview.IsValid())
		{
			// Decode and display screenshot
			// Note: Full implementation would decode base64 and create texture
			ScreenshotPreview->SetVisibility(EVisibility::Visible);
		}
	}

	// Send context request
	AgentClient->SendMessage(ContextMessage, Images);

	return FReply::Handled();
}

FReply SUnrealGPTWidget::OnClearHistoryClicked()
{
	if (AgentClient)
	{
		AgentClient->ClearHistory();
	}

	if (ChatHistoryBox.IsValid())
	{
		ChatHistoryBox->ClearChildren();
	}

	ChatConversationBlockCount = 0;
	SyncEmptyThreadVisibility();

	// Also clear any pending attachments
	PendingAttachedImages.Empty();
	AttachmentBrushes.Empty();
	if (AttachmentPreviewBox.IsValid())
	{
		AttachmentPreviewBox->ClearChildren();
	}

	ToolCallHistory.Empty();

	// Hide reasoning status as the conversation has been reset
	if (ReasoningStatusBorder.IsValid())
	{
		ReasoningStatusBorder->SetVisibility(EVisibility::Collapsed);
	}
	if (ReasoningSummaryText.IsValid())
	{
		ReasoningSummaryText->SetText(FText::GetEmpty());
	}

	return FReply::Handled();
}

FReply SUnrealGPTWidget::OnSettingsClicked()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->ShowViewer("Project", "UnrealGPT", "UnrealGPTSettings");
	}

	return FReply::Handled();
}

void SUnrealGPTWidget::AppendSystemMessage(const FString& Content)
{
	AddConversationTurnWidget(CreateMessageWidget(TEXT("system"), Content), FMargin(UnrealGPTAgentUI::SpaceXs));
}

void SUnrealGPTWidget::StartCodexDeviceAuth()
{
	TSharedPtr<FJsonObject> RequestJson = MakeShareable(new FJsonObject);
	RequestJson->SetStringField(TEXT("client_id"), TEXT("app_EMoamEEZ73f0CkXaXp7hrann"));

	FString RequestBody;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
	FJsonSerializer::Serialize(RequestJson.ToSharedRef(), Writer);

	CodexLoginRequest = FHttpModule::Get().CreateRequest();
	CodexLoginRequest->SetURL(TEXT("https://auth.openai.com/api/accounts/deviceauth/usercode"));
	CodexLoginRequest->SetVerb(TEXT("POST"));
	CodexLoginRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	CodexLoginRequest->SetContentAsString(RequestBody);
	CodexLoginRequest->OnProcessRequestComplete().BindRaw(this, &SUnrealGPTWidget::OnCodexUserCodeResponse);

	bCodexDeviceAuthInProgress = true;
	CodexLoginStartedAt = FSlateApplication::Get().GetCurrentTime();
	AppendSystemMessage(TEXT("Starting Codex device login..."));
	CodexLoginRequest->ProcessRequest();
}

void SUnrealGPTWidget::OnCodexUserCodeResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	CodexLoginRequest.Reset();

	if (!bWasSuccessful || !Response.IsValid())
	{
		AppendSystemMessage(TEXT("Codex device login failed to contact auth.openai.com."));
		ResetCodexLoginProcess();
		return;
	}

	const int32 ResponseCode = Response->GetResponseCode();
	const FString ResponseBody = Response->GetContentAsString();
	if (ResponseCode < 200 || ResponseCode >= 300)
	{
		AppendSystemMessage(FString::Printf(TEXT("Codex device login failed: HTTP %d - %s"), ResponseCode, *ResponseBody.Left(300)));
		ResetCodexLoginProcess();
		return;
	}

	TSharedPtr<FJsonObject> ResponseJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
	{
		AppendSystemMessage(TEXT("Codex device login returned invalid JSON."));
		ResetCodexLoginProcess();
		return;
	}

	ResponseJson->TryGetStringField(TEXT("device_auth_id"), CodexDeviceAuthId);
	if (!ResponseJson->TryGetStringField(TEXT("user_code"), CodexDeviceUserCode))
	{
		ResponseJson->TryGetStringField(TEXT("usercode"), CodexDeviceUserCode);
	}

	FString IntervalString;
	if (ResponseJson->TryGetStringField(TEXT("interval"), IntervalString))
	{
		CodexPollIntervalSeconds = FMath::Max(1, FCString::Atoi(*IntervalString));
	}
	else
	{
		int32 IntervalNumber = 0;
		if (ResponseJson->TryGetNumberField(TEXT("interval"), IntervalNumber))
		{
			CodexPollIntervalSeconds = FMath::Max(1, IntervalNumber);
		}
	}

	if (CodexDeviceAuthId.IsEmpty() || CodexDeviceUserCode.IsEmpty())
	{
		AppendSystemMessage(TEXT("Codex device login did not return a usable device code."));
		ResetCodexLoginProcess();
		return;
	}

	const FString DeviceLoginUrl = TEXT("https://auth.openai.com/codex/device");
	FPlatformProcess::LaunchURL(*DeviceLoginUrl, nullptr, nullptr);
	AppendSystemMessage(FString::Printf(
		TEXT("Codex device login code: %s\n\nEnter this at %s. UnrealGPT will finish login automatically after authorization."),
		*CodexDeviceUserCode,
		*DeviceLoginUrl));

	CodexNextPollTime = FSlateApplication::Get().GetCurrentTime() + CodexPollIntervalSeconds;
}

void SUnrealGPTWidget::PollCodexDeviceAuthToken()
{
	if (!bCodexDeviceAuthInProgress || CodexDeviceAuthId.IsEmpty() || CodexDeviceUserCode.IsEmpty())
	{
		return;
	}

	const double Now = FSlateApplication::Get().GetCurrentTime();
	if (CodexLoginStartedAt > 0.0 && Now - CodexLoginStartedAt > 15.0 * 60.0)
	{
		AppendSystemMessage(TEXT("Codex device login timed out. Please try Codex Login again."));
		ResetCodexLoginProcess();
		return;
	}

	TSharedPtr<FJsonObject> RequestJson = MakeShareable(new FJsonObject);
	RequestJson->SetStringField(TEXT("device_auth_id"), CodexDeviceAuthId);
	RequestJson->SetStringField(TEXT("user_code"), CodexDeviceUserCode);

	FString RequestBody;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
	FJsonSerializer::Serialize(RequestJson.ToSharedRef(), Writer);

	CodexLoginRequest = FHttpModule::Get().CreateRequest();
	CodexLoginRequest->SetURL(TEXT("https://auth.openai.com/api/accounts/deviceauth/token"));
	CodexLoginRequest->SetVerb(TEXT("POST"));
	CodexLoginRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	CodexLoginRequest->SetContentAsString(RequestBody);
	CodexLoginRequest->OnProcessRequestComplete().BindRaw(this, &SUnrealGPTWidget::OnCodexDeviceTokenResponse);
	CodexLoginRequest->ProcessRequest();
}

void SUnrealGPTWidget::OnCodexDeviceTokenResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	CodexLoginRequest.Reset();

	if (!bWasSuccessful || !Response.IsValid())
	{
		CodexNextPollTime = FSlateApplication::Get().GetCurrentTime() + CodexPollIntervalSeconds;
		return;
	}

	const int32 ResponseCode = Response->GetResponseCode();
	const FString ResponseBody = Response->GetContentAsString();
	if (ResponseCode == 403 || ResponseCode == 404
		|| (ResponseCode == 400 && (ResponseBody.Contains(TEXT("token_pending")) || ResponseBody.Contains(TEXT("authorization_pending")) || ResponseBody.Contains(TEXT("slow_down")))))
	{
		if (ResponseBody.Contains(TEXT("slow_down")))
		{
			CodexPollIntervalSeconds = FMath::Max(CodexPollIntervalSeconds + 5, 10);
		}
		CodexNextPollTime = FSlateApplication::Get().GetCurrentTime() + CodexPollIntervalSeconds;
		return;
	}

	if (ResponseCode < 200 || ResponseCode >= 300)
	{
		AppendSystemMessage(FString::Printf(TEXT("Codex device login polling failed: HTTP %d - %s"), ResponseCode, *ResponseBody.Left(300)));
		ResetCodexLoginProcess();
		return;
	}

	TSharedPtr<FJsonObject> ResponseJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
	{
		AppendSystemMessage(TEXT("Codex device login polling returned invalid JSON."));
		ResetCodexLoginProcess();
		return;
	}

	FString AuthorizationCode;
	FString CodeVerifier;
	FString IdToken;
	FString AccessToken;
	FString RefreshToken;
	ResponseJson->TryGetStringField(TEXT("id_token"), IdToken);
	ResponseJson->TryGetStringField(TEXT("access_token"), AccessToken);
	ResponseJson->TryGetStringField(TEXT("refresh_token"), RefreshToken);
	if (!IdToken.IsEmpty() && !AccessToken.IsEmpty() && !RefreshToken.IsEmpty())
	{
		FString PersistError;
		if (!PersistCodexAuth(IdToken, AccessToken, RefreshToken, PersistError))
		{
			AppendSystemMessage(FString::Printf(TEXT("Codex login succeeded, but UnrealGPT could not save auth.json: %s"), *PersistError));
			ResetCodexLoginProcess();
			return;
		}

		AppendSystemMessage(TEXT("Codex login completed. UnrealGPT Codex auth is enabled."));
		ResetCodexLoginProcess();
		return;
	}

	ResponseJson->TryGetStringField(TEXT("authorization_code"), AuthorizationCode);
	ResponseJson->TryGetStringField(TEXT("code_verifier"), CodeVerifier);
	if (AuthorizationCode.IsEmpty() || CodeVerifier.IsEmpty())
	{
		AppendSystemMessage(TEXT("Codex device login completed but did not return an authorization code."));
		ResetCodexLoginProcess();
		return;
	}

	ExchangeCodexAuthorizationCode(AuthorizationCode, CodeVerifier);
}

void SUnrealGPTWidget::ExchangeCodexAuthorizationCode(const FString& AuthorizationCode, const FString& CodeVerifier)
{
	const FString RedirectUri = TEXT("https://auth.openai.com/deviceauth/callback");
	const FString ClientId = TEXT("app_EMoamEEZ73f0CkXaXp7hrann");
	const FString RequestBody = FString::Printf(
		TEXT("grant_type=authorization_code&code=%s&redirect_uri=%s&client_id=%s&code_verifier=%s"),
		*FGenericPlatformHttp::UrlEncode(AuthorizationCode),
		*FGenericPlatformHttp::UrlEncode(RedirectUri),
		*FGenericPlatformHttp::UrlEncode(ClientId),
		*FGenericPlatformHttp::UrlEncode(CodeVerifier));

	CodexLoginRequest = FHttpModule::Get().CreateRequest();
	CodexLoginRequest->SetURL(TEXT("https://auth.openai.com/oauth/token"));
	CodexLoginRequest->SetVerb(TEXT("POST"));
	CodexLoginRequest->SetHeader(TEXT("Content-Type"), TEXT("application/x-www-form-urlencoded"));
	CodexLoginRequest->SetContentAsString(RequestBody);
	CodexLoginRequest->OnProcessRequestComplete().BindRaw(this, &SUnrealGPTWidget::OnCodexTokenExchangeResponse);
	CodexLoginRequest->ProcessRequest();
}

void SUnrealGPTWidget::OnCodexTokenExchangeResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	CodexLoginRequest.Reset();

	if (!bWasSuccessful || !Response.IsValid())
	{
		AppendSystemMessage(TEXT("Codex token exchange failed to contact auth.openai.com."));
		ResetCodexLoginProcess();
		return;
	}

	const int32 ResponseCode = Response->GetResponseCode();
	const FString ResponseBody = Response->GetContentAsString();
	if (ResponseCode < 200 || ResponseCode >= 300)
	{
		AppendSystemMessage(FString::Printf(TEXT("Codex token exchange failed: HTTP %d - %s"), ResponseCode, *ResponseBody.Left(300)));
		ResetCodexLoginProcess();
		return;
	}

	TSharedPtr<FJsonObject> ResponseJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
	{
		AppendSystemMessage(TEXT("Codex token exchange returned invalid JSON."));
		ResetCodexLoginProcess();
		return;
	}

	FString IdToken;
	FString AccessToken;
	FString RefreshToken;
	ResponseJson->TryGetStringField(TEXT("id_token"), IdToken);
	ResponseJson->TryGetStringField(TEXT("access_token"), AccessToken);
	ResponseJson->TryGetStringField(TEXT("refresh_token"), RefreshToken);

	FString PersistError;
	if (IdToken.IsEmpty() || AccessToken.IsEmpty() || RefreshToken.IsEmpty())
	{
		PersistError = TEXT("Token response was missing id_token, access_token, or refresh_token.");
	}

	if (!PersistError.IsEmpty() || !PersistCodexAuth(IdToken, AccessToken, RefreshToken, PersistError))
	{
		AppendSystemMessage(FString::Printf(TEXT("Codex login succeeded, but UnrealGPT could not save auth.json: %s"), *PersistError));
		ResetCodexLoginProcess();
		return;
	}

	AppendSystemMessage(TEXT("Codex login completed. UnrealGPT Codex auth is enabled."));
	ResetCodexLoginProcess();
}

bool SUnrealGPTWidget::PersistCodexAuth(const FString& IdToken, const FString& AccessToken, const FString& RefreshToken, FString& OutError) const
{
	const UUnrealGPTSettings* Settings = GetDefault<UUnrealGPTSettings>();
	if (!Settings)
	{
		OutError = TEXT("UnrealGPT settings are unavailable.");
		return false;
	}

	return Settings->SaveCodexChatGPTAuth(IdToken, AccessToken, RefreshToken, OutError);
}

bool SUnrealGPTWidget::IsCodexLoggedIn() const
{
	const UUnrealGPTSettings* Settings = GetDefault<UUnrealGPTSettings>();
	return Settings && Settings->bUseCodexAuth && Settings->IsUsingCodexChatGPTAuth();
}

FText SUnrealGPTWidget::GetCodexLoginButtonText() const
{
	if (bCodexDeviceAuthInProgress || CodexLoginRequest.IsValid())
	{
		return NSLOCTEXT("UnrealGPT", "CodexLoginInProgress", "Codex Login...");
	}

	return IsCodexLoggedIn()
		? NSLOCTEXT("UnrealGPT", "CodexConnected", "Codex Connected")
		: NSLOCTEXT("UnrealGPT", "CodexLogin", "Codex Login");
}

FText SUnrealGPTWidget::GetCodexLoginButtonTooltip() const
{
	if (IsCodexLoggedIn())
	{
		return NSLOCTEXT("UnrealGPT", "CodexConnectedTooltip", "Codex auth is active. Use Settings to change or clear the auth file.");
	}

	if (bCodexDeviceAuthInProgress || CodexLoginRequest.IsValid())
	{
		return NSLOCTEXT("UnrealGPT", "CodexLoginInProgressTooltip", "Codex device login is already in progress.");
	}

	return NSLOCTEXT("UnrealGPT", "CodexLoginTooltip", "Sign in with Codex device auth.");
}

bool SUnrealGPTWidget::IsCodexLoginButtonEnabled() const
{
	return !IsCodexLoggedIn() && !bCodexDeviceAuthInProgress && !CodexLoginRequest.IsValid();
}

void SUnrealGPTWidget::ResetCodexLoginProcess()
{
	if (CodexLoginRequest.IsValid())
	{
		CodexLoginRequest->CancelRequest();
		CodexLoginRequest.Reset();
	}

	CodexDeviceAuthId.Empty();
	CodexDeviceUserCode.Empty();
	CodexNextPollTime = 0.0;
	CodexLoginStartedAt = 0.0;
	CodexPollIntervalSeconds = 5;
	bCodexDeviceAuthInProgress = false;
}

FReply SUnrealGPTWidget::OnCodexLoginClicked()
{
	if (IsCodexLoggedIn())
	{
		AppendSystemMessage(TEXT("Codex auth is already active."));
		return FReply::Handled();
	}

	if (bCodexDeviceAuthInProgress || CodexLoginRequest.IsValid())
	{
		AppendSystemMessage(TEXT("Codex device login is already running. Use the code shown above or wait for it to finish."));
		return FReply::Handled();
	}

	ResetCodexLoginProcess();

	if (UUnrealGPTSettings* Settings = GetMutableDefault<UUnrealGPTSettings>())
	{
		Settings->bUseCodexAuth = true;
		Settings->bUseCodexResponsesEndpoint = true;
		Settings->SaveConfig();
	}

	StartCodexDeviceAuth();

	return FReply::Handled();
}

FReply SUnrealGPTWidget::OnAttachImageClicked()
{
	// Let the user pick an image file from disk and attach it (base64) to the next message.
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		return FReply::Handled();
	}

	// Determine parent window handle for the file dialog
	void* ParentWindowHandle = nullptr;
	if (TSharedPtr<SWindow> ParentWindow = FSlateApplication::Get().FindWidgetWindow(AsShared()))
	{
		if (ParentWindow->GetNativeWindow().IsValid())
		{
			ParentWindowHandle = ParentWindow->GetNativeWindow()->GetOSWindowHandle();
		}
	}

	TArray<FString> OutFiles;
	const FString DialogTitle = TEXT("Select Image to Attach");
	const FString DefaultPath = FPaths::ProjectDir();
	const FString DefaultFile = TEXT("");
	const FString FileTypes =
		TEXT("Image Files (*.png;*.jpg;*.jpeg)|*.png;*.jpg;*.jpeg|")
		TEXT("All Files (*.*)|*.*");

	const bool bOpened = DesktopPlatform->OpenFileDialog(
		ParentWindowHandle,
		DialogTitle,
		DefaultPath,
		DefaultFile,
		FileTypes,
		EFileDialogFlags::None,
		OutFiles
	);

	if (!bOpened || OutFiles.Num() == 0)
	{
		return FReply::Handled();
	}

	const FString SelectedPath = OutFiles[0];

	TArray<uint8> FileData;
	if (!FFileHelper::LoadFileToArray(FileData, *SelectedPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Failed to read image file: %s"), *SelectedPath);
		return FReply::Handled();
	}

	// Encode raw bytes as base64; the agent client will wrap as data:image/... for OpenAI.
	const FString Base64Image = FBase64::Encode(FileData);
	if (!Base64Image.IsEmpty())
	{
		PendingAttachedImages.Add(Base64Image);

		// Refresh send/stop button enabled state
		if (SendStopButton.IsValid())
		{
			SendStopButton->Invalidate(EInvalidateWidget::LayoutAndVolatility);
		}

		// Also create a small thumbnail preview so the user can see what is attached.
		// Detect format from file extension (PNG vs JPEG); default to PNG.
		EImageFormat ImageFormat = EImageFormat::PNG;
		const FString Extension = FPaths::GetExtension(SelectedPath).ToLower();
		if (Extension == TEXT("jpg") || Extension == TEXT("jpeg"))
		{
			ImageFormat = EImageFormat::JPEG;
		}

		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(ImageFormat);

		if (ImageWrapper.IsValid() && ImageWrapper->SetCompressed(FileData.GetData(), FileData.Num()))
		{
			TArray<uint8> UncompressedBGRA;
			if (ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, UncompressedBGRA))
			{
				const int32 Width = ImageWrapper->GetWidth();
				const int32 Height = ImageWrapper->GetHeight();

				// Convert BGRA bytes into FColor array
				TArray<FColor> ColorData;
				ColorData.Reserve(Width * Height);
				for (int32 i = 0; i + 3 < UncompressedBGRA.Num(); i += 4)
				{
					FColor Color;
					Color.B = UncompressedBGRA[i + 0];
					Color.G = UncompressedBGRA[i + 1];
					Color.R = UncompressedBGRA[i + 2];
					Color.A = UncompressedBGRA[i + 3];
					ColorData.Add(Color);
				}

				FCreateTexture2DParameters TextureParams;
				UTexture2D* Texture = FImageUtils::CreateTexture2D(
					Width,
					Height,
					ColorData,
					GetTransientPackage(),
					TEXT("UnrealGPT_AttachedImage"),
					RF_Transient,
					TextureParams);

				if (Texture)
				{
					TSharedPtr<FSlateBrush> Brush = MakeShareable(new FSlateBrush());
					Brush->SetResourceObject(Texture);
					Brush->ImageSize = FVector2D(Width, Height);
					Brush->ImageType = ESlateBrushImageType::FullColor;

					AttachmentBrushes.Add(Brush);

					if (AttachmentPreviewBox.IsValid())
					{
						AttachmentPreviewBox->AddSlot()
						[
							SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
							.BorderBackgroundColor(FStyleColors::Input)
							.Padding(FMargin(UnrealGPTAgentUI::SpaceXs * 0.25f))
							[
								SNew(SBox)
								.WidthOverride(72.0f)
								.HeightOverride(72.0f)
								[
									SNew(SImage)
									.Image(Brush.Get())
								]
							]
						];
					}
				}
			}
		}
	}

	return FReply::Handled();
}

bool SUnrealGPTWidget::IsSendOrStopEnabled() const
{
	// Always enabled if agent is running (for stop functionality)
	if (bAgentIsRunning)
	{
		return true;
	}
	
	// Otherwise enabled if there's input text or attached images
	if (!InputTextBox.IsValid())
	{
		return false;
	}

	FString Text = InputTextBox->GetText().ToString();
	return !Text.IsEmpty() || PendingAttachedImages.Num() > 0;
}

void SUnrealGPTWidget::SetAgentRunning(bool bRunning)
{
	bAgentIsRunning = bRunning;

	if (SessionStatusLabel.IsValid())
	{
		SessionStatusLabel->SetText(bRunning
			? NSLOCTEXT("UnrealGPT", "SessionStatusRunning", "Running")
			: NSLOCTEXT("UnrealGPT", "SessionStatusIdle", "Ready"));
	}

	// Refresh the button appearance
	if (SendStopButton.IsValid())
	{
		SendStopButton->Invalidate(EInvalidateWidget::LayoutAndVolatility);
	}
}

void SUnrealGPTWidget::HandleAgentMessage(const FString& Role, const FString& Content, const TArray<FString>& ToolCalls)
{
	if (ChatHistoryBox.IsValid())
	{
		AddConversationTurnWidget(CreateMessageWidget(Role, Content), FMargin(UnrealGPTAgentUI::SpaceXs));
	}

	// When we receive a plain assistant message (no tool calls), the agent has finished
	// its current step, so we mark agent as not running.
	// NOTE: We keep the reasoning summary visible so the user can see the agent's thought process.
	// It will be hidden when the user sends a new message or clears history.
	if (Role == TEXT("assistant") && ToolCalls.Num() == 0)
	{
		// Agent has finished - update the button state
		SetAgentRunning(false);
		
		// Keep reasoning visible if we have content, just update the style to indicate completion
		// The reasoning will be hidden when the user sends a new message
	}
	
	// System messages (like iteration limit reached) also indicate the agent has stopped
	if (Role == TEXT("system"))
	{
		SetAgentRunning(false);
		// Hide reasoning for system messages (errors, limits reached, etc.)
		if (ReasoningStatusBorder.IsValid())
		{
			ReasoningStatusBorder->SetVisibility(EVisibility::Collapsed);
		}
	}
}

void SUnrealGPTWidget::HandleAgentReasoning(const FString& ReasoningContent)
{
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT Widget: HandleAgentReasoning called with content length: %d"), ReasoningContent.Len());
	
	if (ReasoningContent.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT Widget: Reasoning content is empty, ignoring"));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT Widget: Setting reasoning text: %s"), *ReasoningContent.Left(100));

	// Ensure the reasoning strip is visible whenever we receive reasoning content or a summary
	if (ReasoningStatusBorder.IsValid())
	{
		ReasoningStatusBorder->SetVisibility(EVisibility::Visible);
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT Widget: Made reasoning strip visible"));
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT Widget: ReasoningStatusBorder is invalid!"));
	}

	if (ReasoningSummaryText.IsValid())
	{
		ReasoningSummaryText->SetText(FText::FromString(ReasoningContent));
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT Widget: Updated reasoning text successfully"));
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT Widget: ReasoningSummaryText is invalid!"));
	}

	// Keep the chat scrolled to the latest messages while reasoning updates arrive
	if (ChatHistoryBox.IsValid())
	{
		ChatHistoryBox->ScrollToEnd();
	}
}

void SUnrealGPTWidget::HandleToolCall(const FString& ToolName, const FString& Arguments)
{
	// Add tool call to history list (internal tracking)
	FString ToolCallInfo = FString::Printf(TEXT("Tool: %s\nArguments: %s"), *ToolName, *Arguments);
	ToolCallHistory.Add(ToolCallInfo);

	// Add visual representation to chat
	if (ChatHistoryBox.IsValid())
	{
		AddConversationTurnWidget(CreateToolSpecificWidget(ToolName, Arguments, TEXT("")), FMargin(12.0f, 6.0f, 12.0f, 10.0f));
	}
}

void SUnrealGPTWidget::HandleToolResult(const FString& ToolCallId, const FString& Result)
{
	// Don't spam the UI with empty results
	const FString Trimmed = Result.TrimStartAndEnd();
	if (Trimmed.IsEmpty() || Trimmed == TEXT("[]"))
	{
		return;
	}

	// Check if this is a base64-encoded screenshot (PNG)
	bool bIsScreenshot = false;
	if (Trimmed.StartsWith(TEXT("iVBORw0KGgo")) && Trimmed.Len() > 100) // Base64 PNG header + reasonable size
	{
		bIsScreenshot = true;
	}

	// Try to generate a friendlier summary for JSON array results (e.g., scene_query)
	FString DisplayText;
	bool bIsSceneQueryResult = false;
	
	if (!bIsScreenshot && Trimmed.StartsWith(TEXT("[")))
	{
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		if (FJsonSerializer::Deserialize(Reader, JsonArray) && JsonArray.Num() > 0)
		{
			bIsSceneQueryResult = true;
			const int32 MaxPreview = 5;
			const int32 Total      = JsonArray.Num();

			DisplayText += FString::Printf(TEXT("Found %d item(s)\n\n"), Total);

			for (int32 Index = 0; Index < Total && Index < MaxPreview; ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = JsonArray[Index];
				TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Obj.IsValid())
				{
					continue;
				}

				FString Label  = Obj->GetStringField(TEXT("label"));
				FString Class  = Obj->GetStringField(TEXT("class"));
				const TSharedPtr<FJsonObject>* LocationObjPtr = nullptr;
				FString LocationStr;
				if (Obj->TryGetObjectField(TEXT("location"), LocationObjPtr) && LocationObjPtr && LocationObjPtr->IsValid())
				{
					const TSharedPtr<FJsonObject> LocationObj = *LocationObjPtr;
					double X = 0.0, Y = 0.0, Z = 0.0;
					LocationObj->TryGetNumberField(TEXT("x"), X);
					LocationObj->TryGetNumberField(TEXT("y"), Y);
					LocationObj->TryGetNumberField(TEXT("z"), Z);
					LocationStr = FString::Printf(TEXT("\n   Location: (%.0f, %.0f, %.0f)"), X, Y, Z);
				}

				if (Label.IsEmpty())
				{
					Label = Obj->GetStringField(TEXT("name"));
				}

				DisplayText += FString::Printf(TEXT("  %d. %s\n   Type: %s%s\n\n"), Index + 1, *Label, *Class, *LocationStr);
			}

			if (Total > MaxPreview)
			{
				DisplayText += FString::Printf(TEXT("  ... and %d more item(s)."), Total - MaxPreview);
			}
		}
	}

	// Fallback: just show the raw result (unless it's a screenshot)
	if (DisplayText.IsEmpty() && !bIsScreenshot)
	{
		// Try to parse Python result JSON structure (status/message/details)
		bool bIsPythonResult = false;
		if (Trimmed.StartsWith(TEXT("{")))
		{
			TSharedPtr<FJsonObject> ResObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
			if (FJsonSerializer::Deserialize(Reader, ResObj) && ResObj.IsValid())
			{
				FString Status;
				FString Message;
				// Check for our standard Python wrapper fields
				if (ResObj->TryGetStringField(TEXT("status"), Status) && ResObj->TryGetStringField(TEXT("message"), Message))
				{
					bIsPythonResult = true;
					bool bSuccess = (Status == TEXT("ok"));
					
					// Format status line
					DisplayText += FString::Printf(TEXT("%s %s\n\n"), 
						bSuccess ? TEXT("SUCCESS") : TEXT("ERROR"), 
						bSuccess ? TEXT("") : TEXT("")); // Placeholder

					// Format message
					DisplayText += Message;
					
					// Format details if interesting
					const TSharedPtr<FJsonObject>* DetailsObj;
					if (ResObj->TryGetObjectField(TEXT("details"), DetailsObj) && DetailsObj && (*DetailsObj).IsValid())
					{
						// Check for specific details we want to highlight
						FString ActorLabel;
						if ((*DetailsObj)->TryGetStringField(TEXT("actor_label"), ActorLabel))
						{
							DisplayText += FString::Printf(TEXT("\n\nActor: %s"), *ActorLabel);
						}
						
						FString Traceback;
						if ((*DetailsObj)->TryGetStringField(TEXT("traceback"), Traceback) && !Traceback.IsEmpty())
						{
							DisplayText += FString::Printf(TEXT("\n\nTraceback:\n%s"), *Traceback);
						}
					}
				}
			}
		}

		if (!bIsPythonResult)
		{
			DisplayText = Result;
		}
	}

	if (ChatHistoryBox.IsValid())
	{
		// Handle screenshot display specially
		if (bIsScreenshot)
		{
			// Decode base64 and create texture
			TArray<uint8> ImageData;
			if (FBase64::Decode(Trimmed, ImageData))
			{
				IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
				TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
				
				if (ImageWrapper.IsValid() && ImageWrapper->SetCompressed(ImageData.GetData(), ImageData.Num()))
				{
					TArray<uint8> UncompressedBGRA;
					if (ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, UncompressedBGRA))
					{
						int32 Width = ImageWrapper->GetWidth();
						int32 Height = ImageWrapper->GetHeight();
						
						// Create texture on game thread using FImageUtils (UE5.6 compatible)
						AsyncTask(ENamedThreads::GameThread, [this, Width, Height, UncompressedBGRA]()
						{
							// Widget may have been destroyed between scheduling and execution
							if (!ChatHistoryBox.IsValid())
							{
								return;
							}

							// Convert BGRA to FColor array for FImageUtils
							TArray<FColor> ColorData;
							ColorData.Reserve(Width * Height);
							for (int32 i = 0; i < UncompressedBGRA.Num(); i += 4)
							{
								// BGRA format: B, G, R, A
								FColor Color;
								Color.B = UncompressedBGRA[i + 0];
								Color.G = UncompressedBGRA[i + 1];
								Color.R = UncompressedBGRA[i + 2];
								Color.A = UncompressedBGRA[i + 3];
								ColorData.Add(Color);
							}
							
							// Create texture using FImageUtils (UE5.6 compatible API).
							// Use the transient package as the outer so the texture is not considered for saving/packaging.
							FCreateTexture2DParameters TextureParams;
							UTexture2D* Texture = FImageUtils::CreateTexture2D(
								Width,
								Height,
								ColorData,
								GetTransientPackage(),			// Outer
								TEXT("ScreenshotTexture"),		// Name
								RF_Transient,					// Flags (not saved/packaged)
								TextureParams);
							
							if (Texture)
							{
								// Create brush from texture (using MakeShareable for proper memory management)
								TSharedPtr<FSlateBrush> Brush = MakeShareable(new FSlateBrush());
								Brush->SetResourceObject(Texture);
								Brush->ImageSize = FVector2D(Width, Height);
								Brush->ImageType = ESlateBrushImageType::FullColor;

								// Keep the brush alive as long as the widget is alive so Slate
								// never dereferences a freed FSlateBrush pointer.
								ScreenshotBrushes.Add(Brush);

								AddConversationTurnWidget(
									WrapThreadTurn(
										SNew(SBorder)
										.BorderImage(FAppStyle::GetBrush("Brushes.White"))
										.BorderBackgroundColor(FStyleColors::Panel)
										.Padding(FMargin(14.0f, 10.0f))
										[
											SNew(SVerticalBox)

											+ SVerticalBox::Slot()
											.AutoHeight()
											.Padding(0.0f, 0.0f, 0.0f, 8.0f)
											[
												SNew(STextBlock)
												.Text(NSLOCTEXT("UnrealGPT", "ScreenshotResult", "Viewport Screenshot"))
												.Font(FAppStyle::GetFontStyle("SmallFontBold"))
												.ColorAndOpacity(FStyleColors::Success)
											]

											+ SVerticalBox::Slot()
											.AutoHeight()
											[
												SNew(SBox)
												.WidthOverride(FMath::Min(800.0f, static_cast<float>(Width)))
												.HeightOverride(FMath::Min(600.0f, static_cast<float>(Height) * 800.0f / static_cast<float>(Width)))
												[
													SNew(SImage)
													.Image(Brush.Get())
												]
											]
										],
										HAlign_Left),
									FMargin(12.0f, 6.0f, 12.0f, 10.0f));
							}
						});
						
						return; // Exit early, screenshot will be added async
					}
				}
			}
			// If decoding failed, fall through to show as text
			DisplayText = TEXT("Screenshot captured (failed to decode image for display)");
		}
		
		// Standard tool result display
		AddConversationTurnWidget(
			WrapThreadTurn(
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.White"))
				.BorderBackgroundColor(FStyleColors::Panel)
				.Padding(FMargin(14.0f, 10.0f))
				[
					SNew(SHorizontalBox)

					// Result icon
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 12.0f, 0.0f)
					[
						SNew(SBox)
						.WidthOverride(32.0f)
						.HeightOverride(32.0f)
						[
							SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush("Brushes.White"))
							.BorderBackgroundColor(bIsSceneQueryResult ? FLinearColor(0.831f, 0.302f, 0.941f, 1.0f)
								: (bIsScreenshot ? FLinearColor(1.0f, 0.478f, 0.239f, 1.0f) : FLinearColor(0.129f, 0.773f, 0.369f, 1.0f)))
							.Padding(0.0f)
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Font(FAppStyle::Get().GetFontStyle("FontAwesome.12"))
								.Text(FText::FromString(bIsScreenshot ? FString(TEXT("\xf030")) : FString(TEXT("\xf00c")))) // Camera or Check icon
								.ColorAndOpacity(FStyleColors::ForegroundInverted)
							]
						]
					]

					// Result content
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(SVerticalBox)

						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 6.0f)
						[
							SNew(STextBlock)
							.Text(NSLOCTEXT("UnrealGPT", "ToolResult", "Tool Result"))
							.Font(UnrealGPTAgentUI::CaptionFont())
							.ColorAndOpacity(FStyleColors::Foreground)
						]

						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(STextBlock)
							.Text(FText::FromString(DisplayText))
							.AutoWrapText(true)
							.Font(bIsSceneQueryResult ? UnrealGPTAgentUI::BodyFont() : FCoreStyle::GetDefaultFontStyle("Mono", 8))
							.ColorAndOpacity(FStyleColors::Foreground)
						]
					]
				],
				HAlign_Left),
			FMargin(12.0f, 6.0f, 12.0f, 10.0f));
	}
}

FReply SUnrealGPTWidget::OnVoiceInputClicked()
{
	if (!VoiceInput)
	{
		return FReply::Handled();
	}

	if (VoiceInput->IsRecording())
	{
		// Stop recording and transcribe
		VoiceInput->StopRecordingAndTranscribe();
	}
	else
	{
		// Start recording
		if (!VoiceInput->StartRecording())
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Failed to start voice recording"));
		}
	}

	return FReply::Handled();
}

void SUnrealGPTWidget::OnTranscriptionComplete(const FString& TranscribedText)
{
	if (!TranscribedText.IsEmpty() && InputTextBox.IsValid())
	{
		// Set transcribed text in input box
		FText CurrentText = InputTextBox->GetText();
		FString NewText = CurrentText.ToString();
		
		// Append transcribed text (or replace if empty)
		if (NewText.IsEmpty())
		{
			NewText = TranscribedText;
		}
		else
		{
			NewText += TEXT(" ") + TranscribedText;
		}
		
		InputTextBox->SetText(FText::FromString(NewText));
		
		// Optionally auto-send (commented out - user can manually send)
		// OnSendOrStopClicked();
	}
}

void SUnrealGPTWidget::OnRecordingStarted()
{
	// Update button appearance if needed
	if (VoiceInputButton.IsValid())
	{
		VoiceInputButton->Invalidate(EInvalidateWidget::LayoutAndVolatility);
	}
}

void SUnrealGPTWidget::OnRecordingStopped()
{
	// Update button appearance if needed
	if (VoiceInputButton.IsValid())
	{
		VoiceInputButton->Invalidate(EInvalidateWidget::LayoutAndVolatility);
	}
}

