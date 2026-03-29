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
  float DistanceScale = 3.0f;

  UPROPERTY(EditAnywhere, Category = "Navigation|Command")
  bool bShowDebugMessages = true;

private:
  void RefreshDestinationMap();

  TMap<FName, FVector> DestinationMap;
  UNavigationSystemV1 *CachedNavSys = nullptr;

  FName ActiveTarget = NAME_None;
  FVector ActiveTargetLocation;
  bool bIsNavigating = false;

  FName LastNavTarget = NAME_None;
  FString LastSpokenDirection = TEXT("");
  float LastSpokenDistance = -1.0f;
  float LastTTSTime = 0.0f;
};
