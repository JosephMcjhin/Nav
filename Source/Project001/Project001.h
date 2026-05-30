// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace Project001Console {
bool IsLocalNavTTSEnabled();
void SetLocalNavTTSEnabled(bool bEnabled);
void SpeakLocalNavText(const FString &Text);
}

