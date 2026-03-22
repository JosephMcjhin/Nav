// ARTrackingComponent.cpp
// 实现跨平台 AR 追踪：
//   - (PLATFORM_ANDROID || PLATFORM_IOS)：使用纯净运动传感器
//   GetInputMotionState
//   - 其他（PC / Editor）：UDP 接收 ARWebSensor 中继数据

#include "ARTrackingComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"

// ─────────────────────────────────────────────────────────────────────────────
// PC / Editor 专用 include
// ─────────────────────────────────────────────────────────────────────────────
#if !(PLATFORM_ANDROID || PLATFORM_IOS)
#include "Common/UdpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#endif

// =============================================================================
// PC 端 UDP 接收线程实现（仅 PC / Editor 编译）
// =============================================================================
#if !(PLATFORM_ANDROID || PLATFORM_IOS)

FAR_UDPReceiver::FAR_UDPReceiver(int32 InPort,
                                 TWeakObjectPtr<UARTrackingComponent> InComp)
    : Port(InPort), Comp(InComp) {}

FAR_UDPReceiver::~FAR_UDPReceiver() { Stop(); }

bool FAR_UDPReceiver::Init() {
  FIPv4Endpoint Endpoint(FIPv4Address::Any, Port);
  RecvSocket = FUdpSocketBuilder(TEXT("ARTracking_UDPReceiver"))
                   .AsNonBlocking()
                   .AsReusable()
                   .BoundToEndpoint(Endpoint)
                   .WithReceiveBufferSize(64 * 1024)
                   .Build();

  if (!RecvSocket) {
    UE_LOG(LogTemp, Error,
           TEXT("[ARTracking] Failed to bind UDP socket on port %d"), Port);
    return false;
  }

  UE_LOG(LogTemp, Log, TEXT("[ARTracking] UDP receiver listening on port %d"),
         Port);
  bRunning = true;
  return true;
}

uint32 FAR_UDPReceiver::Run() {
  TArray<uint8> Buffer;
  Buffer.SetNum(512);

  while (bRunning) {
    if (!RecvSocket) {
      break;
    }

    uint32 PendingSize = 0;
    while (bRunning && RecvSocket->HasPendingData(PendingSize)) {
      int32 BytesRead = 0;
      TSharedRef<FInternetAddr> Sender =
          ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();

      if (RecvSocket->RecvFrom(Buffer.GetData(),
                               FMath::Min((int32)PendingSize, Buffer.Num()),
                               BytesRead, *Sender) &&
          BytesRead > 0) {
        // 转换为字符串
        FUTF8ToTCHAR Convert(reinterpret_cast<const char *>(Buffer.GetData()),
                             BytesRead);
        FString Payload(Convert.Length(), Convert.Get());
        Payload = Payload.TrimStartAndEnd();

        if (Comp.IsValid()) {
          FScopeLock Lock(&Comp->DataLock);
          Comp->RawUDPData = Payload; // 最新一包覆盖旧包
        }
      }
    }

    FPlatformProcess::Sleep(0.016f); // ~60Hz 轮询
  }

  return 0;
}

void FAR_UDPReceiver::Stop() {
  bRunning = false;
  if (RecvSocket) {
    RecvSocket->Close();
    ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(RecvSocket);
    RecvSocket = nullptr;
  }
}

#endif // !(PLATFORM_ANDROID || PLATFORM_IOS)

// =============================================================================
// UARTrackingComponent
// =============================================================================

UARTrackingComponent::UARTrackingComponent() {
  PrimaryComponentTick.bCanEverTick = true;
}

void UARTrackingComponent::BeginPlay() {
  Super::BeginPlay();

#if PLATFORM_ANDROID || PLATFORM_IOS
  UE_LOG(LogTemp, Log,
         TEXT("[ARTracking] Native Mobile Mode: Using local Gyro Sensor"));
  if (GEngine) {
    GEngine->AddOnScreenDebugMessage(
        -1, 10.f, FColor::Orange,
        TEXT("[ARTracking] 移动端启动原生陀螺仪直连"));
  }
#else
  // ── PC / Editor：启动 UDP 接收线程 ────────────────────────────────
  UDPReceiver = new FAR_UDPReceiver(UDPReceivePort,
                                    TWeakObjectPtr<UARTrackingComponent>(this));
  UDPReceiverThread = FRunnableThread::Create(
      UDPReceiver, TEXT("ARTracking_UDPReceiver_Thread"));

  if (GEngine) {
    GEngine->AddOnScreenDebugMessage(
        -1, 10.f, FColor::Cyan,
        FString::Printf(
            TEXT("[ARTracking] PC模式：监听 UDP 端口 %d（ARWebSensor中继）"),
            UDPReceivePort));
  }
  UE_LOG(LogTemp, Log,
         TEXT("[ARTracking] PC mode: UDP receiver started on port %d"),
         UDPReceivePort);
#endif
}

