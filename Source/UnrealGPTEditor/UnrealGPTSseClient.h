#pragma once

#include "CoreMinimal.h"
#include "Http.h"

/**
 * Simple SSE (Server-Sent Events) client helper.
 *
 * This is a low-level utility that can perform a single blocking fetch of an
 * SSE stream and parse it into discrete events. It is intended as a building
 * block for a higher-level MCP runtime that maintains long-lived connections.
 */

/** One parsed SSE event: "event:" and "data:" lines. */
struct FUnrealGPTSseEvent
{
	/** Optional event type name (from "event:" line). */
	FString Event;

	/** Raw data payload (concatenated "data:" lines with '\n'). */
	FString Data;
};

class FUnrealGPTSseClient
{
public:
	/**
	 * Perform a blocking GET request to an SSE endpoint and parse all events.
	 *
	 * This helper:
	 * - Issues a single HTTP GET with "Accept: text/event-stream".
	 * - Waits until the request finishes (or times out).
	 * - Parses the full response body as an SSE stream into events.
	 *
	 * It is not a full streaming client yet, but provides a concrete SSE
	 * parser and network path that a longer-lived MCP runtime can build on.
	 *
	 * @param Url          SSE endpoint URL.
	 * @param Headers      Optional additional headers (e.g., Authorization).
	 * @param OutEvents    Parsed SSE events on success.
	 * @param OutError     Error message on failure.
	 * @return true on success (HTTP 200 and parse OK), false otherwise.
	 */
	static bool FetchEvents(
		const FString& Url,
		const TMap<FString, FString>& Headers,
		TArray<FUnrealGPTSseEvent>& OutEvents,
		FString& OutError);

private:
	/** Parse a full SSE stream string into discrete events. */
	static void ParseSseStream(
		const FString& Stream,
		TArray<FUnrealGPTSseEvent>& OutEvents);
};


