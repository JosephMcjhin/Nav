#include "ServerConnectionComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "IWebSocket.h"
#include "Kismet/GameplayStatics.h"
#include "NavSettingsSaveGame.h"
#include "NavigationComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UWBTargetComponent.h"
#include "VoiceStreamComponent.h"
#include "WebSocketsModule.h"

UServerConnectionComponent::UServerConnectionComponent() {
  PrimaryComponentTick.bCanEverTick = true;
}

void UServerConnectionComponent::BeginPlay() {
  Super::BeginPlay();
  if (!FModuleManager::Get().IsModuleLoaded("WebSockets")) {
    FModuleManager::Get().LoadModule("WebSockets");
  }

  // Connect automatically on game start using cached IP
  AutoConnectFromCache();
}

void UServerConnectionComponent::EndPlay(
    const EEndPlayReason::Type EndPlayReason) {
  Disconnect();
  Super::EndPlay(EndPlayReason);
}

void UServerConnectionComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction *ThisTickFunction) {
  Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

  if (!WebSocket.IsValid() || !WebSocket->IsConnected())
    return;

  IMUSendTimer += DeltaTime;
  if (IMUSendTimer >= 0.05f) { // 20Hz polling
    IMUSendTimer = 0.0f;

    if (APlayerController *PC = GetWorld()->GetFirstPlayerController()) {
      FVector Tilt, RotationRate, Gravity, Acceleration;
      PC->GetInputMotionState(Tilt, RotationRate, Gravity, Acceleration);

      // Convert from Radians ([-PI, PI]) to Degrees ([-180, 180])
      // Using explicit conversion to ensure consistency across mobile platforms
      float DegPitch = FMath::RadiansToDegrees(Tilt.X);
      float DegYaw = FMath::RadiansToDegrees(Tilt.Y);
      float DegRoll = FMath::RadiansToDegrees(Tilt.Z);

      // Only send IMU data if it changed meaningfully (by at least 1 degree)
      FVector DegTilt(DegPitch, DegYaw, DegRoll);
      if (!DegTilt.Equals(LastSentTilt, 1.0f)) {
        LastSentTilt = DegTilt;

        FString Msg = FString::Printf(
            TEXT("{\"type\": \"imu\", \"pitch\": %.2f, \"yaw\": %.2f, \"roll\": "
                 "%.2f}"),
            DegPitch, DegYaw, DegRoll);
        SendString(Msg);
      }

      // Professional HUD Display for Sensor Tuning
      if (GEngine) {
        GEngine->AddOnScreenDebugMessage(
            3004, 0.1f, FColor::Cyan,
            FString::Printf(TEXT("[IMU Degrees] P: %.1f | Y: %.1f | R: %.1f"),
                            DegPitch, DegYaw, DegRoll));
      }
    }
  }
}

void UServerConnectionComponent::ConnectToServer(const FString &InServerURL) {
  ServerBaseURL = InServerURL.Replace(TEXT("ws://"), TEXT("http://"))
                      .Replace(TEXT("/ws"), TEXT(""));

  // If already connected to this URL, do nothing
  if (WebSocket.IsValid() && WebSocket->IsConnected()) {
    return;
  }

  // Close any existing (dead) connection before creating a new one
  if (WebSocket.IsValid()) {
    WebSocket->Close();
    WebSocket.Reset();
  }

  WebSocket = FWebSocketsModule::Get().CreateWebSocket(InServerURL);

  WebSocket->OnConnected().AddLambda([this, InServerURL]() {
    UE_LOG(LogTemp, Log, TEXT("[ServerConnection] WebSocket Connected to %s"),
           *InServerURL);
    ConnectedURL = InServerURL;
    if (GEngine)
      GEngine->AddOnScreenDebugMessage(-1, 2.f, FColor::Green,
                                       TEXT("Connected to Python Server."));
    OnConnectionSuccess.Broadcast(InServerURL);
  });

  WebSocket->OnConnectionError().AddLambda([this](const FString &Error) {
    UE_LOG(LogTemp, Error, TEXT("[ServerConnection] WS Error: %s"), *Error);
    if (GEngine)
      GEngine->AddOnScreenDebugMessage(-1, 2.f, FColor::Red,
                                       TEXT("WS Error: ") + Error);
    OnConnectionFailed.Broadcast(Error);
  });

  WebSocket->OnRawMessage().AddLambda(
      [this](const void *Data, SIZE_T Size, SIZE_T BytesRemaining) {
        HandleBinaryData(Data, Size);
      });

  WebSocket->OnMessage().AddLambda([this](const FString &MessageString) {
    HandleJsonCommand(MessageString);
  });

  WebSocket->OnClosed().AddLambda(
      [this](int32 StatusCode, const FString &Reason, bool bWasClean) {
        UE_LOG(LogTemp, Warning,
               TEXT("[ServerConnection] WS Closed: Code=%d Reason=%s Clean=%d"),
               StatusCode, *Reason, bWasClean);
        ConnectedURL.Empty();
        if (GEngine)
          GEngine->AddOnScreenDebugMessage(
              -1, 5.f, FColor::Red,
              FString::Printf(TEXT("WS Disconnected (Code: %d)"), StatusCode));
        OnConnectionFailed.Broadcast(
            FString::Printf(TEXT("Connection closed: %d"), StatusCode));
      });

  WebSocket->Connect();
}

