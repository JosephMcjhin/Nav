// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "VoiceStreamComponent.generated.h"

class IVoiceCapture;
class IWebSocket;
class USoundWaveProcedural;
class UAudioComponent;

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
};
