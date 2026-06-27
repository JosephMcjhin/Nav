#include "NavigationComponent.h"

#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "NavigationMathLibrary.h"
#include "NavigationSystem.h"
#include "Project001.h"
#include "ServerConnectionComponent.h"

namespace {
void ShowNavDebugMessage(int32 Key, const FString &Message,
                         const FColor &Color = FColor::Cyan,
                         float Duration = 0.5f) {
  if (GEngine) {
    GEngine->AddOnScreenDebugMessage(Key, Duration, Color, Message);
  }
}

void SendNavPromptMessage(AActor *OwnerActor, const FString &Message) {
  if (!OwnerActor || Message.IsEmpty()) {
    return;
  }

  Project001Console::SpeakLocalNavText(Message);

  if (UServerConnectionComponent *ServerComp =
          OwnerActor->FindComponentByClass<UServerConnectionComponent>()) {
    const FString JsonStr =
        FString::Printf(TEXT("{\"type\":\"nav_prompt\",\"text\":\"%s\"}"),
                        *Message);
    ServerComp->SendString(JsonStr);
  }
}

FString BuildStepDistancePrompt(const FString &DirectionText,
                                float DistanceMeters) {
  const int32 StepCount =
      FMath::Max(1, FMath::RoundToInt(DistanceMeters * 3.0f));
  return DirectionText + FString::Printf(TEXT("%s%d%s"),
                                         UTF8_TO_TCHAR(u8"，约"),
                                         StepCount,
                                         UTF8_TO_TCHAR(u8"步"));
}
} // namespace

UNavigationComponent::UNavigationComponent() {
  PrimaryComponentTick.bCanEverTick = true;
  DestinationTags.Add(FName(TEXT("WorkStation")));
  DestinationTags.Add(FName(TEXT("ConferenceTable")));
  DestinationTags.Add(FName(TEXT("Sofa")));
  DestinationTags.Add(FName(TEXT("AirConditioner")));
  DestinationTags.Add(FName(TEXT("Door")));
}

void UNavigationComponent::BeginPlay() {
  Super::BeginPlay();
  CachedNavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
  RefreshDestinationMap();
  ShowNavDebugMessage(
      9200,
      FString::Printf(TEXT("Nav BeginPlay | NavSys:%s | Destinations:%d"),
                      CachedNavSys ? TEXT("OK") : TEXT("NULL"),
                      DestinationMap.Num()),
      CachedNavSys ? FColor::Green : FColor::Red, 5.0f);
}

void UNavigationComponent::RefreshDestinationMap() {
  DestinationMap.Empty();
  for (const FName &Tag : DestinationTags) {
    TArray<AActor *> FoundActors;
    UGameplayStatics::GetAllActorsWithTag(GetWorld(), Tag, FoundActors);

    for (AActor *Actor : FoundActors) {
      if (!Actor) {
        continue;
      }

      FNavLocation ProjectedLoc;
      const FVector ActorLoc = Actor->GetActorLocation();
      if (CachedNavSys && CachedNavSys->ProjectPointToNavigation(
                              ActorLoc, ProjectedLoc, FVector(200.f))) {
        DestinationMap.Add(Tag, ProjectedLoc.Location);
      } else {
        DestinationMap.Add(Tag, ActorLoc);
      }
      break;
    }
  }
}

float UNavigationComponent::EstimatePromptDurationSeconds(
    const FString &Message) const {
  float DurationSeconds = 0.2f;
  for (const TCHAR Char : Message) {
    switch (Char) {
    case 0xFF0C:
    case TEXT(','):
      DurationSeconds += 0.18f;
      break;
    case 0x3002:
    case TEXT('.'):
    case 0xFF01:
    case TEXT('!'):
    case 0xFF1F:
    case TEXT('?'):
      DurationSeconds += 0.35f;
      break;
    case TEXT(' '):
      DurationSeconds += 0.05f;
      break;
    default:
      DurationSeconds += 0.26f;
      break;
    }
  }
  return DurationSeconds;
}

void UNavigationComponent::EnqueueHighPriorityPrompt(const FString &Message) {
  if (!Message.IsEmpty()) {
    HighPriorityPrompts.Add(Message);
  }
}

