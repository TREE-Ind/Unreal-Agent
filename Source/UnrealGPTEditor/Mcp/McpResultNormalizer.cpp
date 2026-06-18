// Copyright (c) 2025 TREE Industries.

#include "McpResultNormalizer.h"

#include "Http.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	FString ExtensionFromMime(const FString& Mime)
	{
		if (Mime.Contains(TEXT("png"))) return TEXT("png");
		if (Mime.Contains(TEXT("jpeg"))) return TEXT("jpg");
		if (Mime.Contains(TEXT("webp"))) return TEXT("webp");
		if (Mime.Contains(TEXT("wav"))) return TEXT("wav");
		if (Mime.Contains(TEXT("mpeg"))) return TEXT("mp3");
		if (Mime.Contains(TEXT("mp4"))) return TEXT("mp4");
		if (Mime.Contains(TEXT("fbx"))) return TEXT("fbx");
		if (Mime.Contains(TEXT("gltf"))) return TEXT("gltf");
		if (Mime.Contains(TEXT("glb"))) return TEXT("glb");
		if (Mime.Contains(TEXT("obj"))) return TEXT("obj");
		return TEXT("bin");
	}

	void CollectContentBlocks(
		const TSharedPtr<FJsonValue>& Value,
		TArray<TSharedPtr<FJsonObject>>& OutBlocks)
	{
		if (!Value.IsValid())
		{
			return;
		}

		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (Obj.IsValid())
			{
				FString Type;
				if (Obj->TryGetStringField(TEXT("type"), Type))
				{
					OutBlocks.Add(Obj);
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
				{
					CollectContentBlocks(Pair.Value, OutBlocks);
				}
			}
			return;
		}

		if (Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				CollectContentBlocks(Item, OutBlocks);
			}
		}
	}
}

FString FMcpResultNormalizer::GetStagingFolderForKind(const FString& Kind)
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

	return BasePath / TEXT("Mcp");
}

FString FMcpResultNormalizer::NormalizeMimeType(const FString& MimeOrExtension)
{
	const FString Lower = MimeOrExtension.ToLower();
	if (Lower.Contains(TEXT("/")))
	{
		return Lower;
	}

	if (Lower == TEXT("png")) return TEXT("image/png");
	if (Lower == TEXT("jpg") || Lower == TEXT("jpeg")) return TEXT("image/jpeg");
	if (Lower == TEXT("webp")) return TEXT("image/webp");
	if (Lower == TEXT("gif")) return TEXT("image/gif");
	if (Lower == TEXT("wav")) return TEXT("audio/wav");
	if (Lower == TEXT("mp3")) return TEXT("audio/mpeg");
	if (Lower == TEXT("ogg")) return TEXT("audio/ogg");
	if (Lower == TEXT("mp4")) return TEXT("video/mp4");
	if (Lower == TEXT("fbx")) return TEXT("model/fbx");
	if (Lower == TEXT("glb")) return TEXT("model/gltf-binary");
	if (Lower == TEXT("gltf")) return TEXT("model/gltf+json");
	if (Lower == TEXT("obj")) return TEXT("model/obj");

	return Lower;
}

FString FMcpResultNormalizer::InferUsageFromMime(const FString& MimeType, const FString& FilePath)
{
	const FString Mime = NormalizeMimeType(MimeType);
	if (Mime.StartsWith(TEXT("image/")))
	{
		return TEXT("image");
	}
	if (Mime.StartsWith(TEXT("audio/")))
	{
		return TEXT("audio");
	}
	if (Mime.StartsWith(TEXT("video/")))
	{
		return TEXT("video");
	}
	if (Mime.Contains(TEXT("model")) || Mime.Contains(TEXT("mesh")))
	{
		return TEXT("3d_model");
	}

	const FString Ext = FPaths::GetExtension(FilePath).ToLower();
	if (Ext == TEXT("png") || Ext == TEXT("jpg") || Ext == TEXT("jpeg") || Ext == TEXT("webp") || Ext == TEXT("gif"))
	{
		return TEXT("image");
	}
	if (Ext == TEXT("wav") || Ext == TEXT("mp3") || Ext == TEXT("ogg"))
	{
		return TEXT("audio");
	}
	if (Ext == TEXT("fbx") || Ext == TEXT("glb") || Ext == TEXT("gltf") || Ext == TEXT("obj"))
	{
		return TEXT("3d_model");
	}

	return TEXT("file");
}

