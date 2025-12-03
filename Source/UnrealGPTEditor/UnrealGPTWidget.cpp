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
#include "Interfaces/IPluginManager.h"

namespace
{
	// Helper to build a Geist-based Slate font from this plugin's Content/Fonts folder.
	// Falls back to the default editor fonts if anything goes wrong.
	FSlateFontInfo MakeGeistFont(int32 Size, bool bBold = false, bool bItalic = false)
	{
		static bool bInitialized = false;
		static FString PluginContentDir;

		if (!bInitialized)
		{
			if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealGPT")))
			{
				PluginContentDir = Plugin->GetContentDir();
			}
			bInitialized = true;
		}

		if (!PluginContentDir.IsEmpty())
		{
			FString FileName;
			if (bBold && bItalic)
			{
				FileName = TEXT("Geist-BoldItalic.ttf");
			}
			else if (bBold)
			{
				FileName = TEXT("Geist-Bold.ttf");
			}
			else if (bItalic)
			{
				FileName = TEXT("Geist-RegularItalic.ttf");
			}
			else
			{
				FileName = TEXT("Geist-Regular.ttf");
			}

			const FString FontPath = FPaths::Combine(PluginContentDir, TEXT("Fonts/Geist/ttf"), FileName);
			return FSlateFontInfo(FontPath, Size);
		}

		// Fallback to the standard editor fonts if the plugin content directory is not available.
		if (bBold)
		{
			return FAppStyle::GetFontStyle("NormalFontBold");
		}
		if (bItalic)
		{
			return FAppStyle::GetFontStyle("NormalFontItalic");
		}
		return FAppStyle::GetFontStyle("NormalFont");
	}

	FSlateFontInfo GetUnrealGPTBodyFont()
	{
		return MakeGeistFont(10, false, false);
	}

	FSlateFontInfo GetUnrealGPTBodyBoldFont()
	{
		return MakeGeistFont(10, true, false);
	}

	FSlateFontInfo GetUnrealGPTSmallBodyFont()
	{
		return MakeGeistFont(8, false, false);
	}

	FSlateFontInfo GetUnrealGPTSmallBodyItalicFont()
	{
		return MakeGeistFont(8, false, true);
	}

	FSlateFontInfo GetUnrealGPTBodyItalicFont()
	{
		return MakeGeistFont(10, false, true);
	}
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
			
