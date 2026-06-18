// Copyright (c) 2025 TREE Industries.

#include "UnrealGPTAssetContext.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Input/DragAndDrop.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectGlobals.h"

namespace UnrealGPTAssetContextPrivate
{
	static FString NormalizeAssetObjectPath(const FString& InPath)
	{
		FString AssetPath = InPath;
		if ((AssetPath.StartsWith(TEXT("/Game/")) || AssetPath.StartsWith(TEXT("/Engine/")))
			&& !AssetPath.Contains(TEXT(".")))
		{
			const FString AssetName = FPaths::GetBaseFilename(AssetPath);
			AssetPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
		}
		return AssetPath;
	}

	static bool EncodeTextureToPngBytes(UTexture2D* Texture, const FString& SourceLabel, TArray<uint8>& OutPngData, FString& OutError)
	{
		if (!Texture)
		{
			OutError = TEXT("Texture is null");
			return false;
		}

		FTexturePlatformData* PlatformData = Texture->GetPlatformData();
		if (!PlatformData || PlatformData->Mips.Num() == 0)
		{
			OutError = FString::Printf(TEXT("Texture '%s' has no platform data or mips"), *SourceLabel);
			return false;
		}

		const FTexture2DMipMap& Mip = PlatformData->Mips[0];
		const int32 Width = Mip.SizeX;
		const int32 Height = Mip.SizeY;

		const void* MipData = Mip.BulkData.LockReadOnly();
		if (!MipData)
		{
			OutError = FString::Printf(TEXT("Failed to lock texture data for '%s'"), *SourceLabel);
			return false;
		}

		const int64 DataSize = Mip.BulkData.GetBulkDataSize();
		TArray<uint8> RawData;
		RawData.SetNumUninitialized(DataSize);
		FMemory::Memcpy(RawData.GetData(), MipData, DataSize);
		Mip.BulkData.Unlock();

		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!ImageWrapper.IsValid())
		{
			OutError = TEXT("Failed to create image wrapper");
			return false;
		}

		ERGBFormat RGBFormat = ERGBFormat::BGRA;
		if (PlatformData->PixelFormat == PF_R8G8B8A8)
		{
			RGBFormat = ERGBFormat::RGBA;
		}

		if (!ImageWrapper->SetRaw(RawData.GetData(), RawData.Num(), Width, Height, RGBFormat, 8))
		{
			OutError = FString::Printf(TEXT("Failed to set raw image data for '%s'"), *SourceLabel);
			return false;
		}

		OutPngData = ImageWrapper->GetCompressed();
		if (OutPngData.Num() == 0)
		{
			OutError = FString::Printf(TEXT("Failed to compress texture '%s' to PNG"), *SourceLabel);
			return false;
		}

		return true;
	}

	static bool ResolveAssetDataFromPath(const FString& AssetPath, FAssetData& OutAssetData)
	{
		const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		const IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		FString ObjectPath = AssetPath;
		if (!ObjectPath.Contains(TEXT(".")))
		{
			const FString AssetName = FPaths::GetBaseFilename(ObjectPath);
			ObjectPath = FString::Printf(TEXT("%s.%s"), *ObjectPath, *AssetName);
		}

		OutAssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
		return OutAssetData.IsValid();
	}

	static bool IsFolderAsset(const FAssetData& AssetData)
	{
		return AssetData.AssetClassPath == FTopLevelAssetPath(TEXT("/Script/CoreUObject"), TEXT("Folder"));
	}
}

TArray<FAssetData> FUnrealGPTAssetContext::ExtractAssetsFromDragDrop(const FDragDropEvent& DragDropEvent)
{
	TArray<FAssetData> Result;

	TSharedPtr<FAssetDragDropOp> AssetOp = DragDropEvent.GetOperationAs<FAssetDragDropOp>();
	if (!AssetOp.IsValid())
	{
		return Result;
	}

	if (AssetOp->HasAssets())
	{
		for (const FAssetData& AssetData : AssetOp->GetAssets())
		{
			if (AssetData.IsValid() && !UnrealGPTAssetContextPrivate::IsFolderAsset(AssetData))
			{
				Result.Add(AssetData);
			}
		}
	}

	if (AssetOp->HasAssetPaths())
	{
		for (const FString& AssetPath : AssetOp->GetAssetPaths())
		{
			FAssetData ResolvedAsset;
			if (UnrealGPTAssetContextPrivate::ResolveAssetDataFromPath(AssetPath, ResolvedAsset)
				&& !UnrealGPTAssetContextPrivate::IsFolderAsset(ResolvedAsset))
			{
				Result.Add(ResolvedAsset);
			}
		}
	}

	return Result;
}

bool FUnrealGPTAssetContext::CanAcceptDragDrop(const FDragDropEvent& DragDropEvent)
{
	return ExtractAssetsFromDragDrop(DragDropEvent).Num() > 0;
}

