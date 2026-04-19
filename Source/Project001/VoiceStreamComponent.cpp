#include "VoiceStreamComponent.h"
#include "Components/AudioComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/Character.h"
#include "Interfaces/VoiceCapture.h"
#include "Misc/ScopeLock.h"
#include "ServerConnectionComponent.h"
#include "Sound/SoundWaveProcedural.h"
#include "VoiceModule.h"
#include "WebSocketsModule.h"

UVoiceStreamComponent::UVoiceStreamComponent() {
  PrimaryComponentTick.bCanEverTick = true;
  PrimaryComponentTick.bStartWithTickEnabled = true;
  // TTSAudioComp will be created in BeginPlay after we have a valid owner
}

void UVoiceStreamComponent::BeginPlay() {
  Super::BeginPlay();

  // Create TTSAudioComp now that we have a valid owning actor.
  // It must be attached to an actor's scene root, NOT constructed as a
  // sub-object of a non-scene component (which causes 0xFFFF access crashes).
  if (AActor *Owner = GetOwner()) {
    TTSAudioComp = NewObject<UAudioComponent>(Owner, TEXT("TTSAudioComp"));
    TTSAudioComp->bAutoActivate = false;
    TTSAudioComp->RegisterComponent();
    // Attach to the actor's root so it has a valid world transform
    if (USceneComponent *Root = Owner->GetRootComponent()) {
      TTSAudioComp->AttachToComponent(
          Root, FAttachmentTransformRules::KeepRelativeTransform);
    }
  }

  if (bAlwaysOn) {
    StartRecording();
  }
}

void UVoiceStreamComponent::EndPlay(const EEndPlayReason::Type EndPlayReason) {
  StopRecording();
  if (TTSAudioComp) {
    TTSAudioComp->Stop();
  }
  if (TTSAudioComp) {
    TTSAudioComp->Stop();
  }
  Super::EndPlay(EndPlayReason);
}

void UVoiceStreamComponent::StartRecording() {
  if (!VoiceCapture.IsValid()) {
    VoiceCapture = FVoiceModule::Get().CreateVoiceCapture(TEXT(""), 16000, 1);
    if (!VoiceCapture.IsValid())
      VoiceCapture = FVoiceModule::Get().CreateVoiceCapture(TEXT(""), 44100, 1);
    if (!VoiceCapture.IsValid())
      VoiceCapture = FVoiceModule::Get().CreateVoiceCapture(TEXT(""), 8000, 1);
  }

  if (VoiceCapture.IsValid()) {
    VoiceCapture->Start();
    bIsCapturing = true;
    if (GEngine)
      GEngine->AddOnScreenDebugMessage(-1, 2.f, FColor::Green,
                                       TEXT("Microphone Recording Started..."));
  }
}

// ConnectWebSocket function removed as per instruction

void UVoiceStreamComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction *ThisTickFunction) {
  Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

  // 1. Process Sequential TTS Overwrite Queue (Game Thread Safe)
  if (TTSAudioComp && !TTSAudioComp->IsPlaying() && PendingPCMData.Num() > 0) {
    FScopeLock Lock(&PendingLock);

    if (!TTSWave) {
      TTSWave = NewObject<USoundWaveProcedural>(this);
      TTSWave->SetSampleRate(16000);
      TTSWave->NumChannels = 1;
      TTSWave->Duration = INDEFINITELY_LOOPING_DURATION;
      TTSWave->SoundGroup = SOUNDGROUP_Voice;
      TTSWave->bLooping = false;
      TTSAudioComp->SetSound(TTSWave);
    }

    TTSWave->ResetAudio();
    TTSWave->QueueAudio(PendingPCMData.GetData(), PendingPCMData.Num());

    ActiveTTSTimestamp = LatestPendingTimestamp;
    PendingPCMData.Empty();
    TTSAudioComp->Play();
  }

  // 2. Voice streaming (Microphone out)
  if (VoiceCapture.IsValid()) {
    uint32 AvailableVoiceData = 0;
    EVoiceCaptureState::Type CaptureState =
        VoiceCapture->GetCaptureState(AvailableVoiceData);

    if (AvailableVoiceData > 0) {
      VoiceBuffer.SetNumUninitialized(AvailableVoiceData);
      uint32 ReadVoiceData = 0;
      CaptureState = VoiceCapture->GetVoiceData(
          VoiceBuffer.GetData(), AvailableVoiceData, ReadVoiceData);

      if (ReadVoiceData > 0 && bAlwaysOn) {
        ProcessAudioChunk(VoiceBuffer.GetData(), ReadVoiceData);
      } else if (bAlwaysOn) {
        ProcessAudioChunk(nullptr, 0);
      }
    } else if (bAlwaysOn) {
      ProcessAudioChunk(nullptr, 0);
    }
  } else if (bAlwaysOn && !bIsCapturing) {
    StartRecording();
  }
}