			// Modern Toolbar with gradient background
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
				.Padding(FMargin(12.0f, 8.0f))
				[
					SNew(SHorizontalBox)
					
					// Left section - Action buttons
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SHorizontalBox)
						
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SBox)
							.MinDesiredWidth(140.0f)
							[
								SAssignNew(RequestContextButton, SButton)
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Success")
								.ForegroundColor(FSlateColor::UseForeground())
								.ContentPadding(FMargin(10.0f, 6.0f))
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
										.Text(FText::FromString(FString(TEXT("\xf030")))) // Camera icon
										.ColorAndOpacity(FLinearColor(0.3f, 0.8f, 0.3f))
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
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SBox)
							.MinDesiredWidth(120.0f)
							[
								SAssignNew(ClearHistoryButton, SButton)
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Default")
								.ForegroundColor(FSlateColor::UseForeground())
								.ContentPadding(FMargin(10.0f, 6.0f))
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
										.Text(FText::FromString(FString(TEXT("\xf014")))) // Trash icon
										.ColorAndOpacity(FLinearColor(0.8f, 0.4f, 0.4f))
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
					
					// Spacer
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNullWidget::NullWidget
					]
					
					// Right section - Settings
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SBox)
						.MinDesiredWidth(100.0f)
						[
							SAssignNew(SettingsButton, SButton)
							.ButtonStyle(FAppStyle::Get(), "FlatButton.Default")
							.ForegroundColor(FSlateColor::UseForeground())
							.ContentPadding(FMargin(10.0f, 6.0f))
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
									.Text(FText::FromString(FString(TEXT("\xf013")))) // Gear icon
									.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.8f))
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
			]
			
			// Screenshot preview with better styling
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(12.0f, 8.0f)
			[
				SAssignNew(ScreenshotPreview, SImage)
				.Visibility(EVisibility::Collapsed)
			]
			
			// Chat history with subtle background
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				.Padding(0.0f)
				[
					SAssignNew(ChatHistoryBox, SScrollBox)
					.Orientation(Orient_Vertical)
					.ScrollBarAlwaysVisible(false)
					.ConsumeMouseWheel(EConsumeMouseWheel::Always)
					+ SScrollBox::Slot()
					.Padding(12.0f, 12.0f)
					[
						SNullWidget::NullWidget
					]
				]
			]

			// Reasoning status / summary strip
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(12.0f, 4.0f, 12.0f, 4.0f))
			[
				SAssignNew(ReasoningStatusBorder, SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
				.BorderBackgroundColor(FLinearColor(0.04f, 0.04f, 0.06f, 1.0f))
				.Padding(FMargin(8.0f, 4.0f))
				.Visibility(EVisibility::Collapsed)
				[
					SNew(SHorizontalBox)
					
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(STextBlock)
						.Font(FAppStyle::Get().GetFontStyle("FontAwesome.10"))
						.Text(FText::FromString(FString(TEXT("\xf0eb")))) // Lightbulb icon
						.ColorAndOpacity(FLinearColor(0.9f, 0.85f, 0.4f, 1.0f))
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SAssignNew(ReasoningSummaryText, STextBlock)
						.Text(NSLOCTEXT("UnrealGPT", "ReasoningPending", "Thinking..."))
						.Font(GetUnrealGPTSmallBodyFont())
						.AutoWrapText(true)
						.ColorAndOpacity(FLinearColor(0.8f, 0.8f, 0.85f, 1.0f))
					]
				]
			]
			
			// Modern Input area
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
				.Padding(FMargin(12.0f, 12.0f))
				[
					SNew(SHorizontalBox)
					
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("Brushes.White"))
						.BorderBackgroundColor(FLinearColor(0.02f, 0.02f, 0.02f, 1.0f))
						.Padding(FMargin(12.0f, 10.0f))
						[
							SAssignNew(InputTextBox, SMultiLineEditableTextBox)
							.HintText(NSLOCTEXT("UnrealGPT", "InputHint", "Ask UnrealGPT anything... (Ctrl+Enter to send)"))
							.Font(GetUnrealGPTBodyFont())
							.Margin(FMargin(0.0f))
							.OnKeyDownHandler_Lambda([this](const FGeometry&, const FKeyEvent& KeyEvent) -> FReply
							{
								if (KeyEvent.GetKey() == EKeys::Enter && KeyEvent.IsControlDown())
								{
									OnSendClicked();
									return FReply::Handled();
								}
								return FReply::Unhandled();
							})
						]
					]
					
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SBox)
						.MinDesiredWidth(48.0f)
						.MinDesiredHeight(48.0f)
						[
							SAssignNew(VoiceInputButton, SButton)
							.ButtonStyle(FAppStyle::Get(), "FlatButton.Default")
							.ForegroundColor(FSlateColor::UseForeground())
							.ContentPadding(FMargin(12.0f, 0.0f))
							.OnClicked(this, &SUnrealGPTWidget::OnVoiceInputClicked)
							[
								SNew(STextBlock)
								.Font(FAppStyle::Get().GetFontStyle("FontAwesome.14"))
								.Text_Lambda([this]() -> FText
								{
									return FText::FromString(VoiceInput && VoiceInput->IsRecording() 
										? FString(TEXT("\xf130")) // Stop icon when recording
										: FString(TEXT("\xf130"))); // Microphone icon
								})
								.ColorAndOpacity_Lambda([this]() -> FSlateColor
								{
									return VoiceInput && VoiceInput->IsRecording() 
										? FSlateColor(FLinearColor(1.0f, 0.3f, 0.3f, 1.0f)) // Red when recording
										: FSlateColor(FLinearColor(0.8f, 0.8f, 0.8f, 1.0f)); // Gray when not recording
								})
							]
						]
					]

					// Attach-image button (paperclip icon) to send local images with the next message
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SBox)
						.MinDesiredWidth(48.0f)
						.MinDesiredHeight(48.0f)
						[
							SNew(SButton)
							.ButtonStyle(FAppStyle::Get(), "FlatButton.Default")
							.ForegroundColor(FSlateColor::UseForeground())
							.ContentPadding(FMargin(12.0f, 0.0f))
							.OnClicked(this, &SUnrealGPTWidget::OnAttachImageClicked)
							[
								SNew(STextBlock)
								.Font(FAppStyle::Get().GetFontStyle("FontAwesome.14"))
								.Text(FText::FromString(FString(TEXT("\xf0c6")))) // Paperclip icon
								.ColorAndOpacity(FLinearColor(0.8f, 0.8f, 0.8f, 1.0f))
							]
						]
					]
					
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SBox)
						.MinDesiredWidth(100.0f)
						.MinDesiredHeight(48.0f)
						[
							SAssignNew(SendButton, SButton)
							.ButtonStyle(FAppStyle::Get(), "FlatButton.Success")
							.ForegroundColor(FSlateColor::UseForeground())
							.ContentPadding(FMargin(20.0f, 0.0f))
							.OnClicked(this, &SUnrealGPTWidget::OnSendClicked)
							.IsEnabled(this, &SUnrealGPTWidget::IsSendEnabled)
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								.Padding(0.0f, 0.0f, 8.0f, 0.0f)
								[
									SNew(STextBlock)
									.Font(FAppStyle::Get().GetFontStyle("FontAwesome.12"))
									.Text(FText::FromString(FString(TEXT("\xf1d8")))) // Paper plane icon
									.ColorAndOpacity(FLinearColor::White)
								]
								+ SHorizontalBox::Slot()
								.VAlign(VAlign_Center)
								[
									SNew(STextBlock)
									.Text(NSLOCTEXT("UnrealGPT", "Send", "Send"))
									.Font(FAppStyle::GetFontStyle("NormalFontBold"))
									.ColorAndOpacity(FLinearColor::White)
								]
							]
						]
					]

					// Small status label showing how many images are attached to the next message
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
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
						.ColorAndOpacity(FLinearColor(0.7f, 0.8f, 1.0f, 1.0f))
					]
				]
			]
		]
	];
}

