#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "NavigationPath.h"

#include "NavigationComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnNavigationStarted, FName,
                                            DestinationTag);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnNavigationArrived, FName,
                                             DestinationTag, bool, bSuccess);

class UNavigationSystemV1;

UCLASS(ClassGroup = (Navigation), meta = (BlueprintSpawnableComponent))
class PROJECT001_API UNavigationComponent : public UActorComponent {
  GENERATED_BODY()

public:
  UNavigationComponent();

protected:
  virtual void BeginPlay() override;

public:
  virtual void
  TickComponent(float DeltaTime, ELevelTick TickType,
                FActorComponentTickFunction *ThisTickFunction) override;

  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  bool NavigateTo(FName DestinationTag);

  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  void StopNavigation();

  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  TArray<FName> GetAvailableDestinations() const;

  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  FName GetCurrentTarget() const { return ActiveTarget; }

  UPROPERTY(BlueprintAssignable, Category = "Navigation|Command")
  FOnNavigationStarted OnNavigationStarted;

  UPROPERTY(BlueprintAssignable, Category = "Navigation|Command")
  FOnNavigationArrived OnNavigationArrived;

  UPROPERTY(EditAnywhere, Category = "Navigation")
  TArray<FName> DestinationTags;

  UPROPERTY(EditAnywhere, Category = "Navigation")
  float DistanceScale = 1.0f;

  UPROPERTY(EditAnywhere, Category = "Navigation")
  float ArrivalDistanceMeters = 0.4f;

  UPROPERTY(EditAnywhere, Category = "Navigation")
  float WaypointReachDistanceMeters = 0.8f;

  UPROPERTY(EditAnywhere, Category = "Navigation")
  float OffRouteAngleDegrees = 75.0f;

  UPROPERTY(EditAnywhere, Category = "Navigation")
  float OffRouteDistanceDeltaMeters = 0.75f;

  UPROPERTY(EditAnywhere, Category = "Navigation|Command")
  bool bShowDebugMessages = true;

private:
  void RefreshDestinationMap();
  void FinishNavigation(bool bSuccess);
  float ComputePathDistanceMeters(const TArray<FVector> &PathPoints) const;
  int32 FindCurrentSegmentIndex(const TArray<FVector> &PathPoints,
                                const FVector &PlayerLoc) const;
  float EstimatePromptDurationSeconds(const FString &Message) const;
  void EnqueueHighPriorityPrompt(const FString &Message);
  void EnqueueMediumPriorityPrompt(const FString &Message);
  void UpdateRealtimePrompt(const FString &Message);
  void ClearRealtimePrompt();
  void ClearNonCriticalPrompts();
  void ProcessPromptScheduler(float CurrentTime);
  FString GetNextDebugPrompt() const;
  void QueueOverviewFromPath(const TArray<FVector> &PathPoints,
                             const FVector &CurrentForward,
                             const FVector &CurrentRight);

  TMap<FName, FVector> DestinationMap;
  UNavigationSystemV1 *CachedNavSys = nullptr;

  FName ActiveTarget = NAME_None;
  FVector ActiveTargetLocation = FVector::ZeroVector;
  bool bIsNavigating = false;
  bool bHasAnnouncedOverview = false;
  bool bHasOffRouteWarning = false;

  FName LastNavTarget = NAME_None;
  float LastSpokenAngle = 0.0f;
  float LastSpokenDistance = -1.0f;
  float LastTTSTime = 0.0f;
  FVector LastPlayerLocation = FVector::ZeroVector;
  float LastDistanceToWaypoint = -1.0f;
  int32 LastProgressSegmentIndex = 0;

  static constexpr float MinSpeakAngleDelta = 35.0f;

  TArray<FString> HighPriorityPrompts;
  TArray<FString> MediumPriorityPrompts;
  FString CurrentRealtimePrompt;
  bool bHasRealtimePromptPending = false;
  float NextPromptDispatchTime = 0.0f;
};