void UNavigationComponent::EnqueueMediumPriorityPrompt(const FString &Message) {
  if (!Message.IsEmpty()) {
    MediumPriorityPrompts.Add(Message);
  }
}

void UNavigationComponent::UpdateRealtimePrompt(const FString &Message) {
  CurrentRealtimePrompt = Message;
  bHasRealtimePromptPending = !Message.IsEmpty();
}

void UNavigationComponent::ClearRealtimePrompt() {
  CurrentRealtimePrompt.Empty();
  bHasRealtimePromptPending = false;
}

void UNavigationComponent::ClearNonCriticalPrompts() {
  MediumPriorityPrompts.Empty();
  ClearRealtimePrompt();
}

FString UNavigationComponent::GetNextDebugPrompt() const {
  if (HighPriorityPrompts.Num() > 0) {
    return HighPriorityPrompts[0];
  }
  if (MediumPriorityPrompts.Num() > 0) {
    return MediumPriorityPrompts[0];
  }
  if (bHasRealtimePromptPending) {
    return CurrentRealtimePrompt;
  }
  return FString();
}

void UNavigationComponent::ProcessPromptScheduler(float CurrentTime) {
  if (CurrentTime < NextPromptDispatchTime) {
    return;
  }

  FString MessageToSend;
  if (HighPriorityPrompts.Num() > 0) {
    MessageToSend = HighPriorityPrompts[0];
    HighPriorityPrompts.RemoveAt(0);
  } else if (MediumPriorityPrompts.Num() > 0) {
    MessageToSend = MediumPriorityPrompts[0];
    MediumPriorityPrompts.RemoveAt(0);
  } else if (bHasRealtimePromptPending) {
    MessageToSend = CurrentRealtimePrompt;
    ClearRealtimePrompt();
  }

  if (MessageToSend.IsEmpty()) {
    return;
  }

  SendNavPromptMessage(GetOwner(), MessageToSend);
  NextPromptDispatchTime =
      CurrentTime + EstimatePromptDurationSeconds(MessageToSend) + 1.0f;
}

bool UNavigationComponent::NavigateTo(FName DestinationTag) {
  if (!GetOwner()) {
    return false;
  }

  if (!DestinationMap.Contains(DestinationTag)) {
    FString AvailableStr;
    for (const auto &Pair : DestinationMap) {
      AvailableStr += Pair.Key.ToString() + TEXT(", ");
    }
    UE_LOG(LogTemp, Warning,
           TEXT("[NavigationComponent] Unknown tag: %s. Available: %s"),
           *DestinationTag.ToString(), *AvailableStr);
    if (GEngine) {
      GEngine->AddOnScreenDebugMessage(
          9103, 6.0f, FColor::Red,
          FString::Printf(TEXT("Unknown nav tag: %s | Available: %s"),
                          *DestinationTag.ToString(), *AvailableStr));
    }
    EnqueueHighPriorityPrompt(UTF8_TO_TCHAR(u8"未知的目的地"));
    return false;
  }

  ActiveTarget = DestinationTag;
  ActiveTargetLocation = DestinationMap[DestinationTag];
  ShowNavDebugMessage(
      9103,
      FString::Printf(TEXT("Navigation target set: %s | Loc:(%.0f, %.0f, %.0f)"),
                      *DestinationTag.ToString(), ActiveTargetLocation.X,
                      ActiveTargetLocation.Y, ActiveTargetLocation.Z),
      FColor::Green, 5.0f);
  bIsNavigating = true;
  bHasAnnouncedOverview = false;
  bHasOffRouteWarning = false;
  LastProgressSegmentIndex = 0;
  LastDistanceToWaypoint = -1.0f;

  LastNavTarget = NAME_None;
  LastSpokenAngle = 0.0f;
  LastSpokenDistance = -1.0f;
  LastTTSTime = GetWorld()->GetTimeSeconds();
  LastPlayerLocation = GetOwner()->GetActorLocation();

  HighPriorityPrompts.Empty();
  ClearNonCriticalPrompts();
  EnqueueHighPriorityPrompt(
      UNavigationMathLibrary::GetDestinationDisplayName(ActiveTarget) +
      UTF8_TO_TCHAR(u8"导航开始。"));
  OnNavigationStarted.Broadcast(DestinationTag);
  return true;
}

