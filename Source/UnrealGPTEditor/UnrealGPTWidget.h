#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableViewBase.h"
#include "Widgets/Views/STableRow.h"
#include "UnrealGPTAgentClient.h"

// Forward declaration from Slate (declared as struct in Engine headers)
struct FSlateBrush;

class SUnrealGPTWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUnrealGPTWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	friend class UUnrealGPTWidgetDelegateHandler;

private:
	/** Create chat message widget */
	TSharedRef<SWidget> CreateMessageWidget(const FString& Role, const FString& Content);

	/** Create tool call widget */
	TSharedRef<SWidget> CreateToolCallWidget(const FString& ToolName, const FString& Arguments, const FString& Result);

	/** Parse markdown to rich text for better message display */
	TSharedRef<SWidget> CreateMarkdownWidget(const FString& Content);

	/** Create specialized widget for specific tool types */
	TSharedRef<SWidget> CreateToolSpecificWidget(const FString& ToolName, const FString& Arguments, const FString& Result);

	/** Get color scheme for message role */
	FLinearColor GetRoleColor(const FString& Role) const;

	/** Get icon brush for tool name */
	const FSlateBrush* GetToolIcon(const FString& ToolName) const;

	/** Handle send button clicked */
	FReply OnSendClicked();

	/** Handle request context button clicked */
	FReply OnRequestContextClicked();

	/** Handle clear history button clicked */
	FReply OnClearHistoryClicked();

	/** Handle settings button clicked */
	FReply OnSettingsClicked();

	/** Handle voice input button clicked */
	FReply OnVoiceInputClicked();

	/** Handle attach-image button clicked */
	FReply OnAttachImageClicked();

	/** Handle transcription complete */
	void OnTranscriptionComplete(const FString& TranscribedText);

	/** Handle recording started */
	void OnRecordingStarted();

	/** Handle recording stopped */
	void OnRecordingStopped();

	/** Check if send button should be enabled */
	bool IsSendEnabled() const;

	/** Handle agent message delegate - called from agent client */
	void HandleAgentMessage(const FString& Role, const FString& Content, const TArray<FString>& ToolCalls);

	/** Handle agent reasoning delegate - called from agent client */
	void HandleAgentReasoning(const FString& ReasoningContent);

	/** Handle tool call delegate - called from agent client */
	void HandleToolCall(const FString& ToolName, const FString& Arguments);

	/** Handle tool result delegate - called from agent client */
	void HandleToolResult(const FString& ToolCallId, const FString& Result);

	/** Agent client instance */
	UPROPERTY()
	UUnrealGPTAgentClient* AgentClient;

	/** Delegate handler for dynamic delegates */
	UPROPERTY()
	class UUnrealGPTWidgetDelegateHandler* DelegateHandler;

	/** Chat history scroll box */
	TSharedPtr<class SScrollBox> ChatHistoryBox;

	/** Input text box */
	TSharedPtr<SMultiLineEditableTextBox> InputTextBox;

	/** Send button */
	TSharedPtr<SButton> SendButton;

	/** Request context button */
	TSharedPtr<SButton> RequestContextButton;

	/** Clear history button */
	TSharedPtr<SButton> ClearHistoryButton;

	/** Settings button */
	TSharedPtr<SButton> SettingsButton;

	/** Voice input button */
	TSharedPtr<SButton> VoiceInputButton;

	/** Screenshot preview image */
	TSharedPtr<class SImage> ScreenshotPreview;

	/** Voice input instance */
	UPROPERTY()
	class UUnrealGPTVoiceInput* VoiceInput;

	/** Tool call list */
	TArray<FString> ToolCallHistory;

	/** Pending images attached by the user (base64-encoded) to be sent with the next message */
	TArray<FString> PendingAttachedImages;

	/** Persistent brushes for screenshots to ensure Slate does not reference freed memory */
	TArray<TSharedPtr<FSlateBrush>> ScreenshotBrushes;

	/** Compact, dynamic area that shows when the agent is reasoning and its reasoning summary */
	TSharedPtr<class SBorder> ReasoningStatusBorder;

	/** Text block used to display the latest reasoning summary from the agent */
	TSharedPtr<class STextBlock> ReasoningSummaryText;
};

