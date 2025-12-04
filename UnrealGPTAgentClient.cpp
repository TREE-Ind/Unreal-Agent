#include "UnrealGPTAgentClient.h"
#include "UnrealGPTSettings.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Base64.h"
#include "IPythonScriptPlugin.h"
#include "LevelEditor.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Editor/EditorEngine.h"
#include "UnrealGPTSceneContext.h"
// #include "UnrealGPTComputerUse.h" // Computer Use tool disabled
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "Engine/Selection.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Async/Async.h"

namespace
{
	/** Helper: convert an FProperty into a compact JSON description */
	TSharedPtr<FJsonObject> BuildPropertyJson(FProperty* Property)
	{
		if (!Property)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> PropJson = MakeShareable(new FJsonObject);
		PropJson->SetStringField(TEXT("name"), Property->GetName());
		PropJson->SetStringField(TEXT("cpp_type"), Property->GetCPPType(nullptr, 0));
		PropJson->SetStringField(TEXT("ue_type"), Property->GetClass() ? Property->GetClass()->GetName() : TEXT("Unknown"));

		// Basic, high-signal property flags that are relevant for Python/Blueprint use.
		TArray<FString> Flags;
		if (Property->HasAnyPropertyFlags(CPF_Edit))
		{
			Flags.Add(TEXT("Edit"));
		}
		if (Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
		{
			Flags.Add(TEXT("BlueprintVisible"));
		}
		if (Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
		{
			Flags.Add(TEXT("BlueprintReadOnly"));
		}
		if (Property->HasAnyPropertyFlags(CPF_Transient))
		{
			Flags.Add(TEXT("Transient"));
		}
		if (Property->HasAnyPropertyFlags(CPF_Config))
		{
			Flags.Add(TEXT("Config"));
		}

		if (Flags.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> FlagValues;
			for (const FString& Flag : Flags)
			{
				FlagValues.Add(MakeShareable(new FJsonValueString(Flag)));
			}
			PropJson->SetArrayField(TEXT("flags"), FlagValues);
		}

		return PropJson;
	}

	/** Helper: convert a UFunction into a compact JSON description */
	TSharedPtr<FJsonObject> BuildFunctionJson(UFunction* Function)
	{
		if (!Function)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> FuncJson = MakeShareable(new FJsonObject);
		FuncJson->SetStringField(TEXT("name"), Function->GetName());

		// Function flags: only expose the ones that matter for scripting.
		TArray<FString> Flags;
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
		{
			Flags.Add(TEXT("BlueprintCallable"));
		}
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintPure))
		{
			Flags.Add(TEXT("BlueprintPure"));
		}
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
		{
			Flags.Add(TEXT("BlueprintEvent"));
		}
		if (Function->HasAnyFunctionFlags(FUNC_Net))
		{
			Flags.Add(TEXT("Net"));
		}
		if (Function->HasAnyFunctionFlags(FUNC_Static))
		{
			Flags.Add(TEXT("Static"));
		}

		if (Flags.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> FlagValues;
			for (const FString& Flag : Flags)
			{
				FlagValues.Add(MakeShareable(new FJsonValueString(Flag)));
			}
			FuncJson->SetArrayField(TEXT("flags"), FlagValues);
		}

		// Parameters and return type.
		TArray<TSharedPtr<FJsonValue>> ParamsJson;
		TSharedPtr<FJsonObject> ReturnJson;

		for (TFieldIterator<FProperty> ParamIt(Function); ParamIt; ++ParamIt)
		{
			FProperty* ParamProp = *ParamIt;
			if (!ParamProp)
			{
				continue;
			}

			const bool bIsReturn = ParamProp->HasAnyPropertyFlags(CPF_ReturnParm);
			if (bIsReturn)
			{
				ReturnJson = MakeShareable(new FJsonObject);
				ReturnJson->SetStringField(TEXT("name"), ParamProp->GetName());
				ReturnJson->SetStringField(TEXT("cpp_type"), ParamProp->GetCPPType(nullptr, 0));
				ReturnJson->SetStringField(TEXT("ue_type"), ParamProp->GetClass() ? ParamProp->GetClass()->GetName() : TEXT("Unknown"));
				continue;
			}

			if (!ParamProp->HasAnyPropertyFlags(CPF_Parm))
			{
				continue;
			}

			TSharedPtr<FJsonObject> ParamJson = MakeShareable(new FJsonObject);
			ParamJson->SetStringField(TEXT("name"), ParamProp->GetName());
			ParamJson->SetStringField(TEXT("cpp_type"), ParamProp->GetCPPType(nullptr, 0));
			ParamJson->SetStringField(TEXT("ue_type"), ParamProp->GetClass() ? ParamProp->GetClass()->GetName() : TEXT("Unknown"));
			ParamJson->SetBoolField(TEXT("is_out"), ParamProp->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm));

			ParamsJson.Add(MakeShareable(new FJsonValueObject(ParamJson)));
		}

		if (ParamsJson.Num() > 0)
		{
			FuncJson->SetArrayField(TEXT("parameters"), ParamsJson);
		}
		if (ReturnJson.IsValid())
		{
			FuncJson->SetObjectField(TEXT("return"), ReturnJson);
		}

		return FuncJson;
	}

	/** Helper: build a reflection "schema" JSON object for a class */
	FString BuildReflectionSchemaJson(UClass* Class)
	{
		if (!Class)
		{
			TSharedPtr<FJsonObject> ErrorObj = MakeShareable(new FJsonObject);
			ErrorObj->SetStringField(TEXT("status"), TEXT("error"));
			ErrorObj->SetStringField(TEXT("message"), TEXT("Class not found"));

			FString ErrorJson;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ErrorJson);
			FJsonSerializer::Serialize(ErrorObj.ToSharedRef(), Writer);
			return ErrorJson;
		}

		TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
		Root->SetStringField(TEXT("status"), TEXT("ok"));
		Root->SetStringField(TEXT("class_name"), Class->GetName());
		Root->SetStringField(TEXT("path_name"), Class->GetPathName());
		Root->SetStringField(TEXT("cpp_type"), FString::Printf(TEXT("%s*"), *Class->GetName()));

		// Properties
		TArray<TSharedPtr<FJsonValue>> PropertiesJson;
		for (TFieldIterator<FProperty> PropIt(Class, EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;
			TSharedPtr<FJsonObject> PropJson = BuildPropertyJson(Property);
			if (PropJson.IsValid())
			{
				PropertiesJson.Add(MakeShareable(new FJsonValueObject(PropJson)));
			}
		}
		Root->SetArrayField(TEXT("properties"), PropertiesJson);

		// Functions
		TArray<TSharedPtr<FJsonValue>> FunctionsJson;
		for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
		{
			UFunction* Function = *FuncIt;
			TSharedPtr<FJsonObject> FuncJson = BuildFunctionJson(Function);
			if (FuncJson.IsValid())
			{
				FunctionsJson.Add(MakeShareable(new FJsonValueObject(FuncJson)));
			}
		}
		Root->SetArrayField(TEXT("functions"), FunctionsJson);

		FString OutJson;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return OutJson;
	}
}

UUnrealGPTAgentClient::UUnrealGPTAgentClient()
	: ToolCallIterationCount(0)
	, bRequestInProgress(false)
{
	Settings = GetMutableDefault<UUnrealGPTSettings>();
	ExecutedToolCallSignatures.Reset();
	bLastToolWasPythonExecute = false;
	bLastSceneQueryFoundResults = false;
}

void UUnrealGPTAgentClient::Initialize()
{
	// Prevent this agent client from being garbage collected while the widget is alive.
	// The Slate widget holds a raw pointer (not a UPROPERTY), so we must root this object
	// to ensure it stays valid across level loads and GC runs.
	if (!IsRooted())
	{
		AddToRoot();
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: AgentClient added to root to prevent GC"));
	}

	// Ensure settings are loaded
	if (!Settings)
	{
		Settings = GetMutableDefault<UUnrealGPTSettings>();
	}
}

