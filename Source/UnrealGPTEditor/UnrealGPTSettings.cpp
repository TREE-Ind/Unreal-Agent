// Copyright (c) 2025 TREE Industries.

#include "UnrealGPTSettings.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	bool LoadCodexAuthJson(const UUnrealGPTSettings& Settings, TSharedPtr<FJsonObject>& OutAuthJson, FString& OutError)
	{
		const FString AuthPath = Settings.GetResolvedCodexAuthFilePath();
		if (AuthPath.IsEmpty())
		{
			OutError = TEXT("Could not resolve Codex auth file path.");
			return false;
		}

		FString AuthJsonText;
		if (!FFileHelper::LoadFileToString(AuthJsonText, *AuthPath))
		{
			OutError = FString::Printf(
				TEXT("Codex auth file not found or unreadable: %s. Use the UnrealGPT Codex Login button to sign in."),
				*AuthPath);
			return false;
		}

		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(AuthJsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutAuthJson) || !OutAuthJson.IsValid())
		{
			OutError = FString::Printf(TEXT("Codex auth file is not valid JSON: %s"), *AuthPath);
			return false;
		}

		return true;
	}

	bool TryGetCodexApiKey(const TSharedPtr<FJsonObject>& AuthJson, FString& OutApiKey)
	{
		return AuthJson.IsValid()
			&& AuthJson->TryGetStringField(TEXT("OPENAI_API_KEY"), OutApiKey)
			&& !OutApiKey.IsEmpty();
	}

	bool TryGetCodexChatGPTTokens(const TSharedPtr<FJsonObject>& AuthJson, FString& OutAccessToken, FString& OutAccountId)
	{
		if (!AuthJson.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* TokensObject = nullptr;
		if (!AuthJson->TryGetObjectField(TEXT("tokens"), TokensObject) || !TokensObject || !TokensObject->IsValid())
		{
			return false;
		}

		(*TokensObject)->TryGetStringField(TEXT("access_token"), OutAccessToken);
		(*TokensObject)->TryGetStringField(TEXT("account_id"), OutAccountId);
		return !OutAccessToken.IsEmpty();
	}

	bool TryGetCodexRefreshToken(const TSharedPtr<FJsonObject>& AuthJson, FString& OutRefreshToken)
	{
		if (!AuthJson.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* TokensObject = nullptr;
		if (!AuthJson->TryGetObjectField(TEXT("tokens"), TokensObject) || !TokensObject || !TokensObject->IsValid())
		{
			return false;
		}

		(*TokensObject)->TryGetStringField(TEXT("refresh_token"), OutRefreshToken);
		return !OutRefreshToken.IsEmpty();
	}

	FString ExtractJwtStringClaim(const FString& Jwt, const FString& ClaimName)
	{
		TArray<FString> Parts;
		Jwt.ParseIntoArray(Parts, TEXT("."), false);
		if (Parts.Num() < 2)
		{
			return FString();
		}

		FString Payload = Parts[1].Replace(TEXT("-"), TEXT("+")).Replace(TEXT("_"), TEXT("/"));
		while (Payload.Len() % 4 != 0)
		{
			Payload += TEXT("=");
		}

		FString PayloadJsonText;
		if (!FBase64::Decode(Payload, PayloadJsonText))
		{
			return FString();
		}

		TSharedPtr<FJsonObject> PayloadJson;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PayloadJsonText);
		if (!FJsonSerializer::Deserialize(Reader, PayloadJson) || !PayloadJson.IsValid())
		{
			return FString();
		}

		FString ClaimValue;
		PayloadJson->TryGetStringField(ClaimName, ClaimValue);
		return ClaimValue;
	}
}

UUnrealGPTSettings::UUnrealGPTSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

bool UUnrealGPTSettings::ResolveAuthHeaders(FString& OutBearerToken, FString& OutChatGPTAccountId, FString& OutError) const
{
	OutBearerToken.Empty();
	OutChatGPTAccountId.Empty();
	OutError.Empty();

	if (!bUseCodexAuth)
	{
		if (ApiKey.IsEmpty())
		{
			OutError = TEXT("API Key not set in UnrealGPT settings.");
			return false;
		}

		OutBearerToken = ApiKey;
		return true;
	}

	TSharedPtr<FJsonObject> AuthJson;
	if (!LoadCodexAuthJson(*this, AuthJson, OutError))
	{
		return false;
	}

	FString AuthMode;
	AuthJson->TryGetStringField(TEXT("auth_mode"), AuthMode);

	FString CodexApiKey;
	if (TryGetCodexApiKey(AuthJson, CodexApiKey))
	{
		OutBearerToken = CodexApiKey;
		return true;
	}

	FString AccessToken;
	FString AccountId;
	if (TryGetCodexChatGPTTokens(AuthJson, AccessToken, AccountId))
	{
		OutBearerToken = AccessToken;
		OutChatGPTAccountId = AccountId;
		return true;
	}

	OutError = FString::Printf(
		TEXT("Codex auth file does not contain a usable OPENAI_API_KEY or tokens.access_token (auth_mode: %s)."),
		AuthMode.IsEmpty() ? TEXT("unknown") : *AuthMode);
	return false;
}

