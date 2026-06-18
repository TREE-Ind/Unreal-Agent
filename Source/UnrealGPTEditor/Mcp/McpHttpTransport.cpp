// Copyright (c) 2025 TREE Industries.

#include "McpHttpTransport.h"

#include "Http.h"
#include "UnrealGPTSseClient.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

bool FMcpHttpTransport::Connect(FString& OutError)
{
	if (Config.Url.IsEmpty())
	{
		OutError = TEXT("MCP HTTP server URL is empty");
		return false;
	}

	bConnected = true;
	return true;
}

void FMcpHttpTransport::Disconnect()
{
	bConnected = false;
	PendingResponse.Empty();
}

bool FMcpHttpTransport::IsConnected() const
{
	return bConnected;
}

bool FMcpHttpTransport::SendMessage(const FString& JsonMessage, FString& OutError)
{
	if (!bConnected)
	{
		OutError = TEXT("MCP HTTP transport is not connected");
		return false;
	}

	FString ResponseBody;
	if (!PerformHttpExchange(JsonMessage, 120.0f, ResponseBody, OutError))
	{
		return false;
	}

	PendingResponse = ResponseBody;
	return true;
}

bool FMcpHttpTransport::ReadMessage(FString& OutMessage, float TimeoutSeconds, FString& OutError)
{
	if (!PendingResponse.IsEmpty())
	{
		FString JsonPayload;
		if (ExtractJsonFromSse(PendingResponse, JsonPayload))
		{
			OutMessage = JsonPayload;
		}
		else
		{
			OutMessage = PendingResponse;
		}
		PendingResponse.Empty();
		return true;
	}

	OutError = TEXT("No pending MCP HTTP response");
	return false;
}

bool FMcpHttpTransport::PerformHttpExchange(
	const FString& JsonMessage,
	float TimeoutSeconds,
	FString& OutResponseBody,
	FString& OutError)
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Config.Url);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json, text/event-stream"));

	for (const FMcpHeader& Header : Config.Headers)
	{
		if (!Header.Key.IsEmpty())
		{
			Request->SetHeader(Header.Key, Header.Value);
		}
	}

	Request->SetContentAsString(JsonMessage);
	Request->SetTimeout(TimeoutSeconds);

	bool bDone = false;
	bool bOk = false;

	Request->OnProcessRequestComplete().BindLambda(
		[&](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
		{
			if (bConnectedSuccessfully && Response.IsValid() && Response->GetResponseCode() >= 200 && Response->GetResponseCode() < 300)
			{
				OutResponseBody = Response->GetContentAsString();
				bOk = true;
			}
			else if (Response.IsValid())
			{
				OutError = FString::Printf(
					TEXT("MCP HTTP error %d: %s"),
					Response->GetResponseCode(),
					*Response->GetContentAsString().Left(500));
			}
			else
			{
				OutError = TEXT("MCP HTTP request failed");
			}
			bDone = true;
		});

	Request->ProcessRequest();
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (!bDone && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::Sleep(0.01f);
	}

	return bOk;
}

bool FMcpHttpTransport::ExtractJsonFromSse(const FString& SseBody, FString& OutJson) const
{
	if (!SseBody.Contains(TEXT("data:")))
	{
		return false;
	}

	TArray<FUnrealGPTSseEvent> Events;
	if (!FUnrealGPTSseClient::ParseEventsFromBody(SseBody, Events))
	{
		TArray<FString> Lines;
		SseBody.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(TEXT("data:")))
			{
				OutJson = Line.Mid(5).TrimStartAndEnd();
				return !OutJson.IsEmpty();
			}
		}
		return false;
	}

	for (const FUnrealGPTSseEvent& Event : Events)
	{
		if (!Event.Data.IsEmpty())
		{
			OutJson = Event.Data;
			return true;
		}
	}

	return false;
}

FMcpHttpTransport::FMcpHttpTransport(const FMcpServerConfig& InConfig)
	: Config(InConfig)
{
}