void UUnrealGPTAgentClient::SendMessage(const FString& UserMessage, const TArray<FString>& ImageBase64)
{
	if (bRequestInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Request already in progress"));
		return;
	}

	// Ensure Settings is valid
	if (!Settings)
	{
		Settings = GetMutableDefault<UUnrealGPTSettings>();
		if (!Settings)
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Settings is null and could not be retrieved"));
			return;
		}
	}

	if (Settings->ApiKey.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: API Key not set in settings"));
		return;
	}

	// Reset tool call iteration counter for new user messages
	const bool bIsNewUserMessage = !UserMessage.IsEmpty();
	if (bIsNewUserMessage)
	{
		ToolCallIterationCount = 0;
		// If history is empty, clear PreviousResponseId as it's a fresh conversation
		if (ConversationHistory.Num() == 0)
		{
			PreviousResponseId.Empty();
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: New user message with empty history - clearing previous_response_id"));
		}
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: New user message - resetting tool call iteration counter"));
	}
		else
		{
			// Increment counter for tool call continuation
			ToolCallIterationCount++;

			// Use configurable max iterations from settings, with a safe minimum of 1.
			const int32 MaxIterations = FMath::Max(1, Settings->MaxToolCallIterations);

			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Tool call continuation - iteration %d/%d"), ToolCallIterationCount, MaxIterations);
			if (ToolCallIterationCount >= MaxIterations)
			{
				UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Maximum tool call iterations (%d) reached. Stopping to prevent infinite loop."), MaxIterations);
				ToolCallIterationCount = 0;
				bRequestInProgress = false;
				return;
			}
		}

	// Add user message to history only if not empty (empty means continuing after tool call)
	// CRITICAL: Do NOT add empty user messages - this breaks Responses API tool continuation
	if (!UserMessage.IsEmpty())
	{
		FAgentMessage UserMsg;
		UserMsg.Role = TEXT("user");
		UserMsg.Content = UserMessage;
		ConversationHistory.Add(UserMsg);
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added user message to history: %s"), *UserMessage.Left(100));
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Empty user message - this is a tool continuation, NOT adding to history"));
	}

	// Build request JSON
	// Note: Using Responses API (/v1/responses) for better agentic tool calling support
	TSharedPtr<FJsonObject> RequestJson = MakeShareable(new FJsonObject);
	RequestJson->SetStringField(TEXT("model"), Settings->DefaultModel);
	const bool bUseResponsesApi = IsUsingResponsesApi();

	// Configure reasoning effort if supported (Responses API + gpt-5/o-series models)
	if (bUseResponsesApi)
	{
		// Simple check for models that likely support reasoning
		const FString ModelName = Settings->DefaultModel.ToLower();
		const bool bSupportsReasoning = ModelName.Contains(TEXT("gpt-5")) || ModelName.Contains(TEXT("o1")) || ModelName.Contains(TEXT("o3"));
		
		if (bSupportsReasoning)
		{
			TSharedPtr<FJsonObject> ReasoningObj = MakeShareable(new FJsonObject);
			// Enable lightweight server-side reasoning; optionally request a compact reasoning summary
			// when the organization is allowed to use this feature.
			ReasoningObj->SetStringField(TEXT("effort"), TEXT("low"));
			if (bAllowReasoningSummary)
			{
				ReasoningObj->SetStringField(TEXT("summary"), TEXT("auto"));
			}
			RequestJson->SetObjectField(TEXT("reasoning"), ReasoningObj);
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Enabled reasoning (effort: low%s) for model %s"),
				bAllowReasoningSummary ? TEXT(", summary: auto") : TEXT(""),
				*Settings->DefaultModel);
		}
	}

	// High-level behavior instructions for the agent.
	// Removed the explicit "plan first" requirement in favor of native model reasoning.
	// This reduces chat noise and lets the reasoning model handle the step-by-step logic internally.
	const FString AgentInstructions =
		TEXT("You are UnrealGPT, an expert Unreal Engine 5 editor copilot running inside the Unreal Editor. ")
		TEXT("You are a **primarily action-based agent**: your job is to directly change the project and level by calling tools, ")
		TEXT("not to give the user step-by-step instructions they could perform manually.\n\n")
		TEXT("You can modify the level using Python via the 'python_execute' tool, query the world with 'scene_query', ")
		TEXT("inspect or capture the viewport with 'viewport_screenshot', ")
		TEXT("look up documentation or examples using the built-in 'file_search' tool, and search the attached UE 5.6 Python API vector store via the 'file_search' tool. ")
		TEXT("Treat each user request as a task to be carried out through these tools.\n\n")
		TEXT("You are not limited to level/scene changes. Using Python editor scripting you can also work with assets, Blueprints, and other editor systems.\n")
		TEXT("This includes creating, duplicating, and renaming assets; setting up new actor Blueprints; adjusting project or editor settings; and automating repetitive content browser workflows.\n")
		TEXT("When you are unsure about the exact Unreal Engine 5.6 Python API to use, first call 'file_search' with a focused query (for example, ")
		TEXT("\"EditorActorSubsystem spawn_actor_from_asset\" or \"LevelEditorSubsystem new_level\") to search the attached UE 5.6 Python API docs vector store, then adapt those patterns in your 'python_execute' code.\n")
		TEXT("If you still need more detail or broader context, call the 'web_search' tool with queries that include the UE 5.6 Python API docs, e.g. ")
		TEXT("\"site:dev.epicgames.com unreal-engine python api 5.6 StaticMeshActor\". Prefer information from the official UE 5.6 Python API documentation.\n\n")
		TEXT("When the user asks for asset-, Blueprint-, or pipeline-related tasks (for example, \"create a new Actor Blueprint in /Game/MyFolder\"), plan to solve them with Python editor APIs, not just level edits.\n\n")
		// Removed explicit "share your plan" instruction.
		TEXT("After each tool call, evaluate whether the current step is complete based on tool outputs, scene state, and screenshot.\n")
		TEXT("If a step fails or only partially succeeds, fix it before moving on.\n\n")
		TEXT("After any 'python_execute' call, your NEXT tool call should always be a verification tool: 'scene_query' and/or 'viewport_screenshot'. ")
		TEXT("You should NOT call 'python_execute' twice in a row for the same step unless verification has clearly shown that nothing changed.\n")
		TEXT("In addition, your Python code is executed inside a wrapper that exposes a shared 'result' dict and writes it as JSON to the tool output.\n")
		TEXT(" - You can set 'result[\"status\"]' (e.g., 'ok' or 'error'), 'result[\"message\"]', and add rich details under 'result[\"details\"]' (such as asset paths, actor counts, or custom flags).\n")
		TEXT(" - When creating actors or assets, include 'result[\"details\"][\"actor_name\"]' or 'result[\"details\"][\"actor_label\"]' to enable automatic viewport focusing on the created object.\n")
		TEXT(" - If an exception is raised, the wrapper automatically sets 'status' to 'error' and includes a traceback; you should read this JSON to decide what to do next.\n")
		TEXT("Use both the JSON result and scene_query / viewport_screenshot to determine whether a step truly succeeded before moving on.\n")
		TEXT("For creation-style requests (\"add a cube\", \"create a new light\", \"make a Blueprint\"), after executing python_execute and then scene_query, ")
		TEXT("you will be prompted to evaluate whether the task is complete. Use your reasoning to determine if the user's request has been fulfilled. ")
		TEXT("If scene_query found the requested objects and python_execute reported success, the task is likely complete - provide a brief confirmation and STOP.\n")
		TEXT("When the user specifies a quantity (for example, \"add one cube\" or \"create three point lights\"), you MUST reason about how many objects have ALREADY been successfully created based on tool outputs. ")
		TEXT("Use verification tools to do this, it's important to get this right.\n")
		TEXT("As soon as the number of created objects you can infer from JSON + verification meets the requested quantity, you MUST STOP and provide a completion message. ")
		TEXT("Do NOT execute any further python_execute calls for that request.\n")
		TEXT("Do NOT keep planning additional python_execute calls to spawn more copies unless the user explicitly asked for multiple instances beyond the count already created or the verification tools show that the object is missing or wrong.\n")
		TEXT("CRITICAL: After scene_query finds matching objects, carefully evaluate if the task is complete. ")
		TEXT("If it is complete, provide a brief confirmation message and STOP. Do NOT continue executing tools unnecessarily.\n")
		TEXT("Avoid re-running the same or very similar Python code multiple times in a row; trust the JSON result and scene_query findings and report completion when they confirm success.\n\n")
		TEXT("By default, you SHOULD respond with one or more tool calls to accomplish the task. However, when scene_query or ")
		TEXT("viewport_screenshot confirms that the user's request has been fulfilled, you MUST provide a final text response ")
		TEXT("confirming completion rather than continuing to execute more code. Do NOT answer with only written suggestions ")
		TEXT("or editor UI instructions when you can perform the change yourself via tools, but DO provide a completion message ")
		TEXT("when verification tools confirm the task is done. Only skip tool use when the user explicitly asks for ")
		TEXT("explanation-only help or explicitly says not to modify the project.\n\n")
		TEXT("When the user asks for an environment or lighting setup (for example, an outdoor lighting setup):\n")
		TEXT(" - First, inspect the existing scene with Python and scene_query / other tools to understand what already exists.\n")
		TEXT(" - Prefer reusing and adjusting existing actors instead of always spawning new ones.\n")
		TEXT(" - Never keep adding more copies of the same core actors (such as DirectionalLight, SkyLight, SkyAtmosphere, ExponentialHeightFog)\n")
		TEXT("   unless the user explicitly asks for multiple. If a directional light already exists, adjust its properties instead of spawning another.\n")
		TEXT(" - Break complex edits into several smaller python_execute calls, each focused and idempotent when possible.\n")
		TEXT(" - After running Python, infer the result from logs/behavior; if something failed, correct it in a follow-up tool call rather than repeating the same action.\n")
		TEXT(" - Optimize for a clean, physically plausible lighting setup, not for spawning maximal numbers of actors.\n\n")
		TEXT("Python best practices in Unreal Editor:\n")
		TEXT(" - Always 'import unreal' at the top of your scripts.\n")
		TEXT(" - Do NOT access actors via 'world.all_actors' or other non-existent attributes on UWorld; these will raise AttributeError.\n")
		TEXT(" - To iterate actors, use 'unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()' or ")
		TEXT("   'unreal.EditorLevelLibrary.get_all_level_actors()' (even if marked deprecated) instead.\n")
		TEXT(" - If a helper like get_actor_by_class is needed, implement it yourself using these APIs instead of assuming it exists.\n\n")
		TEXT("Engine and API assumptions:\n")
		TEXT(" - Assume Unreal Engine 5.6 Editor is running, and always prefer APIs that are valid for UE 5.6's Python editor API.\n")
		TEXT(" - Avoid relying on older UE 5.0/5.1-era patterns if there is a clearer or more direct UE 5.6 API.\n")
		TEXT(" - Code always runs in the Editor (not in a packaged game), with full access to the 'unreal' module and editor subsystems.\n")
		TEXT(" - When choosing between multiple possible APIs, prefer the one that is documented for UE 5.6 Editor Python.\n\n")
		TEXT("Safety and non-destructive editing:\n")
		TEXT(" - Be conservative with destructive operations such as deleting actors/assets, mass renames, or overwriting content.\n")
		TEXT(" - Prefer edits that are scoped and idempotent (safe to re-run) rather than broad, sweeping changes.\n")
		TEXT(" - If a task appears destructive (e.g., bulk delete or irreversible reimport), either operate on a clearly limited selection or ask the user for explicit confirmation in natural language before proceeding.\n");

		TEXT("You can also use the 'file_search' tool to search the attached UE 5.6 Python API docs vector store for information on how to use the Python API.  Prefer the 'file_search' tool to search for information on how to use the Python API, not the 'web_search' tool.\n")
		TEXT("When you need to know exactly what reflected C++ or Blueprint members exist, use the 'reflection_query' tool to inspect a UClass (including custom plugins and project types) via Unreal's reflection system before writing Python code.\n")
		TEXT("Check your tasks against the 'scene_query' and 'viewport_screenshot' tools after each tool call to ensure you have completed the task correctly, and aren't repeating the same task unnecessarily.\n\n")
		TEXT("When Replicate is configured in settings, you have access to a dedicated 'replicate_generate' tool for content generation (images, video, audio, and 3D files).\n")
		TEXT("Use 'replicate_generate' to call Replicate models directly over HTTP. ")
		TEXT("For images, set output_kind=\"image\" so the helper can route files to the image staging folder and use the preferred image model from settings when no explicit version is provided.\n")
		TEXT("For 3D assets, set output_kind=\"3d\" so the helper can route files to the 3D staging folder and use the preferred 3D model from settings.\n")
		TEXT("For audio, set output_kind=\"audio\" and, when possible, use output_subkind=\"sfx\", \"music\", or \"speech\" so the helper can choose between the SFX, music, and speech models configured in settings.\n")
		TEXT("When the user names a specific Replicate model (for example 'black-forest-labs/flux-dev'), pass that identifier as the 'version' argument to replicate_generate so the helper can route the call correctly.\n")
		TEXT("The tool returns JSON with 'details.files' containing local file paths for any downloaded outputs. ")
		TEXT("After calling 'replicate_generate', typically call 'python_execute' to import the downloaded files into the project (for example using the 'unrealgpt_mcp_import' helpers for textures, meshes, or audio) ")
		TEXT("and then use 'scene_query' and/or 'viewport_screenshot' to verify that the imported assets are correctly created and used in the level.\n\n")
		TEXT("IMPORTANT: SCENE BUILDING STRATEGY:\n")
		TEXT("When the user provides an image reference and asks to 'build this scene' (or similar), do NOT just place basic engine shapes (cubes, spheres) to approximate it.\n")
		TEXT("Instead, adopt a high-fidelity, magical workflow that make full use of the Replicate generate tool:\n")
		TEXT("1. Decompose the scene into key objects.\n")
		TEXT("2. For each object that cannot be created with basic shapes, use 'replicate_generate' with output_kind=\"3d\" to create a high-quality mesh.\n")
		TEXT("3. For surfaces or unique looks, use 'replicate_generate' with output_kind=\"image\" (texture generation) to create textures.\n")
		TEXT("4. Import these assets using 'python_execute' and the 'unrealgpt_mcp_import' helpers.\n")
		TEXT("5. Create Materials from the generated textures and apply them to your generated meshes or even basic shapes if needed.\n")
		TEXT("6. Place and arrange these high-quality assets to match the reference image composition.\n")
		TEXT("Use your full range of capabilities to create a rich, detailed result rather than a low-poly approximation.\n");
	if (bUseResponsesApi)
	{
		RequestJson->SetStringField(TEXT("instructions"), AgentInstructions);

		TSharedPtr<FJsonObject> TextObj = MakeShareable(new FJsonObject);
		TextObj->SetStringField(TEXT("verbosity"), TEXT("low"));
		RequestJson->SetObjectField(TEXT("text"), TextObj);

		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Set Responses API verbosity to low for concise outputs"));
	}
	// Temporarily disable streaming for Responses API until SSE parser fully supports new event schema
	RequestJson->SetBoolField(TEXT("stream"), !bUseResponsesApi);
	
	if (bUseResponsesApi)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Using Responses API for agentic tool calling"));
		
		// For Responses API, use previous_response_id if available
		if (!PreviousResponseId.IsEmpty())
		{
			RequestJson->SetStringField(TEXT("previous_response_id"), PreviousResponseId);
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Using previous_response_id: %s"), *PreviousResponseId);
		}
	}

	// Build messages array
	TArray<TSharedPtr<FJsonValue>> MessagesArray;
	
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Building messages array from history. History size: %d"), ConversationHistory.Num());
	
	// For Responses API, handle input differently:
	// - Use previous_response_id to maintain state
	// - Only include new user messages in input (or tool results when continuing after tool execution)
	// - Function results are provided as function_call_output items when continuing after tool execution
	// For legacy API, include full conversation history
	int32 StartIndex = 0;
	TArray<FAgentMessage> ToolResultsToInclude; // For Responses API, we'll add function results as input items
	
	if (bUseResponsesApi && !PreviousResponseId.IsEmpty())
	{
		if (bIsNewUserMessage)
		{
			// For new user messages, only include the new message (it's already added to history)
			// Don't look for tool results - those are only relevant when continuing after tool execution
			// The new user message was just added, so it's the last item in history
			const int32 HistorySize = ConversationHistory.Num();
			if (HistorySize > 0)
			{
				StartIndex = HistorySize - 1; // Only include the last message (the new user message)
				// Double-check that StartIndex is valid
				if (StartIndex < 0 || StartIndex >= HistorySize)
				{
					UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Calculated invalid StartIndex %d for history size %d, resetting to 0"), StartIndex, HistorySize);
					StartIndex = 0;
				}
			}
			else
			{
				StartIndex = 0; // Safety fallback - should not happen as we just added a message
				UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: History is empty after adding user message, this should not happen"));
			}
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Responses API - new user message, starting from index %d (history size: %d)"), StartIndex, ConversationHistory.Num());
		}
		else
		{
			// For tool call continuation, find tool results that need to be included
			// Look for tool messages after the last assistant message with tool_calls
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Tool continuation - searching for tool results in history (size: %d)"), ConversationHistory.Num());
			
			for (int32 i = ConversationHistory.Num() - 1; i >= 0; --i)
			{
				if (ConversationHistory[i].Role == TEXT("assistant") && 
					(ConversationHistory[i].ToolCallIds.Num() > 0 || !ConversationHistory[i].ToolCallsJson.IsEmpty()))
				{
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found assistant message with tool_calls at index %d (tool_call_ids: %d)"), 
						i, ConversationHistory[i].ToolCallIds.Num());
					
					// Found the assistant message with tool_calls
					// Collect tool results that follow it
					for (int32 j = i + 1; j < ConversationHistory.Num(); ++j)
					{
						if (ConversationHistory[j].Role == TEXT("tool"))
						{
							ToolResultsToInclude.Add(ConversationHistory[j]);
							UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added tool result to include: call_id=%s, content_length=%d"), 
								*ConversationHistory[j].ToolCallId, ConversationHistory[j].Content.Len());
						}
						else if (ConversationHistory[j].Role == TEXT("user"))
						{
							StartIndex = j; // Start from this user message
							UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found user message at index %d, stopping tool result collection"), j);
							break;
						}
					}
					break;
				}
			}
			
			// If no user message found, start from after tool results
			if (StartIndex == 0 && ToolResultsToInclude.Num() > 0)
			{
				StartIndex = ConversationHistory.Num(); // Don't include any history messages, only tool results
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: No user message found, starting from end of history (index %d)"), StartIndex);
			}
			
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Responses API - tool continuation, starting from index %d, will include %d tool results"), StartIndex, ToolResultsToInclude.Num());
			
			// Log all tool call IDs we're looking for vs what we found
			if (ConversationHistory.Num() > 0)
			{
				const FAgentMessage& LastAssistantMsg = ConversationHistory[ConversationHistory.Num() - 1];
				if (LastAssistantMsg.Role == TEXT("assistant") && LastAssistantMsg.ToolCallIds.Num() > 0)
				{
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Last assistant message has %d tool_call_ids"), LastAssistantMsg.ToolCallIds.Num());
					for (const FString& ExpectedCallId : LastAssistantMsg.ToolCallIds)
					{
						bool bFound = false;
						for (const FAgentMessage& ToolResult : ToolResultsToInclude)
						{
							if (ToolResult.ToolCallId == ExpectedCallId)
							{
								bFound = true;
								break;
							}
						}
						UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Tool call %s: %s"), *ExpectedCallId, bFound ? TEXT("FOUND") : TEXT("MISSING"));
					}
				}
			}
		}
	}
	
	// For Responses API, add function results as input items with type "function_call_output"
	// IMPORTANT: Only include tool results that are reasonably sized to prevent context overflow
	// CRITICAL: For tool continuation (empty UserMessage), we MUST include tool results
	if (bUseResponsesApi)
	{
		// If this is a tool continuation (empty UserMessage), ensure we have tool results
		if (UserMessage.IsEmpty() && ToolResultsToInclude.Num() == 0)
		{
			// Try to find tool results from the most recent assistant message with tool_calls
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Tool continuation but no tool results found - searching history for recent tool results"));
			for (int32 i = ConversationHistory.Num() - 1; i >= 0 && i >= ConversationHistory.Num() - 10; --i)
			{
				if (ConversationHistory[i].Role == TEXT("tool"))
				{
					ToolResultsToInclude.Add(ConversationHistory[i]);
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found tool result in history at index %d: call_id=%s"), 
						i, *ConversationHistory[i].ToolCallId);
				}
				else if (ConversationHistory[i].Role == TEXT("assistant") && 
					(ConversationHistory[i].ToolCallIds.Num() > 0 || !ConversationHistory[i].ToolCallsJson.IsEmpty()))
				{
					// Found assistant message with tool_calls, stop searching backwards
					break;
				}
			}
		}
		
		if (ToolResultsToInclude.Num() > 0)
		{
			int32 TotalSize = 0;
			for (const FAgentMessage& ToolResult : ToolResultsToInclude)
			{
				// Skip tool results that are already truncated/summarized (they're already safe)
				// But also check total size to be safe
				int32 ResultSize = ToolResult.Content.Len();
				if (TotalSize + ResultSize > MaxToolResultSize * 5) // Allow up to 5x max size total
				{
					UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Skipping tool result (size: %d) to prevent context overflow. Total size: %d"), 
						ResultSize, TotalSize);
					continue;
				}
				
				// Create function_call_output input item
				TSharedPtr<FJsonObject> FunctionResultObj = MakeShareable(new FJsonObject);
				FunctionResultObj->SetStringField(TEXT("type"), TEXT("function_call_output"));
				FunctionResultObj->SetStringField(TEXT("call_id"), ToolResult.ToolCallId);
				FunctionResultObj->SetStringField(TEXT("output"), ToolResult.Content);
				
				MessagesArray.Add(MakeShareable(new FJsonValueObject(FunctionResultObj)));
				TotalSize += ResultSize;
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added function_call_output input for call_id: %s (size: %d, total: %d)"), 
					*ToolResult.ToolCallId, ResultSize, TotalSize);
			}
		}
		else if (UserMessage.IsEmpty())
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Tool continuation with empty message but no tool results found! This will cause API error."));
		}
	}
	
	// Add conversation history (or subset for Responses API)
	// Ensure StartIndex is valid (non-negative and within bounds)
	const int32 HistorySize = ConversationHistory.Num();
	if (StartIndex < 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Invalid StartIndex (%d), resetting to 0"), StartIndex);
		StartIndex = 0;
	}
	if (StartIndex >= HistorySize)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: StartIndex (%d) >= history size (%d), resetting to 0"), StartIndex, HistorySize);
		StartIndex = 0;
	}
	
	// Final safety check before accessing array
	if (HistorySize == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Conversation history is empty, skipping message processing"));
	}
	else
	{
		for (int32 i = StartIndex; i < HistorySize; ++i)
		{
			// Additional bounds check inside loop for extra safety
			if (i < 0 || i >= HistorySize)
			{
				UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Array index %d out of bounds (size: %d), breaking loop"), i, HistorySize);
				break;
			}
			
			const FAgentMessage& Msg = ConversationHistory[i];
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Processing message %d: role=%s, hasToolCallsJson=%d, ToolCallIds.Num()=%d"), 
				i, *Msg.Role, !Msg.ToolCallsJson.IsEmpty(), Msg.ToolCallIds.Num());
			
			TSharedPtr<FJsonObject> MsgObj = MakeShareable(new FJsonObject);
			MsgObj->SetStringField(TEXT("role"), Msg.Role);
			
			if (Msg.Role == TEXT("user") && ImageBase64.Num() > 0)
			{
				// Multimodal content
				TArray<TSharedPtr<FJsonValue>> ContentArray;
				
				TSharedPtr<FJsonObject> TextContent = MakeShareable(new FJsonObject);
				
				// For Responses API, use input_text; for legacy Chat Completions, use text
				if (bUseResponsesApi)
				{
					TextContent->SetStringField(TEXT("type"), TEXT("input_text"));
					TextContent->SetStringField(TEXT("text"), Msg.Content);
				}
				else
				{
					TextContent->SetStringField(TEXT("type"), TEXT("text"));
					TextContent->SetStringField(TEXT("text"), Msg.Content);
				}
				
				ContentArray.Add(MakeShareable(new FJsonValueObject(TextContent)));
				
				for (const FString& ImageData : ImageBase64)
				{
					TSharedPtr<FJsonObject> ImageContent = MakeShareable(new FJsonObject);
					
					if (bUseResponsesApi)
					{
						// OpenAI Responses API multimodal schema:
						// { "type": "input_image", "image_url": "data:image/png;base64,..." }
						ImageContent->SetStringField(TEXT("type"), TEXT("input_image"));
						ImageContent->SetStringField(
							TEXT("image_url"),
							FString::Printf(TEXT("data:image/png;base64,%s"), *ImageData));
					}
					else
					{
						// Legacy Chat Completions multimodal schema:
						// { "type": "image_url", "image_url": { "url": "data:image/png;base64,..." } }
						ImageContent->SetStringField(TEXT("type"), TEXT("image_url"));
						TSharedPtr<FJsonObject> ImageUrl = MakeShareable(new FJsonObject);
						ImageUrl->SetStringField(
							TEXT("url"),
							FString::Printf(TEXT("data:image/png;base64,%s"), *ImageData));
						ImageContent->SetObjectField(TEXT("image_url"), ImageUrl);
					}
					
					ContentArray.Add(MakeShareable(new FJsonValueObject(ImageContent)));
				}
				
				MsgObj->SetArrayField(TEXT("content"), ContentArray);
			}
			else if (Msg.Role == TEXT("assistant") && (Msg.ToolCallIds.Num() > 0 || !Msg.ToolCallsJson.IsEmpty()))
			{
				// For Responses API, skip assistant messages with tool_calls - the API maintains tool call state internally
				if (IsUsingResponsesApi())
				{
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Skipping assistant message with tool_calls for Responses API (state maintained by API)"));
					continue;
				}
				
				// For legacy Chat Completions API, include assistant messages with tool_calls
				// Content can be null or empty when tool_calls are present
				if (!Msg.Content.IsEmpty())
				{
					MsgObj->SetStringField(TEXT("content"), Msg.Content);
				}
				else
				{
					// Set content to null (empty string should work, but null is more correct)
					MsgObj->SetStringField(TEXT("content"), TEXT(""));
				}
				
				// Parse and add tool_calls array - CRITICAL: must succeed
				bool bToolCallsAdded = false;
				if (!Msg.ToolCallsJson.IsEmpty())
				{
					TSharedRef<TJsonReader<>> ToolCallsReader = TJsonReaderFactory<>::Create(Msg.ToolCallsJson);
					TArray<TSharedPtr<FJsonValue>> ToolCallsArray;
					if (FJsonSerializer::Deserialize(ToolCallsReader, ToolCallsArray) && ToolCallsArray.Num() > 0)
					{
						MsgObj->SetArrayField(TEXT("tool_calls"), ToolCallsArray);
						bToolCallsAdded = true;
						UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Successfully added tool_calls to assistant message. ToolCalls count: %d"), ToolCallsArray.Num());
					}
					else
					{
						// If deserialization failed, try to reconstruct from stored data
						UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Failed to deserialize tool_calls JSON: %s. Attempting reconstruction."), *Msg.ToolCallsJson);
					}
				}
				
				// If tool_calls weren't added and we have ToolCallIds, try to reconstruct
				if (!bToolCallsAdded && Msg.ToolCallIds.Num() > 0)
				{
					// Reconstruct minimal tool_calls array from ToolCallIds
					// Note: This is a fallback - we don't have the full tool call info, but we can create a minimal structure
					TArray<TSharedPtr<FJsonValue>> ToolCallsArray;
					for (const FString& ToolCallId : Msg.ToolCallIds)
					{
						TSharedPtr<FJsonObject> ToolCallObj = MakeShareable(new FJsonObject);
						ToolCallObj->SetStringField(TEXT("id"), ToolCallId);
						ToolCallObj->SetStringField(TEXT("type"), TEXT("function"));
						
						// We don't have the function name/args, so create empty function object
						TSharedPtr<FJsonObject> FunctionObj = MakeShareable(new FJsonObject);
						FunctionObj->SetStringField(TEXT("name"), TEXT("unknown"));
						FunctionObj->SetStringField(TEXT("arguments"), TEXT("{}"));
						ToolCallObj->SetObjectField(TEXT("function"), FunctionObj);
						
						ToolCallsArray.Add(MakeShareable(new FJsonValueObject(ToolCallObj)));
					}
					
					if (ToolCallsArray.Num() > 0)
					{
						MsgObj->SetArrayField(TEXT("tool_calls"), ToolCallsArray);
						bToolCallsAdded = true;
						UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Reconstructed tool_calls from ToolCallIds. Count: %d"), ToolCallsArray.Num());
					}
				}
				
				// If we still couldn't add tool_calls, this is a critical error
				if (!bToolCallsAdded)
				{
					UE_LOG(LogTemp, Error, TEXT("UnrealGPT: CRITICAL: Cannot add tool_calls to assistant message. ToolCallsJson empty: %d, ToolCallIds.Num(): %d"), 
						Msg.ToolCallsJson.IsEmpty(), Msg.ToolCallIds.Num());
					// Skip this message to avoid API error - but this will cause the tool message to be orphaned
					UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Skipping assistant message without valid tool_calls to prevent API error"));
					continue;
				}
			}
			else if (Msg.Role == TEXT("tool"))
			{
				// Tool messages are NOT supported in Responses API input array
				// The API maintains tool call state internally via previous_response_id
				if (bUseResponsesApi)
				{
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Skipping tool message for Responses API (state maintained via previous_response_id)"));
					continue;
				}
				
				// For legacy API, tool messages must follow an assistant message with tool_calls
				bool bCanAddToolMessage = false;
				if (MessagesArray.Num() > 0)
				{
					const TSharedPtr<FJsonValue>& LastMsgValue = MessagesArray.Last();
					if (LastMsgValue.IsValid() && LastMsgValue->Type == EJson::Object)
					{
						TSharedPtr<FJsonObject> LastMsgObj = LastMsgValue->AsObject();
						if (LastMsgObj.IsValid())
						{
							FString LastRole;
							if (LastMsgObj->TryGetStringField(TEXT("role"), LastRole))
							{
								if (LastRole == TEXT("assistant") && LastMsgObj->HasField(TEXT("tool_calls")))
								{
									bCanAddToolMessage = true;
								}
								else
								{
									UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Tool message at index %d does not follow assistant message with tool_calls. Previous role: %s, has tool_calls: %d"), 
										i, *LastRole, LastMsgObj->HasField(TEXT("tool_calls")));
								}
							}
						}
					}
				}
				else
				{
					UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Tool message at index %d has no preceding messages"), i);
				}
				
				// Only add tool message if it follows a valid assistant message with tool_calls
				if (!bCanAddToolMessage)
				{
					UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Skipping tool message at index %d to prevent API error"), i);
					continue;
				}
				
				MsgObj->SetStringField(TEXT("content"), Msg.Content);
				MsgObj->SetStringField(TEXT("tool_call_id"), Msg.ToolCallId);
			}
			else
			{
				MsgObj->SetStringField(TEXT("content"), Msg.Content);
			}
			
			MessagesArray.Add(MakeShareable(new FJsonValueObject(MsgObj)));
		}
	}

	const FString ConversationFieldName = IsUsingResponsesApi() ? TEXT("input") : TEXT("messages");
	
	// For Responses API, if we have function results, they're already added as input items
	// Otherwise, add the messages array
	if (MessagesArray.Num() > 0 || !bUseResponsesApi)
	{
		RequestJson->SetArrayField(ConversationFieldName, MessagesArray);
	}
	else if (bUseResponsesApi && MessagesArray.Num() == 0)
	{
		// For Responses API, if we have previous_response_id but no new input, 
		// we still need to provide an empty array or the API might error
		RequestJson->SetArrayField(ConversationFieldName, MessagesArray);
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Responses API request with previous_response_id but empty input array"));
	}

	// Add tools
	TArray<TSharedPtr<FJsonValue>> ToolsArray;
	for (const auto& ToolDef : BuildToolDefinitions())
	{
		ToolsArray.Add(MakeShareable(new FJsonValueObject(ToolDef)));
	}
	if (ToolsArray.Num() > 0)
	{
		RequestJson->SetArrayField(TEXT("tools"), ToolsArray);
	}

	// Serialize to string
	FString RequestBody;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
	FJsonSerializer::Serialize(RequestJson.ToSharedRef(), Writer);

	// Cache the body so we can safely retry with small modifications (e.g., stripping reasoning.summary)
	LastRequestBody = RequestBody;

	// Create HTTP request
	CurrentRequest = CreateHttpRequest();
	CurrentRequest->SetURL(GetEffectiveApiUrl());
	CurrentRequest->SetVerb(TEXT("POST"));
	CurrentRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	CurrentRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Settings->ApiKey));
	CurrentRequest->SetContentAsString(RequestBody);
	CurrentRequest->OnProcessRequestComplete().BindUObject(this, &UUnrealGPTAgentClient::OnResponseReceived);
	
	bRequestInProgress = true;
	CurrentRequest->ProcessRequest();
}

