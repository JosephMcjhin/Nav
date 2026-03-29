#include "ServerConnectionComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/Character.h"
#include "IWebSocket.h"
#include "NavigationComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UWBTargetComponent.h"
#include "VoiceStreamComponent.h"
#include "WebSocketsModule.h"

UServerConnectionComponent::UServerConnectionComponent() {
  PrimaryComponentTick.bCanEverTick = false;
}

void UServerConnectionComponent::BeginPlay() {
  Super::BeginPlay();
  if (!FModuleManager::Get().IsModuleLoaded("WebSockets")) {
    FModuleManager::Get().LoadModule("WebSockets");
  }

  // Connect automatically on game start!
  // if (!DefaultServerURL.IsEmpty()) {
  //  ConnectToServer(DefaultServerURL);
  // }
}

void UServerConnectionComponent::EndPlay(
    const EEndPlayReason::Type EndPlayReason) {
  Disconnect();
  Super::EndPlay(EndPlayReason);
}

void UServerConnectionComponent::ConnectToServer(const FString &InServerURL) {
  ServerBaseURL = InServerURL.Replace(TEXT("ws://"), TEXT("http://"))
                      .Replace(TEXT("/ws"), TEXT(""));

  if (WebSocket.IsValid() && WebSocket->IsConnected()) {
    return;
  }

  WebSocket = FWebSocketsModule::Get().CreateWebSocket(InServerURL);

  WebSocket->OnConnected().AddLambda([]() {
    UE_LOG(LogTemp, Log, TEXT("[ServerConnection] WebSocket Connected!"));
    if (GEngine)
      GEngine->AddOnScreenDebugMessage(-1, 2.f, FColor::Green,
                                       TEXT("Connected to Python Server."));
  });

  WebSocket->OnConnectionError().AddLambda([](const FString &Error) {
    UE_LOG(LogTemp, Error, TEXT("[ServerConnection] WS Error: %s"), *Error);
    if (GEngine)
      GEngine->AddOnScreenDebugMessage(-1, 2.f, FColor::Red,
                                       TEXT("WS Error: ") + Error);
  });

  WebSocket->OnRawMessage().AddLambda(
      [this](const void *Data, SIZE_T Size, SIZE_T BytesRemaining) {
        HandleBinaryData(Data, Size);
      });

  WebSocket->OnMessage().AddLambda([this](const FString &MessageString) {
    HandleJsonCommand(MessageString);
  });

  WebSocket->Connect();
}

void UServerConnectionComponent::Disconnect() {
  if (WebSocket.IsValid() && WebSocket->IsConnected()) {
    WebSocket->Close();
  }
}

void UServerConnectionComponent::SendString(const FString &Message) {
  if (WebSocket.IsValid() && WebSocket->IsConnected()) {
    WebSocket->Send(Message);
  }
}

void UServerConnectionComponent::SendBinary(const uint8 *Data, int32 Size) {
  if (WebSocket.IsValid() && WebSocket->IsConnected()) {
    WebSocket->Send(Data, Size, true);
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
