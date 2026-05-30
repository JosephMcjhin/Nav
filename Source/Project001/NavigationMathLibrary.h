#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NavigationMathLibrary.generated.h"

UCLASS()
class PROJECT001_API UNavigationMathLibrary : public UBlueprintFunctionLibrary {
  GENERATED_BODY()

public:
  UFUNCTION(BlueprintPure, Category = "Navigation|Math")
  static FString GetRelativeDirectionText(const FVector &Forward,
                                          const FVector &Right,
                                          const FVector &DirectionToTarget);

  UFUNCTION(BlueprintPure, Category = "Navigation|Math")
  static FString GetDestinationDisplayName(FName DestinationTag);

  UFUNCTION(BlueprintPure, Category = "Navigation|Math")
  static FString GetFullPathDescription(const TArray<FVector> &PathPoints,
                                        const FVector &CurrentForward,
                                        const FVector &CurrentRight,
                                        float DistanceScale);

  UFUNCTION(BlueprintPure, Category = "Navigation|Math")
  static FString BuildRouteOverview(const TArray<FVector> &PathPoints,
                                    const FVector &CurrentForward,
                                    const FVector &CurrentRight,
                                    float DistanceScale,
                                    const FString &DestinationName);
};
