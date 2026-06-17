// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Centralized tool schema definitions for UnrealGPT.
 * This class provides static methods to build tool definitions for the OpenAI API.
 */
class UNREALGPTEDITOR_API FUnrealGPTToolSchemas
{
public:
	/** Build a tool object with proper format for either Responses API or legacy Chat Completions API */
	static TSharedPtr<FJsonObject> BuildToolObject(
		const FString& Name,
		const FString& Description,
		const TSharedPtr<FJsonObject>& Parameters,
		bool bUseResponsesApi);

	/** Build python_execute tool schema */
	static TSharedPtr<FJsonObject> BuildPythonExecuteTool(bool bUseResponsesApi);

	/** Build viewport_screenshot tool schema */
	static TSharedPtr<FJsonObject> BuildViewportScreenshotTool(bool bUseResponsesApi);

	/** Build scene_query tool schema */
	static TSharedPtr<FJsonObject> BuildSceneQueryTool(bool bUseResponsesApi);

	/** Build reflection_query tool schema */
	static TSharedPtr<FJsonObject> BuildReflectionQueryTool(bool bUseResponsesApi);

	/** Build replicate_generate tool schema */
	static TSharedPtr<FJsonObject> BuildReplicateGenerateTool(bool bUseResponsesApi);

	// New atomic editor tools
	
	/** Build get_actor tool schema - Get detailed actor info by label/name */
	static TSharedPtr<FJsonObject> BuildGetActorTool(bool bUseResponsesApi);

	/** Build set_actor_transform tool schema - Set location/rotation/scale with transaction support */
	static TSharedPtr<FJsonObject> BuildSetActorTransformTool(bool bUseResponsesApi);

	/** Build select_actors tool schema - Select actors by label array */
	static TSharedPtr<FJsonObject> BuildSelectActorsTool(bool bUseResponsesApi);

	/** Build duplicate_actor tool schema - Clone actor with optional offset */
	static TSharedPtr<FJsonObject> BuildDuplicateActorTool(bool bUseResponsesApi);

	/** Build snap_actor_to_ground tool schema - Line trace snap with normal alignment option */
	static TSharedPtr<FJsonObject> BuildSnapActorToGroundTool(bool bUseResponsesApi);
};

