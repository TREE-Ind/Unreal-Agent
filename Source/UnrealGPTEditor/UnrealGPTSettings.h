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

	/** Default model to use (e.g., gpt-5.1) */
	UPROPERTY(config, EditAnywhere, Category = "Model", meta = (DisplayName = "Default Model"))
	FString DefaultModel = TEXT("gpt-5.1");

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

	/** Maximum execution timeout in seconds */
	UPROPERTY(config, EditAnywhere, Category = "Safety", meta = (DisplayName = "Execution Timeout (seconds)"))
	float ExecutionTimeoutSeconds = 30.0f;

	/** Maximum number of consecutive tool call iterations before stopping to prevent infinite loops. */
	UPROPERTY(config, EditAnywhere, Category = "Safety", meta = (DisplayName = "Max Tool Call Iterations", ClampMin = "1", UIMin = "1"))
	int32 MaxToolCallIterations = 25;

	/** Maximum context tokens per request */
	UPROPERTY(config, EditAnywhere, Category = "Context", meta = (DisplayName = "Max Context Tokens"))
	int32 MaxContextTokens = 100000;

	/** Scene summary pagination limit */
	UPROPERTY(config, EditAnywhere, Category = "Context", meta = (DisplayName = "Scene Summary Page Size"))
	int32 SceneSummaryPageSize = 100;

	virtual FName GetCategoryName() const override;
};