void UNavigationComponent::StopNavigation() {
  ClearNonCriticalPrompts();
  EnqueueHighPriorityPrompt(UTF8_TO_TCHAR(u8"导航结束"));
  FinishNavigation(false);
}

TArray<FName> UNavigationComponent::GetAvailableDestinations() const {
  TArray<FName> Keys;
  DestinationMap.GetKeys(Keys);
  return Keys;
}

void UNavigationComponent::FinishNavigation(bool bSuccess) {
  if (!bIsNavigating && ActiveTarget == NAME_None) {
    return;
  }

  const FName FinishedTarget = ActiveTarget;
  bIsNavigating = false;
  bHasAnnouncedOverview = false;
  bHasOffRouteWarning = false;
  ActiveTarget = NAME_None;
  LastNavTarget = NAME_None;
  LastProgressSegmentIndex = 0;
  LastDistanceToWaypoint = -1.0f;
  ClearNonCriticalPrompts();
  OnNavigationArrived.Broadcast(FinishedTarget, bSuccess);
}

float UNavigationComponent::ComputePathDistanceMeters(
    const TArray<FVector> &PathPoints) const {
  float TotalDistance = 0.0f;
  for (int32 i = 0; i < PathPoints.Num() - 1; ++i) {
    TotalDistance += FVector::Dist(PathPoints[i], PathPoints[i + 1]);
  }
  return TotalDistance / 100.0f / DistanceScale;
}

int32 UNavigationComponent::FindCurrentSegmentIndex(
    const TArray<FVector> &PathPoints, const FVector &PlayerLoc) const {
  if (PathPoints.Num() < 2) {
    return 0;
  }

  int32 BestSegmentIndex = 0;
  float BestDistance = TNumericLimits<float>::Max();
  for (int32 i = 0; i < PathPoints.Num() - 1; ++i) {
    const FVector ClosestPoint =
        FMath::ClosestPointOnSegment(PlayerLoc, PathPoints[i], PathPoints[i + 1]);
    const float SegmentDistance = FVector::Dist2D(PlayerLoc, ClosestPoint);
    if (SegmentDistance < BestDistance) {
      BestDistance = SegmentDistance;
      BestSegmentIndex = i;
    }
  }
  return BestSegmentIndex;
}

void UNavigationComponent::QueueOverviewFromPath(
    const TArray<FVector> &PathPoints, const FVector &CurrentForward,
    const FVector &CurrentRight) {
  EnqueueMediumPriorityPrompt(UNavigationMathLibrary::BuildRouteOverview(
      PathPoints, CurrentForward, CurrentRight, DistanceScale,
      UNavigationMathLibrary::GetDestinationDisplayName(ActiveTarget)));
}

void UNavigationComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction *ThisTickFunction) {
  Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

  AActor *Owner = GetOwner();
  if (!Owner) {
    ShowNavDebugMessage(9201, TEXT("Nav Tick: Owner NULL"), FColor::Red);
    return;
  }

  const float CurrentTime = GetWorld()->GetTimeSeconds();

  if (!bIsNavigating || !CachedNavSys || ActiveTarget == NAME_None) {
    ShowNavDebugMessage(
        9201,
        FString::Printf(TEXT("Nav idle | navigating:%d navsys:%s target:%s"),
                        bIsNavigating ? 1 : 0,
                        CachedNavSys ? TEXT("OK") : TEXT("NULL"),
                        *ActiveTarget.ToString()),
        (!CachedNavSys && bIsNavigating) ? FColor::Red : FColor::Silver);
    ProcessPromptScheduler(CurrentTime);
    return;
  }

  const FVector PlayerLoc = Owner->GetActorLocation();
  const FVector PlayerForward = Owner->GetActorForwardVector();
  const FVector PlayerRight = Owner->GetActorRightVector();
  const float DirectDistanceToTarget =
      FVector::Dist2D(PlayerLoc, ActiveTargetLocation) / 100.0f / DistanceScale;

  FNavLocation ProjectedTarget;
  if (!CachedNavSys->ProjectPointToNavigation(ActiveTargetLocation, ProjectedTarget,
                                              FVector(200.f))) {
    ShowNavDebugMessage(
        9202,
        FString::Printf(TEXT("Nav target projection FAILED | %s Loc:(%.0f, %.0f, %.0f)"),
                        *ActiveTarget.ToString(), ActiveTargetLocation.X,
                        ActiveTargetLocation.Y, ActiveTargetLocation.Z),
        FColor::Red, 1.0f);
    LastPlayerLocation = PlayerLoc;
    ProcessPromptScheduler(CurrentTime);
    return;
  }

  UNavigationPath *NavPath = CachedNavSys->FindPathToLocationSynchronously(
      GetWorld(), PlayerLoc, ProjectedTarget.Location);
  if (!NavPath || NavPath->PathPoints.Num() == 0) {
    ShowNavDebugMessage(
        9202,
        FString::Printf(TEXT("Nav path FAILED | path:%s points:%d player:(%.0f, %.0f) target:(%.0f, %.0f)"),
                        NavPath ? TEXT("OK") : TEXT("NULL"),
                        NavPath ? NavPath->PathPoints.Num() : 0, PlayerLoc.X,
                        PlayerLoc.Y, ProjectedTarget.Location.X,
                        ProjectedTarget.Location.Y),
        FColor::Red, 1.0f);
    LastPlayerLocation = PlayerLoc;
    ProcessPromptScheduler(CurrentTime);
    return;
  }

  ShowNavDebugMessage(
      9202,
      FString::Printf(TEXT("Nav path OK | target:%s points:%d direct:%.2fm"),
                      *ActiveTarget.ToString(), NavPath->PathPoints.Num(),
                      DirectDistanceToTarget),
      FColor::Green);

  if (DirectDistanceToTarget <= ArrivalDistanceMeters) {
    ClearNonCriticalPrompts();
    EnqueueHighPriorityPrompt(
        UNavigationMathLibrary::GetDestinationDisplayName(ActiveTarget) +
        UTF8_TO_TCHAR(u8"到了"));
    FinishNavigation(true);
    ProcessPromptScheduler(CurrentTime);
    LastPlayerLocation = PlayerLoc;
    return;
  }

  if (NavPath->PathPoints.Num() < 2) {
    ShowNavDebugMessage(
        9203,
        FString::Printf(TEXT("Nav path has <2 points | points:%d direct:%.2fm"),
                        NavPath->PathPoints.Num(), DirectDistanceToTarget),
        FColor::Orange, 1.0f);
    if (DirectDistanceToTarget <= ArrivalDistanceMeters) {
      ClearNonCriticalPrompts();
      EnqueueHighPriorityPrompt(
          UNavigationMathLibrary::GetDestinationDisplayName(ActiveTarget) +
          UTF8_TO_TCHAR(u8"到了"));
      FinishNavigation(true);
    }
    ProcessPromptScheduler(CurrentTime);
    LastPlayerLocation = PlayerLoc;
    return;
  }

  for (int32 i = 0; i < NavPath->PathPoints.Num() - 1; ++i) {
    DrawDebugLine(GetWorld(), NavPath->PathPoints[i], NavPath->PathPoints[i + 1],
                  FColor::Yellow, false, 0.1f, 0, 1.0f);
  }

  if (!bHasAnnouncedOverview) {
    QueueOverviewFromPath(NavPath->PathPoints, PlayerForward, PlayerRight);
    bHasAnnouncedOverview = true;
  }

  const int32 CurrentSegmentIndex =
      FindCurrentSegmentIndex(NavPath->PathPoints, PlayerLoc);
  if (CurrentSegmentIndex > LastProgressSegmentIndex) {
    LastProgressSegmentIndex = CurrentSegmentIndex;
    QueueOverviewFromPath(NavPath->PathPoints, PlayerForward, PlayerRight);
  }

  const FVector FirstWaypoint = NavPath->PathPoints[1];
  const FVector DirToFirst = (FirstWaypoint - PlayerLoc).GetSafeNormal();
  const float DistToFirst =
      FVector::Dist(PlayerLoc, FirstWaypoint) / 100.0f / DistanceScale;
  const float FirstPathSegmentDistance =
      FVector::Dist(NavPath->PathPoints[0], NavPath->PathPoints[1]) / 100.0f /
      DistanceScale;
  const float ForwardDot = FVector::DotProduct(PlayerForward, DirToFirst);
  const float RightDot = FVector::DotProduct(PlayerRight, DirToFirst);
  const float CurrentAngle =
      FMath::RadiansToDegrees(FMath::Atan2(RightDot, ForwardDot));
  const FString CurrentDirStr = UNavigationMathLibrary::GetRelativeDirectionText(
      PlayerForward, PlayerRight, DirToFirst);
  const FString RealtimePrompt =
      BuildStepDistancePrompt(CurrentDirStr, FirstPathSegmentDistance);
  CurrentRealtimePrompt = RealtimePrompt;
  ShowNavDebugMessage(
      9203,
      FString::Printf(TEXT("Nav next | distFirst:%.2fm seg:%.2fm angle:%.0f prompt:%s"),
                      DistToFirst, FirstPathSegmentDistance, CurrentAngle,
                      *RealtimePrompt),
      FColor::Yellow);

  if (DistToFirst <= WaypointReachDistanceMeters &&
      LastProgressSegmentIndex == CurrentSegmentIndex &&
      NavPath->PathPoints.Num() > 2) {
    LastProgressSegmentIndex = FMath::Min(CurrentSegmentIndex + 1,
                                          NavPath->PathPoints.Num() - 2);
    QueueOverviewFromPath(NavPath->PathPoints, PlayerForward, PlayerRight);
  }

  const FVector MovementDelta = PlayerLoc - LastPlayerLocation;
  const float MovedMeters = MovementDelta.Size2D() / 100.0f / DistanceScale;
  if (MovedMeters > 0.2f) {
    const FVector MoveDir = MovementDelta.GetSafeNormal();
    const float MoveDot = FVector::DotProduct(MoveDir, DirToFirst);
    const float MoveAngle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
        MoveDot, -1.0f, 1.0f)));
    const bool bMovingAway = (LastDistanceToWaypoint > 0.0f &&
                              DistToFirst - LastDistanceToWaypoint >=
                                  OffRouteDistanceDeltaMeters);
    if ((MoveAngle >= OffRouteAngleDegrees || bMovingAway) &&
        !bHasOffRouteWarning) {
      EnqueueMediumPriorityPrompt(
          UTF8_TO_TCHAR(u8"已偏离路线，正在重新规划路线"));
      bHasOffRouteWarning = true;
    } else if (MoveAngle < OffRouteAngleDegrees * 0.7f && !bMovingAway) {
      bHasOffRouteWarning = false;
    }
  }
  LastDistanceToWaypoint = DistToFirst;

  if (ActiveTarget != LastNavTarget) {
    LastNavTarget = ActiveTarget;
    LastSpokenAngle = CurrentAngle;
    LastSpokenDistance = FirstPathSegmentDistance;
    LastTTSTime = CurrentTime;
  } else {
    const float AngleDelta = FMath::Abs(
        FMath::FindDeltaAngleDegrees(LastSpokenAngle, CurrentAngle));
    const bool bDirChanged = (AngleDelta >= MinSpeakAngleDelta);
    const bool bDistChanged =
        (FMath::Abs(FirstPathSegmentDistance - LastSpokenDistance) >= 0.5f);
    const bool bTimerFired = (CurrentTime - LastTTSTime >= 5.0f);

    if (bDirChanged || bDistChanged || bTimerFired) {
      bHasRealtimePromptPending = true;
      LastSpokenAngle = CurrentAngle;
      LastSpokenDistance = FirstPathSegmentDistance;
      LastTTSTime = CurrentTime;
    }
  }

  if (bShowDebugMessages && GEngine) {
    FString DebugPrompt = GetNextDebugPrompt();
    if (DebugPrompt.IsEmpty()) {
      DebugPrompt = RealtimePrompt;
    }
    GEngine->AddOnScreenDebugMessage(
        0, 0.1f, FColor::Yellow,
        FString::Printf(TEXT("To [%s]: %s"), *ActiveTarget.ToString(),
                        *DebugPrompt));
  }

  ProcessPromptScheduler(CurrentTime);
  LastPlayerLocation = PlayerLoc;
}
