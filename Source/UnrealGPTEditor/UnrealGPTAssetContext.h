// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "Input/DragAndDrop.h"

/**
 * Content Browser asset attachment helpers for Unreal Agent chat.
 */
class UNREALGPTEDITOR_API FUnrealGPTAssetContext
{
public:
	static constexpr int32 MaxAttachedAssets = 16;

	/** Extract asset data from a Content Browser drag operation. */
	static TArray<FAssetData> ExtractAssetsFromDragDrop(const FDragDropEvent& DragDropEvent);

	/** Append a structured JSON block describing attached assets for the agent. */
	static FString BuildContextAppendix(const TArray<FAssetData>& Assets);

	/** Raw base64 PNG for a Texture2D asset (empty on failure). */
	static FString EncodeTextureBase64(const FAssetData& Asset);

	/** Collect base64 PNG payloads for all Texture2D assets in the list. */
	static TArray<FString> GetTextureBase64Images(const TArray<FAssetData>& Assets);

	/** Convert a texture asset path or disk image path to a base64 PNG data URI. */
	static FString ConvertImageToBase64DataUri(const FString& ImagePath, FString& OutError);

	/** True when the drag operation carries at least one non-folder asset. */
	static bool CanAcceptDragDrop(const FDragDropEvent& DragDropEvent);

	/** Merge new assets into Pending list with dedupe and cap enforcement. */
	static void MergeAssets(TArray<FAssetData>& InOutPending, const TArray<FAssetData>& NewAssets, int32& OutSkippedCount);
};