void UVoiceStreamComponent::StopRecording() {
  if (VoiceCapture.IsValid()) {
    VoiceCapture->Stop();
    bIsCapturing = false;
  }
  SendEndOfSpeech();
}

// CalibrateCurrentPoint function removed as per instruction

void UVoiceStreamComponent::SendEndOfSpeech() {
  if (UServerConnectionComponent *Conn =
          GetOwner()->FindComponentByClass<UServerConnectionComponent>()) {
    Conn->SendString(TEXT("{\"type\":\"end_of_speech\"}"));
    if (GEngine)
      GEngine->AddOnScreenDebugMessage(-1, 2.f, FColor::Cyan,
                                       TEXT("Processing speech..."));
  }
}

void UVoiceStreamComponent::RequestTTS(const FString &Text) {
  // Generate a high-precision locally monotonic timestamp
  double RequestTimestamp = FPlatformTime::Seconds();

  if (UServerConnectionComponent *Conn =
          GetOwner()->FindComponentByClass<UServerConnectionComponent>()) {
    // Send the timestamp to the server so it can attach it to the binary audio
    FString JsonStr = FString::Printf(
        TEXT("{\"type\":\"tts\",\"text\":\"%s\",\"timestamp\":%f}"), *Text,
        RequestTimestamp);
    Conn->SendString(JsonStr);
  }
}

void UVoiceStreamComponent::HandleIncomingTTSAudio(const void *Data,
                                                   int32 Size) {
  if (!Data || Size < 8) {
    return;
  }

  // Extract the 8-byte double timestamp from the beginning of the payload
  const uint8 *ByteData = static_cast<const uint8 *>(Data);
  double IncomingTimestamp = 0.0;
  FMemory::Memcpy(&IncomingTimestamp, ByteData, sizeof(double));

  const uint8 *AudioPCMData = ByteData + sizeof(double);
  uint32 AudioPCMSize = static_cast<uint32>(Size - sizeof(double));

  // Skip if it's older than what we are currently playing OR already have in
  // the pending slot
  if (IncomingTimestamp <= ActiveTTSTimestamp ||
      IncomingTimestamp <= LatestPendingTimestamp) {
    return;
  }

  // Update the pending buffer (Overwrite previous pending)
  {
    FScopeLock Lock(&PendingLock);
    LatestPendingTimestamp = IncomingTimestamp;
    PendingPCMData.SetNumUninitialized((int32)AudioPCMSize);
    FMemory::Memcpy(PendingPCMData.GetData(), AudioPCMData, AudioPCMSize);
  }
}