void UUnrealGPTAgentClient::CancelRequest()
{
	if (CurrentRequest.IsValid() && bRequestInProgress)
	{
		CurrentRequest->CancelRequest();
		bRequestInProgress = false;
	}
}

void UUnrealGPTAgentClient::ClearHistory()
{
	ConversationHistory.Empty();
	PreviousResponseId.Empty();
	ToolCallIterationCount = 0;
	ExecutedToolCallSignatures.Reset();
	bLastToolWasPythonExecute = false;
	bLastSceneQueryFoundResults = false;
}

TArray<TSharedPtr<FJsonObject>> UUnrealGPTAgentClient::BuildToolDefinitions()
{
	TArray<TSharedPtr<FJsonObject>> Tools;
	const bool bUseResponsesApi = IsUsingResponsesApi();

	auto BuildToolObject = [bUseResponsesApi](const FString& Name, const FString& Description, const TSharedPtr<FJsonObject>& Parameters) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Tool = MakeShareable(new FJsonObject);
		Tool->SetStringField(TEXT("type"), TEXT("function"));

		if (bUseResponsesApi)
		{
			Tool->SetStringField(TEXT("name"), Name);
			Tool->SetStringField(TEXT("description"), Description);
			Tool->SetObjectField(TEXT("parameters"), Parameters);
		}
		else
		{
			TSharedPtr<FJsonObject> FunctionObj = MakeShareable(new FJsonObject);
			FunctionObj->SetStringField(TEXT("name"), Name);
			FunctionObj->SetStringField(TEXT("description"), Description);
			FunctionObj->SetObjectField(TEXT("parameters"), Parameters);
			Tool->SetObjectField(TEXT("function"), FunctionObj);
		}

		return Tool;
	};

	if (Settings->bEnablePythonExecution)
	{
		TSharedPtr<FJsonObject> PythonParams = MakeShareable(new FJsonObject);
		PythonParams->SetStringField(TEXT("type"), TEXT("object"));
		
		TSharedPtr<FJsonObject> CodeProperty = MakeShareable(new FJsonObject);
		CodeProperty->SetStringField(TEXT("type"), TEXT("string"));
		CodeProperty->SetStringField(TEXT("description"), TEXT("Python code to execute"));
		PythonParams->SetObjectField(TEXT("properties"), MakeShareable(new FJsonObject));
		PythonParams->GetObjectField(TEXT("properties"))->SetObjectField(TEXT("code"), CodeProperty);
		
		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShareable(new FJsonValueString(TEXT("code"))));
		PythonParams->SetArrayField(TEXT("required"), Required);
		
		Tools.Add(BuildToolObject(
			TEXT("python_execute"),
			TEXT("Execute Python code in Unreal Engine editor. Use this to manipulate actors, spawn objects, modify properties, automate Content Browser and asset/Blueprint operations, and perform other editor tasks not possible with other tools. ")
			TEXT("Code runs in the editor Python environment with access to the 'unreal' module and editor subsystems. ")
			TEXT("To provide a better UI experience, your code should return a result dict with 'status', 'message', and 'details'. ")
			TEXT("If you populate 'result[\"details\"][\"actor_label\"]' or 'result[\"details\"][\"actor_name\"]' with the name of a created or modified actor, the editor viewport will automatically focus on it."),
			PythonParams));
	}

	// if (Settings->bEnableComputerUse)
	// {
	// 	TSharedPtr<FJsonObject> ComputerUseParams = MakeShareable(new FJsonObject);
	// 	ComputerUseParams->SetStringField(TEXT("type"), TEXT("object"));
	// 	
	// 	TSharedPtr<FJsonObject> ActionProperty = MakeShareable(new FJsonObject);
	// 	ActionProperty->SetStringField(TEXT("type"), TEXT("string"));
	// 	ActionProperty->SetStringField(TEXT("description"), TEXT("JSON string describing the action to perform"));
	// 	ComputerUseParams->SetObjectField(TEXT("properties"), MakeShareable(new FJsonObject));
	// 	ComputerUseParams->GetObjectField(TEXT("properties"))->SetObjectField(TEXT("action"), ActionProperty);
	// 	
	// 	TArray<TSharedPtr<FJsonValue>> Required;
	// 	Required.Add(MakeShareable(new FJsonValueString(TEXT("action"))));
	// 	ComputerUseParams->SetArrayField(TEXT("required"), Required);
	// 	
	// 	Tools.Add(BuildToolObject(
	// 		TEXT("computer_use"),
	// 		TEXT("Execute computer use actions like file operations. ")
	// 		TEXT("The 'action' argument must be a JSON string with a 'type' field. ")
	// 		TEXT("Supported types: 'file_operation' (requires 'operation' ['read', 'write', 'delete', 'exists'] and 'path', optional 'content'). ")
	// 		TEXT("This tool allows system-level access beyond Unreal Python, useful for managing external files or configuration."),
	// 		ComputerUseParams));
	// }

	if (Settings->bEnableViewportScreenshot)
	{
		TSharedPtr<FJsonObject> ScreenshotParams = MakeShareable(new FJsonObject);
		ScreenshotParams->SetStringField(TEXT("type"), TEXT("object"));
		ScreenshotParams->SetObjectField(TEXT("properties"), MakeShareable(new FJsonObject));

		Tools.Add(BuildToolObject(
			TEXT("viewport_screenshot"),
			TEXT("Capture a screenshot of the active viewport. ")
			TEXT("The result is returned as base64-encoded PNG data which is automatically rendered as an image in the chat UI. ")
			TEXT("Use this to visually verify changes, show the user the current state of the scene, or before asking for visual feedback."),
			ScreenshotParams));
	}

	// Generic scene query tool: lets the model search the scene for exactly
	// what it cares about, in a token-efficient way.
	{
		TSharedPtr<FJsonObject> SceneQueryParams = MakeShareable(new FJsonObject);
		SceneQueryParams->SetStringField(TEXT("type"), TEXT("object"));

		TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);

		TSharedPtr<FJsonObject> ClassContainsProp = MakeShareable(new FJsonObject);
		ClassContainsProp->SetStringField(TEXT("type"), TEXT("string"));
		ClassContainsProp->SetStringField(TEXT("description"), TEXT("Optional substring to match in actor class names, e.g., 'DirectionalLight', 'StaticMeshActor'."));
		Properties->SetObjectField(TEXT("class_contains"), ClassContainsProp);

		TSharedPtr<FJsonObject> LabelContainsProp = MakeShareable(new FJsonObject);
		LabelContainsProp->SetStringField(TEXT("type"), TEXT("string"));
		LabelContainsProp->SetStringField(TEXT("description"), TEXT("Optional substring to match in actor labels as shown in the Outliner."));
		Properties->SetObjectField(TEXT("label_contains"), LabelContainsProp);

		TSharedPtr<FJsonObject> NameContainsProp = MakeShareable(new FJsonObject);
		NameContainsProp->SetStringField(TEXT("type"), TEXT("string"));
		NameContainsProp->SetStringField(TEXT("description"), TEXT("Optional substring to match in actor object names."));
		Properties->SetObjectField(TEXT("name_contains"), NameContainsProp);

		TSharedPtr<FJsonObject> ComponentClassContainsProp = MakeShareable(new FJsonObject);
		ComponentClassContainsProp->SetStringField(TEXT("type"), TEXT("string"));
		ComponentClassContainsProp->SetStringField(TEXT("description"), TEXT("Optional substring to match in component class names, e.g., 'DirectionalLightComponent'."));
		Properties->SetObjectField(TEXT("component_class_contains"), ComponentClassContainsProp);

		TSharedPtr<FJsonObject> MaxResultsProp = MakeShareable(new FJsonObject);
		MaxResultsProp->SetStringField(TEXT("type"), TEXT("integer"));
		MaxResultsProp->SetStringField(TEXT("description"), TEXT("Maximum number of matching actors to return (default 20)."));
		MaxResultsProp->SetNumberField(TEXT("default"), 20);
		Properties->SetObjectField(TEXT("max_results"), MaxResultsProp);

		SceneQueryParams->SetObjectField(TEXT("properties"), Properties);

		Tools.Add(BuildToolObject(
			TEXT("scene_query"),
			TEXT("Search the current level for actors matching simple filters. ")
			TEXT("Returns a JSON array of matching actors with their locations, classes, and labels. ")
			TEXT("The results will be displayed to the user as a formatted list, making it easy to identify targets for subsequent python_execute calls. ")
			TEXT("You can filter by class_contains, label_contains, name_contains, component_class_contains, and control max_results."),
			SceneQueryParams));
	}

	// Reflection-based class inspection tool: lets the model inspect reflected
	// properties and functions on any UClass (including custom plugins) to avoid
	// hallucinating members before writing Python.
	{
		TSharedPtr<FJsonObject> ReflectionParams = MakeShareable(new FJsonObject);
		ReflectionParams->SetStringField(TEXT("type"), TEXT("object"));

		TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);

		TSharedPtr<FJsonObject> ClassNameProp = MakeShareable(new FJsonObject);
		ClassNameProp->SetStringField(TEXT("type"), TEXT("string"));
		ClassNameProp->SetStringField(
			TEXT("description"),
			TEXT("Name or path of the UClass to inspect. You can pass a short name like 'StaticMeshActor' or a fully qualified path like '/Script/Engine.StaticMeshActor'."));
		Properties->SetObjectField(TEXT("class_name"), ClassNameProp);

		ReflectionParams->SetObjectField(TEXT("properties"), Properties);

		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShareable(new FJsonValueString(TEXT("class_name"))));
		ReflectionParams->SetArrayField(TEXT("required"), Required);

		Tools.Add(BuildToolObject(
			TEXT("reflection_query"),
			TEXT("Inspect an Unreal UClass via the reflection system at runtime. ")
			TEXT("Given a class_name (C++ or Blueprint), this returns a JSON schema describing its reflected properties and functions, ")
			TEXT("including names, C++ types, and high-signal flags that matter for Python/Blueprint access."),
			ReflectionParams));
	}

	// OpenAI-hosted web_search and file_search tools (Responses API only).
	// web_search lets the model search the web in general.
	// file_search is configured against a UE 5.6 Python API vector store the user has created.
	if (bUseResponsesApi)
	{
		// web_search
		TSharedPtr<FJsonObject> WebSearchTool = MakeShareable(new FJsonObject);
		WebSearchTool->SetStringField(TEXT("type"), TEXT("web_search"));
		Tools.Add(WebSearchTool);

		// file_search over UE 5.6 Python API docs
		TSharedPtr<FJsonObject> FileSearchTool = MakeShareable(new FJsonObject);
		FileSearchTool->SetStringField(TEXT("type"), TEXT("file_search"));

		TArray<TSharedPtr<FJsonValue>> VectorStores;
		VectorStores.Add(MakeShareable(new FJsonValueString(TEXT("vs_691df14e67fc819189353158b9f13942"))));
		FileSearchTool->SetArrayField(TEXT("vector_store_ids"), VectorStores);
		FileSearchTool->SetNumberField(TEXT("max_num_results"), 20);

		Tools.Add(FileSearchTool);
	}

	// Replicate-specific generation tool (direct HTTP integration; no MCP connector required).
	if (Settings && Settings->bEnableReplicateTool && !Settings->ReplicateApiToken.IsEmpty())
	{
		TSharedPtr<FJsonObject> ReplicateParams = MakeShareable(new FJsonObject);
		ReplicateParams->SetStringField(TEXT("type"), TEXT("object"));

		TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);

		TSharedPtr<FJsonObject> PromptProp = MakeShareable(new FJsonObject);
		PromptProp->SetStringField(TEXT("type"), TEXT("string"));
		PromptProp->SetStringField(
			TEXT("description"),
			TEXT("Text prompt describing what to generate (image, video, audio, or 3D asset). For example: ")
			TEXT("'seamless square floral rock wall texture' or 'short ambient forest soundscape'."));
		Properties->SetObjectField(TEXT("prompt"), PromptProp);

		TSharedPtr<FJsonObject> VersionProp = MakeShareable(new FJsonObject);
		VersionProp->SetStringField(TEXT("type"), TEXT("string"));
		VersionProp->SetStringField(
			TEXT("description"),
			TEXT("Optional Replicate model identifier. You can pass either a full version id or an 'owner/model' slug for official models (for example 'black-forest-labs/flux-dev'). "));
		Properties->SetObjectField(TEXT("version"), VersionProp);

		TSharedPtr<FJsonObject> KindProp = MakeShareable(new FJsonObject);
		KindProp->SetStringField(TEXT("type"), TEXT("string"));
		KindProp->SetStringField(
			TEXT("description"),
			TEXT("Optional output kind hint: 'image', 'video', 'audio', or '3d'. Used to pick a staging folder, downstream Unreal import helper, ")
			TEXT("and default Replicate model from plugin settings when no explicit model version is provided."));
		Properties->SetObjectField(TEXT("output_kind"), KindProp);

		TSharedPtr<FJsonObject> SubkindProp = MakeShareable(new FJsonObject);
		SubkindProp->SetStringField(TEXT("type"), TEXT("string"));
		SubkindProp->SetStringField(
			TEXT("description"),
			TEXT("Optional sub-kind for audio or other outputs, e.g. 'sfx', 'music', or 'speech'. ")
			TEXT("This is used to choose between SFX, music, and speech Replicate models configured in settings when 'version' is omitted."));
		Properties->SetObjectField(TEXT("output_subkind"), SubkindProp);

		ReplicateParams->SetObjectField(TEXT("properties"), Properties);

		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShareable(new FJsonValueString(TEXT("prompt"))));
		ReplicateParams->SetArrayField(TEXT("required"), Required);

		Tools.Add(BuildToolObject(
			TEXT("replicate_generate"),
			TEXT("Generate content using Replicate (images, video, audio, or 3D files) via the Replicate HTTP API. ")
			TEXT("Returns JSON with 'status', 'message', and 'details.files' containing local file paths for any downloaded outputs. ")
			TEXT("After calling this, use python_execute (for example with the 'unrealgpt_mcp_import' helpers) to import the files as Unreal assets, ")
			TEXT("then verify placement with scene_query and/or viewport_screenshot."),
			ReplicateParams));
	}

	return Tools;
}

