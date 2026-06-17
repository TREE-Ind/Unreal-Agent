// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Engine/World.h"
#include "UnrealGPTSceneContext.generated.h"

UCLASS()
class UNREALGPTEDITOR_API UUnrealGPTSceneContext : public UObject
{
	GENERATED_BODY()

public:
	/** Capture a screenshot of the active viewport and return as base64 PNG */
	static FString CaptureViewportScreenshot();

	/** Get a JSON summary of the current scene */
	static FString GetSceneSummary(int32 PageSize = 100, int32 PageIndex = 0);

	/** Generic scene query: filters actors/components based on simple criteria.
	 *  ArgumentsJson is a JSON object with optional fields like:
	 *    - class_contains: substring to match in Actor.Class
	 *    - label_contains: substring to match in Actor label
	 *    - name_contains: substring to match in Actor name
	 *    - component_class_contains: substring to match in component class names
	 *    - max_results: maximum number of results to return (default 20)
	 *    - include_transform: include rotation + scale (default true, location always included)
	 *    - include_bounds: include origin + extent (default false)
	 *    - include_components: include root component, mobility, static_mesh_path (default false)
	 *    - include_metadata: include tags, folder_path, parent_actor (default false)
	 */
	static FString QueryScene(const FString& ArgumentsJson);

	/** Capture viewport screenshot with metadata including camera transform, FOV, resolution, selected actors */
	static FString CaptureViewportScreenshotWithMetadata(const FString& FocusActorLabel = TEXT(""));

	/** Get summary of selected actors only */
	static FString GetSelectedActorsSummary();

	// New atomic editor tools
	
	/** Get detailed actor info by label or name */
	static FString GetActor(const FString& ArgumentsJson);

	/** Set actor transform (location/rotation/scale) with transaction support */
	static FString SetActorTransform(const FString& ArgumentsJson);

	/** Batch-set rotation on multiple actors at once */
	static FString SetActorsRotation(const FString& ArgumentsJson);

	/** Select actors by label array */
	static FString SelectActors(const FString& ArgumentsJson);

	/** Duplicate an actor with optional offset */
	static FString DuplicateActor(const FString& ArgumentsJson);

	/** Snap actor to ground using line trace */
	static FString SnapActorToGround(const FString& ArgumentsJson);

private:
	/** Capture viewport using Slate rendering */
	static bool CaptureViewportToImage(TArray<uint8>& OutImageData, int32& OutWidth, int32& OutHeight);

	/** Serialize actor to JSON */
	static TSharedPtr<FJsonObject> SerializeActor(AActor* Actor);

	/** Serialize component to JSON */
	static TSharedPtr<FJsonObject> SerializeComponent(UActorComponent* Component);
};

