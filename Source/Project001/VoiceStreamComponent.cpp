#include "VoiceStreamComponent.h"
#include "Components/AudioComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/Character.h"
#include "Interfaces/VoiceCapture.h"
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

  // ── Voice streaming ───────────────────────────────────────────────────
  if (VoiceCapture.IsValid()) {
    uint32 AvailableVoiceData = 0;
    EVoiceCaptureState::Type CaptureState =
        VoiceCapture->GetCaptureState(AvailableVoiceData);

    if (AvailableVoiceData > 0) {
      VoiceBuffer.Reset();
      VoiceBuffer.SetNumUninitialized(AvailableVoiceData);
      uint32 ReadVoiceData = 0;
      CaptureState = VoiceCapture->GetVoiceData(
          VoiceBuffer.GetData(), AvailableVoiceData, ReadVoiceData);

      if (ReadVoiceData > 0) {
        if (UServerConnectionComponent *Conn =
                GetOwner()
                    ->FindComponentByClass<UServerConnectionComponent>()) {
          Conn->SendBinary(VoiceBuffer.GetData(), ReadVoiceData);
        }
      }
    }
  }
}

void UVoiceStreamComponent::StopRecording() {
  if (VoiceCapture.IsValid()) {
    VoiceCapture->Stop();
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

  if (UServerConnectionComponent *Conn =
          GetOwner()->FindComponentByClass<UServerConnectionComponent>()) {
    FString JsonStr =
        FString::Printf(TEXT("{\"type\":\"tts\",\"text\":\"%s\"}"), *Text);
    Conn->SendString(JsonStr);
  }
}

void UVoiceStreamComponent::HandleIncomingTTSAudio(const void *Data,
                                                   int32 Size) {
  if (!Data || Size <= 0)
    return;

  if (!TTSWave) {
    TTSWave = NewObject<USoundWaveProcedural>();
    TTSWave->SetSampleRate(16000);
    TTSWave->NumChannels = 1;
    TTSWave->Duration = INDEFINITELY_LOOPING_DURATION;
    TTSWave->SoundGroup = SOUNDGROUP_Voice;
    TTSWave->bLooping = false;
  }

  float CurrentTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
  float Duration = (float)Size / 32000.0f;

  if (CurrentTime < TTSFinishTime) {
    // 正在播放中，排队叠加时间，不重置音频缓冲
    TTSFinishTime += Duration;
  } else {
    // 已经播完或从未播放，重置时间并清空之前的缓冲
    TTSFinishTime = CurrentTime + Duration + 0.2f;
    if (TTSWave) {
      TTSWave->ResetAudio();
    }
  }

  // 追加音频数据到缓冲区尾部
  TTSWave->QueueAudio((const uint8 *)Data, Size);

  if (TTSAudioComp) {
    if (TTSAudioComp->Sound != TTSWave) {
      TTSAudioComp->SetSound(TTSWave);
    }
    // 防止重复触发播放
    if (!TTSAudioComp->IsPlaying()) {
      TTSAudioComp->Play();
    }
  }
}
