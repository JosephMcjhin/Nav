#pragma once

#include "Components/ActorComponent.h"
#include "Containers/Queue.h"
#include "CoreMinimal.h"
#include "TTSQueueComponent.generated.h"

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class PROJECT001_API UTTSQueueComponent : public UActorComponent {
  GENERATED_BODY()

public:
  UTTSQueueComponent();

protected:
  virtual void BeginPlay() override;
  virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
  virtual void
  TickComponent(float DeltaTime, ELevelTick TickType,
                FActorComponentTickFunction *ThisTickFunction) override;

  /**
   * Speak a message immediately if the TTS engine is free.
   * If it is currently speaking, the new message is discarded.
   */
  UFUNCTION(BlueprintCallable, Category = "Voice Nav")
  void SpeakIfFree(const FString &Message);

  /** Clear all pending messages */
  UFUNCTION(BlueprintCallable, Category = "Voice Nav")
  void ClearQueue();

  FName VoiceChannelId = FName("MyVoiceChannel");
};
