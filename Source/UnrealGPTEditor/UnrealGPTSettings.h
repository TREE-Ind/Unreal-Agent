// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Engine/DeveloperSettings.h"
#include "UnrealGPTSettings.generated.h"

UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "UnrealGPT"))
class UNREALGPTEDITOR_API UUnrealGPTSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UUnrealGPTSettings(const FObjectInitializer& ObjectInitializer);

	/** Base URL override (optional). If set, this will override the base URL portion of API Endpoint. Leave empty to use full API Endpoint URL. */
	UPROPERTY(config, EditAnywhere, Category = "API", meta = (DisplayName = "Base URL Override"))
	FString BaseUrlOverride;

	/** OpenAI-compatible API endpoint URL (use /v1/responses for Responses API or /v1/chat/completions for legacy API) */
	UPROPERTY(config, EditAnywhere, Category = "API", meta = (DisplayName = "API Endpoint"))
	FString ApiEndpoint = TEXT("https://api.openai.com/v1/responses");

	/** API Key for authentication */
	UPROPERTY(config, EditAnywhere, Category = "API", meta = (DisplayName = "API Key", ConfigRestartRequired = false))
	FString ApiKey;

	/** Use the local Codex auth cache instead of the API Key field. Supports API-key and ChatGPT Codex auth.json files. */
	UPROPERTY(config, EditAnywhere, Category = "API", meta = (DisplayName = "Use Codex Auth", ConfigRestartRequired = false))
	bool bUseCodexAuth = false;

	/** Optional path to Codex auth.json. Leave empty to use CODEX_HOME/auth.json or ~/.codex/auth.json. */
	UPROPERTY(config, EditAnywhere, Category = "API", meta = (DisplayName = "Codex Auth File", ConfigRestartRequired = false, EditCondition = "bUseCodexAuth"))
	FString CodexAuthFilePath;

	/** Route Codex ChatGPT auth to ChatGPT's Codex Responses endpoint when using the default OpenAI Responses endpoint. */
	UPROPERTY(config, EditAnywhere, Category = "API", meta = (DisplayName = "Use Codex Responses Endpoint", ConfigRestartRequired = false, EditCondition = "bUseCodexAuth"))
	bool bUseCodexResponsesEndpoint = true;

	/** Default model to use (e.g., gpt-5.1) */
	UPROPERTY(config, EditAnywhere, Category = "Model", meta = (DisplayName = "Default Model"))
	FString DefaultModel = TEXT("gpt-5.1");

	/** Model to use when authenticated with Codex ChatGPT auth and using the Codex Responses endpoint. */
	UPROPERTY(config, EditAnywhere, Category = "Model", meta = (DisplayName = "Codex Model", ConfigRestartRequired = false, EditCondition = "bUseCodexAuth && bUseCodexResponsesEndpoint"))
	FString CodexModel = TEXT("gpt-5.1-codex");

	/** Enable dynamic reasoning effort (auto-adjusts based on message complexity). When disabled, uses the fixed Reasoning Effort setting. */
	UPROPERTY(config, EditAnywhere, Category = "Reasoning", meta = (DisplayName = "Enable Dynamic Reasoning"))
	bool bEnableDynamicReasoning = true;

	/** Fixed reasoning effort level when dynamic reasoning is disabled. Options: low, medium, high */
	UPROPERTY(config, EditAnywhere, Category = "Reasoning", meta = (DisplayName = "Reasoning Effort", EditCondition = "!bEnableDynamicReasoning"))
	FString ReasoningEffort = TEXT("medium");

	/** Enable Python code execution tool */
	UPROPERTY(config, EditAnywhere, Category = "Tools", meta = (DisplayName = "Enable Python Execution"))
	bool bEnablePythonExecution = true;

	// /** Enable Computer Use tool */
	// UPROPERTY(config, EditAnywhere, Category = "Tools", meta = (DisplayName = "Enable Computer Use"))
	// bool bEnableComputerUse = true;

	/** Enable viewport screenshot tool */
	UPROPERTY(config, EditAnywhere, Category = "Tools", meta = (DisplayName = "Enable Viewport Screenshot"))
	bool bEnableViewportScreenshot = true;

	/** Enable scene summary tool */
	UPROPERTY(config, EditAnywhere, Category = "Tools", meta = (DisplayName = "Enable Scene Summary"))
	bool bEnableSceneSummary = true;

	/** Enable built-in Replicate generation tool (direct HTTP integration, no MCP required) */
	UPROPERTY(config, EditAnywhere, Category = "Replicate", meta = (DisplayName = "Enable Replicate Tool"))
	bool bEnableReplicateTool = false;

	/** Replicate API token (use the 'Token' value from your Replicate account) */
	UPROPERTY(config, EditAnywhere, Category = "Replicate", meta = (DisplayName = "Replicate API Token", ConfigRestartRequired = false))
	FString ReplicateApiToken;

	/** Replicate predictions endpoint URL */
	UPROPERTY(config, EditAnywhere, Category = "Replicate", meta = (DisplayName = "Replicate API URL"))
	FString ReplicateApiUrl = TEXT("https://api.replicate.com/v1/predictions");

	/** Default Replicate model for image generation (e.g. 'owner/image-model:version') */
	UPROPERTY(config, EditAnywhere, Category = "Replicate", meta = (DisplayName = "Image Model"))
	FString ReplicateImageModel;

	/** Default Replicate model for 3D asset generation (e.g. 'owner/3d-model:version') */
	UPROPERTY(config, EditAnywhere, Category = "Replicate", meta = (DisplayName = "3D Model"))
	FString Replicate3DModel;

	/** Default Replicate model for sound effects (SFX) generation */
	UPROPERTY(config, EditAnywhere, Category = "Replicate", meta = (DisplayName = "SFX Model"))
	FString ReplicateSFXModel;

	/** Default Replicate model for music generation */
	UPROPERTY(config, EditAnywhere, Category = "Replicate", meta = (DisplayName = "Music Model"))
	FString ReplicateMusicModel;

	/** Default Replicate model for speech / voice generation */
	UPROPERTY(config, EditAnywhere, Category = "Replicate", meta = (DisplayName = "Speech Model"))
	FString ReplicateSpeechModel;

	/** Default Replicate model for video generation */
	UPROPERTY(config, EditAnywhere, Category = "Replicate", meta = (DisplayName = "Video Model"))
	FString ReplicateVideoModel;

	/** Maximum execution timeout in seconds (default 90s for better reasoning model support) */
	UPROPERTY(config, EditAnywhere, Category = "Safety", meta = (DisplayName = "Execution Timeout (seconds)"))
	float ExecutionTimeoutSeconds = 90.0f;

	/** Maximum number of consecutive tool call iterations before stopping to prevent infinite loops. Set to 0 for unlimited. */
	UPROPERTY(config, EditAnywhere, Category = "Safety", meta = (DisplayName = "Max Tool Call Iterations", ClampMin = "0", UIMin = "0"))
	int32 MaxToolCallIterations = 100;

	/** Maximum context tokens per request */
	UPROPERTY(config, EditAnywhere, Category = "Context", meta = (DisplayName = "Max Context Tokens"))
	int32 MaxContextTokens = 100000;

	/** Scene summary pagination limit */
	UPROPERTY(config, EditAnywhere, Category = "Context", meta = (DisplayName = "Scene Summary Page Size"))
	int32 SceneSummaryPageSize = 100;

	/** OpenAI Vector Store ID for file_search tool (UE Python API documentation). Leave empty to use default. */
	UPROPERTY(config, EditAnywhere, Category = "Tools", meta = (DisplayName = "Vector Store ID"))
	FString VectorStoreId = TEXT("vs_691df14e67fc819189353158b9f13942");

	bool ResolveAuthHeaders(FString& OutBearerToken, FString& OutChatGPTAccountId, FString& OutError) const;
	bool IsUsingCodexChatGPTAuth() const;
	bool GetCodexRefreshToken(FString& OutRefreshToken, FString& OutError) const;
	bool SaveCodexChatGPTAuth(const FString& IdToken, const FString& AccessToken, const FString& RefreshToken, FString& OutError) const;
	FString GetResolvedCodexAuthFilePath() const;

	virtual FName GetCategoryName() const override;
};

