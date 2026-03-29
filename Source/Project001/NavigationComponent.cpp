#include "NavigationComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "GameFramework/Character.h"
#include "Kismet/GameplayStatics.h"
#include "NavigationMathLibrary.h"
#include "NavigationSystem.h"
#include "VoiceStreamComponent.h"

static void SendTTSMessage(AActor *OwnerActor, const FString &Message) {
  if (!OwnerActor || Message.IsEmpty())
    return;
  if (UVoiceStreamComponent *VoiceComp =
          OwnerActor->FindComponentByClass<UVoiceStreamComponent>()) {
    VoiceComp->RequestTTS(Message);
  }
}

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
}

void UNavigationComponent::RefreshDestinationMap() {
  DestinationMap.Empty();
  for (const FName &Tag : DestinationTags) {
    TArray<AActor *> FoundActors;
    UGameplayStatics::GetAllActorsWithTag(GetWorld(), Tag, FoundActors);

    for (AActor *Actor : FoundActors) {
      if (Actor) {
        FNavLocation ProjectedLoc;
        FVector ActorLoc = Actor->GetActorLocation();

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
}

bool UNavigationComponent::NavigateTo(FName DestinationTag) {
  if (!GetOwner())
    return false;

  // 如果目标确实变了或者地图不存在，可选再次调用一次 RefreshDestinationMap()
  // 但为了遵守"不需要一直刷图"的要求，我们只用缓存的 DestinationMap

  if (!DestinationMap.Contains(DestinationTag)) {
    FString AvailableStr;
    for (auto &Pair : DestinationMap) {
      AvailableStr += Pair.Key.ToString() + TEXT(", ");
    }
    UE_LOG(LogTemp, Warning,
           TEXT("[NavigationComponent] Unknown tag: %s. Available: %s"),
           *DestinationTag.ToString(), *AvailableStr);
    SendTTSMessage(GetOwner(), UTF8_TO_TCHAR(u8"未知的目的地"));
    return false;
  }

  ActiveTarget = DestinationTag;
  ActiveTargetLocation = DestinationMap[DestinationTag];
  bIsNavigating = true;

  LastNavTarget = NAME_None;
  LastSpokenDirection = TEXT("");
  LastSpokenDistance = -1.0f;
  LastTTSTime = GetWorld()->GetTimeSeconds();

  SendTTSMessage(GetOwner(), UTF8_TO_TCHAR(u8"开始导航"));
  OnNavigationStarted.Broadcast(DestinationTag);
  return true;
}

void UNavigationComponent::StopNavigation() {
  if (!bIsNavigating)
    return;

  bIsNavigating = false;
  OnNavigationArrived.Broadcast(ActiveTarget, false);
  ActiveTarget = NAME_None;
}

TArray<FName> UNavigationComponent::GetAvailableDestinations() const {
  TArray<FName> Keys;
  DestinationMap.GetKeys(Keys);
  return Keys;
}

void UNavigationComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction *ThisTickFunction) {
  Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

  AActor *Owner = GetOwner();
  if (!Owner || !bIsNavigating || !CachedNavSys || ActiveTarget == NAME_None)
    return;

  float CurrentTime = GetWorld()->GetTimeSeconds();
  FVector PlayerLoc = Owner->GetActorLocation();
  FVector PlayerForward = Owner->GetActorForwardVector();
  FVector PlayerRight = Owner->GetActorRightVector();

  FNavLocation ProjectedTarget;
  if (CachedNavSys->ProjectPointToNavigation(ActiveTargetLocation,
                                             ProjectedTarget, FVector(200.f))) {
    UNavigationPath *NavPath = CachedNavSys->FindPathToLocationSynchronously(
        GetWorld(), PlayerLoc, ProjectedTarget.Location);

    if (NavPath && NavPath->PathPoints.Num() >= 2) {
      FString DestName = ActiveTarget.ToString();
      FString PathDesc = UNavigationMathLibrary::GetFullPathDescription(
          NavPath->PathPoints, PlayerForward, PlayerRight, DistanceScale);

      if (bShowDebugMessages && GEngine) {
        GEngine->AddOnScreenDebugMessage(
            0, 0.1f, FColor::Yellow,
            FString::Printf(TEXT("To [%s]: %s"), *DestName, *PathDesc));
      }

      for (int32 i = 0; i < NavPath->PathPoints.Num() - 1; i++) {
        DrawDebugLine(GetWorld(), NavPath->PathPoints[i],
                      NavPath->PathPoints[i + 1], FColor::Yellow, false, 0.1f,
                      0, 1.0f);
      }

      float TotalDist = 0.0f;
      for (int32 i = 0; i < NavPath->PathPoints.Num() - 1; i++) {
        TotalDist +=
            FVector::Dist(NavPath->PathPoints[i], NavPath->PathPoints[i + 1]);
      }
      TotalDist = TotalDist / 100.0f / DistanceScale;

      if (TotalDist <= 0.1f) {
        if (LastNavTarget != NAME_None) {
          SendTTSMessage(Owner, UTF8_TO_TCHAR(u8"已到达目的地"));
          LastNavTarget = NAME_None;
          OnNavigationArrived.Broadcast(ActiveTarget, true);
          bIsNavigating = false;
          ActiveTarget = NAME_None;
        }
      } else {
        FVector FirstWaypoint = NavPath->PathPoints[1];
        FVector DirToFirst = (FirstWaypoint - PlayerLoc).GetSafeNormal();
        float DistToFirst =
            FVector::Dist(PlayerLoc, FirstWaypoint) / 100.0f / DistanceScale;
        FString CurrentDirStr =
            UNavigationMathLibrary::GetRelativeDirectionText(
                PlayerForward, PlayerRight, DirToFirst);

        if (ActiveTarget != LastNavTarget) {
          LastNavTarget = ActiveTarget;
          LastSpokenDirection = CurrentDirStr;
          LastSpokenDistance = DistToFirst;
          LastTTSTime = CurrentTime;

          FString Msg = CurrentDirStr +
                        FString::Printf(TEXT("%s%.1f%s"), UTF8_TO_TCHAR(u8"，"),
                                        DistToFirst, UTF8_TO_TCHAR(u8"米"));
          SendTTSMessage(Owner, Msg);
        } else {
          bool bDirChanged = (CurrentDirStr != LastSpokenDirection);
          bool bDistChanged =
              (FMath::Abs(DistToFirst - LastSpokenDistance) >= 0.5f);

          if (CurrentTime - LastTTSTime >= 5.0f) {
            FString Msg =
                CurrentDirStr +
                FString::Printf(TEXT("%s%.1f%s"), UTF8_TO_TCHAR(u8"，"),
                                DistToFirst, UTF8_TO_TCHAR(u8"米"));
            LastSpokenDirection = CurrentDirStr;
            LastSpokenDistance = DistToFirst;
            LastTTSTime = CurrentTime;
            SendTTSMessage(Owner, Msg);
          }
        }
      }
    } else if (NavPath && NavPath->PathPoints.Num() < 2) {
      if (LastNavTarget != NAME_None) {
        SendTTSMessage(Owner, UTF8_TO_TCHAR(u8"已到达目的地"));
        LastNavTarget = NAME_None;
        OnNavigationArrived.Broadcast(ActiveTarget, true);
        bIsNavigating = false;
        ActiveTarget = NAME_None;
      }
    }
  }
}
