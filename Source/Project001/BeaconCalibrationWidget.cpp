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

void UBeaconCalibrationWidget::NativeConstruct() {
  Super::NativeConstruct();

  if (BtnCapture1)
    BtnCapture1->OnClicked.AddDynamic(this,
                                      &UBeaconCalibrationWidget::OnCapture1);
  if (BtnCapture2)
    BtnCapture2->OnClicked.AddDynamic(this,
                                      &UBeaconCalibrationWidget::OnCapture2);
  if (BtnCapture3)
    BtnCapture3->OnClicked.AddDynamic(this,
                                      &UBeaconCalibrationWidget::OnCapture3);

  if (BtnSolve)
    BtnSolve->OnClicked.AddDynamic(this, &UBeaconCalibrationWidget::OnSolve);

  if (BtnToggleToolbar)
    BtnToggleToolbar->OnClicked.AddDynamic(
        this, &UBeaconCalibrationWidget::OnToggleToolbar);

  if (BtnConnect) {
    BtnConnect->OnClicked.AddDynamic(
        this, &UBeaconCalibrationWidget::OnConnectClicked);
  }

  if (BtnCalibrateHeading) {
    BtnCalibrateHeading->OnClicked.AddDynamic(
        this, &UBeaconCalibrationWidget::OnCalibrateHeadingClicked);
  }

  if (BtnClearCache) {
    BtnClearCache->OnClicked.AddDynamic(
        this, &UBeaconCalibrationWidget::OnClearCacheClicked);
  }

  if (ConnComp) {
    ConnComp->OnConnectionSuccess.AddDynamic(
        this, &UBeaconCalibrationWidget::OnWSConnected);
    ConnComp->OnConnectionFailed.AddDynamic(
        this, &UBeaconCalibrationWidget::OnWSConnectionError);
    ConnComp->OnServerStatus.AddDynamic(
        this, &UBeaconCalibrationWidget::OnServerStatusReceived);
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

  // Handle race condition: if component already connected before widget was bound,
  // immediately synchronize UI state.
  if (ConnComp && ConnComp->IsConnected()) {
    FString URL = ConnComp->GetConnectedURL();
    FString IP = URL.Replace(TEXT("ws://"), TEXT("")).Replace(TEXT(":8090/ws"), TEXT(""));
    SetStatus(FString::Printf(TEXT("Connected to: %s"), *IP));
    SwitchUIState(1);
  }
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
  UpdateLabel(TxtPoint3, 4, TEXT("Point 3"));

  // Enable Solve only when all 3 points are captured
  if (BtnSolve)
    BtnSolve->SetIsEnabled((CapturedFlags & 0b111) == 0b111);
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
  if (BtnCapture3)
    BtnCapture3->SetVisibility(V1);
  if (BtnSolve)
    BtnSolve->SetVisibility(V1);
  if (TxtPoint1)
    TxtPoint1->SetVisibility(V1);
  if (TxtPoint2)
    TxtPoint2->SetVisibility(V1);
  if (TxtPoint3)
    TxtPoint3->SetVisibility(V1);

  if (BtnCalibrateHeading)
    BtnCalibrateHeading->SetVisibility(V1); // Stage 2 (Calibration)

  if (BtnClearCache)
    BtnClearCache->SetVisibility(
        (State == 1 || State == 2)
            ? ESlateVisibility::Visible
            : ESlateVisibility::Collapsed); // Stage 2 & 3

  if (TxtStatus)
    TxtStatus->SetVisibility(
        ESlateVisibility::Visible); // Always visible for feedback
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
void UBeaconCalibrationWidget::OnCapture3() { CapturePoint(3); }

void UBeaconCalibrationWidget::OnSolve() { SolveCalibration(); }

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

void UBeaconCalibrationWidget::OnConnectClicked() {
  if (ConnComp) {
    FString InputIP = TEXT("127.0.0.1");
    if (TxtServerIP && !TxtServerIP->GetText().IsEmpty()) {
      InputIP = TxtServerIP->GetText().ToString().TrimStartAndEnd();
    }

    SetStatus(FString::Printf(TEXT("Connecting to: %s..."), *InputIP));
    FString TargetURL = FString::Printf(TEXT("ws://%s:8090/ws"), *InputIP);
    ConnComp->ConnectToServer(TargetURL);

    // Save IP to local cache for auto-connect
    ConnComp->SaveIPToCache(InputIP);
  }
}

void UBeaconCalibrationWidget::OnWSConnected(const FString &URL) {
  FString IP =
      URL.Replace(TEXT("ws://"), TEXT("")).Replace(TEXT(":8090/ws"), TEXT(""));
  SetStatus(FString::Printf(TEXT("Connected to: %s"), *IP));
  // Advance to Stage 1 (Calibration) immediately upon connection
  SwitchUIState(1);
}

void UBeaconCalibrationWidget::OnWSConnectionError(const FString &Error) {
  SetStatus(FString::Printf(TEXT("Connection failed: %s"), *Error));
  // Roll back to Stage 0 (Connect)
  SwitchUIState(0);
}

void UBeaconCalibrationWidget::OnServerStatusReceived(bool bIsCalibrated, bool bIsHeadingCalibrated,
                                                      float ImuOffset,
                                                      int32 Points) {
  if (bIsCalibrated && bIsHeadingCalibrated) {
    SetStatus(TEXT("Server calibrated. Voice Nav Ready."));
    SwitchUIState(2);
  } else {
    // If not fully calibrated, ensure we are in Stage 1
    SetStatus(TEXT("Calibration required (Position and/or Heading)."));
    SwitchUIState(1);
  }
  RefreshUI();
}

void UBeaconCalibrationWidget::OnCalibrateHeadingClicked() {
  if (!ConnComp)
    return;

  if (APlayerController *PC = GetWorld()->GetFirstPlayerController()) {
    FVector Tilt, RotationRate, Gravity, Acceleration;
    PC->GetInputMotionState(Tilt, RotationRate, Gravity, Acceleration);

    float CurrentImuYaw = Tilt.Y;

    if (AActor *Owner = ConnComp->GetOwner()) {
      float CurrentUeYaw = Owner->GetActorRotation().Yaw;

      FString JsonStr =
          FString::Printf(TEXT("{\"imu_yaw\": %f, \"target_ue_yaw\": %f}"),
                          CurrentImuYaw, CurrentUeYaw);
      HttpPost(TEXT("/api/calibrate/heading"), JsonStr,
               [this](const FString &Response) {
                 SetStatus(TEXT("Heading aligned and saved to server."));
               });
    }
  }
}

void UBeaconCalibrationWidget::OnClearCacheClicked() {
  if (!ConnComp)
    return;

  HttpPost(TEXT("/api/calibrate/clear"), TEXT("{}"),
           [this](const FString &Response) {
             CapturedFlags = 0;
             SetStatus(TEXT("All caches cleared. Disconnecting."));
             
             // Thorough Reset: clear client IP cache, disconnect, go to Stage 0
             if (ConnComp) {
                 ConnComp->ClearIPCache();
                 ConnComp->Disconnect();
             }
             SwitchUIState(0);
             
             RefreshUI();
           });
}
