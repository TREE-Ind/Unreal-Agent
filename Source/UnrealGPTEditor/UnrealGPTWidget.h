// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableViewBase.h"
#include "Widgets/Views/STableRow.h"
#include "Http.h"
#include "Styling/SlateTypes.h"
#include "UnrealGPTAgentClient.h"
#include "UnrealGPTClarifyTypes.h"

// Forward declaration from Slate (declared as struct in Engine headers)
struct FSlateBrush;

class SUnrealGPTWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUnrealGPTWidget) {}
	SLATE_END_ARGS()

	virtual ~SUnrealGPTWidget();

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	virtual FReply OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual FReply OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual void OnDragLeave(const FDragDropEvent& DragDropEvent) override;

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

	/** Create interactive clarify tool widget */
	TSharedRef<SWidget> CreateClarifyTurnWidget(const FString& ToolCallId, const FString& Arguments);

	/** Role accent for icons and labels (theme-aware). */
	FSlateColor GetRoleColor(const FString& Role) const;

	/** Get icon brush for tool name */
	const FSlateBrush* GetToolIcon(const FString& ToolName) const;

	/** Handle send/stop button clicked - toggles based on agent state */
	FReply OnSendOrStopClicked();

	/** Handle request context button clicked */
	FReply OnRequestContextClicked();

	/** Handle clear history button clicked */
	FReply OnClearHistoryClicked();

	/** Handle settings button clicked */
	FReply OnSettingsClicked();

	/** Handle Codex device-login button clicked */
	FReply OnCodexLoginClicked();

	/** Handle voice input button clicked */
	FReply OnVoiceInputClicked();

	/** Handle attach-image button clicked */
	FReply OnAttachImageClicked();

	/** Add assets dropped from the Content Browser. */
	void AddAttachedAssets(const TArray<FAssetData>& Assets);

	/** Remove a pending attached asset by index. */
	FReply OnRemoveAttachedAsset(int32 AssetIndex);

	/** Rebuild attachment preview chips (images + assets). */
	void RefreshAttachmentPreviews();

	/** Build composer attachment summary text. */
	FText GetAttachmentSummaryText() const;

	/** Whether any attachments are pending for the next message. */
	bool HasPendingAttachments() const;

	/** Handle transcription complete */
	void OnTranscriptionComplete(const FString& TranscribedText);

	/** Handle recording started */
	void OnRecordingStarted();

	/** Handle recording stopped */
	void OnRecordingStopped();

	/** Check if send/stop button should be enabled */
	bool IsSendOrStopEnabled() const;

	/** Set agent running state and refresh button */
	void SetAgentRunning(bool bRunning);

	/** Handle agent message delegate - called from agent client */
	void HandleAgentMessage(const FString& Role, const FString& Content, const TArray<FString>& ToolCalls);

	/** Handle agent reasoning delegate - called from agent client */
	void HandleAgentReasoning(const FString& ReasoningContent);

	/** Handle tool call delegate - called from agent client */
	void HandleToolCall(const FString& ToolCallId, const FString& ToolName, const FString& Arguments);

	/** Handle tool result delegate - called from agent client */
	void HandleToolResult(const FString& ToolCallId, const FString& Result);

	void AppendSystemMessage(const FString& Content);

	/** Adds a widget to the conversation scroll and updates empty-state chrome. */
	void AddConversationTurnWidget(const TSharedRef<SWidget>& Content, const FMargin& SlotPadding = FMargin(0.f, 0.f, 0.f, 12.f));

	void SyncEmptyThreadVisibility();

	void ResetCodexLoginProcess();
	void StartCodexDeviceAuth();
	void OnCodexUserCodeResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	void PollCodexDeviceAuthToken();
	void OnCodexDeviceTokenResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	void ExchangeCodexAuthorizationCode(const FString& AuthorizationCode, const FString& CodeVerifier);
	void OnCodexTokenExchangeResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	bool PersistCodexAuth(const FString& IdToken, const FString& AccessToken, const FString& RefreshToken, FString& OutError) const;
	bool IsCodexLoggedIn() const;
	FText GetCodexLoginButtonText() const;
	FText GetCodexLoginButtonTooltip() const;
	bool IsCodexLoginButtonEnabled() const;

	/** Agent client instance */
	UPROPERTY()
	UUnrealGPTAgentClient* AgentClient;

	/** Delegate handler for dynamic delegates */
	UPROPERTY()
	class UUnrealGPTWidgetDelegateHandler* DelegateHandler;

	/** Chat history scroll box */
	TSharedPtr<class SScrollBox> ChatHistoryBox;

	/** Number of primary turns in the thread (each AddConversationTurnWidget increments). */
	int32 ChatConversationBlockCount = 0;

	/** Centered onboarding when the thread has no turns. */
	TSharedPtr<class SBorder> EmptyThreadChrome;

	/** Input text box */
	TSharedPtr<SMultiLineEditableTextBox> InputTextBox;

	/** Send button (visible when agent is idle) */
	TSharedPtr<SButton> SendStopButton;

	/** Whether the agent is currently processing a request */
	bool bAgentIsRunning = false;

	/** Request context button */
	TSharedPtr<SButton> RequestContextButton;

	/** Clear history button */
	TSharedPtr<SButton> ClearHistoryButton;

	/** Settings button */
	TSharedPtr<SButton> SettingsButton;

	/** Codex login button */
	TSharedPtr<SButton> CodexLoginButton;

	TSharedPtr<IHttpRequest> CodexLoginRequest;
	FString CodexDeviceAuthId;
	FString CodexDeviceUserCode;
	double CodexNextPollTime = 0.0;
	double CodexLoginStartedAt = 0.0;
	int32 CodexPollIntervalSeconds = 5;
	bool bCodexDeviceAuthInProgress = false;

	/** Voice input button */
	TSharedPtr<SButton> VoiceInputButton;

	/** Screenshot preview image */
	TSharedPtr<class SImage> ScreenshotPreview;

	/** Voice input instance */
	UPROPERTY()
	class UUnrealGPTVoiceInput* VoiceInput;

	/** Tool call list */
	TArray<FString> ToolCallHistory;

	/** Parsed clarify requests keyed by tool call id for result summaries */
	TMap<FString, FClarifyRequest> ClarifyRequestsByToolCallId;

	/** Pending images attached by the user (base64-encoded) to be sent with the next message */
	TArray<FString> PendingAttachedImages;

	/** Pending UE assets attached via Content Browser drag-drop */
	TArray<FAssetData> PendingAttachedAssets;

	/** Thumbnail pool for asset attachment chips */
	TSharedPtr<class FAssetThumbnailPool> AssetThumbnailPool;

	/** Keep asset thumbnails alive for Slate */
	TArray<TSharedPtr<class FAssetThumbnail>> AssetThumbnailWidgets;

	/** Thumbnail preview container for any images attached to the next message */
	TSharedPtr<class SWrapBox> AttachmentPreviewBox;

	/** Composer border used for drag-over feedback */
	TSharedPtr<class SBorder> ComposerInputBorder;

	/** True while a valid asset drag is hovering the widget */
	bool bAssetDragActive = false;

	/** Brushes backing attached-image thumbnails so Slate never references freed memory */
	TArray<TSharedPtr<FSlateBrush>> AttachmentBrushes;

	/** Persistent brushes for screenshots to ensure Slate does not reference freed memory */
	TArray<TSharedPtr<FSlateBrush>> ScreenshotBrushes;

	/** Session status chip in the harness header (Idle / Running). */
	TSharedPtr<STextBlock> SessionStatusLabel;

	/** MCP connection summary shown when servers are configured. */
	TSharedPtr<STextBlock> McpStatusLabel;

	/** Compact, dynamic area that shows when the agent is reasoning and its reasoning summary */
	TSharedPtr<class SBorder> ReasoningStatusBorder;

	/** Text block used to display the latest reasoning summary from the agent */
	TSharedPtr<class STextBlock> ReasoningSummaryText;
};

