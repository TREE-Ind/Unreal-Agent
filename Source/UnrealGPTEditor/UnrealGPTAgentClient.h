#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Http.h"
#include "Dom/JsonObject.h"
#include "UnrealGPTAgentClient.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnAgentMessage, const FString&, Role, const FString&, Content, const TArray<FString>&, ToolCalls);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAgentReasoning, const FString&, ReasoningContent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnToolCall, const FString&, ToolName, const FString&, Arguments);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnToolResult, const FString&, ToolCallId, const FString&, Result);

USTRUCT()
struct FAgentMessage
{
	GENERATED_BODY()

	UPROPERTY()
	FString Role; // "user", "assistant", "system", "tool"

	UPROPERTY()
	FString Content;

	UPROPERTY()
	TArray<FString> ToolCallIds; // For assistant messages with tool_calls, stores the tool call IDs.

	UPROPERTY()
	FString ToolCallId; // For tool messages, the specific tool_call_id

	UPROPERTY()
	FString ToolCallsJson; // For assistant messages, stores the tool_calls array as JSON string
};

USTRUCT()
struct FToolDefinition
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Description;

	UPROPERTY()
	FString ParametersSchema; // JSON schema as string
};

UCLASS()
class UNREALGPTEDITOR_API UUnrealGPTAgentClient : public UObject
{
	GENERATED_BODY()

public:
	UUnrealGPTAgentClient();

	/** Initialize the agent client with settings */
	void Initialize();

	/** Send a message to the agent and get response */
	void SendMessage(const FString& UserMessage, const TArray<FString>& ImageBase64 = TArray<FString>());

	/** Cancel current request */
	void CancelRequest();

	/** Get conversation history */
	TArray<FAgentMessage> GetConversationHistory() const { return ConversationHistory; }

	/** Clear conversation history */
	void ClearHistory();

	/** Delegate for agent messages */
	UPROPERTY(BlueprintAssignable)
	FOnAgentMessage OnAgentMessage;

	/** Delegate for reasoning updates */
	UPROPERTY(BlueprintAssignable)
	FOnAgentReasoning OnAgentReasoning;

	/** Delegate for tool calls */
	UPROPERTY(BlueprintAssignable)
	FOnToolCall OnToolCall;

	/** Delegate for tool results */
	UPROPERTY(BlueprintAssignable)
	FOnToolResult OnToolResult;

private:
	/** Build tool definitions array */
	TArray<TSharedPtr<FJsonObject>> BuildToolDefinitions();

	/** Handle HTTP response */
	void OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

	/** Process streaming response */
	void ProcessStreamingResponse(const FString& ResponseContent);

	/** Process standard JSON response from Responses API (non-streaming) */
	void ProcessResponsesApiResponse(const FString& ResponseContent);

	/** Execute a tool call */
	FString ExecuteToolCall(const FString& ToolName, const FString& ArgumentsJson);

	/** Execute Python code */
	FString ExecutePythonCode(const FString& Code);

	// /** Execute Computer Use action */
	// FString ExecuteComputerUse(const FString& ActionJson);

	/** Get viewport screenshot */
	FString GetViewportScreenshot();

	/** Get scene summary */
	FString GetSceneSummary(int32 PageSize = 100);

	/** Focus viewport on the last created asset/actor */
	void FocusViewportOnCreatedAsset(const FString& ResultJson);

	/** Create HTTP request with proper headers */
	TSharedRef<IHttpRequest> CreateHttpRequest();

	/** Get the effective API URL, applying base URL override if set */
	FString GetEffectiveApiUrl() const;

	/** Check if we're using the Responses API endpoint */
	bool IsUsingResponsesApi() const;

	/** Detect if task completion can be inferred from recent tool results */
	bool DetectTaskCompletion(const TArray<FString>& ToolNames, const TArray<FString>& ToolResults) const;

	/** Current HTTP request */
	TSharedPtr<IHttpRequest> CurrentRequest;

	/** Conversation history */
	TArray<FAgentMessage> ConversationHistory;

	/** Previous response ID for Responses API state management */
	FString PreviousResponseId;

	/** Tool call iteration counter to prevent infinite loops */
	int32 ToolCallIterationCount;

	/** Maximum tool call iterations before stopping.
	 *  Keep this relatively low so the agent cannot get stuck retrying the
	 *  same step over and over after code execution.
	 */
	static constexpr int32 MaxToolCallIterations = 25;

	/** Maximum size (in characters) for tool results to include in conversation history and API requests.
	 *  Large results (like base64 screenshots) will be truncated or summarized to prevent
	 *  context window overflow. This is critical for cost control.
	 */
	static constexpr int32 MaxToolResultSize = 10000; // ~10KB

	/** Signatures of tool calls that have already been executed in this conversation.
	 *  Used to avoid re-running identical python_execute calls in a loop.
	 */
	TSet<FString> ExecutedToolCallSignatures;

	/** Tracks whether the last executed tool was python_execute.
	 *  Used to avoid blindly running python_execute multiple times in a row;
	 *  the agent should instead inspect the scene with scene_query or
	 *  viewport_screenshot before deciding on further code changes.
	 */
	bool bLastToolWasPythonExecute;

	/** Tracks whether the last tool was scene_query and it returned results (non-empty array).
	 *  If true, the next python_execute call should be blocked to prevent loops after
	 *  verification confirms the task is complete.
	 */
	bool bLastSceneQueryFoundResults;

	/** Settings reference */
	class UUnrealGPTSettings* Settings;

	/** Is request in progress */
	bool bRequestInProgress;

	/** Whether we should include reasoning.summary in requests (disabled if the org is not verified). */
	bool bAllowReasoningSummary = true;

	/** Cached copy of the last JSON request body, used for safe retry on specific API errors. */
	FString LastRequestBody;
};