void UUnrealGPTAgentClient::OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	bRequestInProgress = false;

	// Ensure Settings is valid before proceeding
	if (!Settings)
	{
		Settings = GetMutableDefault<UUnrealGPTSettings>();
		if (!Settings)
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Settings is null and could not be retrieved in OnResponseReceived"));
			return;
		}
	}

	if (!bWasSuccessful || !Response.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: HTTP request failed"));
		return;
	}

	int32 ResponseCode = Response->GetResponseCode();
	if (ResponseCode != 200)
	{
		const FString ErrorBody = Response->GetContentAsString();
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: HTTP error %d: %s"), ResponseCode, *ErrorBody);

		// Gracefully handle organizations that are not yet allowed to use reasoning summaries.
		// In that case, we disable reasoning.summary for this session and let the user continue
		// using the agent without having to change any settings.
		if (ResponseCode == 400 && bAllowReasoningSummary && IsUsingResponsesApi())
		{
			TSharedPtr<FJsonObject> ErrorRoot;
			TSharedRef<TJsonReader<>> ErrorReader = TJsonReaderFactory<>::Create(ErrorBody);
			if (FJsonSerializer::Deserialize(ErrorReader, ErrorRoot) && ErrorRoot.IsValid())
			{
				const TSharedPtr<FJsonObject>* ErrorObjPtr = nullptr;
				if (ErrorRoot->TryGetObjectField(TEXT("error"), ErrorObjPtr) && ErrorObjPtr && (*ErrorObjPtr).IsValid())
				{
					FString Param;
					FString Code;
					FString Message;
					(*ErrorObjPtr)->TryGetStringField(TEXT("param"), Param);
					(*ErrorObjPtr)->TryGetStringField(TEXT("code"), Code);
					(*ErrorObjPtr)->TryGetStringField(TEXT("message"), Message);

					if (Param == TEXT("reasoning.summary") && Code == TEXT("unsupported_value"))
					{
						UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Disabling reasoning.summary — org is not verified (%s)"), *Message);
						bAllowReasoningSummary = false;

						// Retry the last request once, without reasoning.summary.
						if (!bRequestInProgress && !LastRequestBody.IsEmpty())
						{
							const FString OriginalBody = LastRequestBody;
							TSharedPtr<FJsonObject> OriginalJson;
							TSharedRef<TJsonReader<>> OriginalReader = TJsonReaderFactory<>::Create(OriginalBody);
							if (FJsonSerializer::Deserialize(OriginalReader, OriginalJson) && OriginalJson.IsValid())
							{
								const TSharedPtr<FJsonObject>* ReasoningObjPtr = nullptr;
								if (OriginalJson->TryGetObjectField(TEXT("reasoning"), ReasoningObjPtr) && ReasoningObjPtr && (*ReasoningObjPtr).IsValid())
								{
									TSharedPtr<FJsonObject> ReasoningObj = *ReasoningObjPtr;
									ReasoningObj->RemoveField(TEXT("summary"));
									OriginalJson->SetObjectField(TEXT("reasoning"), ReasoningObj);

									FString NewBody;
									TSharedRef<TJsonWriter<>> NewWriter = TJsonWriterFactory<>::Create(&NewBody);
									if (FJsonSerializer::Serialize(OriginalJson.ToSharedRef(), NewWriter))
									{
										TSharedRef<IHttpRequest> RetryRequest = CreateHttpRequest();
										RetryRequest->SetURL(GetEffectiveApiUrl());
										RetryRequest->SetVerb(TEXT("POST"));
										RetryRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
										RetryRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Settings->ApiKey));
										RetryRequest->SetContentAsString(NewBody);
										RetryRequest->OnProcessRequestComplete().BindUObject(this, &UUnrealGPTAgentClient::OnResponseReceived);

										bRequestInProgress = true;
										RetryRequest->ProcessRequest();
										return;
									}
								}
							}
						}
					}
				}
			}
		}

		return;
	}

	FString ResponseContent = Response->GetContentAsString();
	
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Received response (length: %d)"), ResponseContent.Len());
	if (ResponseContent.Len() < 500)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Response content: %s"), *ResponseContent);
	}

	if (IsUsingResponsesApi())
	{
		ProcessResponsesApiResponse(ResponseContent);
	}
	else
	{
		ProcessStreamingResponse(ResponseContent);
	}
}

void UUnrealGPTAgentClient::ProcessStreamingResponse(const FString& ResponseContent)
{
	// Parse streaming response (SSE format)
	TArray<FString> Lines;
	ResponseContent.ParseIntoArrayLines(Lines);

	FString AccumulatedContent;
	FString CurrentToolCallId;
	FString CurrentToolName;
	FString CurrentToolArguments;

	for (const FString& Line : Lines)
	{
		if (Line.StartsWith(TEXT("data: ")))
		{
			FString Data = Line.Mid(6); // Remove "data: "
			if (Data == TEXT("[DONE]"))
			{
				break;
			}

			TSharedPtr<FJsonObject> JsonObject;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Data);
			if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
			{
				// Check for choices array
				const TArray<TSharedPtr<FJsonValue>>* ChoicesArray;
				if (JsonObject->TryGetArrayField(TEXT("choices"), ChoicesArray) && ChoicesArray->Num() > 0)
				{
					const TSharedPtr<FJsonObject>* ChoiceObj;
					if ((*ChoicesArray)[0]->TryGetObject(ChoiceObj))
					{
						const TSharedPtr<FJsonObject>* DeltaObj;
						if ((*ChoiceObj)->TryGetObjectField(TEXT("delta"), DeltaObj))
						{
							// Content delta
							FString ContentDelta;
							if ((*DeltaObj)->TryGetStringField(TEXT("content"), ContentDelta))
							{
								AccumulatedContent += ContentDelta;
							}

							// Tool calls delta
							const TArray<TSharedPtr<FJsonValue>>* ToolCallsArray;
							if ((*DeltaObj)->TryGetArrayField(TEXT("tool_calls"), ToolCallsArray))
							{
								for (const TSharedPtr<FJsonValue>& ToolCallValue : *ToolCallsArray)
								{
									const TSharedPtr<FJsonObject>* ToolCallObj;
									if (ToolCallValue->TryGetObject(ToolCallObj))
									{
										(*ToolCallObj)->TryGetStringField(TEXT("id"), CurrentToolCallId);
										
										const TSharedPtr<FJsonObject>* FunctionObj;
										if ((*ToolCallObj)->TryGetObjectField(TEXT("function"), FunctionObj))
										{
											(*FunctionObj)->TryGetStringField(TEXT("name"), CurrentToolName);
											
											FString ArgumentsDelta;
											if ((*FunctionObj)->TryGetStringField(TEXT("arguments"), ArgumentsDelta))
											{
												CurrentToolArguments += ArgumentsDelta;
											}
										}
									}
								}
							}
						}

						// Check if finished
						FString FinishReason;
						if ((*ChoiceObj)->TryGetStringField(TEXT("finish_reason"), FinishReason))
						{
							if (FinishReason == TEXT("tool_calls") && !CurrentToolCallId.IsEmpty())
							{
								// Build tool_calls JSON for assistant message
								TSharedPtr<FJsonObject> ToolCallObj = MakeShareable(new FJsonObject);
								ToolCallObj->SetStringField(TEXT("id"), CurrentToolCallId);
								ToolCallObj->SetStringField(TEXT("type"), TEXT("function"));
								
								TSharedPtr<FJsonObject> FunctionObj = MakeShareable(new FJsonObject);
								FunctionObj->SetStringField(TEXT("name"), CurrentToolName);
								FunctionObj->SetStringField(TEXT("arguments"), CurrentToolArguments);
								ToolCallObj->SetObjectField(TEXT("function"), FunctionObj);

								TArray<TSharedPtr<FJsonValue>> ToolCallsArray;
								ToolCallsArray.Add(MakeShareable(new FJsonValueObject(ToolCallObj)));

								FString ToolCallsJsonString;
								TSharedRef<TJsonWriter<>> ToolCallsWriter = TJsonWriterFactory<>::Create(&ToolCallsJsonString);
								bool bSerialized = FJsonSerializer::Serialize(ToolCallsArray, ToolCallsWriter);
								
								if (!bSerialized || ToolCallsJsonString.IsEmpty())
								{
									UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to serialize tool_calls array"));
								}
								else
								{
									UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Serialized tool_calls: %s"), *ToolCallsJsonString);
								}

								// Add assistant message with tool_calls to history FIRST
								FAgentMessage AssistantMsg;
								AssistantMsg.Role = TEXT("assistant");
								AssistantMsg.Content = AccumulatedContent;
								AssistantMsg.ToolCallIds.Add(CurrentToolCallId);
								AssistantMsg.ToolCallsJson = ToolCallsJsonString;
								ConversationHistory.Add(AssistantMsg);
								
								UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added assistant message with tool_calls to history. History size: %d"), ConversationHistory.Num());

								OnAgentMessage.Broadcast(TEXT("assistant"), AccumulatedContent, TArray<FString>{CurrentToolCallId});

								// Execute tool call
								FString ToolResult = ExecuteToolCall(CurrentToolName, CurrentToolArguments);
								
								// Truncate or summarize large tool results to prevent context window overflow
								FString ToolResultForHistory = ToolResult;
								const bool bIsScreenshot = (CurrentToolName == TEXT("viewport_screenshot"));
								if (ToolResultForHistory.Len() > MaxToolResultSize)
								{
									if (bIsScreenshot && ToolResultForHistory.StartsWith(TEXT("iVBORw0KGgo"))) // Base64 PNG header
									{
										ToolResultForHistory = TEXT("Screenshot captured successfully. [Base64 image data omitted from history to prevent context overflow - ")
											TEXT("the image was captured and can be viewed in the UI. Length: ") + FString::FromInt(ToolResult.Len()) + TEXT(" characters]");
										UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Truncated large screenshot result (%d chars) to prevent context overflow"), ToolResult.Len());
									}
									else
									{
										ToolResultForHistory = ToolResultForHistory.Left(MaxToolResultSize) + 
											TEXT("\n\n[Result truncated - original length: ") + FString::FromInt(ToolResult.Len()) + 
											TEXT(" characters. Full result available in tool output.]");
										UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Truncated large tool result (%d chars) to prevent context overflow"), ToolResult.Len());
									}
								}
								
								// Add tool result to conversation (truncated version)
								FAgentMessage ToolMsg;
								ToolMsg.Role = TEXT("tool");
								ToolMsg.Content = ToolResultForHistory;
								ToolMsg.ToolCallId = CurrentToolCallId;
								ConversationHistory.Add(ToolMsg);

								OnToolResult.Broadcast(CurrentToolCallId, ToolResult);

								// Continue conversation with tool result.
								// If this was a viewport_screenshot call, also forward the image as multimodal input
								// so the model can analyze the actual viewport image (not just a text summary).
								TArray<FString> ScreenshotImages;
								if (bIsScreenshot && !ToolResult.IsEmpty())
								{
									ScreenshotImages.Add(ToolResult);
								}

								SendMessage(TEXT(""), ScreenshotImages);
							}
							else if (!AccumulatedContent.IsEmpty())
							{
								// Add assistant message to history
								FAgentMessage AssistantMsg;
								AssistantMsg.Role = TEXT("assistant");
								AssistantMsg.Content = AccumulatedContent;
								ConversationHistory.Add(AssistantMsg);

								OnAgentMessage.Broadcast(TEXT("assistant"), AccumulatedContent, TArray<FString>());
							}
						}
					}
				}
			}
		}
	}
}

