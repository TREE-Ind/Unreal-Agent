// Copyright (c) 2025 TREE Industries.

#include "UnrealGPTSceneContext.h"
#include "LevelEditor.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Base64.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Editor/EditorEngine.h"
#include "EngineUtils.h"
#include "Engine/Selection.h"
#include "Math/IntRect.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RenderCommandFence.h"
#include "LevelEditorViewport.h"
#include "EditorViewportClient.h"
#include "Subsystems/EditorActorSubsystem.h"

FString UUnrealGPTSceneContext::CaptureViewportScreenshot()
{
	TArray<uint8> ImageData;
	int32 Width = 0;
	int32 Height = 0;

	if (!CaptureViewportToImage(ImageData, Width, Height))
	{
		return TEXT("");
	}

	// Encode to base64
	FString Base64String = FBase64::Encode(ImageData);
	return Base64String;
}

bool UUnrealGPTSceneContext::CaptureViewportToImage(TArray<uint8>& OutImageData, int32& OutWidth, int32& OutHeight)
{
	if (!GEditor)
	{
		return false;
	}

	FViewport* ViewportWidget = GEditor->GetActiveViewport();
	if (!ViewportWidget)
	{
		return false;
	}

	OutWidth = ViewportWidget->GetSizeXY().X;
	OutHeight = ViewportWidget->GetSizeXY().Y;

	if (OutWidth <= 0 || OutHeight <= 0)
	{
		return false;
	}

	// Check if we're on the game thread (required for ReadPixels)
	if (!IsInGameThread())
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: CaptureViewportToImage must be called from game thread"));
		return false;
	}

	// Flush all rendering commands to ensure the viewport is in a stable state
	// This helps prevent accessing render resources that are being destroyed
	FRenderCommandFence Fence;
	Fence.BeginFence();
	Fence.Wait();

	// Re-check viewport size after flushing to ensure it's still valid
	// Re-acquire the viewport pointer in case it changed during flush
	FViewport* CurrentViewport = GEditor->GetActiveViewport();
	if (!CurrentViewport || CurrentViewport != ViewportWidget)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Viewport changed or became invalid after flush"));
		return false;
	}

	// Re-check size to ensure viewport is still valid
	FIntPoint ViewportSize = CurrentViewport->GetSizeXY();
	if (ViewportSize.X <= 0 || ViewportSize.Y <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Viewport has invalid size after flush: %dx%d"), ViewportSize.X, ViewportSize.Y);
		return false;
	}

	// Update dimensions if they changed
	if (ViewportSize.X != OutWidth || ViewportSize.Y != OutHeight)
	{
		OutWidth = ViewportSize.X;
		OutHeight = ViewportSize.Y;
	}

	// Use the current viewport for ReadPixels
	ViewportWidget = CurrentViewport;

	// Use a safer approach: read pixels with proper error handling
	TArray<FColor> Bitmap;
	FIntRect Rect(0, 0, OutWidth, OutHeight);
	FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
	ReadFlags.SetLinearToGamma(false);
	
	// Attempt to read pixels - this can fail if render resources are invalid
	// We'll check the result carefully
	bool bReadSuccess = ViewportWidget->ReadPixels(Bitmap, ReadFlags, Rect);

	if (!bReadSuccess)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: ReadPixels returned false - viewport may be invalid"));
		return false;
	}

	// Validate the bitmap data
	if (Bitmap.Num() <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: ReadPixels returned empty bitmap"));
		return false;
	}

	const int32 ExpectedPixelCount = OutWidth * OutHeight;
	if (Bitmap.Num() != ExpectedPixelCount)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Invalid bitmap size: %d (expected %d)"), Bitmap.Num(), ExpectedPixelCount);
		return false;
	}

	// Convert to PNG
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

	if (!ImageWrapper.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to create image wrapper"));
		return false;
	}

	// Set raw image data - make sure we're using the correct format
	const int32 ImageDataSize = Bitmap.Num() * sizeof(FColor);
	if (!ImageWrapper->SetRaw(Bitmap.GetData(), ImageDataSize, OutWidth, OutHeight, ERGBFormat::BGRA, 8))
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to set raw image data"));
		return false;
	}

	OutImageData = ImageWrapper->GetCompressed();
	if (OutImageData.Num() <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Image compression produced empty result"));
		return false;
	}

	return true;
}

