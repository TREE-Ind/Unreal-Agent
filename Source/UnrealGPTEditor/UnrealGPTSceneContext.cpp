#include "UnrealGPTSceneContext.h"
#include "LevelEditor.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
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

	const FString ClassContains = GetStringArg(TEXT("class_contains"));
	const FString LabelContains = GetStringArg(TEXT("label_contains"));
	const FString NameContains = GetStringArg(TEXT("name_contains"));
	const FString ComponentClassContains = GetStringArg(TEXT("component_class_contains"));
	const int32 MaxResults = FMath::Max(1, GetIntArg(TEXT("max_results"), 20));

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

		// Minimal transform info (location only to save tokens)
		const FVector Location = Actor->GetActorLocation();
		TSharedPtr<FJsonObject> LocationJson = MakeShareable(new FJsonObject);
		LocationJson->SetNumberField(TEXT("x"), Location.X);
		LocationJson->SetNumberField(TEXT("y"), Location.Y);
		LocationJson->SetNumberField(TEXT("z"), Location.Z);
		ActorJson->SetObjectField(TEXT("location"), LocationJson);

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

