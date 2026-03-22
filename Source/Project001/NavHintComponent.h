#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "NavHintComponent.generated.h"

UENUM(BlueprintType)
enum class ETrackingMode : uint8 {
  Keyboard UMETA(DisplayName = "Keyboard/Gamepad (Default)"),
  NetworkSensor UMETA(DisplayName = "Network Sensor (UDP)"),
  LocalAR UMETA(DisplayName = "Local Native AR")
};

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class PROJECT001_API UNavHintComponent : public UActorComponent {
  GENERATED_BODY()

public:
  UNavHintComponent();

protected:
  virtual void BeginPlay() override;

public:
  virtual void
  TickComponent(float DeltaTime, ELevelTick TickType,
                FActorComponentTickFunction *ThisTickFunction) override;

  // 支持多个目的地标签配置
  UPROPERTY(EditAnywhere, Category = "Navigation")
  TArray<FName> DestinationTags;

  // 感知范围
  UPROPERTY(EditAnywhere, Category = "Navigation")
  float AwarenessRadius = 1000.0f;

  // 缩放比例（保留你代码中的 /3.0 逻辑）
  UPROPERTY(EditAnywhere, Category = "Navigation")
  float DistanceScale = 3.0f;

  UPROPERTY(EditAnywhere, Category = "Tracking Strategy")
  ETrackingMode TrackingMode = ETrackingMode::Keyboard;

  UPROPERTY(EditAnywhere, Category = "Tracking Strategy|Network")
  int32 UDPListenPort = 9003;

  UPROPERTY(EditAnywhere, Category = "Tracking Strategy|AR")
  class UARSessionConfig *ARConfig;

private:
  class FSocket *ReceiverSocket = nullptr;
  FVector ARInitialLocation = FVector::ZeroVector;
  FRotator ARInitialRotation = FRotator::ZeroRotator;
  bool bARInitialized = false;

  void ProcessUDPSensorData();
  virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

  // 存储找到的所有目标对象
  UPROPERTY()
  TArray<AActor *> DestinationObjects;

  UPROPERTY()
  TArray<AActor *> AllVolumes;

  void FindAllModifierVolumes();
  void FindDestinations();

  // 状态追踪
  FName LastNavTarget = NAME_None;
  FString LastSpokenDirection = TEXT("");
  float LastSpokenDistance = -1.0f;

  // 辅助函数：获取两向量间的相对方位
  FString GetRelativeDirectionText(const FVector &Forward, const FVector &Right,
                                   const FVector &DirectionToTarget);

  // 辅助函数：解析完整路径并生成描述字符串
  FString GetFullPathDescription(const TArray<FVector> &PathPoints,
                                 const FVector &CurrentForward,
                                 const FVector &CurrentRight);
};