FString UUnrealGPTSceneContext::CaptureViewportScreenshotWithMetadata(const FString& FocusActorLabel)
{
	TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
	
	if (!GEditor)
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("GEditor is not available"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	// If focus_actor is specified, focus on that actor before capture
	if (!FocusActorLabel.IsEmpty())
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (World)
		{
			for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
			{
				AActor* Actor = *ActorItr;
				if (Actor && !Actor->IsPendingKillPending() && Actor->GetActorLabel() == FocusActorLabel)
				{
					// Select the actor and focus viewport on it
					GEditor->SelectNone(false, true, false);
					GEditor->SelectActor(Actor, true, true);
					GEditor->MoveViewportCamerasToActor(*Actor, false);
					break;
				}
			}
		}
	}

	// Get viewport client for camera info
	FEditorViewportClient* ViewportClient = nullptr;
	if (GEditor->GetActiveViewport())
	{
		ViewportClient = static_cast<FEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());
	}

	// Capture the screenshot
	TArray<uint8> ImageData;
	int32 Width = 0;
	int32 Height = 0;

	if (!CaptureViewportToImage(ImageData, Width, Height))
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("Failed to capture viewport"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	// Build result with metadata
	ResultJson->SetStringField(TEXT("status"), TEXT("ok"));
	ResultJson->SetStringField(TEXT("image_base64"), FBase64::Encode(ImageData));
	
	// Resolution
	TSharedPtr<FJsonObject> ResolutionJson = MakeShareable(new FJsonObject);
	ResolutionJson->SetNumberField(TEXT("width"), Width);
	ResolutionJson->SetNumberField(TEXT("height"), Height);
	ResultJson->SetObjectField(TEXT("resolution"), ResolutionJson);
	
	// Camera info
	if (ViewportClient)
	{
		TSharedPtr<FJsonObject> CameraJson = MakeShareable(new FJsonObject);
		
		const FVector CameraLocation = ViewportClient->GetViewLocation();
		TSharedPtr<FJsonObject> LocationJson = MakeShareable(new FJsonObject);
		LocationJson->SetNumberField(TEXT("x"), CameraLocation.X);
		LocationJson->SetNumberField(TEXT("y"), CameraLocation.Y);
		LocationJson->SetNumberField(TEXT("z"), CameraLocation.Z);
		CameraJson->SetObjectField(TEXT("location"), LocationJson);
		
		const FRotator CameraRotation = ViewportClient->GetViewRotation();
		TSharedPtr<FJsonObject> RotationJson = MakeShareable(new FJsonObject);
		RotationJson->SetNumberField(TEXT("pitch"), CameraRotation.Pitch);
		RotationJson->SetNumberField(TEXT("yaw"), CameraRotation.Yaw);
		RotationJson->SetNumberField(TEXT("roll"), CameraRotation.Roll);
		CameraJson->SetObjectField(TEXT("rotation"), RotationJson);
		
		CameraJson->SetNumberField(TEXT("fov"), ViewportClient->ViewFOV);
		
		ResultJson->SetObjectField(TEXT("camera"), CameraJson);
	}
	
	// Selected actors
	TArray<AActor*> SelectedActors;
	GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(SelectedActors);
	
	if (SelectedActors.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> SelectedArray;
		for (AActor* Actor : SelectedActors)
		{
			if (Actor && !Actor->IsPendingKillPending())
			{
				TSharedPtr<FJsonObject> ActorJson = MakeShareable(new FJsonObject);
				ActorJson->SetStringField(TEXT("label"), Actor->GetActorLabel());
				ActorJson->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
				SelectedArray.Add(MakeShareable(new FJsonValueObject(ActorJson)));
			}
		}
		ResultJson->SetArrayField(TEXT("selected_actors"), SelectedArray);
	}

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
	return OutputString;
}

FString UUnrealGPTSceneContext::GetSceneSummary(int32 PageSize, int32 PageIndex)
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return TEXT("{}");
	}

	TArray<TSharedPtr<FJsonValue>> ActorsArray;
	int32 ActorCount = 0;
	int32 StartIndex = PageIndex * PageSize;
	int32 EndIndex = StartIndex + PageSize;

	for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
	{
		AActor* Actor = *ActorItr;
		if (!Actor || Actor->IsPendingKillPending())
		{
			continue;
		}

		if (ActorCount >= StartIndex && ActorCount < EndIndex)
		{
			TSharedPtr<FJsonObject> ActorJson = SerializeActor(Actor);
			if (ActorJson.IsValid())
			{
				ActorsArray.Add(MakeShareable(new FJsonValueObject(ActorJson)));
			}
		}

		ActorCount++;
	}

	TSharedPtr<FJsonObject> SummaryJson = MakeShareable(new FJsonObject);
	SummaryJson->SetNumberField(TEXT("total_actors"), ActorCount);
	SummaryJson->SetNumberField(TEXT("page_size"), PageSize);
	SummaryJson->SetNumberField(TEXT("page_index"), PageIndex);
	SummaryJson->SetNumberField(TEXT("actors_on_page"), ActorsArray.Num());
	SummaryJson->SetArrayField(TEXT("actors"), ActorsArray);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(SummaryJson.ToSharedRef(), Writer);

	return OutputString;
}

