#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
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

  /** Manually set or update from server whether UWB is calibrated. */
  UFUNCTION(BlueprintCallable, Category = "UWB")
  void SetCalibrated(bool bInCalibrated);

  /** Whether the UWB-to-UE transformation is ready. If false, no movement
   * occurs. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UWB")
  bool bIsCalibrated = false;

  /** Movement speed in cm/s when lerping toward UWB target. 0 = teleport
   * instantly. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UWB",
            meta = (ClampMin = "0"))
  float MoveSpeed = 150.f;

  /** Whether a valid UWB target has been received. */
  UPROPERTY(BlueprintReadOnly, Category = "UWB",
            meta = (AllowPrivateAccess = "true"))
  bool bHasTarget = false;

protected:
  virtual void BeginPlay() override;

private:
  float TargetX = 0.f;
  float TargetY = 0.f;
  float DebugTimer = 0.f;
};
