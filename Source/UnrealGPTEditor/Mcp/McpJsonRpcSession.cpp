// Copyright (c) 2025 TREE Industries.

#include "McpJsonRpcSession.h"

#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	FString SerializeJsonObject(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}

	bool ParseJsonObject(const FString& Json, TSharedPtr<FJsonObject>& OutObj)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, OutObj) && OutObj.IsValid();
	}

	FString JsonRpcErrorToString(const TSharedPtr<FJsonObject>& ErrorObj)
	{
		if (!ErrorObj.IsValid())
		{
			return TEXT("Unknown JSON-RPC error");
		}

		FString Message;
		ErrorObj->TryGetStringField(TEXT("message"), Message);
		int32 Code = 0;
		ErrorObj->TryGetNumberField(TEXT("code"), Code);
		return FString::Printf(TEXT("JSON-RPC error %d: %s"), Code, *Message);
	}
}

FMcpJsonRpcSession::FMcpJsonRpcSession(TUniquePtr<IMcpTransport> InTransport)
	: Transport(MoveTemp(InTransport))
{
}

bool FMcpJsonRpcSession::ConnectAndInitialize(float TimeoutSeconds, FString& OutError)
{
	FScopeLock Lock(&SessionLock);

	if (!Transport.IsValid())
	{
		OutError = TEXT("MCP transport is null");
		return false;
	}

	if (!Transport->IsConnected())
	{
		if (!Transport->Connect(OutError))
		{
			return false;
		}
	}

	return PerformInitialize(TimeoutSeconds, OutError);
}

void FMcpJsonRpcSession::Disconnect()
{
	FScopeLock Lock(&SessionLock);
	bInitialized = false;
	if (Transport.IsValid())
	{
		Transport->Disconnect();
	}
}

bool FMcpJsonRpcSession::SendRequest(
	const FString& Method,
	const TSharedPtr<FJsonObject>& Params,
	float TimeoutSeconds,
	TSharedPtr<FJsonObject>& OutResult,
	FString& OutError)
{
	FScopeLock Lock(&SessionLock);

	if (!bInitialized)
	{
		OutError = TEXT("MCP session is not initialized");
		return false;
	}

	const int32 RequestId = NextRequestId++;

	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Request->SetNumberField(TEXT("id"), RequestId);
	Request->SetStringField(TEXT("method"), Method);
	if (Params.IsValid())
	{
		Request->SetObjectField(TEXT("params"), Params);
	}

	if (!SendRawJson(Request, OutError))
	{
		return false;
	}

	return ReadJsonRpcResponse(RequestId, TimeoutSeconds, OutResult, OutError);
}

bool FMcpJsonRpcSession::SendNotification(
	const FString& Method,
	const TSharedPtr<FJsonObject>& Params,
	FString& OutError)
{
	FScopeLock Lock(&SessionLock);

	TSharedPtr<FJsonObject> Notification = MakeShared<FJsonObject>();
	Notification->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Notification->SetStringField(TEXT("method"), Method);
	if (Params.IsValid())
	{
		Notification->SetObjectField(TEXT("params"), Params);
	}

	return SendRawJson(Notification, OutError);
}

bool FMcpJsonRpcSession::SendRawJson(const TSharedPtr<FJsonObject>& Message, FString& OutError)
{
	if (!Transport.IsValid() || !Transport->IsConnected())
	{
		OutError = TEXT("MCP transport is not connected");
		return false;
	}

	const FString Payload = SerializeJsonObject(Message);
	return Transport->SendMessage(Payload, OutError);
}

bool FMcpJsonRpcSession::ReadJsonRpcResponse(
	int32 ExpectedId,
	float TimeoutSeconds,
	TSharedPtr<FJsonObject>& OutResult,
	FString& OutError)
{
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;

	while (FPlatformTime::Seconds() < Deadline)
	{
		const float Remaining = static_cast<float>(Deadline - FPlatformTime::Seconds());
		if (Remaining <= 0.0f)
		{
			break;
		}

		FString Line;
		if (!Transport->ReadMessage(Line, Remaining, OutError))
		{
			continue;
		}

		if (Line.IsEmpty())
		{
			continue;
		}

		TSharedPtr<FJsonObject> Message;
		if (!ParseJsonObject(Line, Message))
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT MCP: Ignoring non-JSON line: %s"), *Line.Left(200));
			continue;
		}

		FString Method;
		if (Message->TryGetStringField(TEXT("method"), Method))
		{
			UE_LOG(LogTemp, Verbose, TEXT("UnrealGPT MCP notification: %s"), *Method);
			continue;
		}

		int32 ResponseId = 0;
		if (!Message->TryGetNumberField(TEXT("id"), ResponseId) || ResponseId != ExpectedId)
		{
			continue;
		}

		const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
		if (Message->TryGetObjectField(TEXT("error"), ErrorObj) && ErrorObj && ErrorObj->IsValid())
		{
			OutError = JsonRpcErrorToString(*ErrorObj);
			return false;
		}

		const TSharedPtr<FJsonObject>* ResultObj = nullptr;
		if (Message->TryGetObjectField(TEXT("result"), ResultObj) && ResultObj && ResultObj->IsValid())
		{
			OutResult = *ResultObj;
			return true;
		}

		OutResult = MakeShared<FJsonObject>();
		return true;
	}

	OutError = FString::Printf(TEXT("Timed out waiting for MCP response (id=%d)"), ExpectedId);
	return false;
}

bool FMcpJsonRpcSession::PerformInitialize(float TimeoutSeconds, FString& OutError)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("protocolVersion"), TEXT("2024-11-05"));

	TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();
	Params->SetObjectField(TEXT("capabilities"), Capabilities);

	TSharedPtr<FJsonObject> ClientInfo = MakeShared<FJsonObject>();
	ClientInfo->SetStringField(TEXT("name"), TEXT("unreal-agent"));
	ClientInfo->SetStringField(TEXT("version"), TEXT("1.0"));
	Params->SetObjectField(TEXT("clientInfo"), ClientInfo);

	const int32 RequestId = NextRequestId++;
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Request->SetNumberField(TEXT("id"), RequestId);
	Request->SetStringField(TEXT("method"), TEXT("initialize"));
	Request->SetObjectField(TEXT("params"), Params);

	if (!SendRawJson(Request, OutError))
	{
		return false;
	}

	TSharedPtr<FJsonObject> InitResult;
	if (!ReadJsonRpcResponse(RequestId, TimeoutSeconds, InitResult, OutError))
	{
		return false;
	}

	TSharedPtr<FJsonObject> InitializedParams = MakeShared<FJsonObject>();
	if (!SendNotification(TEXT("notifications/initialized"), InitializedParams, OutError))
	{
		return false;
	}

	DrainNotifications(0.25f);
	bInitialized = true;
	return true;
}

void FMcpJsonRpcSession::DrainNotifications(float TimeoutSeconds)
{
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (FPlatformTime::Seconds() < Deadline)
	{
		const float Remaining = static_cast<float>(Deadline - FPlatformTime::Seconds());
		FString Line;
		FString IgnoreError;
		if (!Transport->ReadMessage(Line, Remaining, IgnoreError))
		{
			break;
		}
	}
}