void UARTrackingComponent::EndPlay(const EEndPlayReason::Type EndPlayReason) {
#if !(PLATFORM_ANDROID || PLATFORM_IOS)
  // 停止 UDP 接收线程
  if (UDPReceiver) {
    UDPReceiver->Stop();
  }
  if (UDPReceiverThread) {
    UDPReceiverThread->Kill(true);
    delete UDPReceiverThread;
    UDPReceiverThread = nullptr;
  }
  if (UDPReceiver) {
    delete UDPReceiver;
    UDPReceiver = nullptr;
  }
#endif

  Super::EndPlay(EndPlayReason);
}

void UARTrackingComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction *ThisTickFunction) {
  Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

#if PLATFORM_ANDROID || PLATFORM_IOS
  UpdateFromSensors_Mobile(DeltaTime);
#else
  UpdateFromUDP();
#endif

  // ── 将追踪数据应用到 Owner ────────────────────────────────────────
  if (CurrentData.bIsTracking) {
    AActor *Owner = GetOwner();
    APawn *Pawn = Owner ? Cast<APawn>(Owner) : nullptr;

    if (Owner && bApplyYawToOwner) {
      // 首次收到数据时，记录角色当前朝向为"正方向"基准
      if (!bHasInitialYaw) {
        InitialWorldYaw = 90.f; // UE 场景正方向 = Yaw 90°
        bHasInitialYaw = true;

        // 同时将 CharacterMovement MaxWalkSpeed 设为 WalkSpeed
        ACharacter *Char = Cast<ACharacter>(Owner);
        if (Char && Char->GetCharacterMovement()) {
          Char->GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
        }
      }

      // 目标世界 Yaw = 初始朝向 + 手机相对偏角
      const float TargetWorldYaw = InitialWorldYaw + CurrentData.Rotation.Yaw;
      const FRotator TargetRot(0.f, TargetWorldYaw, 0.f);
      const FRotator CurRot = Owner->GetActorRotation();

      // RInterpTo：旋转专用插值，自动走最短路径
      const FRotator SmoothedRot =
          FMath::RInterpTo(CurRot, TargetRot, DeltaTime, YawInterpSpeed);
      Owner->SetActorRotation(SmoothedRot);

      // 步伐检测 → 角色沿当前朝向前进
      if (CurrentData.ForwardSpeed > 0.f && Pawn) {
        Pawn->AddMovementInput(Owner->GetActorForwardVector(), 1.0f);
      }
    }

    // 屏幕调试
    if (GEngine) {
      const bool bWalking = CurrentData.ForwardSpeed > 0.f;
      GEngine->AddOnScreenDebugMessage(
          70, 0.f, bWalking ? FColor::Green : FColor::Cyan,
          FString::Printf(TEXT("[ARTracking] RelYaw=%.1f  %s"),
                          CurrentData.Rotation.Yaw,
                          bWalking ? TEXT("Walking") : TEXT("Still")));
    }
  }
}

// =============================================================================
// 移动端：原生传感器解析 (免去 ARCore 的坑)
// =============================================================================

void UARTrackingComponent::UpdateFromSensors_Mobile(float DeltaTime) {
#if PLATFORM_ANDROID || PLATFORM_IOS
  APlayerController *PC = UGameplayStatics::GetPlayerController(this, 0);
  if (!PC)
    return;

  FVector Tilt, RotationRate, Gravity, Acceleration;
  PC->GetInputMotionState(Tilt, RotationRate, Gravity, Acceleration);

  // 如果无读数则不上报Tracking
  if (Tilt.IsNearlyZero() && Acceleration.IsNearlyZero()) {
    return; // Don't block bIsTracking, just wait for the first real frame
  }

  // 记录航向偏角 (原始值可能为弧度或归一化值)
  if (!bHasInitialSensorYaw) {
    InitialSensorYaw = Tilt.Z;
    bHasInitialSensorYaw = true;
  }

  // 手机端原始值乘以修正系数 (例如将弧度转换为角度度数)
  float RelYaw = (Tilt.Z - InitialSensorYaw) * MobileYawMultiplier;
  float AccelMag = Acceleration.Size();

  CurrentData.Rotation = FRotator(0.f, RelYaw, 0.f);
  CurrentData.ForwardSpeed =
      (AccelMag > StepAccelerationThreshold) ? 1.0f : 0.0f;
  CurrentData.bIsTracking = true;
#endif
}

// =============================================================================
// PC / Editor：UDP 数据解析
// =============================================================================

void UARTrackingComponent::UpdateFromUDP() {
#if !(PLATFORM_ANDROID || PLATFORM_IOS)
  FString LatestData;
  {
    FScopeLock Lock(&DataLock);
    if (RawUDPData.IsEmpty()) {
      return;
    }
    LatestData = RawUDPData;
    RawUDPData.Empty(); // 消费掉，避免重复处理同一帧数据
  }

  // 解析格式："X,Y,Z,Yaw"
  TArray<FString> Parts;
  LatestData.ParseIntoArray(Parts, TEXT(","));
  if (Parts.Num() < 4) {
    return;
  }

  // X = 前进速度信号（0=停止，1=行走）
  float Speed = FCString::Atof(*Parts[0]);
  float Yaw = FCString::Atof(*Parts[3]);

  CurrentData.ForwardSpeed = FMath::Clamp(Speed, 0.f, 1.f);
  CurrentData.Rotation = FRotator(0.f, Yaw, 0.f);
  CurrentData.bIsTracking = true;
#endif
}