void FUnrealGPTAssetContext::MergeAssets(TArray<FAssetData>& InOutPending, const TArray<FAssetData>& NewAssets, int32& OutSkippedCount)
{
	OutSkippedCount = 0;

	TSet<FString> ExistingPaths;
	for (const FAssetData& Existing : InOutPending)
	{
		ExistingPaths.Add(Existing.GetObjectPathString());
	}

	for (const FAssetData& Asset : NewAssets)
	{
		if (!Asset.IsValid())
		{
			continue;
		}

		const FString ObjectPath = Asset.GetObjectPathString();
		if (ExistingPaths.Contains(ObjectPath))
		{
			continue;
		}

		if (InOutPending.Num() >= MaxAttachedAssets)
		{
			++OutSkippedCount;
			continue;
		}

		InOutPending.Add(Asset);
		ExistingPaths.Add(ObjectPath);
	}
}

FString FUnrealGPTAssetContext::BuildContextAppendix(const TArray<FAssetData>& Assets)
{
	if (Assets.Num() == 0)
	{
		return FString();
	}

	TArray<TSharedPtr<FJsonValue>> AssetValues;
	for (const FAssetData& Asset : Assets)
	{
		TSharedPtr<FJsonObject> AssetObj = MakeShareable(new FJsonObject);
		AssetObj->SetStringField(TEXT("object_path"), Asset.GetObjectPathString());
		AssetObj->SetStringField(TEXT("package_name"), Asset.PackageName.ToString());
		AssetObj->SetStringField(TEXT("asset_name"), Asset.AssetName.ToString());
		AssetObj->SetStringField(TEXT("asset_class"), Asset.AssetClassPath.GetAssetName().ToString());
		AssetObj->SetStringField(TEXT("class_path"), Asset.AssetClassPath.ToString());
		AssetValues.Add(MakeShareable(new FJsonValueObject(AssetObj)));
	}

	FString JsonArray;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonArray);
	FJsonSerializer::Serialize(AssetValues, Writer);

	return FString::Printf(
		TEXT("\n\n[Attached UE Assets]\n%s"),
		*JsonArray);
}

FString FUnrealGPTAssetContext::EncodeTextureBase64(const FAssetData& Asset)
{
	if (!Asset.IsValid())
	{
		return FString();
	}

	if (Asset.AssetClassPath != FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("Texture2D")))
	{
		return FString();
	}

	const FString ObjectPath = UnrealGPTAssetContextPrivate::NormalizeAssetObjectPath(Asset.GetObjectPathString());
	UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *ObjectPath);
	if (!Texture)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Failed to load texture asset '%s' for vision attachment"), *ObjectPath);
		return FString();
	}

	TArray<uint8> PngData;
	FString Error;
	if (!UnrealGPTAssetContextPrivate::EncodeTextureToPngBytes(Texture, ObjectPath, PngData, Error))
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: %s"), *Error);
		return FString();
	}

	return FBase64::Encode(PngData);
}

TArray<FString> FUnrealGPTAssetContext::GetTextureBase64Images(const TArray<FAssetData>& Assets)
{
	TArray<FString> Images;
	for (const FAssetData& Asset : Assets)
	{
		const FString Base64 = EncodeTextureBase64(Asset);
		if (!Base64.IsEmpty())
		{
			Images.Add(Base64);
		}
	}
	return Images;
}

FString FUnrealGPTAssetContext::ConvertImageToBase64DataUri(const FString& ImagePath, FString& OutError)
{
	TArray<uint8> PngData;

	FString AssetPath = ImagePath;
	if (AssetPath.StartsWith(TEXT("/Game/")) || AssetPath.StartsWith(TEXT("/Engine/")))
	{
		AssetPath = UnrealGPTAssetContextPrivate::NormalizeAssetObjectPath(AssetPath);
		UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *AssetPath);
		if (Texture)
		{
			if (!UnrealGPTAssetContextPrivate::EncodeTextureToPngBytes(Texture, ImagePath, PngData, OutError))
			{
				return FString();
			}

			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Converted texture asset '%s' to PNG (%d bytes)"), *ImagePath, PngData.Num());
		}
	}

	if (PngData.Num() == 0)
	{
		FString FilePath = ImagePath;
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

		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
		const FString Extension = FPaths::GetExtension(FilePath).ToLower();
		if (Extension == TEXT("png"))
		{
			PngData = MoveTemp(FileData);
		}
		else
		{
			EImageFormat ImageFormat = EImageFormat::Invalid;
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

			TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
			if (!PngWrapper.IsValid()
				|| !PngWrapper->SetRaw(
					RawBGRA.GetData(),
					RawBGRA.Num(),
					SourceWrapper->GetWidth(),
					SourceWrapper->GetHeight(),
					ERGBFormat::BGRA,
					8))
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

	const FString Base64 = FBase64::Encode(PngData);
	return FString::Printf(TEXT("data:image/png;base64,%s"), *Base64);
}