void UUnrealGPTAgentClient::ProcessResponsesApiResponse(const FString& ResponseContent)
{
	TSharedPtr<FJsonObject> RootObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseContent);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to parse Responses API JSON response"));
		return;
	}

	// Log all top-level fields to understand the response structure
	TArray<FString> FieldNames;
	RootObject->Values.GetKeys(FieldNames);
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Response root fields: %s"), *FString::Join(FieldNames, TEXT(", ")));

	// Store the response ID for subsequent requests
	FString ResponseId;
	if (RootObject->TryGetStringField(TEXT("id"), ResponseId))
	{
		PreviousResponseId = ResponseId;
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Stored PreviousResponseId: %s"), *PreviousResponseId);
	}
	
	// Check response status
	FString Status;
	if (RootObject->TryGetStringField(TEXT("status"), Status))
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Response status: %s"), *Status);
		if (Status == TEXT("failed") || Status == TEXT("cancelled"))
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Response status indicates failure: %s"), *Status);
			ToolCallIterationCount = 0; // Reset on failure
			bRequestInProgress = false;
			return;
		}
	}

	// If the model provided a reasoning summary, surface it immediately for the UI.
	const TSharedPtr<FJsonObject>* ReasoningObjPtr = nullptr;
	if (RootObject->TryGetObjectField(TEXT("reasoning"), ReasoningObjPtr) && ReasoningObjPtr && (*ReasoningObjPtr).IsValid())
	{
		FString ReasoningSummary;
		if ((*ReasoningObjPtr)->TryGetStringField(TEXT("summary"), ReasoningSummary) && !ReasoningSummary.IsEmpty())
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Received reasoning summary (length: %d)"), ReasoningSummary.Len());
			OnAgentReasoning.Broadcast(ReasoningSummary);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* OutputArray = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("output"), OutputArray) || !OutputArray)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Responses API response missing 'output' array. Checking for streaming format..."));
		
		// Check if it's actually a streaming response (SSE format)
		if (ResponseContent.Contains(TEXT("data: ")))
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Response appears to be streaming format, processing as SSE"));
			ProcessStreamingResponse(ResponseContent);
			return;
		}
		
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found output array with %d items"), OutputArray->Num());

	// Accumulate text and tool calls across all output items in this response.
	FString AccumulatedText;
	struct FToolCallInfo
	{
		FString Id;
		FString Name;
		FString Arguments;
	};
	TArray<FToolCallInfo> ToolCalls;

	for (int32 i = 0; i < OutputArray->Num(); ++i)
	{
		const TSharedPtr<FJsonValue>& OutputValue = (*OutputArray)[i];
		TSharedPtr<FJsonObject> OutputObj = OutputValue.IsValid() ? OutputValue->AsObject() : nullptr;
		if (!OutputObj.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Output item %d is not a valid object"), i);
			continue;
		}

		FString OutputType;
		OutputObj->TryGetStringField(TEXT("type"), OutputType);
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Output item %d type: %s"), i, *OutputType);

		if (OutputType == TEXT("function_call"))
		{
			// Direct function_call output item
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Processing function_call output"));
			
			// Log all fields in the function_call object to understand its structure
			TArray<FString> FunctionCallFields;
			OutputObj->Values.GetKeys(FunctionCallFields);
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: function_call fields: %s"), *FString::Join(FunctionCallFields, TEXT(", ")));
			
			FToolCallInfo Info;
			
			// Try different possible field names for the function call ID
			// The Responses API uses "call_id" for the function call ID
			if (!OutputObj->TryGetStringField(TEXT("call_id"), Info.Id))
			{
				if (!OutputObj->TryGetStringField(TEXT("id"), Info.Id))
				{
					OutputObj->TryGetStringField(TEXT("function_call_id"), Info.Id);
				}
			}
			
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Extracted function call id: %s"), *Info.Id);
			
			// Try to get function information - could be nested under "function" or directly in the object
			const TSharedPtr<FJsonObject>* FunctionObjPtr = nullptr;
			if (OutputObj->TryGetObjectField(TEXT("function"), FunctionObjPtr) && FunctionObjPtr && FunctionObjPtr->IsValid())
			{
				const TSharedPtr<FJsonObject> FunctionObj = *FunctionObjPtr;
				FunctionObj->TryGetStringField(TEXT("name"), Info.Name);
				FunctionObj->TryGetStringField(TEXT("arguments"), Info.Arguments);
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found function object - name: %s, args length: %d"), *Info.Name, Info.Arguments.Len());
			}
			else
			{
				// Try direct fields on the output object
				OutputObj->TryGetStringField(TEXT("name"), Info.Name);
				OutputObj->TryGetStringField(TEXT("function_name"), Info.Name);
				OutputObj->TryGetStringField(TEXT("arguments"), Info.Arguments);
				OutputObj->TryGetStringField(TEXT("function_arguments"), Info.Arguments);
				
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Trying direct fields - name: %s, args length: %d"), *Info.Name, Info.Arguments.Len());
			}

			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Final check - id: '%s' (len: %d), name: '%s' (len: %d)"), *Info.Id, Info.Id.Len(), *Info.Name, Info.Name.Len());
			
			if (!Info.Id.IsEmpty() && !Info.Name.IsEmpty())
			{
				ToolCalls.Add(Info); // Don't use MoveTemp - we need to keep the values
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found function call - id: %s, name: %s"), *Info.Id, *Info.Name);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: function_call output missing required fields - id: '%s', name: '%s'"), *Info.Id, *Info.Name);
				
				// Log the entire object as JSON for debugging
				FString FunctionCallJson;
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&FunctionCallJson);
				FJsonSerializer::Serialize(OutputObj.ToSharedRef(), Writer);
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: function_call object JSON: %s"), *FunctionCallJson);
			}
		}
		else if (OutputType == TEXT("file_search_call") || OutputType == TEXT("web_search_call"))
		{
			// Handle specialized OpenAI-hosted search tools (file_search / web_search).
			// These are executed entirely server-side; we SHOULD NOT send function_call_output
			// back for them, otherwise the Responses API will error with:
			// "No tool call found for function call output with call_id ...".
			// Instead, we only surface them to the UI via OnToolCall.
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Processing specialized search tool call output: %s"), *OutputType);

			const bool bIsFileSearch = (OutputType == TEXT("file_search_call"));
			const FString ToolName = bIsFileSearch ? TEXT("file_search") : TEXT("web_search");

			FString CallId;
			if (!OutputObj->TryGetStringField(TEXT("call_id"), CallId))
			{
				OutputObj->TryGetStringField(TEXT("id"), CallId);
			}

			// Build a compact arguments JSON object for the UI, primarily carrying the query text.
			TSharedPtr<FJsonObject> ArgsJson = MakeShareable(new FJsonObject);

			// New Responses API schema nests details under "file_search" / "web_search".
			const FString NestedField = bIsFileSearch ? TEXT("file_search") : TEXT("web_search");
			const TSharedPtr<FJsonObject>* NestedObjPtr = nullptr;
			if (OutputObj->TryGetObjectField(NestedField, NestedObjPtr) && NestedObjPtr && (*NestedObjPtr).IsValid())
			{
				const TSharedPtr<FJsonObject> NestedObj = *NestedObjPtr;

				FString Query;
				if (NestedObj->TryGetStringField(TEXT("query"), Query))
				{
					ArgsJson->SetStringField(TEXT("query"), Query);
				}

				// Copy any other fields through so the UI (or future logic) can inspect them if needed.
				for (const auto& Pair : NestedObj->Values)
				{
					if (Pair.Key != TEXT("query"))
					{
						ArgsJson->SetField(Pair.Key, Pair.Value);
					}
				}
			}
			else
			{
				// Fallback for older/alternative schemas – try a top-level "query" or "arguments" object.
				FString Query;
				if (OutputObj->TryGetStringField(TEXT("query"), Query))
				{
					ArgsJson->SetStringField(TEXT("query"), Query);
				}
				else
				{
					const TSharedPtr<FJsonObject>* ArgsObjPtr = nullptr;
					if (OutputObj->TryGetObjectField(TEXT("arguments"), ArgsObjPtr) && ArgsObjPtr && (*ArgsObjPtr).IsValid())
					{
						for (const auto& Pair : (*ArgsObjPtr)->Values)
						{
							ArgsJson->SetField(Pair.Key, Pair.Value);
						}
					}
				}
			}

			// Serialize arguments so Slate can display them (e.g. Documentation Search query text).
			FString ArgsString;
			{
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ArgsString);
				FJsonSerializer::Serialize(ArgsJson.ToSharedRef(), Writer);
			}

			// Broadcast to UI; ensure we hop to the game thread if needed.
			if (IsInGameThread())
			{
				OnToolCall.Broadcast(ToolName, ArgsString);
			}
			else
			{
				const FString ToolNameCopy = ToolName;
				const FString ArgsCopy = ArgsString;
				AsyncTask(ENamedThreads::GameThread, [this, ToolNameCopy, ArgsCopy]()
				{
					OnToolCall.Broadcast(ToolNameCopy, ArgsCopy);
				});
			}

			if (!CallId.IsEmpty())
			{
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Recorded specialized search call for UI only - id: %s, tool: %s, args_len: %d"),
					*CallId, *ToolName, ArgsString.Len());
			}

			// NOTE: Intentionally NOT adding this to ToolCalls; these are server-side tools and
			// should not produce local function_call_output entries.
		}
		else if (OutputType == TEXT("message"))
		{
			// Message output with content array
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Processing message output"));
			
			const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
			if (!OutputObj->TryGetArrayField(TEXT("content"), ContentArray) || !ContentArray)
			{
				UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Output message missing 'content' array"));
				continue;
			}
			
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Processing message with %d content items"), ContentArray->Num());

			for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
			{
				TSharedPtr<FJsonObject> ContentObj = ContentValue.IsValid() ? ContentValue->AsObject() : nullptr;
				if (!ContentObj.IsValid())
				{
					continue;
				}

				FString ContentType;
				ContentObj->TryGetStringField(TEXT("type"), ContentType);

				if (ContentType == TEXT("output_text") || ContentType == TEXT("text"))
				{
					FString TextChunk;
					if (ContentObj->TryGetStringField(TEXT("text"), TextChunk))
					{
						AccumulatedText += TextChunk;
					}
				}
				else if (ContentType == TEXT("reasoning") || ContentType == TEXT("thought"))
				{
					// Handle reasoning/thought content (e.g. from o1 models or custom agentic flows)
					FString ReasoningChunk;
					if (ContentObj->TryGetStringField(TEXT("text"), ReasoningChunk))
					{
						// Broadcast reasoning immediately for UI updates
						OnAgentReasoning.Broadcast(ReasoningChunk);
					}
				}
				else if (ContentType == TEXT("tool_call"))
				{
					const TSharedPtr<FJsonObject>* ToolCallObjPtr = nullptr;
					if (ContentObj->TryGetObjectField(TEXT("tool_call"), ToolCallObjPtr) && ToolCallObjPtr && ToolCallObjPtr->IsValid())
					{
						const TSharedPtr<FJsonObject> ToolCallObj = *ToolCallObjPtr;
						FToolCallInfo Info;
						ToolCallObj->TryGetStringField(TEXT("id"), Info.Id);

						const TSharedPtr<FJsonObject>* FunctionObjPtr = nullptr;
						if (ToolCallObj->TryGetObjectField(TEXT("function"), FunctionObjPtr) && FunctionObjPtr && FunctionObjPtr->IsValid())
						{
							const TSharedPtr<FJsonObject> FunctionObj = *FunctionObjPtr;
							FunctionObj->TryGetStringField(TEXT("name"), Info.Name);
							FunctionObj->TryGetStringField(TEXT("arguments"), Info.Arguments);
						}

						if (!Info.Id.IsEmpty() && !Info.Name.IsEmpty())
						{
							ToolCalls.Add(MoveTemp(Info));
						}
					}
				}
			}
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Skipping output item %d (type: %s)"), i, *OutputType);
			continue;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Accumulated text length: %d, Tool calls: %d"), AccumulatedText.Len(), ToolCalls.Num());
		
	if (ToolCalls.Num() > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Processing %d tool calls"), ToolCalls.Num());
		
		// Serialize tool_calls array for compatibility with existing history handling
		TArray<TSharedPtr<FJsonValue>> ToolCallsJsonArray;
		for (const FToolCallInfo& CallInfo : ToolCalls)
		{
			TSharedPtr<FJsonObject> ToolCallJson = MakeShareable(new FJsonObject);
			ToolCallJson->SetStringField(TEXT("id"), CallInfo.Id);
			ToolCallJson->SetStringField(TEXT("type"), TEXT("function"));

			TSharedPtr<FJsonObject> FunctionJson = MakeShareable(new FJsonObject);
			FunctionJson->SetStringField(TEXT("name"), CallInfo.Name);
			FunctionJson->SetStringField(TEXT("arguments"), CallInfo.Arguments);
			ToolCallJson->SetObjectField(TEXT("function"), FunctionJson);

			ToolCallsJsonArray.Add(MakeShareable(new FJsonValueObject(ToolCallJson)));
		}

		FString ToolCallsJsonString;
		TSharedRef<TJsonWriter<>> ToolCallsWriter = TJsonWriterFactory<>::Create(&ToolCallsJsonString);
		FJsonSerializer::Serialize(ToolCallsJsonArray, ToolCallsWriter);

		FAgentMessage AssistantMsg;
		AssistantMsg.Role = TEXT("assistant");
		AssistantMsg.Content = AccumulatedText;
		for (const FToolCallInfo& CallInfo : ToolCalls)
		{
			AssistantMsg.ToolCallIds.Add(CallInfo.Id);
		}
		AssistantMsg.ToolCallsJson = ToolCallsJsonString;
		ConversationHistory.Add(AssistantMsg);

		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added assistant message with tool calls to history"));

		// Always broadcast assistant messages to show agent thoughts and planning.
		// Even if AccumulatedText is empty, we should show something to indicate the agent is working.
		if (!AccumulatedText.IsEmpty())
		{
			OnAgentMessage.Broadcast(TEXT("assistant"), AccumulatedText, AssistantMsg.ToolCallIds);
		}
		else
		{
			// If there's no text but there are tool calls, show a brief message indicating the agent is executing tools
			OnAgentMessage.Broadcast(TEXT("assistant"), TEXT("Executing tools..."), AssistantMsg.ToolCallIds);
		}

		// Execute tool calls sequentially. Rely on the model's own reasoning
		// (plus safety guards like the MaxToolCallIterations setting) to decide when a
		// multi-step task is complete, using the actual tool outputs
		// (especially the JSON result from python_execute) as context.
		auto IsServerSideTool = [](const FString& ToolName) -> bool
		{
			return ToolName == TEXT("file_search") || ToolName == TEXT("web_search");
		};

		bool bHasClientSideTools = false;
		bool bHasAsyncReplicateTools = false;
		TArray<FString> ToolResults; // Store results for completion detection
		TArray<FString> ScreenshotImages; // Viewport screenshots to forward as image input
		
		// Store the assistant message index so we can ensure tool results are included
		const int32 AssistantMessageIndex = ConversationHistory.Num() - 1;

		for (const FToolCallInfo& CallInfo : ToolCalls)
		{
			const bool bIsScreenshot = (CallInfo.Name == TEXT("viewport_screenshot"));
			const bool bIsServerSideTool = IsServerSideTool(CallInfo.Name);
			const bool bIsAsyncReplicateTool = (CallInfo.Name == TEXT("replicate_generate"));

			if (!bIsServerSideTool)
			{
				bHasClientSideTools = true;
			}

			// For Replicate generation, execute asynchronously on a background thread to avoid blocking the editor.
			if (bIsAsyncReplicateTool)
			{
				bHasAsyncReplicateTools = true;

				const FString ToolNameCopy = CallInfo.Name;
				const FString ArgsCopy = CallInfo.Arguments;
				const FString CallIdCopy = CallInfo.Id;
				const int32 MaxToolResultSizeLocal = MaxToolResultSize;

				Async(EAsyncExecution::ThreadPool, [this, ToolNameCopy, ArgsCopy, CallIdCopy, MaxToolResultSizeLocal, bIsScreenshot]()
				{
					// Execute the Replicate tool (may block, but only on this background thread).
					const FString ToolResult = ExecuteToolCall(ToolNameCopy, ArgsCopy);

					// Prepare truncated version for history.
					FString ToolResultForHistory = ToolResult;
					if (ToolResultForHistory.Len() > MaxToolResultSizeLocal)
					{
						if (bIsScreenshot && ToolResultForHistory.StartsWith(TEXT("iVBORw0KGgo")))
						{
							ToolResultForHistory = TEXT("Screenshot captured successfully. [Base64 image data omitted from history to prevent context overflow - ")
								TEXT("the image was captured and can be viewed in the UI. Length: ") + FString::FromInt(ToolResult.Len()) + TEXT(" characters]");
						}
						else
						{
							ToolResultForHistory = ToolResultForHistory.Left(MaxToolResultSizeLocal) +
								TEXT("\n\n[Result truncated - original length: ") + FString::FromInt(ToolResult.Len()) +
								TEXT(" characters. Full result available in tool output.]");
						}
					}

					// Back on game thread: update history, broadcast result, and continue conversation.
					AsyncTask(ENamedThreads::GameThread, [this, CallIdCopy, ToolNameCopy, ToolResult, ToolResultForHistory]()
					{
						FAgentMessage ToolMsg;
						ToolMsg.Role = TEXT("tool");
						ToolMsg.ToolCallId = CallIdCopy;
						ToolMsg.Content = ToolResultForHistory;
						ConversationHistory.Add(ToolMsg);

						OnToolResult.Broadcast(CallIdCopy, ToolResult);

						// Continue conversation with tool result.
						SendMessage(TEXT(""), TArray<FString>());
					});
				});

				// Do not execute synchronously here.
				continue;
			}

			// Synchronous execution for non-MCP tools (existing behavior).
			FString ToolResult = ExecuteToolCall(CallInfo.Name, CallInfo.Arguments);
			ToolResults.Add(ToolResult); // Store for completion detection

			// If this tool captured a viewport screenshot, keep the raw base64 PNG so we can
			// send it back to the model as multimodal input on the very next request. This
			// lets the agent actually *see* the scene when it calls viewport_screenshot,
			// instead of only getting a textual confirmation in the tool result.
			if (bIsScreenshot && !ToolResult.IsEmpty())
			{
				ScreenshotImages.Add(ToolResult);
			}

			// Truncate or summarize large tool results to prevent context window overflow
			FString ToolResultForHistory = ToolResult;
			if (ToolResultForHistory.Len() > MaxToolResultSize)
			{
				if (bIsScreenshot && ToolResultForHistory.StartsWith(TEXT("iVBORw0KGgo"))) // Base64 PNG header
				{
					// For screenshots, replace base64 with a summary
					ToolResultForHistory = TEXT("Screenshot captured successfully. [Base64 image data omitted from history to prevent context overflow - ")
						TEXT("the image was captured and can be viewed in the UI. Length: ") + FString::FromInt(ToolResult.Len()) + TEXT(" characters]");
					UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Truncated large screenshot result (%d chars) to prevent context overflow"), ToolResult.Len());
				}
				else
				{
					// For other large results, truncate with a note
					ToolResultForHistory = ToolResultForHistory.Left(MaxToolResultSize) + 
						TEXT("\n\n[Result truncated - original length: ") + FString::FromInt(ToolResult.Len()) + 
						TEXT(" characters. Full result available in tool output.]");
					UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Truncated large tool result (%d chars) to prevent context overflow"), ToolResult.Len());
				}
			}

			FAgentMessage ToolMsg;
			ToolMsg.Role = TEXT("tool");
			ToolMsg.ToolCallId = CallInfo.Id;
			
			// For python_execute results with success status, append a completion evaluation prompt
			if (CallInfo.Name == TEXT("python_execute") && !ToolResult.IsEmpty() && ToolResult.StartsWith(TEXT("{")))
			{
				TSharedPtr<FJsonObject> ResultObj;
				TSharedRef<TJsonReader<>> PythonResultReader = TJsonReaderFactory<>::Create(ToolResult);
				if (FJsonSerializer::Deserialize(PythonResultReader, ResultObj) && ResultObj.IsValid())
				{
					FString PythonStatus;
					if (ResultObj->TryGetStringField(TEXT("status"), PythonStatus) && PythonStatus == TEXT("ok"))
					{
						// Always append completion evaluation instruction to successful python_execute results
						// This ensures the agent reasons about completion after each code execution
						FString CompletionPrompt = TEXT("\n\n[System: After this python_execute, you MUST reason about whether the task is complete. ")
							TEXT("Consider: Does the JSON result indicate success, but always use verification tools to check the scene state.")
							TEXT("If the task appears complete, provide a brief confirmation and STOP. ")
							TEXT("If you need to verify, use scene_query or viewport_screenshot to check the scene state. ")
							TEXT("Do NOT execute another python_execute unless verification clearly shows the task failed or is incomplete.]");
						
						ToolMsg.Content = ToolResultForHistory + CompletionPrompt;
						UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added completion evaluation prompt to python_execute result"));
					}
					else
					{
						ToolMsg.Content = ToolResultForHistory;
					}
				}
				else
				{
					ToolMsg.Content = ToolResultForHistory;
				}
			}
			// For scene_query results, append a completion evaluation prompt
			else if (CallInfo.Name == TEXT("scene_query") && !ToolResult.IsEmpty() && ToolResult != TEXT("[]") && ToolResult.StartsWith(TEXT("[")))
			{
				// Check if scene_query found results
				TSharedPtr<FJsonValue> JsonValue;
				TSharedRef<TJsonReader<>> SceneQueryReader = TJsonReaderFactory<>::Create(ToolResult);
				if (FJsonSerializer::Deserialize(SceneQueryReader, JsonValue) && JsonValue.IsValid())
				{
					const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
					if (JsonValue->Type == EJson::Array && JsonValue->TryGetArray(JsonArray) && JsonArray->Num() > 0)
					{
						// Append completion evaluation instruction to the tool result
						ToolMsg.Content = ToolResultForHistory + TEXT("\n\n[System: Based on these scene_query results and previous tool outputs, evaluate whether the user's request has been completed. If the task is complete, provide a brief confirmation message and STOP. If not complete, continue with next steps.]");
						UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added completion evaluation prompt to scene_query result"));
					}
					else
					{
						ToolMsg.Content = ToolResultForHistory;
					}
				}
				else
				{
					ToolMsg.Content = ToolResultForHistory;
				}
			}
			else
			{
				ToolMsg.Content = ToolResultForHistory;
			}
			
			ConversationHistory.Add(ToolMsg);

			// Always broadcast the full result to UI (not truncated)
			OnToolResult.Broadcast(CallInfo.Id, ToolResult);
		}

		if (!bHasClientSideTools)
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: All executed tools were server-side. Waiting for server to continue or user input."));
			ToolCallIterationCount = 0;
			return;
		}

		// If we scheduled any async Replicate tools, do not continue the conversation here.
		// The async callbacks will add tool results and call SendMessage("") when ready.
		if (bHasAsyncReplicateTools)
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Async Replicate tool calls scheduled; waiting for completion callbacks to continue conversation."));
			return;
		}

		// Continue conversation with tool results
		// For Responses API, this will use previous_response_id and include tool results in input
		// For legacy API, this will include full conversation history
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Continuing conversation after tool execution (iteration %d)"), ToolCallIterationCount + 1);
		
		// Verify tool results are in conversation history before continuing
		// This helps debug issues where tool results might not be included
		int32 RecentToolResults = 0;
		for (int32 i = ConversationHistory.Num() - 1; i >= FMath::Max(0, ConversationHistory.Num() - 10); --i)
		{
			if (ConversationHistory[i].Role == TEXT("tool"))
			{
				RecentToolResults++;
				UE_LOG(LogTemp, VeryVerbose, TEXT("UnrealGPT: Found tool result in history at index %d: call_id=%s"), 
					i, *ConversationHistory[i].ToolCallId);
			}
		}
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found %d recent tool results in conversation history"), RecentToolResults);
		
		// Check if we've exceeded max iterations (check before incrementing in SendMessage)
		const int32 MaxIterations = FMath::Max(1, Settings ? Settings->MaxToolCallIterations : 25);
		if (ToolCallIterationCount >= MaxIterations - 1)
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Reached maximum tool call iterations (%d). Stopping to prevent infinite loop."), MaxIterations);
			ToolCallIterationCount = 0;
			bRequestInProgress = false;
			return;
		}
		
		// Continue with empty message to include tool results. If we captured any viewport
		// screenshots, forward them as image input so the model can analyze the viewport.
		SendMessage(TEXT(""), ScreenshotImages);
		return;
	}

	if (!AccumulatedText.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Processing regular assistant message (no tool calls)"));
		FAgentMessage AssistantMsg;
		AssistantMsg.Role = TEXT("assistant");
		AssistantMsg.Content = AccumulatedText;
		ConversationHistory.Add(AssistantMsg);

		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added assistant message to history, broadcasting"));
		OnAgentMessage.Broadcast(TEXT("assistant"), AccumulatedText, TArray<FString>());
		
		// Reset tool call iteration counter on successful completion
		ToolCallIterationCount = 0;
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Message output had no text content and no tool calls"));
		// Reset counter if we got a response but it had no content
		ToolCallIterationCount = 0;
	}
}

