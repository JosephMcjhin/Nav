// NavCommandComponent.cpp
// Implements runtime navigation command handling for the blind navigation
// project. Receives a tag name (e.g. "entrance") and moves the owner character
// to that tagged actor.
//
// Navigation strategy: Compute a NavMesh path to target, then drive the
// Character via AddMovementInput() each Tick – no AIController required.

#include "NavCommandComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "TTSQueueComponent.h"

// Helper function to play TTS
static void PlayTTSFeedback(AActor *OwnerActor, const FString &Message) {
  if (!OwnerActor)
    return;
  if (UTTSQueueComponent *TTSComp =
          OwnerActor->FindComponentByClass<UTTSQueueComponent>()) {
    TTSComp->SpeakIfFree(Message);
  }
}

UNavCommandComponent::UNavCommandComponent() {
  PrimaryComponentTick.bCanEverTick = true;

  // Default destination tags – user can add more in the editor or via code
  DestinationTags.Add(FName("Goal"));
}

void UNavCommandComponent::BeginPlay() {
  Super::BeginPlay();
  RefreshDestinationMap();
}

void UNavCommandComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction *ThisTickFunction) {
  Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

  // Periodically refresh destination map in case actors spawn/despawn
  TimeSinceRefresh += DeltaTime;
  if (TimeSinceRefresh >= 2.0f) {
    RefreshDestinationMap();
    TimeSinceRefresh = 0.0f;
  }

  if (bIsNavigating) {
    FollowPath(DeltaTime);
  }
}

// ============================================================
// Public API
// ============================================================

bool UNavCommandComponent::NavigateTo(FName DestinationTag, bool bInAutoMove) {
  if (!GetOwner()) {
    return false;
  }

  // Make sure our map is fresh
  RefreshDestinationMap();

  if (!DestinationMap.Contains(DestinationTag)) {
    FString AvailableStr;
    for (auto &Pair : DestinationMap) {
      AvailableStr += Pair.Key.ToString() + TEXT(", ");
    }

    if (bShowDebugMessages && GEngine) {
      GEngine->AddOnScreenDebugMessage(
          99, 5.0f, FColor::Red,
          FString::Printf(
              TEXT("[NavCmd] Unknown destination: '%s'. Available: %s"),
              *DestinationTag.ToString(), *AvailableStr));
    }
    UE_LOG(
        LogTemp, Warning,
        TEXT(
            "[NavCommandComponent] Unknown destination tag: %s. Available: %s"),
        *DestinationTag.ToString(), *AvailableStr);

    PlayTTSFeedback(GetOwner(), TEXT("未知的目的地"));
    return false;
  }

  CurrentTarget = DestinationTag;
  CurrentTargetLocation = DestinationMap[DestinationTag];

  // -------------------------------------------------------
  // Compute NavMesh path to target
  // -------------------------------------------------------
  ACharacter *CharOwner = Cast<ACharacter>(GetOwner());
  if (!CharOwner) {
    UE_LOG(LogTemp, Warning,
           TEXT("[NavCommandComponent] Owner is not a Character, cannot "
                "navigate."));
    return false;
  }

  UNavigationSystemV1 *NavSys =
      FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
  if (!NavSys) {
    UE_LOG(LogTemp, Warning,
           TEXT("[NavCommandComponent] No NavigationSystem found."));
    return false;
  }

  UNavigationPath *NavPath = NavSys->FindPathToLocationSynchronously(
      GetWorld(), CharOwner->GetActorLocation(), CurrentTargetLocation,
      CharOwner);

  if (!NavPath || NavPath->PathPoints.Num() < 2) {
    UE_LOG(LogTemp, Warning,
           TEXT("[NavCommandComponent] Could not compute NavMesh path to '%s'. "
                "Make sure a NavMeshBoundsVolume covers the scene."),
           *DestinationTag.ToString());

    if (bShowDebugMessages && GEngine) {
      GEngine->AddOnScreenDebugMessage(
          99, 5.0f, FColor::Red,
          FString::Printf(TEXT("[NavCmd] No path to '%s'. Check NavMesh!"),
                          *DestinationTag.ToString()));
    }
    PlayTTSFeedback(GetOwner(), TEXT("无法规划导航路径"));
    return false;
  }

  // Cache path points (skip index 0 — that's the start location)
  CachedPathPoints = NavPath->PathPoints;
  CurrentPathIndex = 1; // start heading toward waypoint 1
  bIsNavigating = true;
  bAutoMove = bInAutoMove;

  if (bShowDebugMessages && GEngine) {
    GEngine->AddOnScreenDebugMessage(
        98, 5.0f, FColor::Green,
        FString::Printf(TEXT("[NavCmd] Navigating to: %s (%d waypoints)"),
                        *DestinationTag.ToString(), CachedPathPoints.Num()));
  }
  UE_LOG(LogTemp, Log,
         TEXT("[NavCommandComponent] NavigateTo started: %s (%d waypoints)"),
         *DestinationTag.ToString(), CachedPathPoints.Num());

  // Initialize TTS tracking
  LastTTSLocation = CharOwner->GetActorLocation();
  TimeSinceLastTTS = 0.0f;

  PlayTTSFeedback(GetOwner(), TEXT("开始导航"));
  OnNavigationStarted.Broadcast(DestinationTag);
  return true;
}