void UServerConnectionComponent::Disconnect() {
  if (WebSocket.IsValid() && WebSocket->IsConnected()) {
    WebSocket->Close();
  }
  ConnectedURL.Empty();

  // Wipe cached server states to prevent ghost-calibration on reconnect
  bLastIsCalibrated = false;
  bLastIsHeadingCalibrated = false;
  LastImuOffset = 0.0;
  LastPoints = 0;
}

bool UServerConnectionComponent::IsConnected() const {
  return WebSocket.IsValid() && WebSocket->IsConnected();
}

void UServerConnectionComponent::SendString(const FString &Message) {
  if (WebSocket.IsValid() && WebSocket->IsConnected()) {
    WebSocket->Send(Message);
  }
}

void UServerConnectionComponent::SendBinary(const uint8 *Data, int32 Size) {
  if (!Data || Size <= 0)
    return;

  if (WebSocket.IsValid() && WebSocket->IsConnected()) {
    WebSocket->Send(Data, Size, true);
  }
}

void UServerConnectionComponent::AutoConnectFromCache() {
  FString CachedIP = LoadIPFromCache().TrimStartAndEnd();
  if (!CachedIP.IsEmpty()) {
    FString TargetURL = CachedIP;
    if (!TargetURL.StartsWith(TEXT("ws://"))) {
      TargetURL = FString::Printf(TEXT("ws://%s:8090/ws"), *CachedIP);
    }

    if (GEngine) {
      GEngine->AddOnScreenDebugMessage(
          -1, 10.f, FColor::Cyan,
          FString::Printf(TEXT("Auto-connecting to cached server: %s"),
                          *CachedIP));
    }

    UE_LOG(LogTemp, Log, TEXT("[ServerConnection] Auto-connecting to: %s"),
           *TargetURL);
    ConnectToServer(TargetURL);
  } else {
    UE_LOG(LogTemp, Log, TEXT("[ServerConnection] No cached IP found."));
  }
}

void UServerConnectionComponent::SaveIPToCache(const FString &IP) {
  if (UNavSettingsSaveGame *SaveGameInstance =
          Cast<UNavSettingsSaveGame>(UGameplayStatics::CreateSaveGameObject(
              UNavSettingsSaveGame::StaticClass()))) {
    SaveGameInstance->LastConnectedIP = IP;
    UGameplayStatics::SaveGameToSlot(SaveGameInstance,
                                     SaveGameInstance->SaveSlotName,
                                     SaveGameInstance->UserIndex);
    UE_LOG(LogTemp, Log, TEXT("[ServerConnection] Saved IP to cache: %s"), *IP);
  }
}

FString UServerConnectionComponent::LoadIPFromCache() {
  if (UGameplayStatics::DoesSaveGameExist(TEXT("NavSettingsSlot"), 0)) {
    if (UNavSettingsSaveGame *LoadGameInstance = Cast<UNavSettingsSaveGame>(
            UGameplayStatics::LoadGameFromSlot(TEXT("NavSettingsSlot"), 0))) {
      return LoadGameInstance->LastConnectedIP;
    }
  }
  return FString(); // No cached IP — do NOT auto-connect
}

void UServerConnectionComponent::ClearIPCache() {
  if (UGameplayStatics::DoesSaveGameExist(TEXT("NavSettingsSlot"), 0)) {
    UGameplayStatics::DeleteGameInSlot(TEXT("NavSettingsSlot"), 0);
    UE_LOG(LogTemp, Log, TEXT("[ServerConnection] IP cache deleted."));
  }
}

