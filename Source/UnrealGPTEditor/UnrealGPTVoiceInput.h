#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "AudioCaptureCore.h"
#include "Sound/SoundWave.h"
#include "Http.h"
#include "UnrealGPTVoiceInput.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnTranscriptionComplete, const FString&, TranscribedText);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnRecordingStarted);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnRecordingStopped);

UCLASS()
class UNREALGPTEDITOR_API UUnrealGPTVoiceInput : public UObject
{
	GENERATED_BODY()

public:
	UUnrealGPTVoiceInput();

	/** Initialize the voice input system */
	void Initialize();

	/** Start recording audio from microphone */
	bool StartRecording();

	/** Stop recording and transcribe using Whisper API */
	void StopRecordingAndTranscribe();

	/** Cancel current recording without transcribing */
	void CancelRecording();

	/** Check if currently recording */
	bool IsRecording() const { return bIsRecording; }

	/** Delegate fired when transcription is complete */
	UPROPERTY(BlueprintAssignable)
	FOnTranscriptionComplete OnTranscriptionComplete;

	/** Delegate fired when recording starts */
	UPROPERTY(BlueprintAssignable)
	FOnRecordingStarted OnRecordingStarted;

	/** Delegate fired when recording stops */
	UPROPERTY(BlueprintAssignable)
	FOnRecordingStopped OnRecordingStopped;

private:
	/** Handle Whisper API response */
	void OnWhisperResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

	/** Convert captured audio to WAV format */
	TArray<uint8> ConvertToWAV(const TArray<float>& PCMData, int32 SampleRate, int32 NumChannels);

	/** Captured audio data (interleaved floats) */
	TArray<float> CapturedAudioData;

	/** Low-level audio capture synth (handles microphone capture) */
	Audio::FAudioCaptureSynth CaptureSynth;

	/** Sample rate for recording */
	int32 RecordingSampleRate;

	/** Number of channels for recording */
	int32 RecordingNumChannels;

	/** Is currently recording */
	bool bIsRecording;

	/** Settings reference */
	class UUnrealGPTSettings* Settings;
};

