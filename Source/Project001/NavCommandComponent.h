// NavCommandComponent.h
// Receives navigation commands (e.g. from voice/MCP) and moves the owner
// character to the named tagged destination using UE5 navigation system. Uses
// NavMesh path-point following via AddMovementInput (no AIController needed).

#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "NavigationPath.h"

#include "NavCommandComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnNavigationStarted, FName,
                                            DestinationTag);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnNavigationArrived, FName,
                                             DestinationTag, bool, bSuccess);

UCLASS(ClassGroup = (Navigation), meta = (BlueprintSpawnableComponent))
class PROJECT001_API UNavCommandComponent : public UActorComponent {
  GENERATED_BODY()

public:
  UNavCommandComponent();

protected:
  virtual void BeginPlay() override;

public:
  virtual void
  TickComponent(float DeltaTime, ELevelTick TickType,
                FActorComponentTickFunction *ThisTickFunction) override;

  // ============================================================
  // Blueprint / C++ callable navigation command
  // ============================================================

  /** Navigate the owner to the actor tagged with DestinationTag. */
  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  bool NavigateTo(FName DestinationTag, bool bInAutoMove = false);

  /** Stop any current navigation. */
  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  void StopNavigation();

  /** Return a list of all known destination tag names. */
  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  TArray<FName> GetAvailableDestinations() const;

  /** Get the current active navigation target. */
  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  FName GetCurrentTarget() const { return CurrentTarget; }

  // ============================================================
  // Events
  // ============================================================

  UPROPERTY(BlueprintAssignable, Category = "Navigation|Command")
  FOnNavigationStarted OnNavigationStarted;

  UPROPERTY(BlueprintAssignable, Category = "Navigation|Command")
  FOnNavigationArrived OnNavigationArrived;

  // ============================================================
  // Configuration
  // ============================================================

  /** Tags of actors that act as navigation destinations. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation|Command")
  TArray<FName> DestinationTags;

  /** How close (cm) we need to get to the destination to count as arrived. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation|Command")
  float AcceptanceRadius = 150.0f;

  /** Text shown on screen during navigation (for audio TTS). */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation|Command")
  bool bShowDebugMessages = true;

private:
  // Scans all actors for matching tags and caches their locations
  void RefreshDestinationMap();

  // Called each tick to drive movement along the cached NavMesh path
  void FollowPath(float DeltaTime);

  // Called when the final waypoint is reached
  void ArriveAtDestination();

  // map from tag -> actor world location
  TMap<FName, FVector> DestinationMap;

  FName CurrentTarget;
  FVector CurrentTargetLocation;
  bool bIsNavigating = false;
  bool bAutoMove = false;

  // NavMesh path following state
  TArray<FVector> CachedPathPoints; // World-space waypoints from NavMesh
  int32 CurrentPathIndex = 0; // Which waypoint we are currently heading to

  // Time since last destination refresh (refresh every 2s)
  float TimeSinceRefresh = 0.0f;

  // TTS tracking
  FVector LastTTSLocation;
  float TimeSinceLastTTS = 0.0f;
  float TTSDistanceThreshold = 300.0f; // Speak every 3 meters
  float TTSTimeThreshold = 10.0f;      // or every 10 seconds
};
