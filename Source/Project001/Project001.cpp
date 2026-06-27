// Copyright Epic Games, Inc. All Rights Reserved.

#include "Project001.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "NavigationComponent.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, Project001, "Project001");

namespace {
bool GLocalNavTTSEnabled = false;

UNavigationComponent *FindNavigationComponent(UWorld *World) {
  if (!World) {
    return nullptr;
  }

  APlayerController *PC = World->GetFirstPlayerController();
  if (!PC || !PC->GetPawn()) {
    return nullptr;
  }

  return PC->GetPawn()->FindComponentByClass<UNavigationComponent>();
}

UWorld *FindGameWorld() {
  if (!GEngine) {
    return nullptr;
  }

  for (const FWorldContext &Context : GEngine->GetWorldContexts()) {
    if (Context.WorldType == EWorldType::PIE ||
        Context.WorldType == EWorldType::Game) {
      return Context.World();
    }
  }
  return nullptr;
}

void RunNavigateCommand(const TArray<FString> &Args) {
  if (Args.Num() < 1) {
    UE_LOG(LogTemp, Warning,
           TEXT("Usage: nav.go <DestinationTag>. Example: nav.go Door"));
    return;
  }

  UWorld *World = FindGameWorld();
  UNavigationComponent *NavComp = FindNavigationComponent(World);
  if (!NavComp) {
    UE_LOG(LogTemp, Warning, TEXT("nav.go failed: NavigationComponent not found."));
    return;
  }

  const FString DestinationInput = Args[0].TrimStartAndEnd();
  for (const FName &DestinationTag : NavComp->GetAvailableDestinations()) {
    if (DestinationTag.ToString().Equals(DestinationInput,
                                         ESearchCase::IgnoreCase)) {
      const bool bStarted = NavComp->NavigateTo(DestinationTag);
      UE_LOG(LogTemp, Log, TEXT("nav.go %s => %s"), *DestinationInput,
             bStarted ? TEXT("started") : TEXT("failed"));
      return;
    }
  }

  FString Available = TEXT("Unknown destination. Available: ");
  for (const FName &DestinationTag : NavComp->GetAvailableDestinations()) {
    Available += DestinationTag.ToString() + TEXT(" ");
  }
  UE_LOG(LogTemp, Warning, TEXT("%s"), *Available);
}

void RunTTSCommand(const TArray<FString> &Args) {
  if (Args.Num() < 1) {
    UE_LOG(LogTemp, Warning,
           TEXT("Usage: nav.tts <on|off|1|0>. Current=%s"),
           GLocalNavTTSEnabled ? TEXT("on") : TEXT("off"));
    return;
  }

  const FString Value = Args[0].ToLower();
  if (Value == TEXT("1") || Value == TEXT("on") || Value == TEXT("true")) {
    GLocalNavTTSEnabled = true;
  } else if (Value == TEXT("0") || Value == TEXT("off") ||
             Value == TEXT("false")) {
    GLocalNavTTSEnabled = false;
  } else {
    UE_LOG(LogTemp, Warning, TEXT("Invalid nav.tts value: %s"), *Args[0]);
    return;
  }

  UE_LOG(LogTemp, Log, TEXT("Local nav TTS %s"),
         GLocalNavTTSEnabled ? TEXT("enabled") : TEXT("disabled"));
}

void RunStopCommand(const TArray<FString> &Args) {
  UWorld *World = FindGameWorld();
  UNavigationComponent *NavComp = FindNavigationComponent(World);
  if (!NavComp) {
    UE_LOG(LogTemp, Warning, TEXT("nav.stop failed: NavigationComponent not found."));
    return;
  }

  NavComp->StopNavigation();
  UE_LOG(LogTemp, Log, TEXT("nav.stop executed."));
}

static IConsoleObject *NavGoCommand =
    IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("nav.go"),
        TEXT("Navigate to a destination tag. Example: nav.go Door"),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunNavigateCommand),
        ECVF_Default);

static IConsoleObject *NavTTSCommand =
    IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("nav.tts"),
        TEXT("Toggle UE local TTS. Example: nav.tts on"),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunTTSCommand),
        ECVF_Default);

static IConsoleObject *NavStopCommand =
    IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("nav.stop"),
        TEXT("Stop current navigation."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunStopCommand),
        ECVF_Default);
} // namespace

namespace Project001Console {
bool IsLocalNavTTSEnabled() { return GLocalNavTTSEnabled; }

void SetLocalNavTTSEnabled(bool bEnabled) { GLocalNavTTSEnabled = bEnabled; }

void SpeakLocalNavText(const FString &Text) {
  if (!GLocalNavTTSEnabled || Text.IsEmpty()) {
    return;
  }

#if PLATFORM_WINDOWS
  FString Escaped = Text;
  Escaped.ReplaceInline(TEXT("'"), TEXT("''"));
  const FString Script = FString::Printf(
      TEXT("Add-Type -AssemblyName System.Speech;")
      TEXT("$s=New-Object System.Speech.Synthesis.SpeechSynthesizer;")
      TEXT("$s.Rate=5;")
      TEXT("$s.Speak('%s');"),
      *Escaped);
  FString Command = Script;
  Command.ReplaceInline(TEXT("\""), TEXT("\\\""));
  const FString Args = FString::Printf(
      TEXT("-NoProfile -NonInteractive -WindowStyle Hidden -Command \"%s\""),
      *Command);
  FPlatformProcess::CreateProc(TEXT("powershell.exe"), *Args, false, true, true,
                               nullptr, 0, nullptr, nullptr);
#else
  UE_LOG(LogTemp, Warning, TEXT("Local nav TTS unsupported on this platform."));
#endif
}
} // namespace Project001Console
