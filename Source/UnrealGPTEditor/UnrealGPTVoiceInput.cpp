#include "UnrealGPTVoiceInput.h"
#include "UnrealGPTSettings.h"
#include "Misc/Base64.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundWave.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/FileHelper.h"

UUnrealGPTVoiceInput::UUnrealGPTVoiceInput()
	: RecordingSampleRate(16000)
	, RecordingNumChannels(1)
	, bIsRecording(false)
{
	Settings = GetMutableDefault<UUnrealGPTSettings>();
}

void UUnrealGPTVoiceInput::Initialize()
{
	// Ensure settings are loaded
	if (!Settings)
	{
		Settings = GetMutableDefault<UUnrealGPTSettings>();
	}
}

bool UUnrealGPTVoiceInput::StartRecording()
{
	if (bIsRecording)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Already recording"));
		return false;
	}

	// Clear previous audio data
	CapturedAudioData.Empty();

	// Open default capture stream if needed
	if (!CaptureSynth.IsStreamOpen())
	{
		if (!CaptureSynth.OpenDefaultStream())
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to open default audio capture stream"));
			return false;
		}
	}

	// Query default device info for sample rate / channels
	Audio::FCaptureDeviceInfo DeviceInfo;
	if (CaptureSynth.GetDefaultCaptureDeviceInfo(DeviceInfo))
	{
		RecordingSampleRate = DeviceInfo.PreferredSampleRate;
		RecordingNumChannels = DeviceInfo.InputChannels;
	}

	// Start capturing from microphone
	if (!CaptureSynth.StartCapturing())
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to start audio capture"));
		return false;
	}

	bIsRecording = true;
	OnRecordingStarted.Broadcast();
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Started audio recording"));

	return true;
}

void UUnrealGPTVoiceInput::StopRecordingAndTranscribe()
{
	if (!bIsRecording)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Not recording, cannot stop"));
		return;
	}

	// Stop capturing
	CaptureSynth.StopCapturing();
	
	// Pull all queued audio data from the synth
	TArray<float> Chunk;
	while (CaptureSynth.GetAudioData(Chunk))
	{
		if (Chunk.Num() > 0)
		{
			CapturedAudioData.Append(Chunk);
		}
	}
	
	bIsRecording = false;
	OnRecordingStopped.Broadcast();

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Stopped recording, captured %d samples"), CapturedAudioData.Num());

	if (CapturedAudioData.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: No audio data captured from microphone"));
		OnTranscriptionComplete.Broadcast(TEXT(""));
		return;
	}

	// Ensure Settings is valid
	if (!Settings)
	{
		Settings = GetMutableDefault<UUnrealGPTSettings>();
		if (!Settings)
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Settings is null and could not be retrieved"));
			OnTranscriptionComplete.Broadcast(TEXT(""));
			return;
		}
	}

	if (Settings->ApiKey.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: API Key not set in settings"));
		OnTranscriptionComplete.Broadcast(TEXT(""));
		return;
	}

	// Convert audio to WAV format
	TArray<uint8> WAVData = ConvertToWAV(CapturedAudioData, RecordingSampleRate, RecordingNumChannels);
	if (WAVData.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to convert audio to WAV"));
		OnTranscriptionComplete.Broadcast(TEXT(""));
		return;
	}

	// Create HTTP request to Whisper API
	TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
	
	// Determine Whisper API endpoint (respecting relative or absolute API endpoint settings)
	FString WhisperEndpoint = Settings->ApiEndpoint;

	// First, normalize the path portion for audio transcriptions
	WhisperEndpoint.ReplaceInline(TEXT("/v1/responses"), TEXT("/v1/audio/transcriptions"));
	WhisperEndpoint.ReplaceInline(TEXT("/v1/chat/completions"), TEXT("/v1/audio/transcriptions"));

	// If the configured endpoint is not a full URL, build it from the base URL
	if (!WhisperEndpoint.StartsWith(TEXT("http")))
	{
		FString BaseUrl = Settings->BaseUrlOverride.IsEmpty()
			? TEXT("https://api.openai.com")
			: Settings->BaseUrlOverride;

		// Ensure we have a sensible path
		FString Path = WhisperEndpoint;
		if (Path.IsEmpty())
		{
			Path = TEXT("/v1/audio/transcriptions");
		}
		else
		{
			if (!Path.StartsWith(TEXT("/")))
			{
				Path = TEXT("/") + Path;
			}

			if (!Path.Contains(TEXT("/v1/audio/transcriptions")))
			{
				Path = TEXT("/v1/audio/transcriptions");
			}
		}

		WhisperEndpoint = BaseUrl + Path;
	}

	Request->SetURL(WhisperEndpoint);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Settings->ApiKey));

	// Build multipart form data
	const FString Boundary = TEXT("----UnrealGPTWhisperBoundary");
	Request->SetHeader(TEXT("Content-Type"), FString::Printf(TEXT("multipart/form-data; boundary=%s"), *Boundary));

	TArray<uint8> RequestData;

	// Part 1: file
	{
		FString FileHeader;
		FileHeader += FString::Printf(TEXT("--%s\r\n"), *Boundary);
		FileHeader += TEXT("Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n");
		FileHeader += TEXT("Content-Type: audio/wav\r\n\r\n");

		FTCHARToUTF8 FileHeaderUtf8(*FileHeader);
		RequestData.Append(reinterpret_cast<const uint8*>(FileHeaderUtf8.Get()), FileHeaderUtf8.Length());

		// Binary WAV data
		RequestData.Append(WAVData);
	}

	// Part 2: model
	{
		FString ModelPart;
		ModelPart += TEXT("\r\n");
		ModelPart += FString::Printf(TEXT("--%s\r\n"), *Boundary);
		ModelPart += TEXT("Content-Disposition: form-data; name=\"model\"\r\n\r\n");
		ModelPart += TEXT("whisper-1\r\n");

		FTCHARToUTF8 ModelUtf8(*ModelPart);
		RequestData.Append(reinterpret_cast<const uint8*>(ModelUtf8.Get()), ModelUtf8.Length());
	}

	// Final closing boundary
	{
		FString Closing = FString::Printf(TEXT("--%s--\r\n"), *Boundary);
		FTCHARToUTF8 ClosingUtf8(*Closing);
		RequestData.Append(reinterpret_cast<const uint8*>(ClosingUtf8.Get()), ClosingUtf8.Length());
	}

	Request->SetContent(RequestData);
	Request->OnProcessRequestComplete().BindUObject(this, &UUnrealGPTVoiceInput::OnWhisperResponseReceived);

	if (!Request->ProcessRequest())
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to process Whisper API request"));
		OnTranscriptionComplete.Broadcast(TEXT(""));
	}
}