void UServerConnectionComponent::HandleJsonCommand(
    const FString &MessageString) {
  UE_LOG(LogTemp, Log, TEXT("[ServerConnection] Server: %s"), *MessageString);

  TSharedPtr<FJsonObject> JsonObject;
  TSharedRef<TJsonReader<>> Reader =
      TJsonReaderFactory<>::Create(MessageString);

  if (FJsonSerializer::Deserialize(Reader, JsonObject) &&
      JsonObject.IsValid()) {
    FString MsgType;
    if (JsonObject->TryGetStringField(TEXT("type"), MsgType)) {
      AActor *Owner = GetOwner();
      if (!Owner)
        return;

      // Dispatch "set_target" to UWBTargetComponent
      if (MsgType == TEXT("set_target")) {
        double X = 0, Y = 0;
        JsonObject->TryGetNumberField(TEXT("x"), X);
        JsonObject->TryGetNumberField(TEXT("y"), Y);

        if (UUWBTargetComponent *UWBComp =
                Owner->FindComponentByClass<UUWBTargetComponent>()) {
          UWBComp->SetUWBTarget(static_cast<float>(X), static_cast<float>(Y));
        }

      } else if (MsgType == TEXT("status") ||
                 MsgType == TEXT("status_update")) {
        // Update cached server status for UI logic (only apply fields that
        // exist in payload)
        JsonObject->TryGetBoolField(TEXT("is_calibrated"), bLastIsCalibrated);
        JsonObject->TryGetBoolField(TEXT("is_heading_calibrated"),
                                    bLastIsHeadingCalibrated);
        JsonObject->TryGetNumberField(TEXT("imu_offset"), LastImuOffset);
        JsonObject->TryGetNumberField(TEXT("points"), LastPoints);

        OnServerStatus.Broadcast(bLastIsCalibrated, bLastIsHeadingCalibrated,
                                 static_cast<float>(LastImuOffset), LastPoints);

        // Display calibration cache info on screen (Top-left debug text)
        if (GEngine) {
          FString StatusStr = FString::Printf(
              TEXT("POS:%s HEADING:%s"),
              bLastIsCalibrated ? TEXT("OK") : TEXT("WAIT"),
              bLastIsHeadingCalibrated ? TEXT("OK") : TEXT("WAIT"));
          FColor DisplayColor = (bLastIsCalibrated && bLastIsHeadingCalibrated)
                                    ? FColor::Green
                                    : FColor::Orange;
          GEngine->AddOnScreenDebugMessage(
              3003, 2.0f, DisplayColor,
              FString::Printf(TEXT("[Cache] %s | IMU Offset: %.1f"), *StatusStr,
                              LastImuOffset));
        }

      } else if (MsgType == TEXT("set_rotation")) {
        double Yaw = 0;
        JsonObject->TryGetNumberField(TEXT("yaw"), Yaw);

        if (UUWBTargetComponent *UWBComp =
                Owner->FindComponentByClass<UUWBTargetComponent>()) {
          UWBComp->SetUWBRotation(static_cast<float>(Yaw));
        }

        // Dispatch "navigate_to" to NavCommandComponent
      } else if (MsgType == TEXT("navigate_to")) {
        FString DestString;
        if (!JsonObject->TryGetStringField(TEXT("destination"), DestString)) {
          JsonObject->TryGetStringField(TEXT("target"), DestString);
        }
        if (!DestString.IsEmpty()) {
          if (UNavigationComponent *NavComp =
                  Owner->FindComponentByClass<UNavigationComponent>()) {
            NavComp->NavigateTo(FName(*DestString));
          }
        }
      }
    }
  }
}

void UServerConnectionComponent::HandleBinaryData(const void *Data,
                                                  SIZE_T Size) {
  if (Size == 0)
    return;

  // Hack/Fix: UE invokes OnRawMessage even for text (JSON) messages.
  // If the packet starts with '{', it's our JSON command payload, NOT TTS PCM
  // Audio. Playing ASCII characters as PCM audio creates a loud beeping noise.
  const uint8 *Bytes = static_cast<const uint8 *>(Data);
  if (Bytes[0] == '{') {
    return;
  }

  // Dispatch TTS binary stream to VoiceStreamComponent for audio playback
  if (AActor *Owner = GetOwner()) {
    if (UVoiceStreamComponent *VoiceComp =
            Owner->FindComponentByClass<UVoiceStreamComponent>()) {
      VoiceComp->HandleIncomingTTSAudio(Data, static_cast<int32>(Size));
    }
  }
}