bool FMcpResultNormalizer::SaveBase64ToStaging(
	const FString& Base64Data,
	const FString& MimeType,
	const FString& KindHint,
	FString& OutLocalPath,
	FString& OutError)
{
	FString Payload = Base64Data;
	int32 CommaIndex = INDEX_NONE;
	if (Payload.FindChar(TEXT(','), CommaIndex))
	{
		Payload = Payload.Mid(CommaIndex + 1);
	}

	TArray<uint8> Bytes;
	if (!FBase64::Decode(Payload, Bytes) || Bytes.Num() == 0)
	{
		OutError = TEXT("Failed to decode base64 MCP content");
		return false;
	}

	const FString Mime = NormalizeMimeType(MimeType);
	const FString Ext = ExtensionFromMime(Mime);
	const FString Folder = GetStagingFolderForKind(KindHint);
	IFileManager::Get().MakeDirectory(*Folder, true);
	OutLocalPath = Folder / (FGuid::NewGuid().ToString() + TEXT(".") + Ext);

	if (!FFileHelper::SaveArrayToFile(Bytes, *OutLocalPath))
	{
		OutError = FString::Printf(TEXT("Failed to write MCP staged file: %s"), *OutLocalPath);
		return false;
	}

	return true;
}

bool FMcpResultNormalizer::DownloadUrlToStaging(
	const FString& Url,
	const TMap<FString, FString>& Headers,
	const FString& KindHint,
	FString& OutLocalPath,
	FString& OutMimeType,
	FString& OutError)
{
	TArray<uint8> Content;
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	for (const TPair<FString, FString>& Header : Headers)
	{
		Request->SetHeader(Header.Key, Header.Value);
	}

	bool bDone = false;
	bool bOk = false;
	Request->OnProcessRequestComplete().BindLambda(
		[&](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			if (bConnected && Response.IsValid() && Response->GetResponseCode() >= 200 && Response->GetResponseCode() < 300)
			{
				Content = Response->GetContent();
				OutMimeType = Response->GetContentType();
				bOk = true;
			}
			else if (Response.IsValid())
			{
				OutError = FString::Printf(TEXT("Download failed HTTP %d"), Response->GetResponseCode());
			}
			else
			{
				OutError = TEXT("Download request failed");
			}
			bDone = true;
		});
	Request->ProcessRequest();
	while (!bDone)
	{
		FPlatformProcess::Sleep(0.01f);
	}

	if (!bOk || Content.Num() == 0)
	{
		return false;
	}

	const FString Ext = ExtensionFromMime(NormalizeMimeType(OutMimeType));
	const FString Folder = GetStagingFolderForKind(KindHint);
	IFileManager::Get().MakeDirectory(*Folder, true);
	OutLocalPath = Folder / (FGuid::NewGuid().ToString() + TEXT(".") + Ext);

	if (!FFileHelper::SaveArrayToFile(Content, *OutLocalPath))
	{
		OutError = TEXT("Failed to save downloaded MCP file");
		return false;
	}

	return true;
}

