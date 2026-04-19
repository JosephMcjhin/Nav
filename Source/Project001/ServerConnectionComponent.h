#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "ServerConnectionComponent.generated.h"

class IWebSocket;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnConnectionSuccess, const FString&, URL);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnConnectionFailed, const FString&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnServerStatus, bool, bIsCalibrated, bool, bIsHeadingCalibrated, float, ImuOffset, int32, Points);

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
  UFUNCTION(BlueprintCallable, Category = "Voice Nav")
  void ConnectToServer(const FString &InServerURL);

  UFUNCTION(BlueprintCallable, Category = "Voice Nav")
  void Disconnect();

  UPROPERTY(BlueprintAssignable, Category = "Network")
  FOnConnectionSuccess OnConnectionSuccess;

  UPROPERTY(BlueprintAssignable, Category = "Network")
  FOnConnectionFailed OnConnectionFailed;

  UPROPERTY(BlueprintAssignable, Category = "Network")
  FOnServerStatus OnServerStatus;

  /** Saves the server IP to local USaveGame for future sessions. */
  void SaveIPToCache(const FString &IP);

  /** Loads the cached server IP from local USaveGame. */
  FString LoadIPFromCache();

  // Public methods for other components to send data
  void SendString(const FString &Message);
  void SendBinary(const uint8 *Data, int32 Size);

  /** Returns true if WebSocket is currently connected. */
  bool IsConnected() const;

  /** Returns the URL currently connected to (empty if not connected). */
  FString GetConnectedURL() const { return ConnectedURL; }

  /** Deletes the cached IP SaveGame slot entirely. */
  void ClearIPCache();

  // Expose an event or direct dispatch
  // For simplicity, we dispatch directly to sibling components here.

protected:
  virtual void BeginPlay() override;
  virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

  /** Try to load IP from local storage and connect automatically. */
  void AutoConnectFromCache();
  virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
  TSharedPtr<IWebSocket> WebSocket;
  FString ConnectedURL;
  float IMUSendTimer = 0.0f;
  FVector LastSentTilt = FVector(-1.f, -1.f, -1.f);
  
  // Cached server status
  bool bLastIsCalibrated = false;
  bool bLastIsHeadingCalibrated = false;
  double LastImuOffset = 0.0;
  int32 LastPoints = 0;

  void HandleJsonCommand(const FString &MessageString);
  void HandleBinaryData(const void *Data, SIZE_T Size);
};
