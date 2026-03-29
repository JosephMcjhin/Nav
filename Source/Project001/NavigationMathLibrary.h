#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NavigationMathLibrary.generated.h"

UCLASS()
class PROJECT001_API UNavigationMathLibrary : public UBlueprintFunctionLibrary {
  GENERATED_BODY()

public:
  /** Returns a localized text string like "向左前方" based on the direction
   * vector. */
  UFUNCTION(BlueprintPure, Category = "Navigation|Math")
  static FString GetRelativeDirectionText(const FVector &Forward,
                                          const FVector &Right,
                                          const FVector &DirectionToTarget);

  /** Generates a readable string of the upcoming navigation path (e.g.
   * "[向前 3.0m] -> [向右 1.5m]") */
  UFUNCTION(BlueprintPure, Category = "Navigation|Math")
  static FString GetFullPathDescription(const TArray<FVector> &PathPoints,
                                        const FVector &CurrentForward,
                                        const FVector &CurrentRight,
                                        float DistanceScale);
};