FString FMcpResultNormalizer::BuildToolCallEnvelope(
	const FString& ServerName,
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& McpResult,
	const FString& KindHint)
{
	TArray<TSharedPtr<FJsonValue>> FilesArray;
	TArray<FString> TextParts;

	TArray<TSharedPtr<FJsonObject>> Blocks;
	if (McpResult.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
		if (McpResult->TryGetArrayField(TEXT("content"), ContentArray) && ContentArray)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ContentArray)
			{
				if (Value.IsValid() && Value->Type == EJson::Object)
				{
					Blocks.Add(Value->AsObject());
				}
			}
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : McpResult->Values)
		{
			CollectContentBlocks(Pair.Value, Blocks);
		}
	}

	TMap<FString, FString> EmptyHeaders;
	for (const TSharedPtr<FJsonObject>& Block : Blocks)
	{
		FString Type;
		Block->TryGetStringField(TEXT("type"), Type);
		Type = Type.ToLower();

		if (Type == TEXT("text"))
		{
			FString Text;
			if (Block->TryGetStringField(TEXT("text"), Text))
			{
				TextParts.Add(Text);
			}
		}
		else if (Type == TEXT("image") || Type == TEXT("audio") || Type == TEXT("resource"))
		{
			FString MimeType;
			Block->TryGetStringField(TEXT("mimeType"), MimeType);

			const TSharedPtr<FJsonObject>* ResourceObj = nullptr;
			FString Uri;
			if (Block->TryGetObjectField(TEXT("resource"), ResourceObj) && ResourceObj && ResourceObj->IsValid())
			{
				(*ResourceObj)->TryGetStringField(TEXT("uri"), Uri);
				(*ResourceObj)->TryGetStringField(TEXT("mimeType"), MimeType);
			}

			FString LocalPath;
			FString Error;
			FString Base64Data;
			if (Block->TryGetStringField(TEXT("data"), Base64Data) && !Base64Data.IsEmpty())
			{
				if (SaveBase64ToStaging(Base64Data, MimeType, KindHint, LocalPath, Error))
				{
					TSharedPtr<FJsonObject> FileObj = MakeShared<FJsonObject>();
					const FString NormalizedMime = NormalizeMimeType(MimeType.IsEmpty() ? FPaths::GetExtension(LocalPath) : MimeType);
					FileObj->SetStringField(TEXT("local_path"), LocalPath);
					FileObj->SetStringField(TEXT("mime_type"), NormalizedMime);
					FileObj->SetStringField(TEXT("inferred_usage"), InferUsageFromMime(NormalizedMime, LocalPath));
					FilesArray.Add(MakeShared<FJsonValueObject>(FileObj));
				}
			}
			else if (!Uri.IsEmpty() && (Uri.StartsWith(TEXT("http://")) || Uri.StartsWith(TEXT("https://"))))
			{
				FString DownloadedMime;
				if (DownloadUrlToStaging(Uri, EmptyHeaders, KindHint, LocalPath, DownloadedMime, Error))
				{
					TSharedPtr<FJsonObject> FileObj = MakeShared<FJsonObject>();
					const FString NormalizedMime = NormalizeMimeType(DownloadedMime.IsEmpty() ? MimeType : DownloadedMime);
					FileObj->SetStringField(TEXT("local_path"), LocalPath);
					FileObj->SetStringField(TEXT("mime_type"), NormalizedMime);
					FileObj->SetStringField(TEXT("inferred_usage"), InferUsageFromMime(NormalizedMime, LocalPath));
					FileObj->SetStringField(TEXT("source_uri"), Uri);
					FilesArray.Add(MakeShared<FJsonValueObject>(FileObj));
				}
			}
		}
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("status"), TEXT("ok"));
	ResultObj->SetStringField(
		TEXT("message"),
		FString::Printf(TEXT("MCP tool '%s' on server '%s' completed with %d file(s)."),
			*ToolName, *ServerName, FilesArray.Num()));

	TSharedPtr<FJsonObject> DetailsObj = MakeShared<FJsonObject>();
	DetailsObj->SetStringField(TEXT("server"), ServerName);
	DetailsObj->SetStringField(TEXT("tool"), ToolName);
	DetailsObj->SetArrayField(TEXT("files"), FilesArray);
	if (TextParts.Num() > 0)
	{
		DetailsObj->SetStringField(TEXT("text"), FString::Join(TextParts, TEXT("\n")));
	}
	if (McpResult.IsValid())
	{
		DetailsObj->SetObjectField(TEXT("raw"), McpResult);
	}
	ResultObj->SetObjectField(TEXT("details"), DetailsObj);

	FString OutJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);
	return OutJson;
}