FString UUnrealGPTSceneContext::QueryScene(const FString& ArgumentsJson)
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return TEXT("[]");
	}

	// Parse arguments JSON
	TSharedPtr<FJsonObject> ArgsObj;
	if (!ArgumentsJson.IsEmpty())
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
		FJsonSerializer::Deserialize(Reader, ArgsObj);
	}

	auto GetStringArg = [&ArgsObj](const FString& Key) -> FString
	{
		FString Value;
		if (ArgsObj.IsValid())
		{
			ArgsObj->TryGetStringField(Key, Value);
		}
		return Value;
	};

	auto GetIntArg = [&ArgsObj](const FString& Key, int32 DefaultValue) -> int32
	{
		int32 Value = DefaultValue;
		if (ArgsObj.IsValid())
		{
			ArgsObj->TryGetNumberField(Key, Value);
		}
		return Value;
	};

	auto GetBoolArg = [&ArgsObj](const FString& Key, bool DefaultValue) -> bool
	{
		bool Value = DefaultValue;
		if (ArgsObj.IsValid())
		{
			ArgsObj->TryGetBoolField(Key, Value);
		}
		return Value;
	};

	const FString ClassContains = GetStringArg(TEXT("class_contains"));
	const FString LabelContains = GetStringArg(TEXT("label_contains"));
	const FString NameContains = GetStringArg(TEXT("name_contains"));
	const FString ComponentClassContains = GetStringArg(TEXT("component_class_contains"));
	const int32 MaxResults = FMath::Max(1, GetIntArg(TEXT("max_results"), 20));
	
	// Detail flags to control payload size
	const bool bIncludeTransform = GetBoolArg(TEXT("include_transform"), true);
	const bool bIncludeBounds = GetBoolArg(TEXT("include_bounds"), false);
	const bool bIncludeComponents = GetBoolArg(TEXT("include_components"), false);
	const bool bIncludeMetadata = GetBoolArg(TEXT("include_metadata"), false);

	TArray<TSharedPtr<FJsonValue>> ResultsArray;

	for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
	{
		AActor* Actor = *ActorItr;
		if (!Actor || Actor->IsPendingKillPending())
		{
			continue;
		}

		const FString ClassName = Actor->GetClass()->GetName();
		const FString Label = Actor->GetActorLabel();
		const FString Name = Actor->GetName();

		// Apply simple substring filters (case-insensitive)
		auto MatchesFilter = [](const FString& Source, const FString& Substr) -> bool
		{
			return Substr.IsEmpty() || Source.Contains(Substr, ESearchCase::IgnoreCase);
		};

		if (!MatchesFilter(ClassName, ClassContains))
		{
			continue;
		}
		if (!MatchesFilter(Label, LabelContains))
		{
			continue;
		}
		if (!MatchesFilter(Name, NameContains))
		{
			continue;
		}

		// Optional component class filter
		if (!ComponentClassContains.IsEmpty())
		{
			bool bHasMatchingComponent = false;
			TArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			for (UActorComponent* Component : Components)
			{
				if (Component && Component->GetClass()->GetName().Contains(ComponentClassContains, ESearchCase::IgnoreCase))
				{
					bHasMatchingComponent = true;
					break;
				}
			}

			if (!bHasMatchingComponent)
			{
				continue;
			}
		}

		// Build a compact JSON object for this actor
		TSharedPtr<FJsonObject> ActorJson = MakeShareable(new FJsonObject);
		ActorJson->SetStringField(TEXT("name"), Name);
		ActorJson->SetStringField(TEXT("label"), Label);
		ActorJson->SetStringField(TEXT("class"), ClassName);

		// Location is always included
		const FVector Location = Actor->GetActorLocation();
		TSharedPtr<FJsonObject> LocationJson = MakeShareable(new FJsonObject);
		LocationJson->SetNumberField(TEXT("x"), Location.X);
		LocationJson->SetNumberField(TEXT("y"), Location.Y);
		LocationJson->SetNumberField(TEXT("z"), Location.Z);
		ActorJson->SetObjectField(TEXT("location"), LocationJson);

		// include_transform: rotation + scale
		if (bIncludeTransform)
		{
			const FRotator Rotation = Actor->GetActorRotation();
			TSharedPtr<FJsonObject> RotationJson = MakeShareable(new FJsonObject);
			RotationJson->SetNumberField(TEXT("pitch"), Rotation.Pitch);
			RotationJson->SetNumberField(TEXT("yaw"), Rotation.Yaw);
			RotationJson->SetNumberField(TEXT("roll"), Rotation.Roll);
			ActorJson->SetObjectField(TEXT("rotation"), RotationJson);

			const FVector Scale = Actor->GetActorScale3D();
			TSharedPtr<FJsonObject> ScaleJson = MakeShareable(new FJsonObject);
			ScaleJson->SetNumberField(TEXT("x"), Scale.X);
			ScaleJson->SetNumberField(TEXT("y"), Scale.Y);
			ScaleJson->SetNumberField(TEXT("z"), Scale.Z);
			ActorJson->SetObjectField(TEXT("scale"), ScaleJson);
		}

		// include_bounds: origin + extent
		if (bIncludeBounds)
		{
			FVector Origin, Extent;
			Actor->GetActorBounds(false, Origin, Extent);
			
			TSharedPtr<FJsonObject> BoundsJson = MakeShareable(new FJsonObject);
			TSharedPtr<FJsonObject> OriginJson = MakeShareable(new FJsonObject);
			OriginJson->SetNumberField(TEXT("x"), Origin.X);
			OriginJson->SetNumberField(TEXT("y"), Origin.Y);
			OriginJson->SetNumberField(TEXT("z"), Origin.Z);
			BoundsJson->SetObjectField(TEXT("origin"), OriginJson);
			
			TSharedPtr<FJsonObject> ExtentJson = MakeShareable(new FJsonObject);
			ExtentJson->SetNumberField(TEXT("x"), Extent.X);
			ExtentJson->SetNumberField(TEXT("y"), Extent.Y);
			ExtentJson->SetNumberField(TEXT("z"), Extent.Z);
			BoundsJson->SetObjectField(TEXT("extent"), ExtentJson);
			
			ActorJson->SetObjectField(TEXT("bounds"), BoundsJson);
		}

		// include_components: root component, mobility, static_mesh_path
		if (bIncludeComponents)
		{
			USceneComponent* RootComp = Actor->GetRootComponent();
			if (RootComp)
			{
				ActorJson->SetStringField(TEXT("root_component"), RootComp->GetClass()->GetName());
				ActorJson->SetStringField(TEXT("mobility"), UEnum::GetValueAsString(RootComp->Mobility));
			}
			
			// Check for static mesh
			if (UStaticMeshComponent* MeshComp = Actor->FindComponentByClass<UStaticMeshComponent>())
			{
				if (UStaticMesh* Mesh = MeshComp->GetStaticMesh())
				{
					ActorJson->SetStringField(TEXT("static_mesh_path"), Mesh->GetPathName());
				}
			}
		}

		// include_metadata: tags, folder_path, parent_actor
		if (bIncludeMetadata)
		{
			// Tags
			if (Actor->Tags.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> TagsArray;
				for (const FName& Tag : Actor->Tags)
				{
					TagsArray.Add(MakeShareable(new FJsonValueString(Tag.ToString())));
				}
				ActorJson->SetArrayField(TEXT("tags"), TagsArray);
			}
			
			// Folder path
			ActorJson->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
			
			// Parent actor
			if (AActor* Parent = Actor->GetAttachParentActor())
			{
				ActorJson->SetStringField(TEXT("parent_actor"), Parent->GetActorLabel());
			}
		}

		ResultsArray.Add(MakeShareable(new FJsonValueObject(ActorJson)));

		if (ResultsArray.Num() >= MaxResults)
		{
			break;
		}
	}

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(ResultsArray, Writer);
	return OutputString;
}