void UVoiceStreamComponent::ProcessAudioChunk(const uint8 *Data, uint32 Size) {
  float DeltaTime = 0.0f;
  bool bHasVoice = false;
  float NormalizedAmp = 0.0f;

  if (Data && Size > 0) {
    // Calculate Peak Amplitude to detect silence
    const int16 *PcmData = (const int16 *)Data;
    int32 Samples = Size / 2;
    int16 MaxAmp = 0;
    for (int32 i = 0; i < Samples; i++) {
      int16 AbsVal = FMath::Abs(PcmData[i]);
      if (AbsVal > MaxAmp)
        MaxAmp = AbsVal;
    }
    NormalizedAmp = (float)MaxAmp / 32768.0f;
    DeltaTime = Samples / 16000.0f; // Roughly duration based on 16kHz
    bHasVoice = (NormalizedAmp >= SilenceThreshold);
  } else {
    // Unreal Engine's VoiceCapture noise gate kicked in and provided NO data.
    // This implies total silence.
    DeltaTime = GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.0f;
    bHasVoice = false;
  }

  // If we have literally 0 delta time (should be rare), just return
  if (DeltaTime <= 0.0f)
    return;

  UServerConnectionComponent *Conn =
      GetOwner()
          ? GetOwner()->FindComponentByClass<UServerConnectionComponent>()
          : nullptr;

  if (CurrentVoiceState == EVoiceState::Idle) {
    if (bHasVoice) {
      // Transition to Speaking
      CurrentVoiceState = EVoiceState::Speaking;
      CurrentSilenceTime = 0.0f;
      CurrentSpeechDuration = 0.0f;

      if (GEngine) {
        GEngine->AddOnScreenDebugMessage(3004, 2.f, FColor::Green,
                                         TEXT("正在说话 (开始收音...)"));
      }

      // Send the pre-record buffer first (if we have a connection)
      if (Conn && PreRecordBuffer.Num() > 0) {
        Conn->SendBinary(PreRecordBuffer.GetData(), PreRecordBuffer.Num());
      }
      PreRecordBuffer.Empty();

      // Send current chunk
      if (Conn) {
        Conn->SendBinary(Data, Size);
      }
    } else {
      // Append to rolling PreRecordBuffer (keep ~0.5s of audio)
      // 16kHz * 2 bytes * 0.5s = 16000 bytes max
      if (Data && Size > 0) {
        PreRecordBuffer.Append(Data, Size);
        if (PreRecordBuffer.Num() > 16000) {
          int32 ItemsToRemove = PreRecordBuffer.Num() - 16000;
          PreRecordBuffer.RemoveAt(0, ItemsToRemove, false);
        }
      }
    }
  } else if (CurrentVoiceState == EVoiceState::Speaking) {
    // Send directly
    if (Conn && Data && Size > 0) {
      Conn->SendBinary(Data, Size);
    }

    CurrentSpeechDuration += DeltaTime;

    if (!bHasVoice) {
      CurrentSilenceTime += DeltaTime;
    } else {
      CurrentSilenceTime = 0.0f;
    }

    bool bShouldSlice = false;

    // Rule 1: Silence detected after speaking
    if (CurrentSilenceTime >= SilenceDurationTrigger) {
      bShouldSlice = true;
    }
    // Rule 2: Too long
    else if (CurrentSpeechDuration >= MaxSpeechDuration) {
      bShouldSlice = true;
    }

    if (bShouldSlice) {
      CurrentVoiceState = EVoiceState::Processing;
      SendEndOfSpeech();

      if (GEngine) {
        GEngine->AddOnScreenDebugMessage(3004, 2.f, FColor::Cyan,
                                         TEXT("语音结束 (发送后端中...)"));
      }
    }
  } else if (CurrentVoiceState == EVoiceState::Processing) {
    // Ignore audio while waiting for the server to process.
    // The state will be reset back to Idle when we receive the TTS response
    // or status update, but for now we auto-reset after a short delay or in the
    // TTS handler. Actually, to make it completely hands-free, we can
    // transition back to Idle automatically after sending the speech, or just
    // wait for the TTS. Given the TTS handler plays audio and might overlap, we
    // can reset to Idle immediately and let VAD handle it, but the TTS speaker
    // might trigger our own mic! For now, let's just go straight back to Idle
    // to allow immediate next command.
    CurrentVoiceState = EVoiceState::Idle;
    PreRecordBuffer.Empty();
  }
}
