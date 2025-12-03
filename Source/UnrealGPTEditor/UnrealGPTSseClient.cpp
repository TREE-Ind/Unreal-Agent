#include "UnrealGPTSseClient.h"

#include "HAL/PlatformProcess.h"

bool FUnrealGPTSseClient::FetchEvents(
	const FString& Url,
	const TMap<FString, FString>& Headers,
	TArray<FUnrealGPTSseEvent>& OutEvents,
	FString& OutError)
{
	OutEvents.Reset();
	OutError.Empty();

	if (Url.IsEmpty())
	{
		OutError = TEXT("SSE URL is empty");
		return false;
	}

	TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Accept"), TEXT("text/event-stream"));

	for (const TPair<FString, FString>& Pair : Headers)
	{
		Request->SetHeader(Pair.Key, Pair.Value);
	}

	// Use a reasonably long timeout; SSE endpoints are often long-lived.
	Request->SetTimeout(120.0f);

	Request->ProcessRequest();

	// Simple blocking wait until the request finishes or times out.
	const float TimeoutSeconds = 120.0f;
	float Elapsed = 0.0f;
	const float SleepStep = 0.01f;

	while (Request->GetStatus() == EHttpRequestStatus::Processing && Elapsed < TimeoutSeconds)
	{
		FPlatformProcess::Sleep(SleepStep);
		Elapsed += SleepStep;
	}

	if (Request->GetStatus() != EHttpRequestStatus::Succeeded)
	{
		const EHttpRequestStatus::Type Status = Request->GetStatus();
		FString StatusString;
		switch (Status)
		{
		case EHttpRequestStatus::NotStarted: StatusString = TEXT("NotStarted"); break;
		case EHttpRequestStatus::Processing: StatusString = TEXT("Processing"); break;
		case EHttpRequestStatus::Failed:     StatusString = TEXT("Failed"); break;
		case EHttpRequestStatus::Succeeded:  StatusString = TEXT("Succeeded"); break;
		default:                             StatusString = TEXT("Unknown"); break;
		}

		OutError = FString::Printf(TEXT("SSE request failed (status: %s)"), *StatusString);
		return false;
	}

	FHttpResponsePtr Response = Request->GetResponse();
	if (!Response.IsValid())
	{
		OutError = TEXT("SSE request got invalid response");
		return false;
	}

	const int32 ResponseCode = Response->GetResponseCode();
	if (ResponseCode != 200)
	{
		OutError = FString::Printf(TEXT("SSE request HTTP error %d: %s"),
			ResponseCode, *Response->GetContentAsString());
		return false;
	}

	const FString Body = Response->GetContentAsString();
	if (Body.IsEmpty())
	{
		// Not necessarily an error for SSE, but not useful to us either.
		OutError = TEXT("SSE response body is empty");
		return false;
	}

	ParseSseStream(Body, OutEvents);
	return OutEvents.Num() > 0;
}

void FUnrealGPTSseClient::ParseSseStream(
	const FString& Stream,
	TArray<FUnrealGPTSseEvent>& OutEvents)
{
	OutEvents.Reset();

	TArray<FString> Lines;
	Stream.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);

	FUnrealGPTSseEvent CurrentEvent;

	auto FlushEvent = [&]()
	{
		if (!CurrentEvent.Event.IsEmpty() || !CurrentEvent.Data.IsEmpty())
		{
			OutEvents.Add(CurrentEvent);
		}
		CurrentEvent = FUnrealGPTSseEvent();
	};

	for (const FString& Line : Lines)
	{
		// Empty line indicates end of event
		if (Line.IsEmpty())
		{
			FlushEvent();
			continue;
		}

		if (Line.StartsWith(TEXT("event:")))
		{
			CurrentEvent.Event = Line.Mid(6).TrimStartAndEnd();
		}
		else if (Line.StartsWith(TEXT("data:")))
		{
			FString DataLine = Line.Mid(5).TrimStartAndEnd();
			if (!CurrentEvent.Data.IsEmpty())
			{
				CurrentEvent.Data += TEXT("\n");
			}
			CurrentEvent.Data += DataLine;
		}
		// Ignore other fields (id, retry, comments) for now.
	}

	// Flush last event if any
	FlushEvent();
}