FString UUnrealGPTAgentClient::ExecuteToolCall(const FString& ToolName, const FString& ArgumentsJson)
{
	FString Result;

	const bool bIsPythonExecute = (ToolName == TEXT("python_execute"));
	const bool bIsSceneQuery = (ToolName == TEXT("scene_query"));

	if (bIsPythonExecute)
	{
		TSharedPtr<FJsonObject> ArgsObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
		if (FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid())
		{
			FString Code;
			if (ArgsObj->TryGetStringField(TEXT("code"), Code))
			{
				Result = ExecutePythonCode(Code);
			}
		}
	}
	// else if (ToolName == TEXT("computer_use"))
	// {
	// 	TSharedPtr<FJsonObject> ArgsObj;
	// 	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	// 	if (FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid())
	// 	{
	// 		FString Action;
	// 		if (ArgsObj->TryGetStringField(TEXT("action"), Action))
	// 		{
	// 			Result = ExecuteComputerUse(Action);
	// 		}
	// 	}
	// }
	else if (ToolName == TEXT("viewport_screenshot"))
	{
		Result = GetViewportScreenshot();
	}
	else if (ToolName == TEXT("scene_query"))
	{
		// Pass the raw JSON arguments through to the scene query helper.
		// The JSON can specify filters like class_contains, label_contains,
		// name_contains, component_class_contains, and max_results.
		Result = UUnrealGPTSceneContext::QueryScene(ArgumentsJson);
		
		// Check if scene_query returned results (non-empty JSON array).
		// If it did, mark that we found results so we can block subsequent python_execute calls.
		// scene_query returns a JSON array string, so check if it's not just "[]"
		bLastSceneQueryFoundResults = !Result.IsEmpty() && Result != TEXT("[]") && Result.StartsWith(TEXT("["));
		if (bLastSceneQueryFoundResults)
		{
			// Try to parse and verify it's actually a non-empty array
			TSharedPtr<FJsonValue> JsonValue;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Result);
			if (FJsonSerializer::Deserialize(Reader, JsonValue) && JsonValue.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
				if (JsonValue->Type == EJson::Array && JsonValue->TryGetArray(JsonArray))
				{
					bLastSceneQueryFoundResults = (JsonArray->Num() > 0);
					if (bLastSceneQueryFoundResults)
					{
						UE_LOG(LogTemp, Log, TEXT("UnrealGPT: scene_query found %d results - will block subsequent python_execute"), JsonArray->Num());
					}
				}
				else
				{
					bLastSceneQueryFoundResults = false;
				}
			}
			else
			{
				bLastSceneQueryFoundResults = false;
			}
		}
	}
	else if (ToolName == TEXT("reflection_query"))
	{
		TSharedPtr<FJsonObject> ArgsObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
		if (!(FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid()))
		{
			return TEXT("{\"status\":\"error\",\"message\":\"Failed to parse reflection_query arguments\"}");
		}

		FString ClassName;
		if (!ArgsObj->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
		{
			return TEXT("{\"status\":\"error\",\"message\":\"Missing required field: class_name\"}");
		}

		// Try to resolve the class by short name first, then by treating the
		// input as a fully-qualified object path.
		UClass* TargetClass = FindObject<UClass>(ANY_PACKAGE, *ClassName);
		if (!TargetClass)
		{
			TargetClass = LoadObject<UClass>(nullptr, *ClassName);
		}

		Result = BuildReflectionSchemaJson(TargetClass);
	}
	else if (ToolName == TEXT("replicate_generate"))
	{
		// Dedicated Replicate generation helper using the Replicate HTTP API directly (no MCP).
		if (!Settings)
		{
			Settings = GetMutableDefault<UUnrealGPTSettings>();
		}

		if (!Settings || !Settings->bEnableReplicateTool || Settings->ReplicateApiToken.IsEmpty())
		{
			return TEXT("{\"status\":\"error\",\"message\":\"Replicate tool is not enabled or API token is missing in settings\"}");
		}

		TSharedPtr<FJsonObject> ArgsObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
		if (!(FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid()))
		{
			return TEXT("{\"status\":\"error\",\"message\":\"Failed to parse replicate_generate arguments\"}");
		}

		FString Prompt;
		if (!(ArgsObj->TryGetStringField(TEXT("prompt"), Prompt) && !Prompt.IsEmpty()))
		{
			return TEXT("{\"status\":\"error\",\"message\":\"Missing required field: prompt\"}");
		}

		FString OutputKind;
		ArgsObj->TryGetStringField(TEXT("output_kind"), OutputKind);
		OutputKind = OutputKind.ToLower();
		if (OutputKind.IsEmpty())
		{
			OutputKind = TEXT("image");
		}

		// Resolve the effective Replicate model version to use:
		// 1) If the tool call explicitly provided a 'version', respect that.
		// 2) Otherwise, pick a default per output kind from settings where possible.
		FString Version;
		if (!ArgsObj->TryGetStringField(TEXT("version"), Version) || Version.IsEmpty())
		{
			if (OutputKind == TEXT("image"))
			{
				Version = Settings->ReplicateImageModel;
			}
			else if (OutputKind == TEXT("video"))
			{
				Version = Settings->ReplicateVideoModel;
			}
			else if (OutputKind == TEXT("audio"))
			{
				// For audio we distinguish SFX vs music via conventions in the prompt;
				// by default prefer the SFX model, and the model itself can be a music model if desired.
				Version = Settings->ReplicateSFXModel;
			}
			else if (OutputKind == TEXT("3d") || OutputKind == TEXT("3d_model") || OutputKind == TEXT("model") || OutputKind == TEXT("mesh"))
			{
				Version = Settings->Replicate3DModel;
			}

			// If still empty, try additional hints from optional 'output_subkind',
			// e.g. 'sfx', 'music', or 'speech' for audio cases.
			if (Version.IsEmpty())
			{
				FString OutputSubkind;
				if (ArgsObj->TryGetStringField(TEXT("output_subkind"), OutputSubkind))
				{
					OutputSubkind = OutputSubkind.ToLower();

					if (OutputSubkind == TEXT("sfx"))
					{
						Version = Settings->ReplicateSFXModel;
					}
					else if (OutputSubkind == TEXT("music"))
					{
						Version = Settings->ReplicateMusicModel;
					}
					else if (OutputSubkind == TEXT("speech") || OutputSubkind == TEXT("voice"))
					{
						Version = Settings->ReplicateSpeechModel;
					}
				}
			}
		}

		// Detect when the configured identifier looks like an owner/name model slug
		// instead of a raw version id. For official models, Replicate supports
		// POST /v1/models/{owner}/{name}/predictions without a version field.
		const bool bLooksLikeModelSlug = Version.Contains(TEXT("/"));

		// If we still don't have any identifier at this point, fail fast with a clear error
		// instead of sending an invalid request to Replicate.
		if (Version.IsEmpty())
		{
			return TEXT("{\"status\":\"error\",\"message\":\"Replicate prediction requires a model identifier. Configure a default model (owner/name slug or version id) in UnrealGPT settings or pass 'version' explicitly in replicate_generate arguments.\"}");
		}

		// Build Replicate prediction request body.
		TSharedPtr<FJsonObject> RequestObj = MakeShareable(new FJsonObject);

		TSharedPtr<FJsonObject> InputObj = MakeShareable(new FJsonObject);
		InputObj->SetStringField(TEXT("prompt"), Prompt);

		// For image generation, request PNG output directly from the model where supported.
		// Many Replicate image models accept an 'output_format' parameter; models that do not
		// simply ignore unknown fields, so this is safe as a default.
		if (OutputKind == TEXT("image"))
		{
			InputObj->SetStringField(TEXT("output_format"), TEXT("png"));
		}

		RequestObj->SetObjectField(TEXT("input"), InputObj);

		FString RequestBody;
		{
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
			FJsonSerializer::Serialize(RequestObj.ToSharedRef(), Writer);
		}

		FString ApiUrl = Settings->ReplicateApiUrl.IsEmpty()
			? TEXT("https://api.replicate.com/v1/predictions")
			: Settings->ReplicateApiUrl;

		// If the identifier looks like an owner/name slug and we are using the default
		// predictions endpoint, route this call through the official models endpoint
		// so the user does not need to look up a separate version id.
		const bool bIsDefaultPredictionsEndpoint =
			(ApiUrl == TEXT("https://api.replicate.com/v1/predictions") || ApiUrl.EndsWith(TEXT("/v1/predictions")));

		const bool bUseOfficialModelsEndpoint = bLooksLikeModelSlug && bIsDefaultPredictionsEndpoint;

		if (bUseOfficialModelsEndpoint)
		{
			ApiUrl = FString::Printf(TEXT("https://api.replicate.com/v1/models/%s/predictions"), *Version);
		}
		else
		{
			// For the unified predictions endpoint, send the identifier as the 'version' field.
			RequestObj->SetStringField(TEXT("version"), Version);
		}

		// Helper lambda to perform a blocking HTTP request (runs on background thread for this tool).
		auto PerformHttpRequest = [](const FString& Url, const FString& Verb, const FString& Body, const FString& AuthToken, FString& OutResponse, int32 TimeoutSeconds) -> bool
		{
			TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
			Request->SetURL(Url);
			Request->SetVerb(Verb);
			Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			// Replicate HTTP API expects Bearer tokens: Authorization: Bearer <token>
			Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AuthToken));

			if (!Body.IsEmpty())
			{
				Request->SetContentAsString(Body);
			}

			bool bRequestComplete = false;
			bool bSuccess = false;

			Request->OnProcessRequestComplete().BindLambda(
				[&](FHttpRequestPtr Req, FHttpResponsePtr Res, bool bConnected)
				{
					if (bConnected && Res.IsValid() && Res->GetResponseCode() >= 200 && Res->GetResponseCode() < 300)
					{
						OutResponse = Res->GetContentAsString();
						bSuccess = true;
					}
					else if (Res.IsValid())
					{
						OutResponse = Res->GetContentAsString();
					}
					bRequestComplete = true;
				});

			Request->ProcessRequest();

			const double StartTime = FPlatformTime::Seconds();
			while (!bRequestComplete)
			{
				FPlatformProcess::Sleep(0.01f);
				if (FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
				{
					Request->CancelRequest();
					OutResponse = TEXT("{\"error\":\"Request timed out\"}");
					return false;
				}
			}

			return bSuccess;
		};

		// 1) Create prediction
		FString CreateResponse;
		if (!PerformHttpRequest(ApiUrl, TEXT("POST"), RequestBody, Settings->ReplicateApiToken, CreateResponse, 60))
		{
			return FString::Printf(TEXT("{\"status\":\"error\",\"message\":\"Failed to create Replicate prediction: %s\"}"), *CreateResponse);
		}

		TSharedPtr<FJsonObject> CreateObj;
		{
			TSharedRef<TJsonReader<>> CreateReader = TJsonReaderFactory<>::Create(CreateResponse);
			if (!(FJsonSerializer::Deserialize(CreateReader, CreateObj) && CreateObj.IsValid()))
			{
				return TEXT("{\"status\":\"error\",\"message\":\"Failed to parse Replicate create prediction response\"}");
			}
		}

		FString PollUrl;
		{
			const TSharedPtr<FJsonObject>* UrlsObj = nullptr;
			if (CreateObj->TryGetObjectField(TEXT("urls"), UrlsObj) && UrlsObj && UrlsObj->IsValid())
			{
				(*UrlsObj)->TryGetStringField(TEXT("get"), PollUrl);
			}
		}

		if (PollUrl.IsEmpty())
		{
			return TEXT("{\"status\":\"error\",\"message\":\"Replicate response did not include a poll URL\"}");
		}

		// 2) Poll prediction until it completes
		FString FinalResponse;
		TSharedPtr<FJsonObject> FinalObj;
		const int32 MaxPollSeconds = 300;
		const double PollStart = FPlatformTime::Seconds();
		while (true)
		{
			if (!PerformHttpRequest(PollUrl, TEXT("GET"), FString(), Settings->ReplicateApiToken, FinalResponse, 60))
			{
				return FString::Printf(TEXT("{\"status\":\"error\",\"message\":\"Failed while polling Replicate prediction: %s\"}"), *FinalResponse);
			}

			TSharedRef<TJsonReader<>> FinalReader = TJsonReaderFactory<>::Create(FinalResponse);
			if (!(FJsonSerializer::Deserialize(FinalReader, FinalObj) && FinalObj.IsValid()))
			{
				return TEXT("{\"status\":\"error\",\"message\":\"Failed to parse Replicate poll response\"}");
			}

			FString PredStatus;
			FinalObj->TryGetStringField(TEXT("status"), PredStatus);

			if (PredStatus == TEXT("succeeded"))
			{
				break;
			}

			if (PredStatus == TEXT("failed") || PredStatus == TEXT("canceled"))
			{
				FString ErrorMsg;
				FinalObj->TryGetStringField(TEXT("error"), ErrorMsg);
				return FString::Printf(TEXT("{\"status\":\"error\",\"message\":\"Replicate prediction %s: %s\"}"), *PredStatus, *ErrorMsg);
			}

			if (FPlatformTime::Seconds() - PollStart > MaxPollSeconds)
			{
				return TEXT("{\"status\":\"error\",\"message\":\"Replicate prediction polling timed out\"}");
			}

			FPlatformProcess::Sleep(0.5f);
		}

		// 3) Extract output URIs. For most models, 'output' is an array of URLs,
		// but some return nested objects or arrays containing HTTPS URLs.
		TArray<FString> OutputUris;
		TFunction<void(const TSharedPtr<FJsonValue>&)> CollectUrisFromJsonValue;
		CollectUrisFromJsonValue = [&OutputUris, &CollectUrisFromJsonValue](const TSharedPtr<FJsonValue>& Val)
		{
			if (!Val.IsValid())
			{
				return;
			}

			switch (Val->Type)
			{
			case EJson::String:
				{
					const FString Str = Val->AsString();
					if (Str.StartsWith(TEXT("http://")) || Str.StartsWith(TEXT("https://")))
					{
						OutputUris.AddUnique(Str);
					}
					break;
				}
			case EJson::Array:
				{
					const TArray<TSharedPtr<FJsonValue>>& Arr = Val->AsArray();
					for (const TSharedPtr<FJsonValue>& Elem : Arr)
					{
						CollectUrisFromJsonValue(Elem);
					}
					break;
				}
			case EJson::Object:
				{
					const TSharedPtr<FJsonObject> Obj = Val->AsObject();
					if (Obj.IsValid())
					{
						for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
						{
							CollectUrisFromJsonValue(Pair.Value);
						}
					}
					break;
				}
			default:
				break;
			}
		};

		// Prefer the 'output' field if present.
		{
			const TArray<TSharedPtr<FJsonValue>>* OutputArray = nullptr;
			if (FinalObj->TryGetArrayField(TEXT("output"), OutputArray) && OutputArray)
			{
				for (const TSharedPtr<FJsonValue>& Val : *OutputArray)
				{
					CollectUrisFromJsonValue(Val);
				}
			}
			else
			{
				const TSharedPtr<FJsonObject>* OutputObj = nullptr;
				if (FinalObj->TryGetObjectField(TEXT("output"), OutputObj) && OutputObj && OutputObj->IsValid())
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*OutputObj)->Values)
					{
						CollectUrisFromJsonValue(Pair.Value);
					}
				}
				else
				{
					FString OutputStr;
					if (FinalObj->TryGetStringField(TEXT("output"), OutputStr) &&
						(OutputStr.StartsWith(TEXT("http://")) || OutputStr.StartsWith(TEXT("https://"))))
					{
						OutputUris.AddUnique(OutputStr);
					}
				}
			}
		}

		// As a fallback, scan the entire response object for HTTPS URLs if we didn't find any in 'output'.
		if (OutputUris.Num() == 0)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : FinalObj->Values)
			{
				CollectUrisFromJsonValue(Pair.Value);
			}
		}

		// Helper to map output kind to a staging subfolder.
		auto GetReplicateStagingFolder = [](const FString& Kind) -> FString
		{
			const FString BasePath = FPaths::ProjectContentDir() / TEXT("UnrealGPT/Generated");
			const FString K = Kind.ToLower();

			if (K == TEXT("image"))
			{
				return BasePath / TEXT("Images");
			}
			if (K == TEXT("audio"))
			{
				return BasePath / TEXT("Audio");
			}
			if (K == TEXT("video"))
			{
				return BasePath / TEXT("Video");
			}
			if (K == TEXT("3d") || K == TEXT("3d_model") || K == TEXT("model") || K == TEXT("mesh"))
			{
				return BasePath / TEXT("Models");
			}

			return BasePath / TEXT("Misc");
		};

		// Helper to download a file from URL to a staging folder.
		auto DownloadReplicateFile = [&](const FString& Uri, const FString& Kind) -> FString
		{
			TArray<uint8> Content;

			// Try HTTP download.
			TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
			Request->SetURL(Uri);
			Request->SetVerb(TEXT("GET"));
			// Replicate file URLs may require the same Bearer token.
			Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Settings->ReplicateApiToken));

			bool bComplete = false;

			Request->OnProcessRequestComplete().BindLambda(
				[&](FHttpRequestPtr Req, FHttpResponsePtr Res, bool bSuccess)
				{
					if (bSuccess && Res.IsValid() && Res->GetResponseCode() == 200)
					{
						Content = Res->GetContent();
					}
					bComplete = true;
				});

			Request->ProcessRequest();

			const double StartTime = FPlatformTime::Seconds();
			while (!bComplete)
			{
				FPlatformProcess::Sleep(0.01f);
				if (FPlatformTime::Seconds() - StartTime > 120.0)
				{
					Request->CancelRequest();
					break;
				}
			}

			if (Content.Num() == 0)
			{
				return FString();
			}

			FString Ext = FPaths::GetExtension(Uri);
			if (Ext.IsEmpty())
			{
				Ext = TEXT("dat");
			}

			const FString Filename = FGuid::NewGuid().ToString() + TEXT(".") + Ext;
			const FString SavePath = GetReplicateStagingFolder(Kind) / Filename;

			if (FFileHelper::SaveArrayToFile(Content, *SavePath))
			{
				return SavePath;
			}

			return FString();
		};

		// 4) Download any output files and build result JSON.
		TArray<TSharedPtr<FJsonValue>> FilesArray;
		for (const FString& Uri : OutputUris)
		{
			const FString LocalPath = DownloadReplicateFile(Uri, OutputKind);
			if (!LocalPath.IsEmpty())
			{
				TSharedPtr<FJsonObject> FileObj = MakeShareable(new FJsonObject);
				FileObj->SetStringField(TEXT("local_path"), LocalPath);
				FileObj->SetStringField(TEXT("mime_type"), FPaths::GetExtension(LocalPath));
				FileObj->SetStringField(TEXT("description"), TEXT("Downloaded output from Replicate prediction"));
				FilesArray.Add(MakeShareable(new FJsonValueObject(FileObj)));
			}
		}

		TSharedPtr<FJsonObject> ResultObj = MakeShareable(new FJsonObject);
		ResultObj->SetStringField(TEXT("status"), TEXT("success"));

		const int32 NumFiles = FilesArray.Num();
		ResultObj->SetStringField(
			TEXT("message"),
			FString::Printf(TEXT("Replicate prediction succeeded with %d downloaded file(s)."), NumFiles));

		TSharedPtr<FJsonObject> DetailsObj = MakeShareable(new FJsonObject);
		DetailsObj->SetStringField(TEXT("provider"), TEXT("replicate"));
		DetailsObj->SetStringField(TEXT("output_kind"), OutputKind);
		DetailsObj->SetArrayField(TEXT("files"), FilesArray);

		ResultObj->SetObjectField(TEXT("details"), DetailsObj);

		FString ResultJsonString;
		{
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultJsonString);
			FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);
		}

		Result = ResultJsonString;
	}
	else if (ToolName == TEXT("file_search") || ToolName == TEXT("web_search"))
	{
		// These are server-side tools executed by the model/platform. 
		// We just acknowledge them to avoid "Unknown tool" errors and allow the loop to continue.
		// The actual results are typically incorporated into the model's context or subsequent messages.
		Result = FString::Printf(TEXT("Tool '%s' executed successfully by server."), *ToolName);
	}
	else
	{
		Result = FString::Printf(TEXT("Unknown tool: %s"), *ToolName);
	}

	// Track last tool type so we can avoid repeated python_execute runs.
	bLastToolWasPythonExecute = bIsPythonExecute;
	
	// Reset scene_query results flag if we're running a different tool (not scene_query)
	if (!bIsSceneQuery)
	{
		bLastSceneQueryFoundResults = false;
	}

	// Ensure OnToolCall delegate (which can touch Slate/UI) is always broadcast on the game thread.
	if (IsInGameThread())
	{
		OnToolCall.Broadcast(ToolName, ArgumentsJson);
	}
	else
	{
		const FString ToolNameCopy = ToolName;
		const FString ArgsCopy = ArgumentsJson;
		AsyncTask(ENamedThreads::GameThread, [this, ToolNameCopy, ArgsCopy]()
		{
			OnToolCall.Broadcast(ToolNameCopy, ArgsCopy);
		});
	}

	return Result;
}

