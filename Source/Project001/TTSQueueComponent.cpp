#include "TTSQueueComponent.h"
#include "Engine/Engine.h"
#include "VoiceStreamComponent.h"

UTTSQueueComponent::UTTSQueueComponent() {
  PrimaryComponentTick.bCanEverTick = true;
  PrimaryComponentTick.TickInterval = 0.1f;
}

void UTTSQueueComponent::BeginPlay() { Super::BeginPlay(); }

void UTTSQueueComponent::EndPlay(const EEndPlayReason::Type EndPlayReason) {
  Super::EndPlay(EndPlayReason);
}

void UTTSQueueComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction *ThisTickFunction) {
  Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

void UTTSQueueComponent::SpeakIfFree(const FString &Message) {
  if (Message.IsEmpty())
    return;

  if (UVoiceStreamComponent *VoiceComp =
          GetOwner()->FindComponentByClass<UVoiceStreamComponent>()) {
    VoiceComp->RequestTTS(Message);
    UE_LOG(LogTemp, Log, TEXT("[TTSQueue] Forwarded Cloud TTS: %s"), *Message);
  } else {
    UE_LOG(LogTemp, Warning,
           TEXT("[TTSQueue] Failed to find VoiceStreamComponent!"));
  }
}

void UTTSQueueComponent::ClearQueue() {
  // Queue removed, nothing to clear. Kept for Blueprint compatibility if
  // needed, or can be removed if not called from BP.
}