FLinearColor SUnrealGPTWidget::GetRoleColor(const FString& Role) const
{
	if (Role == TEXT("user"))
	{
		return FLinearColor(0.2f, 0.6f, 0.9f, 1.0f); // Blue
	}
	else if (Role == TEXT("assistant"))
	{
		return FLinearColor(0.4f, 0.8f, 0.4f, 0.0f); // Green
	}
	else if (Role == TEXT("system"))
	{
		return FLinearColor(0.8f, 0.6f, 0.2f, 1.0f); // Orange
	}
	return FLinearColor(0.5f, 0.5f, 0.5f, 1.0f); // Gray
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
					.ColorAndOpacity(FLinearColor(0.9f, 0.9f, 0.9f, 1.0f))
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
					.Font(GetUnrealGPTBodyBoldFont())
					.ColorAndOpacity(FLinearColor(0.96f, 0.96f, 0.96f, 1.0f))
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
					.Font(GetUnrealGPTBodyBoldFont())
					.ColorAndOpacity(FLinearColor(0.96f, 0.96f, 0.96f, 1.0f))
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
					.Font(GetUnrealGPTBodyBoldFont())
					.ColorAndOpacity(FLinearColor(0.98f, 0.98f, 0.98f, 1.0f))
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
						.Font(GetUnrealGPTBodyFont())
						.ColorAndOpacity(FLinearColor(0.95f, 0.95f, 0.95f, 1.0f))
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Top)
					[
						SNew(STextBlock)
						.Text(FText::FromString(ItemText))
						.AutoWrapText(true)
						.Font(GetUnrealGPTBodyFont())
						.ColorAndOpacity(FLinearColor(0.95f, 0.95f, 0.95f, 1.0f))
					]
				];

			continue;
		}

		// Default paragraph line
		Container->AddSlot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(Line))
				.AutoWrapText(true)
				.Font(GetUnrealGPTBodyFont())
				.ColorAndOpacity(FLinearColor(0.95f, 0.95f, 0.95f, 1.0f))
			];
	}

	return Container;
}

