// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Converts MCP tool/resource results into Unreal Agent import-friendly JSON envelopes. */
class FMcpResultNormalizer
{
public:
	static FString GetStagingFolderForKind(const FString& Kind);
	static FString InferUsageFromMime(const FString& MimeType, const FString& FilePath);
	static FString NormalizeMimeType(const FString& MimeOrExtension);

	static bool SaveBase64ToStaging(
		const FString& Base64Data,
		const FString& MimeType,
		const FString& KindHint,
		FString& OutLocalPath,
		FString& OutError);

	static bool DownloadUrlToStaging(
		const FString& Url,
		const TMap<FString, FString>& Headers,
		const FString& KindHint,
		FString& OutLocalPath,
		FString& OutMimeType,
		FString& OutError);

	static FString BuildToolCallEnvelope(
		const FString& ServerName,
		const FString& ToolName,
		const TSharedPtr<FJsonObject>& McpResult,
		const FString& KindHint = TEXT("misc"));
};
