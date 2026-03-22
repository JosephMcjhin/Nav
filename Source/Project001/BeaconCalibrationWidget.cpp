// Fill out your copyright notice in the Description page of Project Settings.

#include "BeaconCalibrationWidget.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "GameFramework/Character.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
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

  if (BtnSolve)
    BtnSolve->OnClicked.AddDynamic(this, &UBeaconCalibrationWidget::OnSolve);
  if (BtnReset)
    BtnReset->OnClicked.AddDynamic(this, &UBeaconCalibrationWidget::OnReset);

  RefreshUI();
}

void UBeaconCalibrationWidget::NativeTick(const FGeometry &MyGeometry,
                                          float InDeltaTime) {
  Super::NativeTick(MyGeometry, InDeltaTime);

  // Apply joystick input to the owned character every frame
  if (ConnComp &&
      (FMath::Abs(JoystickX) > 0.01f || FMath::Abs(JoystickY) > 0.01f)) {
    ACharacter *Char = Cast<ACharacter>(ConnComp->GetOwner());
    if (Char) {
      Char->AddMovementInput(Char->GetActorForwardVector(), JoystickY);
      Char->AddMovementInput(Char->GetActorRightVector(), JoystickX);
    }
  }
}

void UBeaconCalibrationWidget::SetConnectionComponent(
    UServerConnectionComponent *InComponent) {
  ConnComp = InComponent;
}

void UBeaconCalibrationWidget::CapturePoint(int32 PointIndex) {
  if (!ConnComp) {
    SetStatus(TEXT("错误：ConnectionComponent 未绑定"));
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
  SetStatus(FString::Printf(TEXT("点 %d 已采集 ✅"), PointIndex));
  RefreshUI();
}

void UBeaconCalibrationWidget::SolveCalibration() {
  if (!ConnComp) {
    SetStatus(TEXT("错误：ConnectionComponent 未绑定"));
    return;
  }

  SetStatus(TEXT("正在计算信标位置..."));
  HttpPost(TEXT("/api/calibrate/solve"), TEXT("{}"),
           [this](const FString &ResponseBody) {
             SetStatus(FString::Printf(
                 TEXT("标定完成！服务端开始追踪位置。\n%s"), *ResponseBody));
           });
}

void UBeaconCalibrationWidget::ResetCalibration() {
  CapturedFlags = 0;
  HttpPost(TEXT("/api/calibrate/clear"), TEXT("{}"), nullptr);
  SetStatus(TEXT("已重置，请重新采集两个点。"));
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
        FText::FromString(bDone ? Name + TEXT(" ✅") : Name + TEXT(" ——")));
    Txt->SetColorAndOpacity(bDone ? FSlateColor(FLinearColor::Green)
                                  : FSlateColor(FLinearColor::Gray));
  };
  UpdateLabel(TxtPoint1, 1, TEXT("点 1"));
  UpdateLabel(TxtPoint2, 2, TEXT("点 2"));

  // Enable Solve only when all 2 points are captured
  if (BtnSolve)
    BtnSolve->SetIsEnabled((CapturedFlags & 0b11) == 0b11);
}

void UBeaconCalibrationWidget::SetStatus(const FString &Msg) {
  if (TxtStatus)
    TxtStatus->SetText(FText::FromString(Msg));

  UE_LOG(LogTemp, Log, TEXT("[BeaconCalib] %s"), *Msg);
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
