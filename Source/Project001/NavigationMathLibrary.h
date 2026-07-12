#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NavigationMathLibrary.generated.h"

UCLASS()
class PROJECT001_API UNavigationMathLibrary : public UBlueprintFunctionLibrary {
  GENERATED_BODY()

public:
  UFUNCTION(BlueprintPure, Category = "Navigation|Math")
  static FString GetDestinationDisplayName(FName DestinationTag);

  /**
   * 计算目标相对玩家朝向的 8 方位文本（正前方/左前方/左侧/...）。
   *
   * @param PlayerForward 玩家前向（建议 2D 归一化）
   * @param PlayerRight   玩家右向（建议 2D 归一化）
   * @param ToTarget      玩家指向目标的向量（建议 2D 归一化）
   * @return              中文方位短语，例如 "正前方"、"右前方"、"左侧"、"右后方"
   */
  UFUNCTION(BlueprintPure, Category = "Navigation|Math")
  static FString GetRelativeDirectionPhrase(const FVector &PlayerForward,
                                            const FVector &PlayerRight,
                                            const FVector &ToTarget);
};
