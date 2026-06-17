// Copyright (c) 2025 TREE Industries.

#include "UnrealGPTAgentClient.h"
#include "UnrealGPTSettings.h"
#include "UnrealGPTAgentInstructions.h"
#include "GenericPlatform/GenericPlatformHttp.h"
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
#include "UObject/UObjectGlobals.h" // For FindObject / FindFirstObject
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"

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

	/**
	 * Helper: Convert an image from a texture asset path or file path to base64 PNG data URI.
	 * Returns an empty string on failure.
	 * @param ImagePath Asset path (e.g. "/Game/Textures/MyTex") or file path on disk.
	 * @param OutError Receives an error message on failure.
	 */
	FString ConvertImageToBase64DataUri(const FString& ImagePath, FString& OutError)
	{
		TArray<uint8> PngData;

		// First try to load as an Unreal asset (UTexture2D)
		FString AssetPath = ImagePath;
		// Normalize asset path - ensure it has the object name for LoadObject
		if (AssetPath.StartsWith(TEXT("/Game/")) || AssetPath.StartsWith(TEXT("/Engine/")))
		{
			// If path doesn't have an object name (e.g. "/Game/Textures/MyTex" vs "/Game/Textures/MyTex.MyTex")
			if (!AssetPath.Contains(TEXT(".")))
			{
				// Extract the asset name and append it
				FString AssetName = FPaths::GetBaseFilename(AssetPath);
				AssetPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
			}

			UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *AssetPath);
			if (Texture)
			{
				// Get the platform data to read pixels
				FTexturePlatformData* PlatformData = Texture->GetPlatformData();
				if (!PlatformData || PlatformData->Mips.Num() == 0)
				{
					OutError = FString::Printf(TEXT("Texture '%s' has no platform data or mips"), *ImagePath);
					return FString();
				}

				const FTexture2DMipMap& Mip = PlatformData->Mips[0];
				const int32 Width = Mip.SizeX;
				const int32 Height = Mip.SizeY;

				// Lock the mip data for reading
				const void* MipData = Mip.BulkData.LockReadOnly();
				if (!MipData)
				{
					OutError = FString::Printf(TEXT("Failed to lock texture data for '%s'"), *ImagePath);
					return FString();
				}

				// Copy the raw pixel data
				const int64 DataSize = Mip.BulkData.GetBulkDataSize();
				TArray<uint8> RawData;
				RawData.SetNumUninitialized(DataSize);
				FMemory::Memcpy(RawData.GetData(), MipData, DataSize);
				Mip.BulkData.Unlock();

				// Convert to PNG using ImageWrapper
				IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
				TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

				if (!ImageWrapper.IsValid())
				{
					OutError = TEXT("Failed to create image wrapper");
					return FString();
				}

				// Determine the pixel format - most imported textures use BGRA
				ERGBFormat RGBFormat = ERGBFormat::BGRA;
				if (PlatformData->PixelFormat == PF_R8G8B8A8)
				{
					RGBFormat = ERGBFormat::RGBA;
				}

				if (!ImageWrapper->SetRaw(RawData.GetData(), RawData.Num(), Width, Height, RGBFormat, 8))
				{
					OutError = FString::Printf(TEXT("Failed to set raw image data for '%s'"), *ImagePath);
					return FString();
				}

				PngData = ImageWrapper->GetCompressed();
				if (PngData.Num() == 0)
				{
					OutError = FString::Printf(TEXT("Failed to compress texture '%s' to PNG"), *ImagePath);
					return FString();
				}

				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Converted texture asset '%s' to PNG (%d bytes)"), *ImagePath, PngData.Num());
			}
		}

		// If we didn't load as an asset, try loading as a file from disk
		if (PngData.Num() == 0)
		{
			FString FilePath = ImagePath;
			
			// Make absolute if relative
			if (FPaths::IsRelative(FilePath))
			{
				FilePath = FPaths::Combine(FPaths::ProjectDir(), FilePath);
			}

			if (!FPaths::FileExists(FilePath))
			{
				OutError = FString::Printf(TEXT("Image not found as asset or file: '%s'"), *ImagePath);
				return FString();
			}

			TArray<uint8> FileData;
			if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
			{
				OutError = FString::Printf(TEXT("Failed to read file: '%s'"), *FilePath);
				return FString();
			}

			// Detect image format and convert to PNG if needed
			IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
			
			// Try to detect format from file extension first
			EImageFormat ImageFormat = EImageFormat::Invalid;
			FString Extension = FPaths::GetExtension(FilePath).ToLower();
			if (Extension == TEXT("png"))
			{
				ImageFormat = EImageFormat::PNG;
				// PNG can be used directly
				PngData = MoveTemp(FileData);
			}
			else
			{
				// Detect format and convert to PNG
				if (Extension == TEXT("jpg") || Extension == TEXT("jpeg"))
				{
					ImageFormat = EImageFormat::JPEG;
				}
				else if (Extension == TEXT("bmp"))
				{
					ImageFormat = EImageFormat::BMP;
				}
				else if (Extension == TEXT("tga"))
				{
					ImageFormat = EImageFormat::TGA;
				}

				if (ImageFormat == EImageFormat::Invalid)
				{
					OutError = FString::Printf(TEXT("Unsupported image format for file: '%s'"), *FilePath);
					return FString();
				}

				// Decompress the source format
				TSharedPtr<IImageWrapper> SourceWrapper = ImageWrapperModule.CreateImageWrapper(ImageFormat);
				if (!SourceWrapper.IsValid() || !SourceWrapper->SetCompressed(FileData.GetData(), FileData.Num()))
				{
					OutError = FString::Printf(TEXT("Failed to decode image file: '%s'"), *FilePath);
					return FString();
				}

				TArray<uint8> RawBGRA;
				if (!SourceWrapper->GetRaw(ERGBFormat::BGRA, 8, RawBGRA))
				{
					OutError = FString::Printf(TEXT("Failed to get raw pixel data from: '%s'"), *FilePath);
					return FString();
				}

				// Re-encode as PNG
				TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
				if (!PngWrapper.IsValid() || 
					!PngWrapper->SetRaw(RawBGRA.GetData(), RawBGRA.Num(), SourceWrapper->GetWidth(), SourceWrapper->GetHeight(), ERGBFormat::BGRA, 8))
				{
					OutError = FString::Printf(TEXT("Failed to encode PNG from: '%s'"), *FilePath);
					return FString();
				}

				PngData = PngWrapper->GetCompressed();
			}

			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Loaded image file '%s' as PNG (%d bytes)"), *FilePath, PngData.Num());
		}

		if (PngData.Num() == 0)
		{
			OutError = FString::Printf(TEXT("Failed to load image from: '%s'"), *ImagePath);
			return FString();
		}

		// Convert to base64 data URI
		FString Base64 = FBase64::Encode(PngData);
		return FString::Printf(TEXT("data:image/png;base64,%s"), *Base64);
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

	FString AuthToken;
	FString ChatGPTAccountId;
	FString AuthError;
	if (!Settings->ResolveAuthHeaders(AuthToken, ChatGPTAccountId, AuthError))
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: %s"), *AuthError);
		OnAgentMessage.Broadcast(TEXT("system"), FString::Printf(TEXT("Error: %s"), *AuthError), TArray<FString>());
		return;
	}

	// Reset tool call iteration counter for new user messages
	// Note: Images (like screenshots from tool calls) are NOT considered new user messages - they're tool continuations
	const bool bIsNewUserMessage = !UserMessage.IsEmpty();
	if (bIsNewUserMessage)
	{
		ToolCallIterationCount = 0;
		bHasRetriedAfterCodexRefresh = false;
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

			// Use configurable max iterations from settings (0 = unlimited)
			const int32 MaxIterations = Settings->MaxToolCallIterations;

			if (MaxIterations > 0)
			{
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Tool call continuation - iteration %d/%d"), ToolCallIterationCount, MaxIterations);
				if (ToolCallIterationCount >= MaxIterations)
				{
					UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Maximum tool call iterations (%d) reached. Stopping to prevent infinite loop."), MaxIterations);
					
					// Add interruption notice to conversation history for seamless continuation
					FAgentMessage InterruptMsg;
					InterruptMsg.Role = TEXT("assistant");
					InterruptMsg.Content = TEXT("[Agent paused: Maximum tool call iterations reached. You can continue the conversation to resume.]");
					ConversationHistory.Add(InterruptMsg);
					
					ToolCallIterationCount = 0;
					bRequestInProgress = false;
					
					// Notify UI that we've stopped
					OnAgentMessage.Broadcast(TEXT("system"), TEXT("Agent paused: Maximum tool call iterations reached. Send a message to continue."), TArray<FString>());
					return;
				}
			}
			else
			{
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Tool call continuation - iteration %d (unlimited)"), ToolCallIterationCount);
			}
		}

	// Add user message to history only if not empty (empty means continuing after tool call)
	// CRITICAL: Do NOT add empty user messages - this breaks Responses API tool continuation
	// EXCEPTION: If we have images (e.g., viewport screenshots), add a placeholder message for them
	if (!UserMessage.IsEmpty())
	{
		FAgentMessage UserMsg;
		UserMsg.Role = TEXT("user");
		UserMsg.Content = UserMessage;
		ConversationHistory.Add(UserMsg);
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added user message to history: %s"), *UserMessage.Left(100));
	}
	else if (ImageBase64.Num() > 0)
	{
		// We have images (screenshots) but no text - add a placeholder message
		// This is needed so the images have a user message to attach to
		FAgentMessage ImageMsg;
		ImageMsg.Role = TEXT("user");
		ImageMsg.Content = TEXT("[Viewport screenshot for visual verification]");
		ConversationHistory.Add(ImageMsg);
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added placeholder user message for %d image(s)"), ImageBase64.Num());
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Empty user message - this is a tool continuation, NOT adding to history"));
	}

	// Build request JSON
	// Note: Using Responses API (/v1/responses) for better agentic tool calling support
	TSharedPtr<FJsonObject> RequestJson = MakeShareable(new FJsonObject);
	FString EffectiveModel = Settings->DefaultModel;
	const bool bUseCodexChatGPTEndpoint = Settings->bUseCodexAuth && Settings->bUseCodexResponsesEndpoint && Settings->IsUsingCodexChatGPTAuth();
	if (bUseCodexChatGPTEndpoint)
	{
		EffectiveModel = Settings->CodexModel.IsEmpty() ? TEXT("gpt-5.1-codex") : Settings->CodexModel;
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Using Codex model %s instead of configured provider model %s"), *EffectiveModel, *Settings->DefaultModel);
	}
	RequestJson->SetStringField(TEXT("model"), EffectiveModel);
	const bool bUseResponsesApi = IsUsingResponsesApi();

	// Configure reasoning effort if supported (Responses API + gpt-5/o-series models)
	if (bUseResponsesApi)
	{
		// Simple check for models that likely support reasoning
		const FString ModelName = EffectiveModel.ToLower();
		const bool bSupportsReasoning = ModelName.Contains(TEXT("gpt-5")) || ModelName.Contains(TEXT("o1")) || ModelName.Contains(TEXT("o3"));
		
		if (bSupportsReasoning)
		{
			TSharedPtr<FJsonObject> ReasoningObj = MakeShareable(new FJsonObject);
			
			// Use dynamic or fixed reasoning effort based on settings
			FString EffortLevel;
			if (Settings->bEnableDynamicReasoning)
			{
				EffortLevel = DetermineReasoningEffort(UserMessage, ImageBase64);
			}
			else
			{
				// Use fixed effort from settings, validate it's a valid value
				EffortLevel = Settings->ReasoningEffort.ToLower();
				if (EffortLevel != TEXT("low") && EffortLevel != TEXT("medium") && EffortLevel != TEXT("high"))
				{
					EffortLevel = TEXT("medium"); // Default to medium if invalid
				}
			}
			
			ReasoningObj->SetStringField(TEXT("effort"), EffortLevel);
			if (bAllowReasoningSummary)
			{
				ReasoningObj->SetStringField(TEXT("summary"), TEXT("auto"));
			}
			RequestJson->SetObjectField(TEXT("reasoning"), ReasoningObj);
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Enabled reasoning (effort: %s, mode: %s%s) for model %s"),
				*EffortLevel,
				Settings->bEnableDynamicReasoning ? TEXT("dynamic") : TEXT("fixed"),
				bAllowReasoningSummary ? TEXT(", summary: auto") : TEXT(""),
				*EffectiveModel);
		}
	}

	// High-level behavior instructions for the agent.
	// Enforces a disciplined Observe→Act→Verify→Stop workflow.
	const FString EngineVersion = FString::Printf(TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);
	const FString AgentInstructions = UnrealGPTAgentInstructions::GetInstructions(EngineVersion);
	if (bUseResponsesApi)
	{
		RequestJson->SetStringField(TEXT("instructions"), AgentInstructions);

		// Note: text.verbosity is intentionally not set to allow model defaults.
		// Some models (like gpt-5.1-codex) only support "medium" verbosity,
		// while others support "low". Letting the API use its default avoids
		// compatibility issues across different models.
	}
	// Codex ChatGPT backend requires streaming; regular Responses API remains non-streaming.
	RequestJson->SetBoolField(TEXT("stream"), bUseCodexChatGPTEndpoint ? true : !bUseResponsesApi);
	if (bUseCodexChatGPTEndpoint)
	{
		RequestJson->SetBoolField(TEXT("store"), false);
	}
	
	// Check if we're using OpenAI's endpoint - only OpenAI supports stateful previous_response_id
	const FString ApiUrlForStateCheck = GetEffectiveApiUrl();
	const bool bIsOpenAIForState = ApiUrlForStateCheck.Contains(TEXT("api.openai.com"));
	
	if (bUseResponsesApi)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Using Responses API for agentic tool calling"));
		
		// For Responses API, use previous_response_id if available
		// NOTE: Only use this with OpenAI's endpoint - other providers (OpenRouter, etc.) don't support stateful conversations
		if (!PreviousResponseId.IsEmpty() && bIsOpenAIForState)
		{
			RequestJson->SetStringField(TEXT("previous_response_id"), PreviousResponseId);
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Using previous_response_id: %s"), *PreviousResponseId);
		}
		else if (!PreviousResponseId.IsEmpty() && !bIsOpenAIForState)
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Skipping previous_response_id for non-OpenAI endpoint (stateless mode)"));
		}
	}

	// Build messages array
	TArray<TSharedPtr<FJsonValue>> MessagesArray;
	
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Building messages array from history. History size: %d"), ConversationHistory.Num());
	
	// For Responses API with OpenAI, handle input differently:
	// - Use previous_response_id to maintain state
	// - Only include new user messages in input (or tool results when continuing after tool execution)
	// - Function results are provided as function_call_output items when continuing after tool execution
	// For legacy API or non-OpenAI providers (like OpenRouter), include full conversation history
	int32 StartIndex = 0;
	TArray<FAgentMessage> ToolResultsToInclude; // For Responses API, we'll add function results as input items
	
	// Only use stateful approach with OpenAI's endpoint - other providers don't support it
	if (bUseResponsesApi && !PreviousResponseId.IsEmpty() && bIsOpenAIForState)
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
	
	// For Responses API with OpenAI, add function results as input items with type "function_call_output"
	// IMPORTANT: Only include tool results that are reasonably sized to prevent context overflow
	// CRITICAL: For tool continuation (empty UserMessage), we MUST include tool results
	// NOTE: Only use function_call_output with OpenAI's endpoint - other providers don't support this format
	if (bUseResponsesApi && bIsOpenAIForState)
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
			
			// For Responses API, add "type": "message" to each message in the input array
			// This is required by OpenRouter and other Responses API implementations
			if (bUseResponsesApi)
			{
				MsgObj->SetStringField(TEXT("type"), TEXT("message"));
			}
			
			MsgObj->SetStringField(TEXT("role"), Msg.Role);
			
			// Only attach images to the LAST user message in the history, not to all user messages
			// ImageBase64 contains images for the current request, not historical ones
			const bool bIsLastMessage = (i == HistorySize - 1);
			const bool bShouldAttachImages = bIsLastMessage && (Msg.Role == TEXT("user")) && (ImageBase64.Num() > 0);
			
			if (bShouldAttachImages)
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
						if (bIsOpenAIForState)
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
							// OpenRouter Responses API multimodal schema:
							// { "type": "input_image", "image_url": "data:image/png;base64,...", "detail": "auto" }
							ImageContent->SetStringField(TEXT("type"), TEXT("input_image"));
							ImageContent->SetStringField(
								TEXT("image_url"),
								FString::Printf(TEXT("data:image/png;base64,%s"), *ImageData));
							ImageContent->SetStringField(TEXT("detail"), TEXT("auto"));
						}
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
				// For Responses API with OpenAI's stateful approach, skip assistant messages with tool_calls
				// For non-OpenAI endpoints (like OpenRouter), we must include them since we're in stateless mode
				if (IsUsingResponsesApi() && bIsOpenAIForState)
				{
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Skipping assistant message with tool_calls for Responses API (state maintained by API)"));
					continue;
				}
				
				// For Responses API in stateless mode (non-OpenAI like OpenRouter), convert tool call
				// requests to an assistant message describing what was called. This gives the model
				// context about what tools were invoked without confusing it with function_call items
				// (which are output types, not input types in the Responses API).
				if (bUseResponsesApi && !bIsOpenAIForState)
				{
					// Build a summary of what tools were called
					FString ToolCallSummary;
					if (!Msg.ToolCallsJson.IsEmpty())
					{
						TSharedRef<TJsonReader<>> ToolCallsReader = TJsonReaderFactory<>::Create(Msg.ToolCallsJson);
						TArray<TSharedPtr<FJsonValue>> ToolCallsArray;
						if (FJsonSerializer::Deserialize(ToolCallsReader, ToolCallsArray) && ToolCallsArray.Num() > 0)
						{
							TArray<FString> ToolDescriptions;
							for (const TSharedPtr<FJsonValue>& ToolCallValue : ToolCallsArray)
							{
								TSharedPtr<FJsonObject> ToolCallObj = ToolCallValue.IsValid() ? ToolCallValue->AsObject() : nullptr;
								if (!ToolCallObj.IsValid()) continue;
								
								const TSharedPtr<FJsonObject>* FunctionObjPtr = nullptr;
								if (ToolCallObj->TryGetObjectField(TEXT("function"), FunctionObjPtr) && FunctionObjPtr && FunctionObjPtr->IsValid())
								{
									FString FuncName;
									(*FunctionObjPtr)->TryGetStringField(TEXT("name"), FuncName);
									if (!FuncName.IsEmpty())
									{
										ToolDescriptions.Add(FuncName);
									}
								}
							}
							
							if (ToolDescriptions.Num() > 0)
							{
								ToolCallSummary = TEXT("[Called tool(s): ") + FString::Join(ToolDescriptions, TEXT(", ")) + TEXT("]");
							}
						}
					}
					
					// Create assistant message with content (including any text + tool call summary)
					TSharedPtr<FJsonObject> AssistantMsg = MakeShareable(new FJsonObject);
					AssistantMsg->SetStringField(TEXT("type"), TEXT("message"));
					AssistantMsg->SetStringField(TEXT("role"), TEXT("assistant"));
					
					FString CombinedContent = Msg.Content;
					if (!ToolCallSummary.IsEmpty())
					{
						if (!CombinedContent.IsEmpty())
						{
							CombinedContent += TEXT("\n\n");
						}
						CombinedContent += ToolCallSummary;
					}
					
					TArray<TSharedPtr<FJsonValue>> ContentArray;
					TSharedPtr<FJsonObject> TextContent = MakeShareable(new FJsonObject);
					TextContent->SetStringField(TEXT("type"), TEXT("output_text"));
					TextContent->SetStringField(TEXT("text"), CombinedContent);
					ContentArray.Add(MakeShareable(new FJsonValueObject(TextContent)));
					AssistantMsg->SetArrayField(TEXT("content"), ContentArray);
					
					MessagesArray.Add(MakeShareable(new FJsonValueObject(AssistantMsg)));
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added assistant message with tool call summary for stateless Responses API"));
					continue; // Skip the normal message processing
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
				// Tool messages are NOT supported in Responses API input array when using OpenAI's stateful approach
				// For non-OpenAI endpoints (like OpenRouter), we must include them since we're in stateless mode
				if (bUseResponsesApi && bIsOpenAIForState)
				{
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Skipping tool message for Responses API (state maintained via previous_response_id)"));
					continue;
				}
				
				// For Responses API in stateless mode (non-OpenAI), format tool results as a 
				// system or user message with clear context about what the tool returned.
				// This helps the model understand the results without using function_call_output
				// items which may confuse non-OpenAI providers.
				if (bUseResponsesApi && !bIsOpenAIForState)
				{
					// Create a user message with the tool result, clearly labeled
					TSharedPtr<FJsonObject> ToolResultMsg = MakeShareable(new FJsonObject);
					ToolResultMsg->SetStringField(TEXT("type"), TEXT("message"));
					ToolResultMsg->SetStringField(TEXT("role"), TEXT("user"));
					
					// Format the tool result with clear context
					FString FormattedResult = FString::Printf(
						TEXT("[Tool Result]\n%s"),
						*Msg.Content);
					
					TArray<TSharedPtr<FJsonValue>> ContentArray;
					TSharedPtr<FJsonObject> TextContent = MakeShareable(new FJsonObject);
					TextContent->SetStringField(TEXT("type"), TEXT("input_text"));
					TextContent->SetStringField(TEXT("text"), FormattedResult);
					ContentArray.Add(MakeShareable(new FJsonValueObject(TextContent)));
					ToolResultMsg->SetArrayField(TEXT("content"), ContentArray);
					
					MessagesArray.Add(MakeShareable(new FJsonValueObject(ToolResultMsg)));
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added tool result message for stateless Responses API: call_id=%s"), *Msg.ToolCallId);
					continue; // Skip the normal message processing
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
				// For Responses API in stateless mode (non-OpenAI), format content as array
				if (bUseResponsesApi && !bIsOpenAIForState)
				{
					TArray<TSharedPtr<FJsonValue>> ContentArray;
					TSharedPtr<FJsonObject> TextContent = MakeShareable(new FJsonObject);
					
					// Use input_text for user messages, output_text for assistant messages
					if (Msg.Role == TEXT("assistant"))
					{
						TextContent->SetStringField(TEXT("type"), TEXT("output_text"));
						TextContent->SetStringField(TEXT("text"), Msg.Content);
					}
					else
					{
						TextContent->SetStringField(TEXT("type"), TEXT("input_text"));
						TextContent->SetStringField(TEXT("text"), Msg.Content);
					}
					
					ContentArray.Add(MakeShareable(new FJsonValueObject(TextContent)));
					MsgObj->SetArrayField(TEXT("content"), ContentArray);
				}
				else
				{
					MsgObj->SetStringField(TEXT("content"), Msg.Content);
				}
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
	CurrentRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AuthToken));
	if (!ChatGPTAccountId.IsEmpty())
	{
		CurrentRequest->SetHeader(TEXT("ChatGPT-Account-Id"), ChatGPTAccountId);
		CurrentRequest->SetHeader(TEXT("Origin"), TEXT("https://chatgpt.com"));
		CurrentRequest->SetHeader(TEXT("Referer"), TEXT("https://chatgpt.com/"));
	}
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

	// OpenAI-hosted web_search and file_search tools (Responses API only, OpenAI endpoint only).
	// These are OpenAI-specific built-in tools that don't work with OpenRouter or other providers.
	// web_search lets the model search the web in general.
	// file_search is configured against a UE Python API vector store the user has created.
	const FString EffectiveApiUrl = GetEffectiveApiUrl();
	const bool bIsOpenAIEndpoint = EffectiveApiUrl.Contains(TEXT("api.openai.com"));
	if (bUseResponsesApi && bIsOpenAIEndpoint)
	{
		// web_search
		TSharedPtr<FJsonObject> WebSearchTool = MakeShareable(new FJsonObject);
		WebSearchTool->SetStringField(TEXT("type"), TEXT("web_search"));
		Tools.Add(WebSearchTool);

		// file_search over UE Python API docs - use configurable vector store ID
		const FString VectorStoreId = Settings ? Settings->VectorStoreId : TEXT("vs_691df14e67fc819189353158b9f13942");
		if (!VectorStoreId.IsEmpty())
		{
			TSharedPtr<FJsonObject> FileSearchTool = MakeShareable(new FJsonObject);
			FileSearchTool->SetStringField(TEXT("type"), TEXT("file_search"));

			TArray<TSharedPtr<FJsonValue>> VectorStores;
			VectorStores.Add(MakeShareable(new FJsonValueString(VectorStoreId)));
			FileSearchTool->SetArrayField(TEXT("vector_store_ids"), VectorStores);
			FileSearchTool->SetNumberField(TEXT("max_num_results"), 20);

			Tools.Add(FileSearchTool);
		}
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

	// New atomic editor tools - purpose-built to reduce reliance on freeform Python
	{
		// get_actor
		TSharedPtr<FJsonObject> GetActorParams = MakeShareable(new FJsonObject);
		GetActorParams->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> GetActorProps = MakeShareable(new FJsonObject);
		
		TSharedPtr<FJsonObject> LabelProp = MakeShareable(new FJsonObject);
		LabelProp->SetStringField(TEXT("type"), TEXT("string"));
		LabelProp->SetStringField(TEXT("description"), TEXT("Actor label as shown in the World Outliner (preferred)."));
		GetActorProps->SetObjectField(TEXT("label"), LabelProp);
		
		TSharedPtr<FJsonObject> NameProp = MakeShareable(new FJsonObject);
		NameProp->SetStringField(TEXT("type"), TEXT("string"));
		NameProp->SetStringField(TEXT("description"), TEXT("Actor object name (alternative to label)."));
		GetActorProps->SetObjectField(TEXT("name"), NameProp);
		
		GetActorParams->SetObjectField(TEXT("properties"), GetActorProps);
		
		Tools.Add(BuildToolObject(
			TEXT("get_actor"),
			TEXT("Get detailed information about a specific actor by label or name. ")
			TEXT("Returns JSON with actor details including transform, class, components, bounds, tags, and more."),
			GetActorParams));
	}
	
	{
		// set_actor_transform
		TSharedPtr<FJsonObject> SetTransformParams = MakeShareable(new FJsonObject);
		SetTransformParams->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> SetTransformProps = MakeShareable(new FJsonObject);
		
		TSharedPtr<FJsonObject> LabelProp = MakeShareable(new FJsonObject);
		LabelProp->SetStringField(TEXT("type"), TEXT("string"));
		LabelProp->SetStringField(TEXT("description"), TEXT("Actor label as shown in the World Outliner."));
		SetTransformProps->SetObjectField(TEXT("label"), LabelProp);
		
		TSharedPtr<FJsonObject> LocationProp = MakeShareable(new FJsonObject);
		LocationProp->SetStringField(TEXT("type"), TEXT("object"));
		LocationProp->SetStringField(TEXT("description"), TEXT("New location {x, y, z}. Omit to keep current."));
		SetTransformProps->SetObjectField(TEXT("location"), LocationProp);
		
		TSharedPtr<FJsonObject> RotationProp = MakeShareable(new FJsonObject);
		RotationProp->SetStringField(TEXT("type"), TEXT("object"));
		RotationProp->SetStringField(TEXT("description"), TEXT("New rotation {pitch, yaw, roll} in degrees. Omit to keep current."));
		SetTransformProps->SetObjectField(TEXT("rotation"), RotationProp);
		
		TSharedPtr<FJsonObject> ScaleProp = MakeShareable(new FJsonObject);
		ScaleProp->SetStringField(TEXT("type"), TEXT("object"));
		ScaleProp->SetStringField(TEXT("description"), TEXT("New scale {x, y, z}. Omit to keep current."));
		SetTransformProps->SetObjectField(TEXT("scale"), ScaleProp);
		
		SetTransformParams->SetObjectField(TEXT("properties"), SetTransformProps);
		
		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShareable(new FJsonValueString(TEXT("label"))));
		SetTransformParams->SetArrayField(TEXT("required"), Required);
		
		Tools.Add(BuildToolObject(
			TEXT("set_actor_transform"),
			TEXT("Set the transform (location, rotation, scale) of an actor by label. ")
			TEXT("Wrapped in an editor transaction for Undo support."),
			SetTransformParams));
	}
	
	{
		// set_actors_rotation
		TSharedPtr<FJsonObject> SetRotParams = MakeShareable(new FJsonObject);
		SetRotParams->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> SetRotProps = MakeShareable(new FJsonObject);
		
		TSharedPtr<FJsonObject> LabelsProp = MakeShareable(new FJsonObject);
		LabelsProp->SetStringField(TEXT("type"), TEXT("array"));
		LabelsProp->SetStringField(TEXT("description"), TEXT("Array of actor labels to set rotation on."));
		TSharedPtr<FJsonObject> ItemsProp = MakeShareable(new FJsonObject);
		ItemsProp->SetStringField(TEXT("type"), TEXT("string"));
		LabelsProp->SetObjectField(TEXT("items"), ItemsProp);
		SetRotProps->SetObjectField(TEXT("labels"), LabelsProp);
		
		TSharedPtr<FJsonObject> RotationProp = MakeShareable(new FJsonObject);
		RotationProp->SetStringField(TEXT("type"), TEXT("object"));
		RotationProp->SetStringField(TEXT("description"), TEXT("Rotation to apply. Object with pitch, yaw, roll fields (degrees)."));
		TSharedPtr<FJsonObject> RotProps = MakeShareable(new FJsonObject);
		TSharedPtr<FJsonObject> PitchProp = MakeShareable(new FJsonObject);
		PitchProp->SetStringField(TEXT("type"), TEXT("number"));
		RotProps->SetObjectField(TEXT("pitch"), PitchProp);
		TSharedPtr<FJsonObject> YawProp = MakeShareable(new FJsonObject);
		YawProp->SetStringField(TEXT("type"), TEXT("number"));
		RotProps->SetObjectField(TEXT("yaw"), YawProp);
		TSharedPtr<FJsonObject> RollProp = MakeShareable(new FJsonObject);
		RollProp->SetStringField(TEXT("type"), TEXT("number"));
		RotProps->SetObjectField(TEXT("roll"), RollProp);
		RotationProp->SetObjectField(TEXT("properties"), RotProps);
		SetRotProps->SetObjectField(TEXT("rotation"), RotationProp);
		
		SetRotParams->SetObjectField(TEXT("properties"), SetRotProps);
		
		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShareable(new FJsonValueString(TEXT("labels"))));
		Required.Add(MakeShareable(new FJsonValueString(TEXT("rotation"))));
		SetRotParams->SetArrayField(TEXT("required"), Required);
		
		Tools.Add(BuildToolObject(
			TEXT("set_actors_rotation"),
			TEXT("Batch-set the rotation on multiple actors at once by their labels. ")
			TEXT("Wrapped in an editor transaction for Undo support."),
			SetRotParams));
	}
	
	{
		// select_actors
		TSharedPtr<FJsonObject> SelectParams = MakeShareable(new FJsonObject);
		SelectParams->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> SelectProps = MakeShareable(new FJsonObject);
		
		TSharedPtr<FJsonObject> LabelsProp = MakeShareable(new FJsonObject);
		LabelsProp->SetStringField(TEXT("type"), TEXT("array"));
		LabelsProp->SetStringField(TEXT("description"), TEXT("Array of actor labels to select."));
		TSharedPtr<FJsonObject> ItemsProp = MakeShareable(new FJsonObject);
		ItemsProp->SetStringField(TEXT("type"), TEXT("string"));
		LabelsProp->SetObjectField(TEXT("items"), ItemsProp);
		SelectProps->SetObjectField(TEXT("labels"), LabelsProp);
		
		TSharedPtr<FJsonObject> AddToSelectionProp = MakeShareable(new FJsonObject);
		AddToSelectionProp->SetStringField(TEXT("type"), TEXT("boolean"));
		AddToSelectionProp->SetStringField(TEXT("description"), TEXT("If true, add to existing selection. Default false."));
		SelectProps->SetObjectField(TEXT("add_to_selection"), AddToSelectionProp);
		
		SelectParams->SetObjectField(TEXT("properties"), SelectProps);
		
		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShareable(new FJsonValueString(TEXT("labels"))));
		SelectParams->SetArrayField(TEXT("required"), Required);
		
		Tools.Add(BuildToolObject(
			TEXT("select_actors"),
			TEXT("Select one or more actors in the editor by their labels."),
			SelectParams));
	}
	
	{
		// duplicate_actor
		TSharedPtr<FJsonObject> DupParams = MakeShareable(new FJsonObject);
		DupParams->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> DupProps = MakeShareable(new FJsonObject);
		
		TSharedPtr<FJsonObject> LabelProp = MakeShareable(new FJsonObject);
		LabelProp->SetStringField(TEXT("type"), TEXT("string"));
		LabelProp->SetStringField(TEXT("description"), TEXT("Label of the actor to duplicate."));
		DupProps->SetObjectField(TEXT("label"), LabelProp);
		
		TSharedPtr<FJsonObject> OffsetProp = MakeShareable(new FJsonObject);
		OffsetProp->SetStringField(TEXT("type"), TEXT("object"));
		OffsetProp->SetStringField(TEXT("description"), TEXT("Position offset {x, y, z} for the duplicate. Default is no offset."));
		DupProps->SetObjectField(TEXT("offset"), OffsetProp);
		
		TSharedPtr<FJsonObject> NewLabelProp = MakeShareable(new FJsonObject);
		NewLabelProp->SetStringField(TEXT("type"), TEXT("string"));
		NewLabelProp->SetStringField(TEXT("description"), TEXT("Optional new label for the duplicated actor."));
		DupProps->SetObjectField(TEXT("new_label"), NewLabelProp);
		
		DupParams->SetObjectField(TEXT("properties"), DupProps);
		
		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShareable(new FJsonValueString(TEXT("label"))));
		DupParams->SetArrayField(TEXT("required"), Required);
		
		Tools.Add(BuildToolObject(
			TEXT("duplicate_actor"),
			TEXT("Duplicate an actor in the level with optional offset. ")
			TEXT("Wrapped in an editor transaction for Undo support."),
			DupParams));
	}
	
	{
		// snap_actor_to_ground
		TSharedPtr<FJsonObject> SnapParams = MakeShareable(new FJsonObject);
		SnapParams->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> SnapProps = MakeShareable(new FJsonObject);
		
		TSharedPtr<FJsonObject> LabelProp = MakeShareable(new FJsonObject);
		LabelProp->SetStringField(TEXT("type"), TEXT("string"));
		LabelProp->SetStringField(TEXT("description"), TEXT("Label of the actor to snap to ground."));
		SnapProps->SetObjectField(TEXT("label"), LabelProp);
		
		TSharedPtr<FJsonObject> AlignProp = MakeShareable(new FJsonObject);
		AlignProp->SetStringField(TEXT("type"), TEXT("boolean"));
		AlignProp->SetStringField(TEXT("description"), TEXT("If true, align actor to surface normal. Default false."));
		SnapProps->SetObjectField(TEXT("align_to_normal"), AlignProp);
		
		TSharedPtr<FJsonObject> OffsetProp = MakeShareable(new FJsonObject);
		OffsetProp->SetStringField(TEXT("type"), TEXT("number"));
		OffsetProp->SetStringField(TEXT("description"), TEXT("Vertical offset from the hit point."));
		SnapProps->SetObjectField(TEXT("offset"), OffsetProp);
		
		SnapParams->SetObjectField(TEXT("properties"), SnapProps);
		
		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShareable(new FJsonValueString(TEXT("label"))));
		SnapParams->SetArrayField(TEXT("required"), Required);
		
		Tools.Add(BuildToolObject(
			TEXT("snap_actor_to_ground"),
			TEXT("Snap an actor to the ground using a line trace. ")
			TEXT("Wrapped in an editor transaction for Undo support."),
			SnapParams));
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
		OnAgentMessage.Broadcast(TEXT("system"), TEXT("Error: HTTP request failed. Please try again."), TArray<FString>());
		return;
	}

	int32 ResponseCode = Response->GetResponseCode();
	if (ResponseCode != 200)
	{
		const FString ErrorBody = Response->GetContentAsString();
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: HTTP error %d: %s"), ResponseCode, *ErrorBody);

		if (ResponseCode == 401
			&& Settings
			&& Settings->bUseCodexAuth
			&& Settings->IsUsingCodexChatGPTAuth()
			&& !bHasRetriedAfterCodexRefresh
			&& !LastRequestBody.IsEmpty())
		{
			bHasRetriedAfterCodexRefresh = true;
			if (RefreshCodexAuthAndRetry(LastRequestBody))
			{
				return;
			}
		}

		// Gracefully handle organizations that are not yet allowed to use reasoning summaries.
		// In that case, we disable reasoning.summary for this session and let the user continue
		// using the agent without having to change any settings.
		// NOTE: Only apply this to OpenAI's endpoint - other providers (like OpenRouter) don't have this restriction.
		const FString ErrorApiUrl = GetEffectiveApiUrl();
		const bool bIsOpenAIForReasoning = ErrorApiUrl.Contains(TEXT("api.openai.com"));
		if (ResponseCode == 400 && bAllowReasoningSummary && IsUsingResponsesApi() && bIsOpenAIForReasoning)
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
										FString RetryAuthError;
										if (!ApplyAuthHeaders(RetryRequest, RetryAuthError))
										{
											UE_LOG(LogTemp, Error, TEXT("UnrealGPT: %s"), *RetryAuthError);
											OnAgentMessage.Broadcast(TEXT("system"), FString::Printf(TEXT("Error: %s"), *RetryAuthError), TArray<FString>());
											return;
										}
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

		// Broadcast error to UI so the stop button state updates
		OnAgentMessage.Broadcast(TEXT("system"), FString::Printf(TEXT("Error: HTTP %d - %s"), ResponseCode, *ErrorBody.Left(200)), TArray<FString>());
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
	// OpenAI-style streams normally send one terminal chunk; some OpenRouter / proxy streams
	// repeat finish_reason (e.g. "stop" on both the last delta chunk and the trailing empty chunk).
	// Without this guard we append duplicate assistant messages and double-broadcast to the UI.
	bool bStreamTerminalEventHandled = false;

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
						if ((*ChoiceObj)->TryGetStringField(TEXT("finish_reason"), FinishReason) && !FinishReason.IsEmpty())
						{
							if (bStreamTerminalEventHandled)
							{
								UE_LOG(LogTemp, VeryVerbose, TEXT("UnrealGPT: Skipping duplicate chat stream terminal chunk (finish_reason=%s)"), *FinishReason);
								continue;
							}

							if (FinishReason == TEXT("tool_calls") && !CurrentToolCallId.IsEmpty())
							{
								bStreamTerminalEventHandled = true;

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
								bStreamTerminalEventHandled = true;

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
	if (ResponseContent.Contains(TEXT("data: ")))
	{
		TArray<FString> Lines;
		ResponseContent.ParseIntoArrayLines(Lines);

		FString StreamText;
		FString StreamReasoningSummary;
		TSharedPtr<FJsonObject> CompletedResponseObject;
		TArray<TSharedPtr<FJsonValue>> StreamOutputItems;

		for (const FString& Line : Lines)
		{
			if (!Line.StartsWith(TEXT("data: ")))
			{
				continue;
			}

			const FString Data = Line.Mid(6).TrimStartAndEnd();
			if (Data.IsEmpty() || Data == TEXT("[DONE]"))
			{
				continue;
			}

			TSharedPtr<FJsonObject> EventObject;
			TSharedRef<TJsonReader<>> EventReader = TJsonReaderFactory<>::Create(Data);
			if (!FJsonSerializer::Deserialize(EventReader, EventObject) || !EventObject.IsValid())
			{
				continue;
			}

			FString EventType;
			EventObject->TryGetStringField(TEXT("type"), EventType);
			if (EventType == TEXT("response.output_text.delta"))
			{
				FString Delta;
				if (EventObject->TryGetStringField(TEXT("delta"), Delta))
				{
					StreamText += Delta;
				}
			}
			else if (EventType == TEXT("response.output_text.done"))
			{
				FString Text;
				if (StreamText.IsEmpty() && EventObject->TryGetStringField(TEXT("text"), Text))
				{
					StreamText = Text;
				}
			}
			else if (EventType == TEXT("response.reasoning_summary_text.delta"))
			{
				FString Delta;
				if (EventObject->TryGetStringField(TEXT("delta"), Delta))
				{
					StreamReasoningSummary += Delta;
				}
			}
			else if (EventType == TEXT("response.reasoning_summary_text.done"))
			{
				FString Text;
				if (StreamReasoningSummary.IsEmpty() && EventObject->TryGetStringField(TEXT("text"), Text))
				{
					StreamReasoningSummary = Text;
				}
			}
			else if (EventType == TEXT("response.output_item.done"))
			{
				const TSharedPtr<FJsonObject>* ItemObject = nullptr;
				if (EventObject->TryGetObjectField(TEXT("item"), ItemObject) && ItemObject && ItemObject->IsValid())
				{
					FString ItemType;
					(*ItemObject)->TryGetStringField(TEXT("type"), ItemType);
					if (ItemType == TEXT("function_call"))
					{
						StreamOutputItems.Add(MakeShareable(new FJsonValueObject(*ItemObject)));
					}
					else if (ItemType == TEXT("message") && StreamText.IsEmpty())
					{
						const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
						if ((*ItemObject)->TryGetArrayField(TEXT("content"), ContentArray) && ContentArray)
						{
							for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
							{
								TSharedPtr<FJsonObject> ContentObject = ContentValue.IsValid() ? ContentValue->AsObject() : nullptr;
								if (!ContentObject.IsValid())
								{
									continue;
								}

								FString ContentType;
								ContentObject->TryGetStringField(TEXT("type"), ContentType);
								if (ContentType == TEXT("output_text") || ContentType == TEXT("text"))
								{
									FString Text;
									if (ContentObject->TryGetStringField(TEXT("text"), Text))
									{
										StreamText += Text;
									}
								}
							}
						}
					}
				}
			}
			else if (EventType == TEXT("response.completed"))
			{
				const TSharedPtr<FJsonObject>* ResponseObject = nullptr;
				if (EventObject->TryGetObjectField(TEXT("response"), ResponseObject) && ResponseObject && ResponseObject->IsValid())
				{
					CompletedResponseObject = *ResponseObject;
				}
			}
			else if (EventType == TEXT("response.failed") || EventType == TEXT("response.incomplete"))
			{
				UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Responses API stream ended with event: %s"), *EventType);
				OnAgentMessage.Broadcast(TEXT("system"), FString::Printf(TEXT("Error: %s"), *Data.Left(300)), TArray<FString>());
				return;
			}
		}

		if (!StreamText.IsEmpty())
		{
			TSharedPtr<FJsonObject> MessageObject = MakeShareable(new FJsonObject);
			MessageObject->SetStringField(TEXT("type"), TEXT("message"));
			MessageObject->SetStringField(TEXT("role"), TEXT("assistant"));

			TArray<TSharedPtr<FJsonValue>> ContentArray;
			TSharedPtr<FJsonObject> TextObject = MakeShareable(new FJsonObject);
			TextObject->SetStringField(TEXT("type"), TEXT("output_text"));
			TextObject->SetStringField(TEXT("text"), StreamText);
			ContentArray.Add(MakeShareable(new FJsonValueObject(TextObject)));
			MessageObject->SetArrayField(TEXT("content"), ContentArray);
			StreamOutputItems.Add(MakeShareable(new FJsonValueObject(MessageObject)));
		}

		if (CompletedResponseObject.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* CompletedOutputArray = nullptr;
			if (StreamOutputItems.Num() > 0 || !CompletedResponseObject->TryGetArrayField(TEXT("output"), CompletedOutputArray) || !CompletedOutputArray || CompletedOutputArray->Num() == 0)
			{
				CompletedResponseObject->SetArrayField(TEXT("output"), StreamOutputItems);
			}

			if (!StreamReasoningSummary.IsEmpty())
			{
				TSharedPtr<FJsonObject> ReasoningObject = MakeShareable(new FJsonObject);
				ReasoningObject->SetStringField(TEXT("summary"), StreamReasoningSummary);
				CompletedResponseObject->SetObjectField(TEXT("reasoning"), ReasoningObject);
			}

			FString CompletedResponseJson;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&CompletedResponseJson);
			FJsonSerializer::Serialize(CompletedResponseObject.ToSharedRef(), Writer);
			ProcessResponsesApiResponse(CompletedResponseJson);
			return;
		}

		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Responses API stream did not contain response.completed"));
		return;
	}

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
	// Note: OpenAI returns summary as a string, OpenRouter returns it as an array of strings
	// Debug: Check if reasoning field exists at all
	const bool bHasReasoningField = RootObject->HasField(TEXT("reasoning"));
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Response has 'reasoning' field: %s"), bHasReasoningField ? TEXT("YES") : TEXT("NO"));
	
	const TSharedPtr<FJsonObject>* ReasoningObjPtr = nullptr;
	if (RootObject->TryGetObjectField(TEXT("reasoning"), ReasoningObjPtr) && ReasoningObjPtr && (*ReasoningObjPtr).IsValid())
	{
		FString ReasoningSummary;
		
		// Try string format first (OpenAI)
		if ((*ReasoningObjPtr)->TryGetStringField(TEXT("summary"), ReasoningSummary) && !ReasoningSummary.IsEmpty())
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Received reasoning summary string (length: %d)"), ReasoningSummary.Len());
			OnAgentReasoning.Broadcast(ReasoningSummary);
		}
		else
		{
			// Try array format (OpenRouter)
			const TArray<TSharedPtr<FJsonValue>>* SummaryArray = nullptr;
			if ((*ReasoningObjPtr)->TryGetArrayField(TEXT("summary"), SummaryArray) && SummaryArray && SummaryArray->Num() > 0)
			{
				TArray<FString> SummaryLines;
				for (const TSharedPtr<FJsonValue>& SummaryValue : *SummaryArray)
				{
					if (SummaryValue.IsValid() && SummaryValue->Type == EJson::String)
					{
						SummaryLines.Add(SummaryValue->AsString());
					}
				}
				
				if (SummaryLines.Num() > 0)
				{
					ReasoningSummary = FString::Join(SummaryLines, TEXT("\n"));
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Received reasoning summary array with %d steps (length: %d)"), SummaryLines.Num(), ReasoningSummary.Len());
					OnAgentReasoning.Broadcast(ReasoningSummary);
				}
			}
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

	// Log all output types at once for easy debugging
	TArray<FString> AllOutputTypes;
	for (const TSharedPtr<FJsonValue>& OVal : *OutputArray)
	{
		if (OVal.IsValid() && OVal->Type == EJson::Object)
		{
			TSharedPtr<FJsonObject> OObj = OVal->AsObject();
			FString OType;
			if (OObj.IsValid() && OObj->TryGetStringField(TEXT("type"), OType))
			{
				AllOutputTypes.Add(OType);
			}
		}
	}
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Output types in response: [%s]"), *FString::Join(AllOutputTypes, TEXT(", ")));

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
		else if (OutputType == TEXT("reasoning"))
		{
			// Handle reasoning output from Responses API (OpenRouter and OpenAI)
			// The summary field is an array of strings describing the reasoning steps
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Processing reasoning output - FOUND reasoning in output array!"));
			
			// Log all fields in the reasoning object to understand its structure
			TArray<FString> ReasoningFields;
			OutputObj->Values.GetKeys(ReasoningFields);
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Reasoning output fields: %s"), *FString::Join(ReasoningFields, TEXT(", ")));
			
			// Debug: Check what type the summary field actually is
			const TSharedPtr<FJsonValue>* SummaryValuePtr = OutputObj->Values.Find(TEXT("summary"));
			if (SummaryValuePtr && (*SummaryValuePtr).IsValid())
			{
				const EJson SummaryType = (*SummaryValuePtr)->Type;
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Summary field type: %d (None=0, Null=1, String=2, Number=3, Boolean=4, Array=5, Object=6)"), (int32)SummaryType);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Summary field not found or invalid!"));
			}
			
			FString ReasoningSummary;
			bool bFoundSummary = false;
			
			// Try array format first (OpenRouter typical format)
			const TArray<TSharedPtr<FJsonValue>>* SummaryArray = nullptr;
			if (OutputObj->TryGetArrayField(TEXT("summary"), SummaryArray) && SummaryArray && SummaryArray->Num() > 0)
			{
				// Join the summary array into a single string with newlines
				TArray<FString> SummaryLines;
				for (const TSharedPtr<FJsonValue>& SummaryValue : *SummaryArray)
				{
					if (SummaryValue.IsValid() && SummaryValue->Type == EJson::String)
					{
						SummaryLines.Add(SummaryValue->AsString());
					}
				}
				
				if (SummaryLines.Num() > 0)
				{
					ReasoningSummary = FString::Join(SummaryLines, TEXT("\n"));
					bFoundSummary = true;
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Extracted reasoning from array with %d steps (length: %d)"), SummaryLines.Num(), ReasoningSummary.Len());
				}
			}
			
			// Try string format
			if (!bFoundSummary)
			{
				if (OutputObj->TryGetStringField(TEXT("summary"), ReasoningSummary) && !ReasoningSummary.IsEmpty())
				{
					bFoundSummary = true;
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Extracted reasoning from string (length: %d)"), ReasoningSummary.Len());
				}
			}
			
			// Broadcast if we found reasoning
			if (bFoundSummary && !ReasoningSummary.IsEmpty())
			{
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Broadcasting reasoning summary to UI: %s"), *ReasoningSummary.Left(200));
				OnAgentReasoning.Broadcast(ReasoningSummary);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Could not extract reasoning summary from output (tried array and string formats)"));
			}
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
			ToolMsg.Content = ToolResultForHistory;
			
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
		// Note: 0 = unlimited
		const int32 MaxIterations = Settings ? Settings->MaxToolCallIterations : 100;
		if (MaxIterations > 0 && ToolCallIterationCount >= MaxIterations - 1)
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Reached maximum tool call iterations (%d). Stopping to prevent infinite loop."), MaxIterations);
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
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
		UClass* TargetClass = FindFirstObject<UClass>(*ClassName);
#else
		UClass* TargetClass = FindObject<UClass>(ANY_PACKAGE, *ClassName);
#endif
		if (!TargetClass)
		{
			TargetClass = LoadObject<UClass>(nullptr, *ClassName);
		}

		Result = BuildReflectionSchemaJson(TargetClass);
	}
	// New atomic editor tools
	else if (ToolName == TEXT("get_actor"))
	{
		Result = UUnrealGPTSceneContext::GetActor(ArgumentsJson);
	}
	else if (ToolName == TEXT("set_actor_transform"))
	{
		Result = UUnrealGPTSceneContext::SetActorTransform(ArgumentsJson);
	}
	else if (ToolName == TEXT("set_actors_rotation"))
	{
		Result = UUnrealGPTSceneContext::SetActorsRotation(ArgumentsJson);
	}
	else if (ToolName == TEXT("select_actors"))
	{
		Result = UUnrealGPTSceneContext::SelectActors(ArgumentsJson);
	}
	else if (ToolName == TEXT("duplicate_actor"))
	{
		Result = UUnrealGPTSceneContext::DuplicateActor(ArgumentsJson);
	}
	else if (ToolName == TEXT("snap_actor_to_ground"))
	{
		Result = UUnrealGPTSceneContext::SnapActorToGround(ArgumentsJson);
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

		// Handle optional input_image for image-to-image models
		FString InputImagePath;
		if (ArgsObj->TryGetStringField(TEXT("input_image"), InputImagePath) && !InputImagePath.IsEmpty())
		{
			FString ImageError;
			FString ImageDataUri = ConvertImageToBase64DataUri(InputImagePath, ImageError);
			
			if (ImageDataUri.IsEmpty())
			{
				return FString::Printf(TEXT("{\"status\":\"error\",\"message\":\"Failed to load input_image: %s\"}"), *ImageError);
			}

			// Determine the parameter name for the image (default: "image")
			FString ImageParamName = TEXT("image");
			ArgsObj->TryGetStringField(TEXT("input_image_param"), ImageParamName);
			if (ImageParamName.IsEmpty())
			{
				ImageParamName = TEXT("image");
			}

			InputObj->SetStringField(*ImageParamName, ImageDataUri);
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added input image as '%s' parameter (%d chars)"), *ImageParamName, ImageDataUri.Len());
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

bool UUnrealGPTAgentClient::ApplyAuthHeaders(TSharedRef<IHttpRequest> Request, FString& OutError) const
{
	UUnrealGPTSettings* SafeSettings = GetMutableDefault<UUnrealGPTSettings>();
	if (!SafeSettings || !IsValid(SafeSettings))
	{
		OutError = TEXT("Settings is null or invalid and could not be retrieved.");
		return false;
	}

	FString BearerToken;
	FString ChatGPTAccountId;
	if (!SafeSettings->ResolveAuthHeaders(BearerToken, ChatGPTAccountId, OutError))
	{
		return false;
	}

	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *BearerToken));
	if (!ChatGPTAccountId.IsEmpty())
	{
		Request->SetHeader(TEXT("ChatGPT-Account-Id"), ChatGPTAccountId);
		Request->SetHeader(TEXT("Origin"), TEXT("https://chatgpt.com"));
		Request->SetHeader(TEXT("Referer"), TEXT("https://chatgpt.com/"));
	}

	return true;
}

bool UUnrealGPTAgentClient::RefreshCodexAuthAndRetry(const FString& RequestBody)
{
	UUnrealGPTSettings* SafeSettings = GetMutableDefault<UUnrealGPTSettings>();
	if (!SafeSettings || !IsValid(SafeSettings))
	{
		return false;
	}

	FString RefreshToken;
	FString RefreshError;
	if (!SafeSettings->GetCodexRefreshToken(RefreshToken, RefreshError))
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Cannot refresh Codex auth: %s"), *RefreshError);
		return false;
	}

	PendingCodexRefreshRetryBody = RequestBody;

	const FString RequestForm = FString::Printf(
		TEXT("grant_type=refresh_token&refresh_token=%s&client_id=%s"),
		*FGenericPlatformHttp::UrlEncode(RefreshToken),
		*FGenericPlatformHttp::UrlEncode(TEXT("app_EMoamEEZ73f0CkXaXp7hrann")));

	TSharedRef<IHttpRequest> RefreshRequest = CreateHttpRequest();
	RefreshRequest->SetURL(TEXT("https://auth.openai.com/oauth/token"));
	RefreshRequest->SetVerb(TEXT("POST"));
	RefreshRequest->SetHeader(TEXT("Content-Type"), TEXT("application/x-www-form-urlencoded"));
	RefreshRequest->SetContentAsString(RequestForm);
	RefreshRequest->OnProcessRequestComplete().BindUObject(this, &UUnrealGPTAgentClient::OnCodexAuthRefreshResponse);

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Refreshing Codex auth after 401"));
	bRequestInProgress = true;
	RefreshRequest->ProcessRequest();
	return true;
}