void UNavCommandComponent::StopNavigation() {
  if (!bIsNavigating)
    return;

  // Stop the character by zeroing out its movement velocity
  ACharacter *CharOwner = Cast<ACharacter>(GetOwner());
  if (CharOwner && CharOwner->GetCharacterMovement()) {
    CharOwner->GetCharacterMovement()->StopMovementImmediately();
  }

  OnNavigationArrived.Broadcast(CurrentTarget, false);
  bIsNavigating = false;
  CurrentTarget = NAME_None;
  CachedPathPoints.Empty();
  CurrentPathIndex = 0;
}

TArray<FName> UNavCommandComponent::GetAvailableDestinations() const {
  TArray<FName> Keys;
  DestinationMap.GetKeys(Keys);
  return Keys;
}

// ============================================================
// Private helpers
// ============================================================

void UNavCommandComponent::RefreshDestinationMap() {
  DestinationMap.Empty();

  for (const FName &Tag : DestinationTags) {
    TArray<AActor *> FoundActors;
    UGameplayStatics::GetAllActorsWithTag(GetWorld(), Tag, FoundActors);

    for (AActor *Actor : FoundActors) {
      if (Actor) {
        // Project location onto navmesh
        UNavigationSystemV1 *NavSys =
            FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
        FNavLocation ProjectedLoc;
        FVector ActorLoc = Actor->GetActorLocation();

        if (NavSys && NavSys->ProjectPointToNavigation(ActorLoc, ProjectedLoc,
                                                       FVector(200.f))) {
          DestinationMap.Add(Tag, ProjectedLoc.Location);
        } else {
          // Fallback to raw actor location
          DestinationMap.Add(Tag, ActorLoc);
        }
        break; // use first match per tag
      }
    }
  }

  UE_LOG(LogTemp, Verbose,
         TEXT("[NavCommandComponent] Refreshed destinations. Count: %d"),
         DestinationMap.Num());
}

void UNavCommandComponent::FollowPath(float DeltaTime) {
  if (!bIsNavigating || CachedPathPoints.Num() == 0)
    return;

  ACharacter *CharOwner = Cast<ACharacter>(GetOwner());
  if (!CharOwner)
    return;

  // Safety: clamp index
  if (CurrentPathIndex >= CachedPathPoints.Num()) {
    // Reached end of path
    ArriveAtDestination();
    return;
  }

  FVector CurrentPos = CharOwner->GetActorLocation();

  // Handle continuous TTS feedback
  TimeSinceLastTTS += DeltaTime;
  float DistMovedSinceTTS = FVector::Dist(CurrentPos, LastTTSLocation);

  if (TimeSinceLastTTS >= TTSTimeThreshold ||
      DistMovedSinceTTS >= TTSDistanceThreshold) {
    TimeSinceLastTTS = 0.0f;
    LastTTSLocation = CurrentPos;
    PlayTTSFeedback(CharOwner, TEXT("正在导航中"));
  }

  FVector WaypointPos = CachedPathPoints[CurrentPathIndex];

  // Only compare XY distance; ignore Z to avoid vertical stalling
  FVector ToWaypoint = WaypointPos - CurrentPos;
  ToWaypoint.Z = 0.f;
  float DistXY = ToWaypoint.Size();

  // If within waypoint acceptance radius, advance to next waypoint
  const float WaypointRadius = 80.f; // cm
  if (DistXY <= WaypointRadius) {
    CurrentPathIndex++;
    if (CurrentPathIndex >= CachedPathPoints.Num()) {
      ArriveAtDestination();
      return;
    }
    // Re-evaluate next waypoint direction next frame
    return;
  }

  // (User requested: do not move the character during navigation)
}

void UNavCommandComponent::ArriveAtDestination() {
  // Stop movement
  ACharacter *CharOwner = Cast<ACharacter>(GetOwner());
  if (CharOwner && CharOwner->GetCharacterMovement()) {
    CharOwner->GetCharacterMovement()->StopMovementImmediately();
  }

  bIsNavigating = false;

  if (bShowDebugMessages && GEngine) {
    GEngine->AddOnScreenDebugMessage(
        97, 5.0f, FColor::Cyan,
        FString::Printf(TEXT("[NavCmd] Arrived at: %s"),
                        *CurrentTarget.ToString()));
  }
  UE_LOG(LogTemp, Log, TEXT("[NavCommandComponent] Arrived at: %s"),
         *CurrentTarget.ToString());

  PlayTTSFeedback(GetOwner(), TEXT("已到达目的地"));
  OnNavigationArrived.Broadcast(CurrentTarget, true);
  CurrentTarget = NAME_None;
  CachedPathPoints.Empty();
  CurrentPathIndex = 0;
}
