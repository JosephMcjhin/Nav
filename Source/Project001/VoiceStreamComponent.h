// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "VoiceStreamComponent.generated.h"

class IVoiceCapture;
class IWebSocket;
class USoundWaveProcedural;
class UAudioComponent;

UENUM(BlueprintType)
enum class EVoiceState : uint8 { Idle, Speaking, Processing };

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnVoiceCommandReceived, FString,
                                            CommandResult);

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class PROJECT001_API UVoiceStreamComponent : public UActorComponent {
  GENERATED_BODY()

public:
  UVoiceStreamComponent();

protected:
  virtual void BeginPlay() override;
  virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
  virtual void
  TickComponent(float DeltaTime, ELevelTick TickType,
                FActorComponentTickFunction *ThisTickFunction) override;

  /** Fired when server sends a text command (NAV=, TTS=, MOVETO= etc.) */
  UPROPERTY(BlueprintAssignable, Category = "Voice Nav")
  FOnVoiceCommandReceived OnCommandReceived;

  /** Start capturing voice and streaming to the WebSocket server. */
  UFUNCTION(BlueprintCallable, Category = "Voice Nav")
  void StartRecording();

  /** Stop capturing voice and notify the server to start recognition. */
  UFUNCTION(BlueprintCallable, Category = "Voice Nav")
  void StopRecording();

  /** Stop distance: character stops when within this radius of target (cm). */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voice Nav")
  float AcceptanceRadius = 60.f;

  /** Stop distance: character stops when within this radius of target (cm). */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voice Nav|Beacon")
  float BeaconScale = 100.f;

  // HTTP base URL derived from WebSocket URL (e.g. "http://127.0.0.1:8090")
  // Made public so UI widgets can POST calibration data directly.
  UPROPERTY(BlueprintReadOnly, Category = "Voice Nav")
  FString ServerBaseURL;

  /** Request the python server to synthesize TTS */
  UFUNCTION(BlueprintCallable, Category = "Voice Nav")
  void RequestTTS(const FString &Text);

  /** Play back received PCM binary data */
  void HandleIncomingTTSAudio(const void *Data, int32 Size);

  /** If true, the microphone is always capturing and auto-detecting silence to
   * send packets. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voice Nav")
  bool bAlwaysOn = true;

  /** Silence threshold for VAD (absolute amplitude). */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voice Nav")
  float SilenceThreshold = 0.01f;

  /** Time in seconds before a silence is considered end-of-speech. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voice Nav")
  float SilenceDurationTrigger = 1.0f;

  /** Maximum duration of a single speech segment in seconds. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voice Nav")
  float MaxSpeechDuration = 10.0f;

private:
  TSharedPtr<IVoiceCapture> VoiceCapture;
  TArray<uint8> VoiceBuffer;

  bool bHasTarget = false;
  FVector TargetLocation = FVector::ZeroVector;

  // ── UWB Calibration ────────────────────────────────────────

  void SendEndOfSpeech();

  // For TTS Playback
  UPROPERTY(Transient)
  USoundWaveProcedural *TTSWave;

  UPROPERTY(Transient)
  UAudioComponent *TTSAudioComp;

  float LastSpokenDistance = -1.0f;
  float LastTTSTime = 0.0f;

  // New robust queuing logic
  double ActiveTTSTimestamp = 0.0; // Timestamp of the currently playing audio
  double LatestPendingTimestamp = 0.0; // Timestamp of the next up audio
  TArray<uint8> PendingPCMData;        // Buffer for the next up audio
  FCriticalSection
      PendingLock; // Lock to protect PendingPCMData between threads

  bool bTTSInterrupted = false;

  // VAD Internal State
  EVoiceState CurrentVoiceState = EVoiceState::Idle;
  TArray<uint8> PreRecordBuffer;

  float CurrentSilenceTime = 0.0f;
  float CurrentSpeechDuration = 0.0f;
  bool bIsCapturing = false;

  /** Process incoming chunks against VAD state machine and handle transmission
   */
  void ProcessAudioChunk(const uint8 *Data, uint32 Size);
};
