#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "NavSettingsSaveGame.generated.h"

/**
 * Persistently stores navigation settings (e.g. server IP) across sessions.
 * Compatible with PC, Mobile, and Glasses.
 */
UCLASS()
class PROJECT001_API UNavSettingsSaveGame : public USaveGame {
  GENERATED_BODY()

public:
  UPROPERTY(VisibleAnywhere, Category = "Settings")
  FString LastConnectedIP;

  UPROPERTY(VisibleAnywhere, Category = "Settings")
  FString SaveSlotName = TEXT("NavSettingsSlot");

  UPROPERTY(VisibleAnywhere, Category = "Settings")
  uint32 UserIndex = 0;
};
