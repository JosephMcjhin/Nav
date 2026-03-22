#include "NavHintComponent.h"
#include "ARBlueprintLibrary.h"
#include "Common/UdpSocketBuilder.h"
#include "Components/BrushComponent.h"
#include "DrawDebugHelpers.h"
#include "GameFramework/Character.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Kismet/GameplayStatics.h"
#include "NavCommandComponent.h"
#include "NavModifierVolume.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "SocketSubsystem.h"
#include "Sockets.h"


static void SendTTSMessage(const FString &Message) {
  if (Message.IsEmpty())
    return;

  ISocketSubsystem *SocketSubsystem =
      ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
  if (!SocketSubsystem)
    return;

  TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
  bool bIsValid;
  Addr->SetIp(TEXT("127.0.0.1"), bIsValid);
  Addr->SetPort(9002);

  FSocket *UdpSocket =
      FUdpSocketBuilder(TEXT("TTS_UDP_Sender")).AsNonBlocking().AsReusable();

  if (UdpSocket) {
    FTCHARToUTF8 Convert(*Message);
    int32 BytesSent = 0;
    UdpSocket->SendTo((const uint8 *)Convert.Get(), Convert.Length(), BytesSent,
                      *Addr);

    UdpSocket->Close();
    SocketSubsystem->DestroySocket(UdpSocket);
  }
}

UNavHintComponent::UNavHintComponent() {
  PrimaryComponentTick.bCanEverTick = true;
  // 初始化默认目的地标签
  DestinationTags.Add(FName("Goal"));
}

void UNavHintComponent::BeginPlay() {
  Super::BeginPlay();
  FindDestinations();
  FindAllModifierVolumes();

  if (TrackingMode == ETrackingMode::NetworkSensor) {
    FIPv4Endpoint Endpoint(FIPv4Address::Any, UDPListenPort);
    ReceiverSocket = FUdpSocketBuilder(TEXT("AR_UDP_Receiver"))
                         .AsNonBlocking()
                         .AsReusable()
                         .BoundToEndpoint(Endpoint)
                         .WithReceiveBufferSize(1024);
  } else if (TrackingMode == ETrackingMode::LocalAR) {
    if (ARConfig) {
      UARBlueprintLibrary::StartARSession(ARConfig);
    }
  }
}

void UNavHintComponent::EndPlay(const EEndPlayReason::Type EndPlayReason) {
  if (ReceiverSocket) {
    ReceiverSocket->Close();
    ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)
        ->DestroySocket(ReceiverSocket);
    ReceiverSocket = nullptr;
  }

  if (TrackingMode == ETrackingMode::LocalAR) {
    UARBlueprintLibrary::PauseARSession();
  }

  Super::EndPlay(EndPlayReason);
}

void UNavHintComponent::ProcessUDPSensorData() {
  if (!ReceiverSocket || TrackingMode != ETrackingMode::NetworkSensor ||
      !GetOwner())
    return;

  TArray<uint8> ReceivedData;
  uint32 Size;
  while (ReceiverSocket->HasPendingData(Size)) {
    ReceivedData.Init(0, FMath::Min(Size, 65507u));
    int32 Read = 0;
    ReceiverSocket->Recv(ReceivedData.GetData(), ReceivedData.Num(), Read);

    if (Read > 0) {
      FString Msg = FString(ANSI_TO_TCHAR(reinterpret_cast<const char *>(
                                ReceivedData.GetData())))
                        .Left(Read);
      TArray<FString> Parts;
      Msg.ParseIntoArray(Parts, TEXT(","), true);
      // Example UDP payload: "100.0,0.0,0.0,0.0" -> X,Y,Z,Yaw
      if (Parts.Num() >= 4) {
        float X = FCString::Atof(*Parts[0]);
        float Y = FCString::Atof(*Parts[1]);
        float Z = FCString::Atof(*Parts[2]);
        float Yaw = FCString::Atof(*Parts[3]);

        FVector UDPLoc(X, Y, Z);
        FRotator UDPRot(0, Yaw, 0);

        if (!bARInitialized) {
          ARInitialLocation = GetOwner()->GetActorLocation() - UDPLoc;
          ARInitialRotation = GetOwner()->GetActorRotation() - UDPRot;
          bARInitialized = true;
        }

        GetOwner()->SetActorLocation(ARInitialLocation + UDPLoc);
        GetOwner()->SetActorRotation(ARInitialRotation + UDPRot);
      }
    }
  }
}

void UNavHintComponent::FindDestinations() {
  DestinationObjects.Empty();
  for (const FName &Tag : DestinationTags) {
    TArray<AActor *> FoundActors;
    UGameplayStatics::GetAllActorsWithTag(GetWorld(), Tag, FoundActors);
    DestinationObjects.Append(FoundActors);
  }
}