FString UUnrealGPTSceneContext::GetSelectedActorsSummary()
{
	TArray<AActor*> SelectedActors;
	GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(SelectedActors);

	TArray<TSharedPtr<FJsonValue>> ActorsArray;

	for (AActor* Actor : SelectedActors)
	{
		if (Actor && !Actor->IsPendingKillPending())
		{
			TSharedPtr<FJsonObject> ActorJson = SerializeActor(Actor);
			if (ActorJson.IsValid())
			{
				ActorsArray.Add(MakeShareable(new FJsonValueObject(ActorJson)));
			}
		}
	}

	TSharedPtr<FJsonObject> SummaryJson = MakeShareable(new FJsonObject);
	SummaryJson->SetNumberField(TEXT("selected_count"), SelectedActors.Num());
	SummaryJson->SetArrayField(TEXT("actors"), ActorsArray);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(SummaryJson.ToSharedRef(), Writer);

	return OutputString;
}

// ============================================================================
// New Atomic Editor Tools
// ============================================================================

FString UUnrealGPTSceneContext::GetActor(const FString& ArgumentsJson)
{
	TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
	
	// Parse arguments
	TSharedPtr<FJsonObject> ArgsObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!FJsonSerializer::Deserialize(Reader, ArgsObj) || !ArgsObj.IsValid())
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("Failed to parse arguments"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	FString Label, Name;
	ArgsObj->TryGetStringField(TEXT("label"), Label);
	ArgsObj->TryGetStringField(TEXT("name"), Name);

	if (Label.IsEmpty() && Name.IsEmpty())
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("Either 'label' or 'name' must be provided"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("No world available"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	AActor* FoundActor = nullptr;
	for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
	{
		AActor* Actor = *ActorItr;
		if (!Actor || Actor->IsPendingKillPending())
		{
			continue;
		}

		if ((!Label.IsEmpty() && Actor->GetActorLabel() == Label) ||
			(!Name.IsEmpty() && Actor->GetName() == Name))
		{
			FoundActor = Actor;
			break;
		}
	}

	if (!FoundActor)
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), FString::Printf(TEXT("Actor not found: label='%s', name='%s'"), *Label, *Name));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	ResultJson->SetStringField(TEXT("status"), TEXT("ok"));
	ResultJson->SetObjectField(TEXT("actor"), SerializeActor(FoundActor));
	
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
	return OutputString;
}

