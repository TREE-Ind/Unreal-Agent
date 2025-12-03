#include "UnrealGPTWidgetDelegateHandler.h"
#include "UnrealGPTWidget.h"
#include "UnrealGPTVoiceInput.h"

void UUnrealGPTWidgetDelegateHandler::Initialize(SUnrealGPTWidget* InWidget)
{
	// Prevent this handler from being garbage collected while the widget is alive.
	// The Slate widget only holds a raw pointer (not a UPROPERTY), so we must root
	// this object to ensure delegate calls remain valid.
	if (!IsRooted())
	{
		AddToRoot();
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: WidgetDelegateHandler added to root to prevent GC"));
	}

	Widget = InWidget;
}

void UUnrealGPTWidgetDelegateHandler::OnAgentMessageReceived(const FString& Role, const FString& Content, const TArray<FString>& ToolCalls)
{
	if (Widget)
	{
		Widget->HandleAgentMessage(Role, Content, ToolCalls);
	}
}

void UUnrealGPTWidgetDelegateHandler::OnAgentReasoningReceived(const FString& ReasoningContent)
{
	if (Widget)
	{
		Widget->HandleAgentReasoning(ReasoningContent);
	}
}

void UUnrealGPTWidgetDelegateHandler::OnToolCallReceived(const FString& ToolName, const FString& Arguments)
{
	if (Widget)
	{
		Widget->HandleToolCall(ToolName, Arguments);
	}
}

void UUnrealGPTWidgetDelegateHandler::OnToolResultReceived(const FString& ToolCallId, const FString& Result)
{
	if (Widget)
	{
		Widget->HandleToolResult(ToolCallId, Result);
	}
}

void UUnrealGPTWidgetDelegateHandler::OnTranscriptionCompleteReceived(const FString& TranscribedText)
{
	if (Widget)
	{
		Widget->OnTranscriptionComplete(TranscribedText);
	}
}

void UUnrealGPTWidgetDelegateHandler::OnRecordingStartedReceived()
{
	if (Widget)
	{
		Widget->OnRecordingStarted();
	}
}

void UUnrealGPTWidgetDelegateHandler::OnRecordingStoppedReceived()
{
	if (Widget)
	{
		Widget->OnRecordingStopped();
	}
}