TSharedRef<SWidget> SUnrealGPTWidget::CreateMessageWidget(const FString& Role, const FString& Content)
{
	const bool bIsUser = Role == TEXT("user");
	const FLinearColor RoleColor = GetRoleColor(Role);
	const FLinearColor BackgroundColor = bIsUser 
		? FLinearColor(0.08f, 0.12f, 0.18f, 1.0f)
		: FLinearColor(0.06f, 0.1f, 0.08f, 1.0f);
	
	FString RoleIcon;
	if (bIsUser)
	{
		RoleIcon = FString(TEXT("\xf007")); // User icon
	}
	else
	{
		RoleIcon = FString(TEXT("\xf121")); // Code/Bot icon
	}
	
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.White"))
		.BorderBackgroundColor(BackgroundColor)
		.Padding(FMargin(16.0f, 12.0f))
		[
			SNew(SHorizontalBox)
			
			// Role icon
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 12.0f, 0.0f)
			[
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
						.ColorAndOpacity(FLinearColor::White)
						.Justification(ETextJustify::Center)
					]
				]
			]
			
			// Message content
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SVerticalBox)
				
				// Role header
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(bIsUser ? TEXT("You") : TEXT("UnrealGPT Assistant")))
					.Font(GetUnrealGPTBodyBoldFont())
					.ColorAndOpacity(RoleColor)
				]
				
				// Content with markdown
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					CreateMarkdownWidget(Content)
				]
			]
		];
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
		ToolColor = FLinearColor(0.2f, 0.5f, 0.8f, 1.0f);
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
				.Font(GetUnrealGPTSmallBodyFont())
				.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f, 1.0f))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				.Padding(FMargin(8.0f))
				.BorderBackgroundColor(FLinearColor(0.05f, 0.05f, 0.05f, 1.0f))
				[
					SNew(STextBlock)
					.Text(FText::FromString(Code.TrimStartAndEnd()))
					.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
					.ColorAndOpacity(FLinearColor(0.9f, 0.9f, 0.9f, 1.0f))
					.AutoWrapText(true)
				]
			];
	}
	else if (ToolName == TEXT("scene_query"))
	{
		ToolColor = FLinearColor(0.6f, 0.4f, 0.9f, 1.0f);
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
				.Font(GetUnrealGPTSmallBodyFont())
				.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f, 1.0f))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FilterSummary.TrimEnd()))
				.Font(GetUnrealGPTBodyFont())
				.ColorAndOpacity(FLinearColor(0.9f, 0.9f, 0.9f, 1.0f))
				.AutoWrapText(true)
			];
	}
	else if (ToolName == TEXT("viewport_screenshot"))
	{
		ToolColor = FLinearColor(0.3f, 0.8f, 0.6f, 1.0f);
		ToolIcon = FString(TEXT("\xf030")); // Camera icon
		ToolDisplayName = TEXT("Viewport Screenshot");
		
		ContentWidget = SNew(STextBlock)
			.Text(NSLOCTEXT("UnrealGPT", "ScreenshotMsg", "Capturing current viewport state..."))
			.Font(GetUnrealGPTSmallBodyItalicFont())
			.ColorAndOpacity(FLinearColor(0.8f, 0.8f, 0.8f, 1.0f));
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
	// 			.Font(GetUnrealGPTSmallBodyFont())
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
		ToolColor = FLinearColor(0.2f, 0.7f, 0.9f, 1.0f); // Cyan-ish blue
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
				.Font(GetUnrealGPTSmallBodyFont())
				.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f, 1.0f))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Query))
				.Font(GetUnrealGPTBodyItalicFont())
				.ColorAndOpacity(FLinearColor(0.9f, 0.9f, 0.9f, 1.0f))
				.AutoWrapText(true)
			];
	}
	else if (ToolName == TEXT("file_search"))
	{
		ToolColor = FLinearColor(0.8f, 0.6f, 0.2f, 1.0f); // Amber/Orange
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
				.Font(GetUnrealGPTSmallBodyFont())
				.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f, 1.0f))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Query))
				.Font(GetUnrealGPTBodyItalicFont())
				.ColorAndOpacity(FLinearColor(0.9f, 0.9f, 0.9f, 1.0f))
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
				.Font(GetUnrealGPTSmallBodyFont())
				.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f, 1.0f))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Arguments.Left(200) + (Arguments.Len() > 200 ? TEXT("...") : TEXT(""))))
				.AutoWrapText(true)
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
				.ColorAndOpacity(FLinearColor(0.8f, 0.8f, 0.8f, 1.0f))
			];
	}
	
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.White"))
		.BorderBackgroundColor(FLinearColor(0.04f, 0.04f, 0.04f, 1.0f))
		.Padding(FMargin(12.0f, 10.0f))
		[
			SNew(SVerticalBox)
			
			// Tool header with icon
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SHorizontalBox)
				
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 10.0f, 0.0f)
				[
					SNew(SBox)
					.WidthOverride(32.0f)
					.HeightOverride(32.0f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("Brushes.White"))
						.BorderBackgroundColor(ToolColor)
						.Padding(0.0f)
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Font(FAppStyle::Get().GetFontStyle("FontAwesome.14"))
							.Text(FText::FromString(ToolIcon))
							.ColorAndOpacity(FLinearColor::White)
						]
					]
				]
				
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(ToolDisplayName))
					.Font(GetUnrealGPTBodyBoldFont())
					.ColorAndOpacity(ToolColor)
				]
				
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(NSLOCTEXT("UnrealGPT", "ToolExecuted", "Executed"))
					.Font(FAppStyle::GetFontStyle("SmallFont"))
					.ColorAndOpacity(FLinearColor(0.5f, 0.5f, 0.5f, 1.0f))
				]
			]
			
			// Content Section
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(42.0f, 0.0f, 0.0f, 8.0f)
			[
				ContentWidget.ToSharedRef()
			]
		];
}