FString UUnrealGPTSceneContext::SetActorTransform(const FString& ArgumentsJson)
{
	TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
	
	// Parse arguments
	TSharedPtr<FJsonObject> ArgsObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!FJsonSerializer::Deserialize(Reader, ArgsObj) || !ArgsObj.IsValid())
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("Failed to parse arguments"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	FString Label;
	if (!ArgsObj->TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("'label' is required"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("No world available"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	AActor* FoundActor = nullptr;
	for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
	{
		AActor* Actor = *ActorItr;
		if (!Actor || Actor->IsPendingKillPending())
		{
			continue;
		}

		if (Actor->GetActorLabel() == Label)
		{
			FoundActor = Actor;
			break;
		}
	}

	if (!FoundActor)
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), FString::Printf(TEXT("Actor not found: '%s'"), *Label));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	// Begin transaction for undo support
	GEditor->BeginTransaction(FText::FromString(TEXT("Set Actor Transform")));
	FoundActor->Modify();

	// Apply location if provided
	const TSharedPtr<FJsonObject>* LocationObj = nullptr;
	if (ArgsObj->TryGetObjectField(TEXT("location"), LocationObj) && LocationObj && (*LocationObj).IsValid())
	{
		FVector NewLocation = FoundActor->GetActorLocation();
		double X, Y, Z;
		if ((*LocationObj)->TryGetNumberField(TEXT("x"), X)) NewLocation.X = X;
		if ((*LocationObj)->TryGetNumberField(TEXT("y"), Y)) NewLocation.Y = Y;
		if ((*LocationObj)->TryGetNumberField(TEXT("z"), Z)) NewLocation.Z = Z;
		FoundActor->SetActorLocation(NewLocation);
	}

	// Apply rotation if provided
	const TSharedPtr<FJsonObject>* RotationObj = nullptr;
	if (ArgsObj->TryGetObjectField(TEXT("rotation"), RotationObj) && RotationObj && (*RotationObj).IsValid())
	{
		FRotator NewRotation = FoundActor->GetActorRotation();
		double Pitch, Yaw, Roll;
		if ((*RotationObj)->TryGetNumberField(TEXT("pitch"), Pitch)) NewRotation.Pitch = Pitch;
		if ((*RotationObj)->TryGetNumberField(TEXT("yaw"), Yaw)) NewRotation.Yaw = Yaw;
		if ((*RotationObj)->TryGetNumberField(TEXT("roll"), Roll)) NewRotation.Roll = Roll;
		FoundActor->SetActorRotation(NewRotation);
	}

	// Apply scale if provided
	const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
	if (ArgsObj->TryGetObjectField(TEXT("scale"), ScaleObj) && ScaleObj && (*ScaleObj).IsValid())
	{
		FVector NewScale = FoundActor->GetActorScale3D();
		double X, Y, Z;
		if ((*ScaleObj)->TryGetNumberField(TEXT("x"), X)) NewScale.X = X;
		if ((*ScaleObj)->TryGetNumberField(TEXT("y"), Y)) NewScale.Y = Y;
		if ((*ScaleObj)->TryGetNumberField(TEXT("z"), Z)) NewScale.Z = Z;
		FoundActor->SetActorScale3D(NewScale);
	}

	GEditor->EndTransaction();

	ResultJson->SetStringField(TEXT("status"), TEXT("ok"));
	ResultJson->SetStringField(TEXT("message"), FString::Printf(TEXT("Transform updated for '%s'"), *Label));
	ResultJson->SetStringField(TEXT("transaction"), TEXT("SetActorTransform"));
	
	// Return the new transform
	TSharedPtr<FJsonObject> DetailsJson = MakeShareable(new FJsonObject);
	DetailsJson->SetStringField(TEXT("actor_label"), Label);
	
	const FVector Location = FoundActor->GetActorLocation();
	TSharedPtr<FJsonObject> LocationJson = MakeShareable(new FJsonObject);
	LocationJson->SetNumberField(TEXT("x"), Location.X);
	LocationJson->SetNumberField(TEXT("y"), Location.Y);
	LocationJson->SetNumberField(TEXT("z"), Location.Z);
	DetailsJson->SetObjectField(TEXT("location"), LocationJson);
	
	const FRotator Rotation = FoundActor->GetActorRotation();
	TSharedPtr<FJsonObject> RotationJson = MakeShareable(new FJsonObject);
	RotationJson->SetNumberField(TEXT("pitch"), Rotation.Pitch);
	RotationJson->SetNumberField(TEXT("yaw"), Rotation.Yaw);
	RotationJson->SetNumberField(TEXT("roll"), Rotation.Roll);
	DetailsJson->SetObjectField(TEXT("rotation"), RotationJson);
	
	const FVector Scale = FoundActor->GetActorScale3D();
	TSharedPtr<FJsonObject> ScaleJson = MakeShareable(new FJsonObject);
	ScaleJson->SetNumberField(TEXT("x"), Scale.X);
	ScaleJson->SetNumberField(TEXT("y"), Scale.Y);
	ScaleJson->SetNumberField(TEXT("z"), Scale.Z);
	DetailsJson->SetObjectField(TEXT("scale"), ScaleJson);
	
	ResultJson->SetObjectField(TEXT("details"), DetailsJson);
	
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
	return OutputString;
}

FString UUnrealGPTSceneContext::SetActorsRotation(const FString& ArgumentsJson)
{
	TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
	
	// Parse arguments
	TSharedPtr<FJsonObject> ArgsObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!FJsonSerializer::Deserialize(Reader, ArgsObj) || !ArgsObj.IsValid())
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("Failed to parse arguments"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	// Get labels array
	const TArray<TSharedPtr<FJsonValue>>* LabelsArray = nullptr;
	if (!ArgsObj->TryGetArrayField(TEXT("labels"), LabelsArray) || !LabelsArray || LabelsArray->Num() == 0)
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("'labels' array is required and must not be empty"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	// Get rotation
	const TSharedPtr<FJsonObject>* RotationObj = nullptr;
	if (!ArgsObj->TryGetObjectField(TEXT("rotation"), RotationObj) || !RotationObj || !(*RotationObj).IsValid())
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("'rotation' object is required with pitch, yaw, roll"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
	(*RotationObj)->TryGetNumberField(TEXT("pitch"), Pitch);
	(*RotationObj)->TryGetNumberField(TEXT("yaw"), Yaw);
	(*RotationObj)->TryGetNumberField(TEXT("roll"), Roll);
	FRotator NewRotation(Pitch, Yaw, Roll);

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("No world available"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	// Build a set of labels to find
	TSet<FString> LabelsToFind;
	for (const TSharedPtr<FJsonValue>& LabelVal : *LabelsArray)
	{
		if (LabelVal.IsValid() && LabelVal->Type == EJson::String)
		{
			LabelsToFind.Add(LabelVal->AsString());
		}
	}

	// Find all matching actors
	TArray<AActor*> FoundActors;
	TArray<FString> FoundLabels;
	for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
	{
		AActor* Actor = *ActorItr;
		if (!Actor || Actor->IsPendingKillPending())
		{
			continue;
		}

		FString ActorLabel = Actor->GetActorLabel();
		if (LabelsToFind.Contains(ActorLabel))
		{
			FoundActors.Add(Actor);
			FoundLabels.Add(ActorLabel);
		}
	}

	if (FoundActors.Num() == 0)
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("No actors found matching the provided labels"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	// Begin transaction for undo support
	GEditor->BeginTransaction(FText::FromString(TEXT("Set Actors Rotation")));
	
	for (AActor* Actor : FoundActors)
	{
		Actor->Modify();
		Actor->SetActorRotation(NewRotation);
	}
	
	GEditor->EndTransaction();

	ResultJson->SetStringField(TEXT("status"), TEXT("ok"));
	ResultJson->SetStringField(TEXT("message"), FString::Printf(TEXT("Set rotation on %d actor(s)"), FoundActors.Num()));
	
	TSharedPtr<FJsonObject> DetailsJson = MakeShareable(new FJsonObject);
	DetailsJson->SetNumberField(TEXT("count"), FoundActors.Num());
	
	TArray<TSharedPtr<FJsonValue>> UpdatedLabelsArray;
	for (const FString& Label : FoundLabels)
	{
		UpdatedLabelsArray.Add(MakeShareable(new FJsonValueString(Label)));
	}
	DetailsJson->SetArrayField(TEXT("updated_labels"), UpdatedLabelsArray);
	
	TSharedPtr<FJsonObject> RotationResultJson = MakeShareable(new FJsonObject);
	RotationResultJson->SetNumberField(TEXT("pitch"), NewRotation.Pitch);
	RotationResultJson->SetNumberField(TEXT("yaw"), NewRotation.Yaw);
	RotationResultJson->SetNumberField(TEXT("roll"), NewRotation.Roll);
	DetailsJson->SetObjectField(TEXT("rotation"), RotationResultJson);
	
	ResultJson->SetObjectField(TEXT("details"), DetailsJson);
	
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
	return OutputString;
}

FString UUnrealGPTSceneContext::SelectActors(const FString& ArgumentsJson)
{
	TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
	
	// Parse arguments
	TSharedPtr<FJsonObject> ArgsObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!FJsonSerializer::Deserialize(Reader, ArgsObj) || !ArgsObj.IsValid())
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("Failed to parse arguments"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	const TArray<TSharedPtr<FJsonValue>>* LabelsArray = nullptr;
	if (!ArgsObj->TryGetArrayField(TEXT("labels"), LabelsArray) || !LabelsArray || LabelsArray->Num() == 0)
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("'labels' array is required"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	bool bAddToSelection = false;
	ArgsObj->TryGetBoolField(TEXT("add_to_selection"), bAddToSelection);

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("No world available"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	// Build label set for faster lookup
	TSet<FString> LabelSet;
	for (const TSharedPtr<FJsonValue>& Value : *LabelsArray)
	{
		FString Label;
		if (Value.IsValid() && Value->TryGetString(Label) && !Label.IsEmpty())
		{
			LabelSet.Add(Label);
		}
	}

	// Clear selection if not adding
	if (!bAddToSelection)
	{
		GEditor->SelectNone(false, true, false);
	}

	// Find and select actors
	TArray<FString> SelectedLabels;
	for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
	{
		AActor* Actor = *ActorItr;
		if (!Actor || Actor->IsPendingKillPending())
		{
			continue;
		}

		if (LabelSet.Contains(Actor->GetActorLabel()))
		{
			GEditor->SelectActor(Actor, true, true);
			SelectedLabels.Add(Actor->GetActorLabel());
		}
	}

	ResultJson->SetStringField(TEXT("status"), TEXT("ok"));
	ResultJson->SetStringField(TEXT("message"), FString::Printf(TEXT("Selected %d actors"), SelectedLabels.Num()));
	
	TArray<TSharedPtr<FJsonValue>> SelectedArray;
	for (const FString& Label : SelectedLabels)
	{
		SelectedArray.Add(MakeShareable(new FJsonValueString(Label)));
	}
	ResultJson->SetArrayField(TEXT("selected"), SelectedArray);
	
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
	return OutputString;
}

FString UUnrealGPTSceneContext::DuplicateActor(const FString& ArgumentsJson)
{
	TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
	
	// Parse arguments
	TSharedPtr<FJsonObject> ArgsObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!FJsonSerializer::Deserialize(Reader, ArgsObj) || !ArgsObj.IsValid())
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("Failed to parse arguments"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	FString Label;
	if (!ArgsObj->TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("'label' is required"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("No world available"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	AActor* FoundActor = nullptr;
	for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
	{
		AActor* Actor = *ActorItr;
		if (!Actor || Actor->IsPendingKillPending())
		{
			continue;
		}

		if (Actor->GetActorLabel() == Label)
		{
			FoundActor = Actor;
			break;
		}
	}

	if (!FoundActor)
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), FString::Printf(TEXT("Actor not found: '%s'"), *Label));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	// Get offset
	FVector Offset = FVector::ZeroVector;
	const TSharedPtr<FJsonObject>* OffsetObj = nullptr;
	if (ArgsObj->TryGetObjectField(TEXT("offset"), OffsetObj) && OffsetObj && (*OffsetObj).IsValid())
	{
		double X, Y, Z;
		if ((*OffsetObj)->TryGetNumberField(TEXT("x"), X)) Offset.X = X;
		if ((*OffsetObj)->TryGetNumberField(TEXT("y"), Y)) Offset.Y = Y;
		if ((*OffsetObj)->TryGetNumberField(TEXT("z"), Z)) Offset.Z = Z;
	}

	// Get new label (optional)
	FString NewLabel;
	ArgsObj->TryGetStringField(TEXT("new_label"), NewLabel);

	// Begin transaction for undo support
	GEditor->BeginTransaction(FText::FromString(TEXT("Duplicate Actor")));

	// Use EditorActorSubsystem to duplicate
	UEditorActorSubsystem* ActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
	AActor* DuplicatedActor = nullptr;
	
	if (ActorSubsystem)
	{
		DuplicatedActor = ActorSubsystem->DuplicateActor(FoundActor, World);
	}

	if (!DuplicatedActor)
	{
		GEditor->EndTransaction();
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("Failed to duplicate actor"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	// Apply offset
	if (!Offset.IsZero())
	{
		DuplicatedActor->SetActorLocation(DuplicatedActor->GetActorLocation() + Offset);
	}

	// Set new label if provided
	if (!NewLabel.IsEmpty())
	{
		DuplicatedActor->SetActorLabel(NewLabel);
	}

	GEditor->EndTransaction();

	ResultJson->SetStringField(TEXT("status"), TEXT("ok"));
	ResultJson->SetStringField(TEXT("message"), FString::Printf(TEXT("Duplicated '%s' to '%s'"), *Label, *DuplicatedActor->GetActorLabel()));
	ResultJson->SetStringField(TEXT("transaction"), TEXT("DuplicateActor"));
	
	TSharedPtr<FJsonObject> DetailsJson = MakeShareable(new FJsonObject);
	DetailsJson->SetStringField(TEXT("actor_label"), DuplicatedActor->GetActorLabel());
	DetailsJson->SetStringField(TEXT("actor_name"), DuplicatedActor->GetName());
	
	const FVector Location = DuplicatedActor->GetActorLocation();
	TSharedPtr<FJsonObject> LocationJson = MakeShareable(new FJsonObject);
	LocationJson->SetNumberField(TEXT("x"), Location.X);
	LocationJson->SetNumberField(TEXT("y"), Location.Y);
	LocationJson->SetNumberField(TEXT("z"), Location.Z);
	DetailsJson->SetObjectField(TEXT("location"), LocationJson);
	
	ResultJson->SetObjectField(TEXT("details"), DetailsJson);
	
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
	return OutputString;
}

FString UUnrealGPTSceneContext::SnapActorToGround(const FString& ArgumentsJson)
{
	TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
	
	// Parse arguments
	TSharedPtr<FJsonObject> ArgsObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!FJsonSerializer::Deserialize(Reader, ArgsObj) || !ArgsObj.IsValid())
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("Failed to parse arguments"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	FString Label;
	if (!ArgsObj->TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("'label' is required"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	bool bAlignToNormal = false;
	ArgsObj->TryGetBoolField(TEXT("align_to_normal"), bAlignToNormal);

	double VerticalOffset = 0.0;
	ArgsObj->TryGetNumberField(TEXT("offset"), VerticalOffset);

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("No world available"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	AActor* FoundActor = nullptr;
	for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
	{
		AActor* Actor = *ActorItr;
		if (!Actor || Actor->IsPendingKillPending())
		{
			continue;
		}

		if (Actor->GetActorLabel() == Label)
		{
			FoundActor = Actor;
			break;
		}
	}

	if (!FoundActor)
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), FString::Printf(TEXT("Actor not found: '%s'"), *Label));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	// Perform line trace downward
	FVector Start = FoundActor->GetActorLocation();
	FVector End = Start - FVector(0, 0, 100000.0f); // Trace down 100000 units

	FHitResult HitResult;
	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(FoundActor);

	bool bHit = World->LineTraceSingleByChannel(HitResult, Start, End, ECC_WorldStatic, QueryParams);

	if (!bHit)
	{
		ResultJson->SetStringField(TEXT("status"), TEXT("error"));
		ResultJson->SetStringField(TEXT("message"), TEXT("No ground found below actor"));
		
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
		return OutputString;
	}

	// Begin transaction for undo support
	GEditor->BeginTransaction(FText::FromString(TEXT("Snap Actor to Ground")));
	FoundActor->Modify();

	// Set new location
	FVector NewLocation = HitResult.ImpactPoint + FVector(0, 0, VerticalOffset);
	FoundActor->SetActorLocation(NewLocation);

	// Optionally align to surface normal
	if (bAlignToNormal)
	{
		FVector UpVector = HitResult.ImpactNormal;
		FVector ForwardVector = FoundActor->GetActorForwardVector();
		FVector RightVector = FVector::CrossProduct(UpVector, ForwardVector).GetSafeNormal();
		ForwardVector = FVector::CrossProduct(RightVector, UpVector);
		
		FRotator NewRotation = FRotationMatrix::MakeFromXZ(ForwardVector, UpVector).Rotator();
		FoundActor->SetActorRotation(NewRotation);
	}

	GEditor->EndTransaction();

	ResultJson->SetStringField(TEXT("status"), TEXT("ok"));
	ResultJson->SetStringField(TEXT("message"), FString::Printf(TEXT("Snapped '%s' to ground"), *Label));
	ResultJson->SetStringField(TEXT("transaction"), TEXT("SnapActorToGround"));
	
	TSharedPtr<FJsonObject> DetailsJson = MakeShareable(new FJsonObject);
	DetailsJson->SetStringField(TEXT("actor_label"), Label);
	
	TSharedPtr<FJsonObject> LocationJson = MakeShareable(new FJsonObject);
	LocationJson->SetNumberField(TEXT("x"), NewLocation.X);
	LocationJson->SetNumberField(TEXT("y"), NewLocation.Y);
	LocationJson->SetNumberField(TEXT("z"), NewLocation.Z);
	DetailsJson->SetObjectField(TEXT("location"), LocationJson);
	
	TSharedPtr<FJsonObject> HitJson = MakeShareable(new FJsonObject);
	HitJson->SetStringField(TEXT("hit_actor"), HitResult.GetActor() ? HitResult.GetActor()->GetActorLabel() : TEXT(""));
	TSharedPtr<FJsonObject> NormalJson = MakeShareable(new FJsonObject);
	NormalJson->SetNumberField(TEXT("x"), HitResult.ImpactNormal.X);
	NormalJson->SetNumberField(TEXT("y"), HitResult.ImpactNormal.Y);
	NormalJson->SetNumberField(TEXT("z"), HitResult.ImpactNormal.Z);
	HitJson->SetObjectField(TEXT("normal"), NormalJson);
	DetailsJson->SetObjectField(TEXT("hit"), HitJson);
	
	ResultJson->SetObjectField(TEXT("details"), DetailsJson);
	
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);
	return OutputString;
}

TSharedPtr<FJsonObject> UUnrealGPTSceneContext::SerializeActor(AActor* Actor)
{
	if (!Actor)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> ActorJson = MakeShareable(new FJsonObject);
	
	ActorJson->SetStringField(TEXT("name"), Actor->GetName());
	ActorJson->SetStringField(TEXT("label"), Actor->GetActorLabel());
	ActorJson->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

	// Transform
	FTransform Transform = Actor->GetActorTransform();
	TSharedPtr<FJsonObject> LocationJson = MakeShareable(new FJsonObject);
	LocationJson->SetNumberField(TEXT("x"), Transform.GetLocation().X);
	LocationJson->SetNumberField(TEXT("y"), Transform.GetLocation().Y);
	LocationJson->SetNumberField(TEXT("z"), Transform.GetLocation().Z);
	ActorJson->SetObjectField(TEXT("location"), LocationJson);

	TSharedPtr<FJsonObject> RotationJson = MakeShareable(new FJsonObject);
	FRotator Rotation = Transform.GetRotation().Rotator();
	RotationJson->SetNumberField(TEXT("pitch"), Rotation.Pitch);
	RotationJson->SetNumberField(TEXT("yaw"), Rotation.Yaw);
	RotationJson->SetNumberField(TEXT("roll"), Rotation.Roll);
	ActorJson->SetObjectField(TEXT("rotation"), RotationJson);

	TSharedPtr<FJsonObject> ScaleJson = MakeShareable(new FJsonObject);
	FVector Scale = Transform.GetScale3D();
	ScaleJson->SetNumberField(TEXT("x"), Scale.X);
	ScaleJson->SetNumberField(TEXT("y"), Scale.Y);
	ScaleJson->SetNumberField(TEXT("z"), Scale.Z);
	ActorJson->SetObjectField(TEXT("scale"), ScaleJson);

	// Components
	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);

	TArray<TSharedPtr<FJsonValue>> ComponentsArray;
	for (UActorComponent* Component : Components)
	{
		if (Component)
		{
			TSharedPtr<FJsonObject> ComponentJson = SerializeComponent(Component);
			if (ComponentJson.IsValid())
			{
				ComponentsArray.Add(MakeShareable(new FJsonValueObject(ComponentJson)));
			}
		}
	}
	ActorJson->SetArrayField(TEXT("components"), ComponentsArray);

	return ActorJson;
}

TSharedPtr<FJsonObject> UUnrealGPTSceneContext::SerializeComponent(UActorComponent* Component)
{
	if (!Component)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> ComponentJson = MakeShareable(new FJsonObject);
	ComponentJson->SetStringField(TEXT("name"), Component->GetName());
	ComponentJson->SetStringField(TEXT("class"), Component->GetClass()->GetName());
	ComponentJson->SetBoolField(TEXT("is_active"), Component->IsActive());

	return ComponentJson;
}