void UUnrealGPTAgentClient::OnCodexAuthRefreshResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	bRequestInProgress = false;

	if (!bWasSuccessful || !Response.IsValid())
	{
		OnAgentMessage.Broadcast(TEXT("system"), TEXT("Error: Codex auth refresh failed. Please use Codex Login again."), TArray<FString>());
		PendingCodexRefreshRetryBody.Empty();
		return;
	}

	const int32 ResponseCode = Response->GetResponseCode();
	const FString ResponseBody = Response->GetContentAsString();
	if (ResponseCode < 200 || ResponseCode >= 300)
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Codex auth refresh failed %d: %s"), ResponseCode, *ResponseBody);
		OnAgentMessage.Broadcast(TEXT("system"), FString::Printf(TEXT("Error: Codex auth refresh failed. Please use Codex Login again. HTTP %d"), ResponseCode), TArray<FString>());
		PendingCodexRefreshRetryBody.Empty();
		return;
	}

	TSharedPtr<FJsonObject> ResponseJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
	{
		OnAgentMessage.Broadcast(TEXT("system"), TEXT("Error: Codex auth refresh returned invalid JSON. Please use Codex Login again."), TArray<FString>());
		PendingCodexRefreshRetryBody.Empty();
		return;
	}

	FString IdToken;
	FString AccessToken;
	FString RefreshToken;
	ResponseJson->TryGetStringField(TEXT("id_token"), IdToken);
	ResponseJson->TryGetStringField(TEXT("access_token"), AccessToken);
	ResponseJson->TryGetStringField(TEXT("refresh_token"), RefreshToken);
	if (RefreshToken.IsEmpty())
	{
		FString ExistingRefreshToken;
		FString RefreshError;
		if (UUnrealGPTSettings* SafeSettings = GetMutableDefault<UUnrealGPTSettings>())
		{
			SafeSettings->GetCodexRefreshToken(ExistingRefreshToken, RefreshError);
		}
		RefreshToken = ExistingRefreshToken;
	}

	FString SaveError;
	UUnrealGPTSettings* SafeSettings = GetMutableDefault<UUnrealGPTSettings>();
	if (!SafeSettings || IdToken.IsEmpty() || AccessToken.IsEmpty() || RefreshToken.IsEmpty() || !SafeSettings->SaveCodexChatGPTAuth(IdToken, AccessToken, RefreshToken, SaveError))
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Codex auth refresh could not be saved: %s"), *SaveError);
		OnAgentMessage.Broadcast(TEXT("system"), TEXT("Error: Codex auth refresh could not be saved. Please use Codex Login again."), TArray<FString>());
		PendingCodexRefreshRetryBody.Empty();
		return;
	}

	if (PendingCodexRefreshRetryBody.IsEmpty())
	{
		return;
	}

	TSharedRef<IHttpRequest> RetryRequest = CreateHttpRequest();
	RetryRequest->SetURL(GetEffectiveApiUrl());
	RetryRequest->SetVerb(TEXT("POST"));
	RetryRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	FString AuthError;
	if (!ApplyAuthHeaders(RetryRequest, AuthError))
	{
		OnAgentMessage.Broadcast(TEXT("system"), FString::Printf(TEXT("Error: %s"), *AuthError), TArray<FString>());
		PendingCodexRefreshRetryBody.Empty();
		return;
	}

	RetryRequest->SetContentAsString(PendingCodexRefreshRetryBody);
	PendingCodexRefreshRetryBody.Empty();
	RetryRequest->OnProcessRequestComplete().BindUObject(this, &UUnrealGPTAgentClient::OnResponseReceived);

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Retrying request after Codex auth refresh"));
	bRequestInProgress = true;
	RetryRequest->ProcessRequest();
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

	if (SafeSettings->bUseCodexAuth
		&& SafeSettings->bUseCodexResponsesEndpoint
		&& SafeSettings->IsUsingCodexChatGPTAuth())
	{
		const FString CodexResponsesUrl = TEXT("https://chatgpt.com/backend-api/codex/responses");
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Effective API URL (Codex ChatGPT auth overrides configured provider URL): %s"), *CodexResponsesUrl);
		return CodexResponsesUrl;
	}

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
	return ApiUrl.Contains(TEXT("/v1/responses")) || ApiUrl.Contains(TEXT("/backend-api/codex/responses"));
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

