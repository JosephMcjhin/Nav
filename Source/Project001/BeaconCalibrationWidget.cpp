// Fill out your copyright notice in the Description page of Project Settings.
#pragma execution_character_set("utf-8")
#include "BeaconCalibrationWidget.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Kismet/GameplayStatics.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "VoiceStreamComponent.h"

void UBeaconCalibrationWidget::NativeConstruct() {
  Super::NativeConstruct();

  if (BtnCapture1)
    BtnCapture1->OnClicked.AddDynamic(this,
                                      &UBeaconCalibrationWidget::OnCapture1);
  if (BtnCapture2)
    BtnCapture2->OnClicked.AddDynamic(this,
                                      &UBeaconCalibrationWidget::OnCapture2);

  if (BtnSolve)
    BtnSolve->OnClicked.AddDynamic(this, &UBeaconCalibrationWidget::OnSolve);
  if (BtnReset)
    BtnReset->OnClicked.AddDynamic(this, &UBeaconCalibrationWidget::OnReset);

  if (BtnToggleToolbar)
    BtnToggleToolbar->OnClicked.AddDynamic(
        this, &UBeaconCalibrationWidget::OnToggleToolbar);

  if (BtnSpeak) {
    BtnSpeak->OnPressed.AddDynamic(this,
                                   &UBeaconCalibrationWidget::OnSpeakPressed);
    BtnSpeak->OnReleased.AddDynamic(this,
                                    &UBeaconCalibrationWidget::OnSpeakReleased);
  }

  if (BtnConnect) {
    BtnConnect->OnClicked.AddDynamic(
        this, &UBeaconCalibrationWidget::OnConnectClicked);
  }

  SwitchUIState(0);

  RefreshUI();
}

void UBeaconCalibrationWidget::NativeTick(const FGeometry &MyGeometry,
                                          float InDeltaTime) {
  Super::NativeTick(MyGeometry, InDeltaTime);

  // Custom joystick input logic removed to avoid redundancy with the
  // built-in joystick handling implemented in UWBTargetComponent.
}

void UBeaconCalibrationWidget::SetConnectionComponent(
    UServerConnectionComponent *InComponent) {
  ConnComp = InComponent;
}

void UBeaconCalibrationWidget::CapturePoint(int32 PointIndex) {
  if (!ConnComp) {
    SetStatus(TEXT("Error: ConnectionComponent not bound"));
    return;
  }

  // Ask character to get its location and send via ConnComp
  ACharacter *Char = Cast<ACharacter>(ConnComp->GetOwner());
  if (!Char)
    return;

  FVector Loc = Char->GetActorLocation();

  TSharedPtr<FJsonObject> JsonObj = MakeShareable(new FJsonObject());
  JsonObj->SetNumberField(TEXT("x"), Loc.X);
  JsonObj->SetNumberField(TEXT("y"), Loc.Y);
  // Pass 0-based index so server overwrites the correct calibration slot
  JsonObj->SetNumberField(TEXT("index"), PointIndex - 1);

  FString OutputString;
  TSharedRef<TJsonWriter<>> Writer =
      TJsonWriterFactory<>::Create(&OutputString);
  FJsonSerializer::Serialize(JsonObj.ToSharedRef(), Writer);

  HttpPost(TEXT("/api/calibrate/point"), OutputString, nullptr);

  // Mark the point as captured in the bitmask
  CapturedFlags |= (1 << (PointIndex - 1));
  SetStatus(FString::Printf(TEXT("Point %d Captured"), PointIndex));
  RefreshUI();
}

void UBeaconCalibrationWidget::SolveCalibration() {
  if (!ConnComp) {
    SetStatus(TEXT("Error: ConnectionComponent not bound"));
    return;
  }

  SetStatus(TEXT("Calculating beacon positions..."));
  HttpPost(TEXT("/api/calibrate/solve"), TEXT("{}"),
           [this](const FString &ResponseBody) {
             // Calibration successful, switch to Speak state
             SwitchUIState(2);
           });
}

void UBeaconCalibrationWidget::ResetCalibration() {
  CapturedFlags = 0;
  HttpPost(TEXT("/api/calibrate/clear"), TEXT("{}"), nullptr);
  SetStatus(TEXT("Reset. Please capture 2 points again."));
  RefreshUI();
}

void UBeaconCalibrationWidget::JoystickInput(float AxisX, float AxisY) {
  JoystickX = AxisX;
  JoystickY = AxisY;
}

// ─── Private helpers ────────────────────────────────────────────────────────