TSharedRef<SWidget> SUnrealGPTWidget::CreateToolCallWidget(const FString& ToolName, const FString& Arguments, const FString& Result)
{
	return CreateToolSpecificWidget(ToolName, Arguments, Result);
}

FReply SUnrealGPTWidget::OnSendClicked()
{
	if (!InputTextBox.IsValid() || !AgentClient)
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

		ChatHistoryBox->AddSlot()
			.Padding(5.0f)
			[
				CreateMessageWidget(TEXT("user"), DisplayMessage)
			];
	}

	// Clear input
	InputTextBox->SetText(FText::GetEmpty());

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

	// Also clear any pending attachments
	PendingAttachedImages.Empty();

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

	TArray<uint8> FileData;
	if (!FFileHelper::LoadFileToArray(FileData, *OutFiles[0]))
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Failed to read image file: %s"), *OutFiles[0]);
		return FReply::Handled();
	}

	// Encode raw bytes as base64; the agent client will wrap as data:image/... for OpenAI.
	const FString Base64Image = FBase64::Encode(FileData);
	if (!Base64Image.IsEmpty())
	{
		PendingAttachedImages.Add(Base64Image);

		// Refresh send button enabled state
		if (SendButton.IsValid())
		{
			SendButton->Invalidate(EInvalidateWidget::LayoutAndVolatility);
		}
	}

	return FReply::Handled();
}

