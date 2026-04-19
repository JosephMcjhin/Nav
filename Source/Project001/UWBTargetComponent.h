#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "UWBTargetComponent.generated.h"

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class PROJECT001_API UUWBTargetComponent : public UActorComponent {
  GENERATED_BODY()

public:
  UUWBTargetComponent();

  virtual void
  TickComponent(float DeltaTime, ELevelTick TickType,
                FActorComponentTickFunction *ThisTickFunction) override;

  UFUNCTION(BlueprintCallable, Category = "UWB")
  void SetUWBTarget(float InX, float InY);

  /** Set character yaw rotation (degrees). */
  UFUNCTION(BlueprintCallable, Category = "UWB")
  void SetUWBRotation(float Yaw);

private:
  FVector TargetLocation;
  bool bHasTarget = false;
  FRotator TargetRotation;
  bool bHasRotation = false;
};
