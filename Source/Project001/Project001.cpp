// Copyright Epic Games, Inc. All Rights Reserved.

#include "Project001.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "LocalBeepPlayer.h"
#include "NavigationComponent.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, Project001, "Project001");

namespace {
bool GLocalNavTTSEnabled = false;
FProcHandle GLocalNavTTSProc;

void StopLocalNavTTSProcess() {
  if (!GLocalNavTTSProc.IsValid()) return;

  if (FPlatformProcess::IsProcRunning(GLocalNavTTSProc)) {
    FPlatformProcess::TerminateProc(GLocalNavTTSProc, true);
  }
  FPlatformProcess::CloseProc(GLocalNavTTSProc);
  GLocalNavTTSProc = FProcHandle();
}

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

const FString &GetBuildTimestamp() {
  // __DATE__ 格式："Mmm dd yyyy"（如 "Jul 11 2026"，日 < 10 时前导是空格）
  // __TIME__ 格式："HH:MM:SS"
  // 这里只在第一次调用时解析一次，之后直接返回缓存引用。
  static const FString Cached = []() -> FString {
    const FString DateStr(__DATE__);  // "Mmm dd yyyy"
    const FString TimeStr(__TIME__);  // "HH:MM:SS"

    // 月份缩写 → 数字
    static const TMap<FString, FString> MonthMap = {
        {TEXT("Jan"), TEXT("01")}, {TEXT("Feb"), TEXT("02")},
        {TEXT("Mar"), TEXT("03")}, {TEXT("Apr"), TEXT("04")},
        {TEXT("May"), TEXT("05")}, {TEXT("Jun"), TEXT("06")},
        {TEXT("Jul"), TEXT("07")}, {TEXT("Aug"), TEXT("08")},
        {TEXT("Sep"), TEXT("09")}, {TEXT("Oct"), TEXT("10")},
        {TEXT("Nov"), TEXT("11")}, {TEXT("Dec"), TEXT("12")},
    };

    const FString MonthAbbr = DateStr.Left(3);
    const FString MonthNum = MonthMap.Contains(MonthAbbr) ? MonthMap[MonthAbbr]
                                                          : TEXT("??");
    // 日期：去前导空格，补 0
    FString DayStr = DateStr.Mid(4, 2).TrimStartAndEnd();
    if (DayStr.Len() == 1) DayStr = TEXT("0") + DayStr;
    const FString YearStr = DateStr.Right(4);

    return FString::Printf(TEXT("%s-%s-%s %s"),
                           *YearStr, *MonthNum, *DayStr, *TimeStr);
  }();
  return Cached;
}

void SpeakLocalNavText(const FString &Text) {
  StopLocalNavTTSProcess();
  if (!GLocalNavTTSEnabled || Text.IsEmpty()) {
    return;
  }

#if PLATFORM_WINDOWS
  FString Escaped = Text;
  Escaped.ReplaceInline(TEXT("'"), TEXT("''"));
  // 本地校验固定使用 2.5 倍语速；SAPI Rate 的范围是 -10..10。
  constexpr float LocalTTSSpeedMultiplier = 2.5f;
  const int32 Rate = FMath::Clamp(
      FMath::RoundToInt((LocalTTSSpeedMultiplier - 1.0f) * 5.0f), -10, 10);
  const FString Script = FString::Printf(
      TEXT("Add-Type -AssemblyName System.Speech;")
      TEXT("$s=New-Object System.Speech.Synthesis.SpeechSynthesizer;")
      TEXT("$s.Rate=%d;")
      TEXT("$s.Speak('%s');"),
      Rate, *Escaped);
  FString Command = Script;
  Command.ReplaceInline(TEXT("\""), TEXT("\\\""));
  const FString Args = FString::Printf(
      TEXT("-NoProfile -NonInteractive -WindowStyle Hidden -Command \"%s\""),
      *Command);
  GLocalNavTTSProc = FPlatformProcess::CreateProc(
      TEXT("powershell.exe"), *Args, false, true, true, nullptr, 0, nullptr,
      nullptr);
#else
  UE_LOG(LogTemp, Warning, TEXT("Local nav TTS unsupported on this platform."));
#endif
}

void PlayLocalBeep(bool bActive, int32 FreqHz, float Pan, float Volume,
                   float IntervalMs) {
  // 委托给 FLocalBeepPlayer 单例（见 LocalBeepPlayer.h/.cpp）。
  // 非 Windows 平台为空操作（见 LocalBeepPlayer.cpp）。
  FLocalBeepPlayer::Get().SetActive(bActive, FreqHz, Pan, Volume, IntervalMs);
}
} // namespace Project001Console