// Helper to indent arbitrary Python source one level (4 spaces), preserving empty lines.
static FString IndentPythonCode(const FString& Code)
{
	FString Indented;
	TArray<FString> Lines;
	Code.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		if (Line.TrimStartAndEnd().IsEmpty())
		{
			Indented += TEXT("    \n");
		}
		else
		{
			Indented += TEXT("    ") + Line + TEXT("\n");
		}
	}

	return Indented;
}

FString UUnrealGPTAgentClient::ExecutePythonCode(const FString& Code)
{
	if (!IPythonScriptPlugin::Get()->IsPythonAvailable())
	{
		return TEXT("Error: Python is not available in this Unreal Engine installation");
	}

	// Use a deterministic result file the Python wrapper can write to.
	const FString ResultFilePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("UnrealGPT_PythonResult.json"));

	// Clear any previous result.
	IFileManager::Get().Delete(*ResultFilePath, /*RequireExists=*/false, /*EvenIfReadOnly=*/true);

	const FString IndentedUserCode = IndentPythonCode(Code);

	// Build a small wrapper that:
	// - defines a default JSON-serializable `result` dict,
	// - runs the user code inside try/except (where the code can modify `result`),
	// - writes the final `result` to ResultFilePath as JSON.
	FString WrappedCode;
	WrappedCode += TEXT("import json, traceback, os\n");
	WrappedCode += TEXT("import unreal\n");
	WrappedCode += TEXT("result = {\n");
	WrappedCode += TEXT("    \"status\": \"ok\",\n");
	WrappedCode += TEXT("    \"message\": \"Python code executed. No custom result message was set.\",\n");
	WrappedCode += TEXT("    \"details\": {}\n");
	WrappedCode += TEXT("}\n\n");
	WrappedCode += TEXT("try:\n");
	WrappedCode += IndentedUserCode;
	WrappedCode += TEXT("except Exception as e:\n");
	WrappedCode += TEXT("    result[\"status\"] = \"error\"\n");
	WrappedCode += TEXT("    result[\"message\"] = str(e)\n");
	WrappedCode += TEXT("    result[\"details\"][\"traceback\"] = traceback.format_exc()\n\n");

	const FString EscapedPath = ResultFilePath.ReplaceCharWithEscapedChar();
	WrappedCode += TEXT("result_path = \"") + EscapedPath + TEXT("\"\n");
	WrappedCode += TEXT("with open(result_path, \"w\", encoding=\"utf-8\") as f:\n");
	WrappedCode += TEXT("    f.write(json.dumps(result))\n");

	IPythonScriptPlugin::Get()->ExecPythonCommand(*WrappedCode);

	// Try to read the JSON result back from disk so the agent can reason about it.
	FString ResultJson;
	if (FPaths::FileExists(ResultFilePath))
	{
		if (FFileHelper::LoadFileToString(ResultJson, *ResultFilePath))
		{
			// Try to focus viewport on the last created asset
			FocusViewportOnCreatedAsset(ResultJson);
			return ResultJson;
		}
	}

	// Fallback if no structured result was produced.
	return TEXT(
		"Python code was sent to the Unreal Editor for execution, but no structured result JSON "
		"was produced. The script may have succeeded or failed; check the Unreal Python log for "
		"details, and consider writing to the shared `result` dict for future runs.");
}