void UNavHintComponent::FindAllModifierVolumes() {
  UGameplayStatics::GetAllActorsOfClass(
      GetWorld(), ANavModifierVolume::StaticClass(), AllVolumes);
}

FString
UNavHintComponent::GetRelativeDirectionText(const FVector &Forward,
                                            const FVector &Right,
                                            const FVector &DirectionToTarget) {
  // 利用点积投影获取在 Forward 和 Right 轴上的坐标分量
  float ForwardDot = FVector::DotProduct(Forward, DirectionToTarget);
  float RightDot = FVector::DotProduct(Right, DirectionToTarget);

  // 计算弧度并转为角度 (-180 到 180 度)
  // Atan2(y, x) -> Atan2(Right分量, Forward分量)
  float AngleDegrees =
      FMath::RadiansToDegrees(FMath::Atan2(RightDot, ForwardDot));

  // 0度是正前，90度是正右，-90度是正左，180/-180是正后
  // 每个扇区范围是 45度，所以边界判断点是 22.5度, 67.5度 等

  if (AngleDegrees >= -22.5f && AngleDegrees < 22.5f) {
    return TEXT("向前");
  } else if (AngleDegrees >= 22.5f && AngleDegrees < 67.5f) {
    return TEXT("向右前方");
  } else if (AngleDegrees >= 67.5f && AngleDegrees < 112.5f) {
    return TEXT("向右转");
  } else if (AngleDegrees >= 112.5f && AngleDegrees < 157.5f) {
    return TEXT("向右后方");
  } else if (AngleDegrees >= 157.5f || AngleDegrees < -157.5f) {
    return TEXT("向后转");
  } else if (AngleDegrees >= -157.5f && AngleDegrees < -112.5f) {
    return TEXT("向左后方");
  } else if (AngleDegrees >= -112.5f && AngleDegrees < -67.5f) {
    return TEXT("向左转");
  } else // AngleDegrees >= -67.5f && AngleDegrees < -22.5f
  {
    return TEXT("向左前方");
  }
}

FString
UNavHintComponent::GetFullPathDescription(const TArray<FVector> &PathPoints,
                                          const FVector &CurrentForward,
                                          const FVector &CurrentRight) {
  if (PathPoints.Num() < 2)
    return TEXT("Arrived");

  FString FullDescription = "";
  FVector LastForward = CurrentForward;
  FVector LastRight = CurrentRight;
  FVector LastLoc = PathPoints[0];

  // 只展示前3段路径，防止屏幕文字过长
  int32 MaxSegments = FMath::Min(PathPoints.Num(), 4);

  for (int32 i = 1; i < MaxSegments; i++) {
    FVector SegmentDir = (PathPoints[i] - LastLoc).GetSafeNormal();
    float SegmentDist =
        FVector::Dist(LastLoc, PathPoints[i]) / 100.0f / DistanceScale;

    FString DirText =
        GetRelativeDirectionText(LastForward, LastRight, SegmentDir);

    FullDescription +=
        FString::Printf(TEXT("[%s %.1fm] "), *DirText, SegmentDist);

    if (i < MaxSegments - 1)
      FullDescription += TEXT("-> ");

    // 更新下一段参考系：以当前路段方向作为新的正前方
    LastForward = SegmentDir;
    LastRight = FVector::CrossProduct(FVector::UpVector, LastForward);
    LastLoc = PathPoints[i];
  }

  if (PathPoints.Num() > 4)
    FullDescription += TEXT("...");
  return FullDescription;
}

void UNavHintComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction *ThisTickFunction) {
  Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

  if (TrackingMode == ETrackingMode::NetworkSensor) {
    ProcessUDPSensorData();
  }

  AActor *Owner = GetOwner();
  if (!Owner)
    return;

  FVector PlayerLoc = Owner->GetActorLocation();
  FVector PlayerForward = Owner->GetActorForwardVector();
  FVector PlayerRight = Owner->GetActorRightVector();
  UNavigationSystemV1 *NavSys =
      FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());

  if (!NavSys)
    return;

  int32 MsgKey = 0;

  // --- 1. 处理所有目的地的完整路径提示 ---
  UNavCommandComponent *NavCmd =
      Owner->FindComponentByClass<UNavCommandComponent>();
  FName ActiveTarget = NavCmd ? NavCmd->GetCurrentTarget() : NAME_None;

  if (ActiveTarget != NAME_None) {
    TArray<AActor *> FoundActors;
    UGameplayStatics::GetAllActorsWithTag(GetWorld(), ActiveTarget,
                                          FoundActors);
    for (AActor *Dest : FoundActors) {
      if (!Dest)
        continue;

      FNavLocation ProjectedTarget;
      if (NavSys->ProjectPointToNavigation(Dest->GetActorLocation(),
                                           ProjectedTarget, FVector(200.f))) {
        UNavigationPath *NavPath = NavSys->FindPathToLocationSynchronously(
            GetWorld(), PlayerLoc, ProjectedTarget.Location);
        if (NavPath && NavPath->PathPoints.Num() >= 2) {
          FString DestName = (Dest->Tags.Num() > 0) ? Dest->Tags[0].ToString()
                                                    : Dest->GetName();
          FString PathDesc = GetFullPathDescription(NavPath->PathPoints,
                                                    PlayerForward, PlayerRight);

          FString FinalMsg =
              FString::Printf(TEXT("To [%s]: %s"), *DestName, *PathDesc);
          GEngine->AddOnScreenDebugMessage(MsgKey++, 0.1f, FColor::Yellow,
                                           FinalMsg);

          // 鐢诲嚭瀹屾暣璺緞绾?
          for (int32 i = 0; i < NavPath->PathPoints.Num() - 1; i++) {
            DrawDebugLine(GetWorld(), NavPath->PathPoints[i],
                          NavPath->PathPoints[i + 1], FColor::Yellow, false,
                          0.1f, 0, 1.0f);
          }

          // --- Separate Direction & Distance TTS Logic ---
          float TotalDist = 0.0f;
          for (int32 i = 0; i < NavPath->PathPoints.Num() - 1; i++) {
            TotalDist += FVector::Dist(NavPath->PathPoints[i],
                                       NavPath->PathPoints[i + 1]);
          }
          TotalDist = TotalDist / 100.0f / DistanceScale;

          if (TotalDist <= 0.5f) {
            if (LastNavTarget != NAME_None) {
              SendTTSMessage(TEXT("抵达终点"));
              LastNavTarget = NAME_None;
            }
          } else {
            FVector FirstWaypoint = NavPath->PathPoints[1];
            FVector DirToFirst = (FirstWaypoint - PlayerLoc).GetSafeNormal();
            float DistToFirst = FVector::Dist(PlayerLoc, FirstWaypoint) /
                                100.0f / DistanceScale;
            FString CurrentDirStr = GetRelativeDirectionText(
                PlayerForward, PlayerRight, DirToFirst);

            if (ActiveTarget != LastNavTarget) {
              // Just started a new target
              LastNavTarget = ActiveTarget;
              LastSpokenDirection = CurrentDirStr;
              LastSpokenDistance = DistToFirst;

              FString Msg = CurrentDirStr +
                            FString::Printf(TEXT("，%.1f米"), DistToFirst);
              SendTTSMessage(Msg);
            } else {
              // 方向改变时，或者移动超过 0.3 米时，同时播报方向和距离
              bool bDirChanged = (CurrentDirStr != LastSpokenDirection);
              bool bDistChanged =
                  (FMath::Abs(DistToFirst - LastSpokenDistance) >= 0.5f);

              if (bDirChanged || bDistChanged) {
                FString Msg = CurrentDirStr +
                              FString::Printf(TEXT("，%.1f"), DistToFirst);
                LastSpokenDirection = CurrentDirStr;
                LastSpokenDistance = DistToFirst;
                SendTTSMessage(Msg);
              }
            }
          }

          break; // Found the active target
        } else if (NavPath && NavPath->PathPoints.Num() < 2) {
          if (LastNavTarget !=
              NAME_None) { // Active target but no path = arrived
            SendTTSMessage(TEXT("抵达终点"));
            LastNavTarget = NAME_None;
          }
        }
      }
    }

    // --- 2. 处理周围障碍物感知（基于最短距离） ---
    int32 DisplayIndex = MsgKey + 1;
    for (AActor *Vol : AllVolumes) {
      if (!Vol)
        continue;

      // 获取 Volume 的根组件（通常是 BrushComponent）
      UPrimitiveComponent *Primitive =
          Cast<UPrimitiveComponent>(Vol->GetRootComponent());
      if (!Primitive)
        continue;

      // 获取玩家到该物体表面（包围盒）的最近点
      FVector ClosestPoint =
          Primitive->Bounds.GetBox().GetClosestPointTo(PlayerLoc);
      float ShortestDistance = FVector::Dist(PlayerLoc, ClosestPoint);

      if (ShortestDistance <= AwarenessRadius) {
        FVector DirToPoint = (ClosestPoint - PlayerLoc).GetSafeNormal();
        FString VolDir =
            GetRelativeDirectionText(PlayerForward, PlayerRight, DirToPoint);
        FString VolName =
            (Vol->Tags.Num() > 0) ? Vol->Tags[0].ToString() : Vol->GetName();

        float DistInMeters = ShortestDistance / 100.0f / DistanceScale;

        FString Info = FString::Printf(TEXT("Obstacle [%s]: %.1fm at %s"),
                                       *VolName, DistInMeters, *VolDir);
        GEngine->AddOnScreenDebugMessage(DisplayIndex++, 0.1f, FColor::Cyan,
                                         Info);

        if (DisplayIndex > 15)
          break; // 限制屏幕显示条数
      }
    }
  }
}