bool SUnrealGPTWidget::IsSendEnabled() const
{
	if (!InputTextBox.IsValid())
	{
		return false;
	}

	FString Text = InputTextBox->GetText().ToString();
	return !Text.IsEmpty() || PendingAttachedImages.Num() > 0;
}

void SUnrealGPTWidget::HandleAgentMessage(const FString& Role, const FString& Content, const TArray<FString>& ToolCalls)
{
	if (ChatHistoryBox.IsValid())
	{
		ChatHistoryBox->AddSlot()
			.Padding(5.0f)
			[
				CreateMessageWidget(Role, Content)
			];
	}

	// When we receive a plain assistant message (no tool calls), the agent has finished
	// its current step, so we can hide the thinking UI.
	if (Role == TEXT("assistant") && ToolCalls.Num() == 0)
	{
		if (ReasoningStatusBorder.IsValid())
		{
			ReasoningStatusBorder->SetVisibility(EVisibility::Collapsed);
		}
		if (ReasoningSummaryText.IsValid())
		{
			ReasoningSummaryText->SetText(FText::GetEmpty());
		}
	}
}

void SUnrealGPTWidget::HandleAgentReasoning(const FString& ReasoningContent)
{
	if (ReasoningContent.IsEmpty())
	{
		return;
	}

	// Ensure the reasoning strip is visible whenever we receive reasoning content or a summary
	if (ReasoningStatusBorder.IsValid())
	{
		ReasoningStatusBorder->SetVisibility(EVisibility::Visible);
	}

	if (ReasoningSummaryText.IsValid())
	{
		ReasoningSummaryText->SetText(FText::FromString(ReasoningContent));
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
		ChatHistoryBox->AddSlot()
			.Padding(12.0f, 6.0f)
			[
				CreateToolSpecificWidget(ToolName, Arguments, TEXT(""))
			];
		
		// Scroll to bottom to show new tool call
		ChatHistoryBox->ScrollToEnd();
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
								
								// Add screenshot widget to chat
								if (ChatHistoryBox.IsValid())
								{
									ChatHistoryBox->AddSlot()
										.Padding(12.0f, 6.0f)
										[
											SNew(SBorder)
											.BorderImage(FAppStyle::GetBrush("Brushes.White"))
											.BorderBackgroundColor(FLinearColor(0.05f, 0.05f, 0.06f, 1.0f))
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
													.ColorAndOpacity(FLinearColor(0.3f, 0.8f, 0.6f, 1.0f))
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
											]
										];
								}
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
		ChatHistoryBox->AddSlot()
			.Padding(12.0f, 6.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.White"))
				.BorderBackgroundColor(FLinearColor(0.05f, 0.05f, 0.06f, 1.0f))
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
							.BorderBackgroundColor(bIsSceneQueryResult ? FLinearColor(0.6f, 0.4f, 0.9f, 1.0f) : 
								(bIsScreenshot ? FLinearColor(0.3f, 0.8f, 0.6f, 1.0f) : FLinearColor(0.3f, 0.7f, 0.3f, 1.0f)))
							.Padding(0.0f)
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Font(FAppStyle::Get().GetFontStyle("FontAwesome.12"))
								.Text(FText::FromString(bIsScreenshot ? FString(TEXT("\xf030")) : FString(TEXT("\xf00c")))) // Camera or Check icon
								.ColorAndOpacity(FLinearColor::White)
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
							.Font(GetUnrealGPTSmallBodyFont())
							.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.6f, 1.0f))
						]
						
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(STextBlock)
							.Text(FText::FromString(DisplayText))
							.AutoWrapText(true)
							.Font(bIsSceneQueryResult ? GetUnrealGPTBodyFont() : FCoreStyle::GetDefaultFontStyle("Mono", 8))
							.ColorAndOpacity(FLinearColor(0.9f, 0.9f, 0.9f, 1.0f))
						]
					]
				]
			];
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
		// OnSendClicked();
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