bool UUnrealGPTSettings::IsUsingCodexChatGPTAuth() const
{
	if (!bUseCodexAuth)
	{
		return false;
	}

	TSharedPtr<FJsonObject> AuthJson;
	FString Error;
	if (!LoadCodexAuthJson(*this, AuthJson, Error))
	{
		return false;
	}

	FString AccessToken;
	FString AccountId;
	return TryGetCodexChatGPTTokens(AuthJson, AccessToken, AccountId);
}

bool UUnrealGPTSettings::GetCodexRefreshToken(FString& OutRefreshToken, FString& OutError) const
{
	OutRefreshToken.Empty();
	OutError.Empty();

	TSharedPtr<FJsonObject> AuthJson;
	if (!LoadCodexAuthJson(*this, AuthJson, OutError))
	{
		return false;
	}

	if (!TryGetCodexRefreshToken(AuthJson, OutRefreshToken))
	{
		OutError = TEXT("Codex auth cache does not contain a refresh token. Use Codex Login again.");
		return false;
	}

	return true;
}

bool UUnrealGPTSettings::SaveCodexChatGPTAuth(const FString& IdToken, const FString& AccessToken, const FString& RefreshToken, FString& OutError) const
{
	OutError.Empty();

	const FString AuthPath = GetResolvedCodexAuthFilePath();
	const FString AuthDir = FPaths::GetPath(AuthPath);
	if (!FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*AuthDir))
	{
		OutError = FString::Printf(TEXT("Could not create %s"), *AuthDir);
		return false;
	}

	TSharedPtr<FJsonObject> TokensJson = MakeShareable(new FJsonObject);
	TokensJson->SetStringField(TEXT("id_token"), IdToken);
	TokensJson->SetStringField(TEXT("access_token"), AccessToken);
	TokensJson->SetStringField(TEXT("refresh_token"), RefreshToken);

	const FString AccountId = ExtractJwtStringClaim(IdToken, TEXT("chatgpt_account_id"));
	if (!AccountId.IsEmpty())
	{
		TokensJson->SetStringField(TEXT("account_id"), AccountId);
	}

	TSharedPtr<FJsonObject> AuthJson = MakeShareable(new FJsonObject);
	AuthJson->SetStringField(TEXT("auth_mode"), TEXT("chatgpt"));
	AuthJson->SetObjectField(TEXT("tokens"), TokensJson);
	AuthJson->SetStringField(TEXT("last_refresh"), FDateTime::UtcNow().ToIso8601());

	FString AuthText;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&AuthText);
	FJsonSerializer::Serialize(AuthJson.ToSharedRef(), Writer);

	if (!FFileHelper::SaveStringToFile(AuthText, *AuthPath))
	{
		OutError = FString::Printf(TEXT("Could not write %s"), *AuthPath);
		return false;
	}

	return true;
}

FString UUnrealGPTSettings::GetResolvedCodexAuthFilePath() const
{
	if (!CodexAuthFilePath.IsEmpty())
	{
		return CodexAuthFilePath;
	}

	const FString CodexHome = FPlatformMisc::GetEnvironmentVariable(TEXT("CODEX_HOME"));
	if (!CodexHome.IsEmpty())
	{
		return FPaths::Combine(CodexHome, TEXT("auth.json"));
	}

	const FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
	if (!UserProfile.IsEmpty())
	{
		return FPaths::Combine(UserProfile, TEXT(".codex"), TEXT("auth.json"));
	}

	const FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	if (!Home.IsEmpty())
	{
		return FPaths::Combine(Home, TEXT(".codex"), TEXT("auth.json"));
	}

	return FPaths::Combine(FPlatformProcess::UserDir(), TEXT(".codex"), TEXT("auth.json"));
}

FName UUnrealGPTSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