// FString UUnrealGPTAgentClient::ExecuteComputerUse(const FString& ActionJson)
// {
// 	return UUnrealGPTComputerUse::ExecuteAction(ActionJson);
// }

FString UUnrealGPTAgentClient::GetViewportScreenshot()
{
	// This will be implemented in the scene context provider
	return UUnrealGPTSceneContext::CaptureViewportScreenshot();
}

FString UUnrealGPTAgentClient::GetSceneSummary(int32 PageSize)
{
	// This will be implemented in the scene context provider
	return UUnrealGPTSceneContext::GetSceneSummary(PageSize);
}

void UUnrealGPTAgentClient::FocusViewportOnCreatedAsset(const FString& ResultJson)
{
	if (!GEditor || ResultJson.IsEmpty())
	{
		return;
	}

	// Parse the result JSON to check for created asset information
	TSharedPtr<FJsonObject> ResultObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
	if (!FJsonSerializer::Deserialize(Reader, ResultObj) || !ResultObj.IsValid())
	{
		return;
	}

	// Check if the operation was successful
	FString Status;
	if (!ResultObj->TryGetStringField(TEXT("status"), Status) || Status != TEXT("ok"))
	{
		return; // Don't focus on failed operations
	}

	// Check for actor information in details
	const TSharedPtr<FJsonObject>* DetailsObj = nullptr;
	if (!ResultObj->TryGetObjectField(TEXT("details"), DetailsObj) || !DetailsObj || !DetailsObj->IsValid())
	{
		return;
	}

	FString ActorName;
	FString ActorLabel;
	FString AssetPath;

	(*DetailsObj)->TryGetStringField(TEXT("actor_name"), ActorName);
	(*DetailsObj)->TryGetStringField(TEXT("actor_label"), ActorLabel);
	(*DetailsObj)->TryGetStringField(TEXT("asset_path"), AssetPath);

	// If we have actor information, try to find and focus on the actor
	if (!ActorName.IsEmpty() || !ActorLabel.IsEmpty())
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			return;
		}

		AActor* FoundActor = nullptr;

		// Search for actor by name or label
		for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
		{
			AActor* Actor = *ActorItr;
			if (!Actor || Actor->IsPendingKillPending())
			{
				continue;
			}

			// Check by name or label
			if ((!ActorName.IsEmpty() && Actor->GetName() == ActorName) ||
				(!ActorLabel.IsEmpty() && Actor->GetActorLabel() == ActorLabel))
			{
				FoundActor = Actor;
				break;
			}
		}

		if (FoundActor)
		{
			// Select the actor
			GEditor->SelectActor(FoundActor, true, true);
			
			// Focus viewport cameras on the actor (pass by reference)
			GEditor->MoveViewportCamerasToActor(*FoundActor, false);
			
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Focused viewport on created actor: %s"), *FoundActor->GetActorLabel());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Could not find created actor (name: %s, label: %s)"), *ActorName, *ActorLabel);
		}
	}
	// If we have asset path information, we could focus Content Browser on it
	// For now, we'll focus on actors in the level
}

TSharedRef<IHttpRequest> UUnrealGPTAgentClient::CreateHttpRequest()
{
	TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();

	// Apply per-request timeout from settings if configured
	if (UUnrealGPTSettings* SafeSettings = GetMutableDefault<UUnrealGPTSettings>())
	{
		if (SafeSettings->ExecutionTimeoutSeconds > 0.0f)
		{
			Request->SetTimeout(SafeSettings->ExecutionTimeoutSeconds);
		}
	}

	return Request;
}

FString UUnrealGPTAgentClient::GetEffectiveApiUrl() const
{
	// Always get a fresh reference to settings to avoid accessing invalid cached pointers
	// Settings can become invalid if the object is garbage collected
	UUnrealGPTSettings* SafeSettings = GetMutableDefault<UUnrealGPTSettings>();
	if (!SafeSettings || !IsValid(SafeSettings))
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Settings is null or invalid and could not be retrieved"));
		return TEXT("https://api.openai.com/v1/responses"); // Default fallback
	}
	
	// Build effective URL based on BaseUrlOverride and ApiEndpoint
	FString BaseUrl = SafeSettings->BaseUrlOverride;
	FString ApiEndpoint = SafeSettings->ApiEndpoint;

	// If no override is set, use ApiEndpoint as-is (caller should provide full URL)
	if (BaseUrl.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Effective API URL (no override): %s"), *ApiEndpoint);
		return ApiEndpoint;
	}

	// Normalize base URL (remove trailing slash)
	if (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.RemoveAt(BaseUrl.Len() - 1);
	}

	// If ApiEndpoint is empty, just use the base URL
	if (ApiEndpoint.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Effective API URL (override only): %s"), *BaseUrl);
		return BaseUrl;
	}

	// If ApiEndpoint is a full URL, extract its path portion and append to BaseUrl
	int32 ProtocolIndex = ApiEndpoint.Find(TEXT("://"));
	if (ProtocolIndex != INDEX_NONE)
	{
		int32 PathStartIndex = ApiEndpoint.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, ProtocolIndex + 3);
		if (PathStartIndex != INDEX_NONE && PathStartIndex < ApiEndpoint.Len())
		{
			FString Path = ApiEndpoint.Mid(PathStartIndex);
			if (!Path.StartsWith(TEXT("/")))
			{
				Path = TEXT("/") + Path;
			}

			const FString EffectiveUrl = BaseUrl + Path;
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Effective API URL (override + parsed path): %s"), *EffectiveUrl);
			return EffectiveUrl;
		}
	}

	// Otherwise treat ApiEndpoint as a path relative to BaseUrl
	FString Path = ApiEndpoint;
	if (!Path.StartsWith(TEXT("/")))
	{
		Path = TEXT("/") + Path;
	}

	const FString EffectiveUrl = BaseUrl + Path;
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Effective API URL (override + relative path): %s"), *EffectiveUrl);
	return EffectiveUrl;
}

bool UUnrealGPTAgentClient::IsUsingResponsesApi() const
{
	FString ApiUrl = GetEffectiveApiUrl();
	return ApiUrl.Contains(TEXT("/v1/responses"));
}

bool UUnrealGPTAgentClient::DetectTaskCompletion(const TArray<FString>& ToolNames, const TArray<FString>& ToolResults) const
{
	if (ToolNames.Num() != ToolResults.Num() || ToolNames.Num() == 0)
	{
		UE_LOG(LogTemp, VeryVerbose, TEXT("UnrealGPT: DetectTaskCompletion - invalid input (names: %d, results: %d)"), ToolNames.Num(), ToolResults.Num());
		return false;
	}

	bool bFoundSuccessfulPythonExecute = false;
	bool bFoundSuccessfulSceneQuery = false;
	bool bFoundScreenshot = false;
	bool bFoundSuccessfulReplicateCall = false;
	bool bFoundReplicateImport = false;

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: DetectTaskCompletion - analyzing %d tools"), ToolNames.Num());

	// Analyze tool results to detect completion signals
	for (int32 i = 0; i < ToolNames.Num(); ++i)
	{
		const FString& ToolName = ToolNames[i];
		const FString& ToolResult = ToolResults[i];
		
		UE_LOG(LogTemp, VeryVerbose, TEXT("UnrealGPT: Checking tool %d: %s (result length: %d)"), i, *ToolName, ToolResult.Len());

		if (ToolName == TEXT("python_execute"))
		{
			// Check if Python execution succeeded
			TSharedPtr<FJsonObject> ResultObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ToolResult);
			if (FJsonSerializer::Deserialize(Reader, ResultObj) && ResultObj.IsValid())
			{
				FString Status;
				if (ResultObj->TryGetStringField(TEXT("status"), Status) && Status == TEXT("ok"))
				{
					bFoundSuccessfulPythonExecute = true;
					
					// Check if this is an import of generated content (import_mcp_* helpers, etc.)
					FString Message;
					if (ResultObj->TryGetStringField(TEXT("message"), Message))
					{
						FString LowerMessage = Message.ToLower();
						if (LowerMessage.Contains(TEXT("imported")) && 
							(LowerMessage.Contains(TEXT("texture")) || 
							 LowerMessage.Contains(TEXT("mesh")) || 
							 LowerMessage.Contains(TEXT("audio"))))
						{
							bFoundReplicateImport = true;
							UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Detected content import in python_execute: %s"), *Message);
						}
						
						// Look for completion keywords in the message
						if (LowerMessage.Contains(TEXT("success")) || 
							LowerMessage.Contains(TEXT("created")) ||
							LowerMessage.Contains(TEXT("added")) ||
							LowerMessage.Contains(TEXT("completed")) ||
							LowerMessage.Contains(TEXT("done")))
						{
							UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Completion detected - python_execute succeeded with completion keywords: %s"), *Message);
						}
					}
				}
			}
		}
		else if (ToolName == TEXT("replicate_generate"))
		{
			// Check if Replicate call succeeded and produced files
			TSharedPtr<FJsonObject> ResultObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ToolResult);
			if (FJsonSerializer::Deserialize(Reader, ResultObj) && ResultObj.IsValid())
			{
				FString Status;
				if (ResultObj->TryGetStringField(TEXT("status"), Status) && Status == TEXT("success"))
				{
					// Check if files were downloaded
					const TSharedPtr<FJsonObject>* DetailsObj = nullptr;
					if (ResultObj->TryGetObjectField(TEXT("details"), DetailsObj) && DetailsObj && DetailsObj->IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* FilesArray = nullptr;
						if ((*DetailsObj)->TryGetArrayField(TEXT("files"), FilesArray) && FilesArray && FilesArray->Num() > 0)
						{
							bFoundSuccessfulReplicateCall = true;
							UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Completion detected - replicate_generate succeeded with %d file(s)"), FilesArray->Num());
						}
					}
				}
			}
		}
		else if (ToolName == TEXT("scene_query"))
		{
			// Check if scene_query found matching objects
			if (!ToolResult.IsEmpty() && ToolResult != TEXT("[]") && ToolResult.StartsWith(TEXT("[")))
			{
				TSharedPtr<FJsonValue> JsonValue;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ToolResult);
				if (FJsonSerializer::Deserialize(Reader, JsonValue) && JsonValue.IsValid())
				{
					const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
					if (JsonValue->Type == EJson::Array && JsonValue->TryGetArray(JsonArray) && JsonArray->Num() > 0)
					{
						bFoundSuccessfulSceneQuery = true;
						UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Completion detected - scene_query found %d matching objects"), JsonArray->Num());
					}
				}
			}
		}
		else if (ToolName == TEXT("viewport_screenshot"))
		{
			// Screenshot capture is a verification step
			if (!ToolResult.IsEmpty() && ToolResult.StartsWith(TEXT("iVBORw0KGgo")))
			{
				bFoundScreenshot = true;
			}
		}
	}

	// Completion is detected if:
	// 1. Python execution succeeded AND scene_query found matching objects (strong signal)
	//    This pattern indicates: creation succeeded + verification confirmed = task complete
	// 2. Replicate call succeeded AND import succeeded AND scene_query found objects (content creation workflow)
	//    This pattern indicates: content generated + imported + verified = task complete
	// We require BOTH creation/import AND verification signals to avoid false positives
	bool bCompletionDetected = false;
	if (bFoundSuccessfulPythonExecute && bFoundSuccessfulSceneQuery)
	{
		bCompletionDetected = true;
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Task completion detected: python_execute succeeded + scene_query found objects"));
	}
	else if (bFoundSuccessfulReplicateCall && bFoundReplicateImport && bFoundSuccessfulSceneQuery)
	{
		bCompletionDetected = true;
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Task completion detected: replicate_generate succeeded + import succeeded + scene_query found objects"));
	}
	else
	{
		UE_LOG(LogTemp, VeryVerbose, TEXT("UnrealGPT: Completion not detected - python_execute: %d, replicate_generate: %d, content_import: %d, scene_query: %d"), 
			bFoundSuccessfulPythonExecute ? 1 : 0, 
			bFoundSuccessfulReplicateCall ? 1 : 0,
			bFoundReplicateImport ? 1 : 0,
			bFoundSuccessfulSceneQuery ? 1 : 0);
	}

	return bCompletionDetected;
}