void UBeaconCalibrationWidget::RefreshUI() {
  // Update point labels
  auto UpdateLabel = [&](UTextBlock *Txt, int32 Bit, const FString &Name) {
    if (!Txt)
      return;
    const bool bDone = (CapturedFlags & Bit) != 0;
    Txt->SetText(
        FText::FromString(bDone ? Name + TEXT(" Done") : Name + TEXT(" Wait")));
    Txt->SetColorAndOpacity(bDone ? FSlateColor(FLinearColor::Green)
                                  : FSlateColor(FLinearColor::Black));
  };
  UpdateLabel(TxtPoint1, 1, TEXT("Point 1"));
  UpdateLabel(TxtPoint2, 2, TEXT("Point 2"));

  // Enable Solve only when all 2 points are captured
  if (BtnSolve)
    BtnSolve->SetIsEnabled((CapturedFlags & 0b11) == 0b11);
}

void UBeaconCalibrationWidget::SetStatus(const FString &Msg) {
  if (TxtStatus)
    TxtStatus->SetText(FText::FromString(Msg));

  UE_LOG(LogTemp, Log, TEXT("[BeaconCalib] %s"), *Msg);
}

void UBeaconCalibrationWidget::SwitchUIState(int32 State) {
  // State 0: Connect, State 1: Calibration, State 2: Voice
  ESlateVisibility V0 =
      (State == 0) ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
  ESlateVisibility V1 =
      (State == 1) ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
  ESlateVisibility V2 =
      (State == 2) ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;

  if (TxtServerIP)
    TxtServerIP->SetVisibility(V0);
  if (BtnConnect)
    BtnConnect->SetVisibility(V0);

  if (BtnCapture1)
    BtnCapture1->SetVisibility(V1);
  if (BtnCapture2)
    BtnCapture2->SetVisibility(V1);
  if (BtnSolve)
    BtnSolve->SetVisibility(V1);
  if (BtnReset)
    BtnReset->SetVisibility(V1);
  if (TxtPoint1)
    TxtPoint1->SetVisibility(V1);
  if (TxtPoint2)
    TxtPoint2->SetVisibility(V1);
  if (TxtStatus)
    TxtStatus->SetVisibility(V1);

  if (BtnSpeak)
    BtnSpeak->SetVisibility(V2);
}

void UBeaconCalibrationWidget::HttpPost(
    const FString &RelPath, const FString &JsonBody,
    TFunction<void(const FString &)> OnResponse) {
  if (!ConnComp)
    return;

  FHttpModule *Http = &FHttpModule::Get();
  auto Request = Http->CreateRequest();
  Request->SetURL(ConnComp->ServerBaseURL + RelPath);
  Request->SetVerb(TEXT("POST"));
  Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
  Request->SetContentAsString(JsonBody);

  if (OnResponse) {
    Request->OnProcessRequestComplete().BindLambda(
        [OnResponse](FHttpRequestPtr, FHttpResponsePtr Response, bool bOK) {
          FString Body = (bOK && Response) ? Response->GetContentAsString()
                                           : TEXT("HTTP error");
          OnResponse(Body);
        });
  }

  Request->ProcessRequest();
}

// ─── Button delegate handlers (declared in .h too, see below) ───────────────

void UBeaconCalibrationWidget::OnCapture1() { CapturePoint(1); }
void UBeaconCalibrationWidget::OnCapture2() { CapturePoint(2); }

void UBeaconCalibrationWidget::OnSolve() { SolveCalibration(); }
void UBeaconCalibrationWidget::OnReset() { ResetCalibration(); }

void UBeaconCalibrationWidget::OnToggleToolbar() {
  if (ToolbarContainer) {
    if (ToolbarContainer->GetVisibility() == ESlateVisibility::Collapsed ||
        ToolbarContainer->GetVisibility() == ESlateVisibility::Hidden) {
      ToolbarContainer->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
    } else {
      ToolbarContainer->SetVisibility(ESlateVisibility::Collapsed);
    }
  }
}

void UBeaconCalibrationWidget::OnSpeakPressed() {
  if (APawn *PlayerPawn = UGameplayStatics::GetPlayerPawn(this, 0)) {
    if (UVoiceStreamComponent *VoiceComp =
            PlayerPawn->FindComponentByClass<UVoiceStreamComponent>()) {
      VoiceComp->StartRecording();
    }
  }
}

void UBeaconCalibrationWidget::OnSpeakReleased() {
  if (APawn *PlayerPawn = UGameplayStatics::GetPlayerPawn(this, 0)) {
    if (UVoiceStreamComponent *VoiceComp =
            PlayerPawn->FindComponentByClass<UVoiceStreamComponent>()) {
      VoiceComp->StopRecording();
    }
  }
}

void UBeaconCalibrationWidget::OnConnectClicked() {
  if (ConnComp) {
    FString TargetURL = TEXT("ws://127.0.0.1:8090/ws");
    if (TxtServerIP && !TxtServerIP->GetText().IsEmpty()) {
      FString InputIP = TxtServerIP->GetText().ToString().TrimStartAndEnd();
      TargetURL = FString::Printf(TEXT("ws://%s:8090/ws"), *InputIP);
    }
    ConnComp->ConnectToServer(TargetURL);
  }

  // Connection initiated, switch to Calibration State
  SwitchUIState(1);
}
