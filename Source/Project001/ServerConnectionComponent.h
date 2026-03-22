#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "ServerConnectionComponent.generated.h"

class IWebSocket;

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class PROJECT001_API UServerConnectionComponent : public UActorComponent {
  GENERATED_BODY()

public:
  UServerConnectionComponent();

  // Default WebSocket URL to connect at startup
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Network")
  FString DefaultServerURL = TEXT("ws://127.0.0.1:8090/ws");

  // Basic properties
  UPROPERTY(BlueprintReadOnly, Category = "Network")
  FString ServerBaseURL;

  // Initialize the connection
  UFUNCTION(BlueprintCallable, Category = "Network")
  void ConnectToServer(const FString &InServerURL);

  UFUNCTION(BlueprintCallable, Category = "Network")
  void Disconnect();

  // Public methods for other components to send data
  void SendString(const FString &Message);
  void SendBinary(const uint8 *Data, int32 Size);

  // Expose an event or direct dispatch
  // For simplicity, we dispatch directly to sibling components here.

protected:
  virtual void BeginPlay() override;
  virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
  TSharedPtr<IWebSocket> WebSocket;

  void HandleJsonCommand(const FString &MessageString);
  void HandleBinaryData(const void *Data, SIZE_T Size);
};