FString UUnrealGPTAgentClient::DetermineReasoningEffort(const FString& UserMessage, const TArray<FString>& ImageBase64) const
{
	// Analyze message complexity to determine appropriate reasoning effort
	// Returns "low", "medium", or "high"
	
	// Start with low as baseline
	int32 ComplexityScore = 0;
	
	const FString LowerMessage = UserMessage.ToLower();
	
	// Keywords indicating complex tasks that need higher reasoning
	static const TArray<FString> HighComplexityKeywords = {
		TEXT("architecture"), TEXT("design"), TEXT("implement"), TEXT("refactor"),
		TEXT("optimize"), TEXT("debug"), TEXT("analyze"), TEXT("explain"),
		TEXT("compare"), TEXT("evaluate"), TEXT("strategy"), TEXT("plan"),
		TEXT("complex"), TEXT("multiple"), TEXT("several"), TEXT("many"),
		TEXT("workflow"), TEXT("pipeline"), TEXT("system"), TEXT("framework")
	};
	
	static const TArray<FString> MediumComplexityKeywords = {
		TEXT("create"), TEXT("add"), TEXT("modify"), TEXT("change"),
		TEXT("update"), TEXT("fix"), TEXT("adjust"), TEXT("configure"),
		TEXT("setup"), TEXT("build"), TEXT("make"), TEXT("generate")
	};
	
	// Check for high complexity keywords
	for (const FString& Keyword : HighComplexityKeywords)
	{
		if (LowerMessage.Contains(Keyword))
		{
			ComplexityScore += 3;
		}
	}
	
	// Check for medium complexity keywords
	for (const FString& Keyword : MediumComplexityKeywords)
	{
		if (LowerMessage.Contains(Keyword))
		{
			ComplexityScore += 1;
		}
	}
	
	// Code fences suggest technical/complex request
	if (UserMessage.Contains(TEXT("```")))
	{
		ComplexityScore += 3;
	}
	
	// Images always increase complexity (visual analysis needed)
	if (ImageBase64.Num() > 0)
	{
		ComplexityScore += 5;
	}
	
	// Long messages suggest more complex requests
	if (UserMessage.Len() > 500)
	{
		ComplexityScore += 2;
	}
	else if (UserMessage.Len() > 200)
	{
		ComplexityScore += 1;
	}
	
	// Question marks suggest explanation requests
	int32 QuestionCount = 0;
	for (const TCHAR& Char : UserMessage)
	{
		if (Char == TEXT('?'))
		{
			QuestionCount++;
		}
	}
	if (QuestionCount >= 2)
	{
		ComplexityScore += 2;
	}
	
	// Determine effort level based on score
	if (ComplexityScore >= 8)
	{
		return TEXT("high");
	}
	else if (ComplexityScore >= 4)
	{
		return TEXT("medium");
	}
	else
	{
		return TEXT("low");
	}
}