void UUnrealGPTVoiceInput::CancelRecording()
{
	if (!bIsRecording)
	{
		return;
	}

	// Stop capturing if active
	CaptureSynth.StopCapturing();
	
	bIsRecording = false;
	CapturedAudioData.Empty();
	OnRecordingStopped.Broadcast();
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Recording cancelled"));
}

void UUnrealGPTVoiceInput::OnWhisperResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	FString TranscribedText;

	if (!bWasSuccessful || !Response.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Whisper API request failed"));
		OnTranscriptionComplete.Broadcast(TEXT(""));
		return;
	}

	int32 ResponseCode = Response->GetResponseCode();
	if (ResponseCode != 200)
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Whisper API returned error code %d: %s"), 
			ResponseCode, *Response->GetContentAsString());
		OnTranscriptionComplete.Broadcast(TEXT(""));
		return;
	}

	// Parse JSON response
	FString ResponseContent = Response->GetContentAsString();
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseContent);

	if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
	{
		if (JsonObject->TryGetStringField(TEXT("text"), TranscribedText))
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Transcription successful: %s"), *TranscribedText);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Whisper response missing 'text' field"));
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to parse Whisper API response: %s"), *ResponseContent);
	}

	OnTranscriptionComplete.Broadcast(TranscribedText);
}

TArray<uint8> UUnrealGPTVoiceInput::ConvertToWAV(const TArray<float>& PCMData, int32 SampleRate, int32 NumChannels)
{
	TArray<uint8> WAVData;

	if (PCMData.Num() == 0)
	{
		return WAVData;
	}

	// WAV file header structure
	struct FWAVHeader
	{
		uint32 ChunkID;        // "RIFF"
		uint32 ChunkSize;      // File size - 8
		uint32 Format;         // "WAVE"
		uint32 Subchunk1ID;    // "fmt "
		uint32 Subchunk1Size;  // 16 for PCM
		uint16 AudioFormat;    // 1 for PCM
		uint16 NumChannels;    // 1 or 2
		uint32 SampleRate;     // e.g., 16000
		uint32 ByteRate;       // SampleRate * NumChannels * BitsPerSample/8
		uint16 BlockAlign;     // NumChannels * BitsPerSample/8
		uint16 BitsPerSample;  // 16
		uint32 Subchunk2ID;    // "data"
		uint32 Subchunk2Size;  // NumSamples * NumChannels * BitsPerSample/8
	};

	const int32 BitsPerSample = 16;
	const int32 NumSamples = PCMData.Num() / NumChannels;
	const int32 DataSize = NumSamples * NumChannels * (BitsPerSample / 8);

	FWAVHeader Header;
	Header.ChunkID = 0x46464952; // "RIFF"
	Header.ChunkSize = 36 + DataSize;
	Header.Format = 0x45564157; // "WAVE"
	Header.Subchunk1ID = 0x20746D66; // "fmt "
	Header.Subchunk1Size = 16;
	Header.AudioFormat = 1; // PCM
	Header.NumChannels = NumChannels;
	Header.SampleRate = SampleRate;
	Header.ByteRate = SampleRate * NumChannels * (BitsPerSample / 8);
	Header.BlockAlign = NumChannels * (BitsPerSample / 8);
	Header.BitsPerSample = BitsPerSample;
	Header.Subchunk2ID = 0x61746164; // "data"
	Header.Subchunk2Size = DataSize;

	// Write header
	WAVData.Append(reinterpret_cast<const uint8*>(&Header), sizeof(FWAVHeader));

	// Convert float samples to 16-bit PCM
	for (int32 i = 0; i < PCMData.Num(); ++i)
	{
		// Clamp and convert to int16
		float Sample = FMath::Clamp(PCMData[i], -1.0f, 1.0f);
		int16 PCMSample = static_cast<int16>(Sample * 32767.0f);
		
		// Write as little-endian
		WAVData.Add(static_cast<uint8>(PCMSample & 0xFF));
		WAVData.Add(static_cast<uint8>((PCMSample >> 8) & 0xFF));
	}

	return WAVData;
}

