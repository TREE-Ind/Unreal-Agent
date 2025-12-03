#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "UnrealGPTAgentClient.h"
#include "UnrealGPTWidgetDelegateHandler.generated.h"

class SUnrealGPTWidget;

UCLASS()
class UNREALGPTEDITOR_API UUnrealGPTWidgetDelegateHandler : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(SUnrealGPTWidget* InWidget);

	UFUNCTION()
	void OnAgentMessageReceived(const FString& Role, const FString& Content, const TArray<FString>& ToolCalls);

	UFUNCTION()
	void OnAgentReasoningReceived(const FString& ReasoningContent);

	UFUNCTION()
	void OnToolCallReceived(const FString& ToolName, const FString& Arguments);

	UFUNCTION()
	void OnToolResultReceived(const FString& ToolCallId, const FString& Result);

	UFUNCTION()
	void OnTranscriptionCompleteReceived(const FString& TranscribedText);

	UFUNCTION()
	void OnRecordingStartedReceived();

	UFUNCTION()
	void OnRecordingStoppedReceived();

private:
	SUnrealGPTWidget* Widget;
};

