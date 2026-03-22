// TcpCommandServer.h
// PC/Editor 专用 TCP 服务器（监听 9001 端口，接受 Python/MCP JSON 导航指令）
//
// 移动端 (Android/iOS) 原生打包时，此文件除 UTcpCommandServer 的空壳声明外
// 全部被宏排除，避免 Socket 相关符号链接失败。
//
// Command format (JSON, newline-terminated):
//   {"action":"navigate_to","target":"entrance"}
//   {"action":"stop"}
//   {"action":"list_destinations"}

#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"

// 仅 PC / Editor 平台需要 Socket 相关头文件
#if !(PLATFORM_ANDROID || PLATFORM_IOS)
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#endif

#include "TcpCommandServer.generated.h"

// Forward declarations
class UNavCommandComponent;

#if !(PLATFORM_ANDROID || PLATFORM_IOS)
class FSocket;

/**
 * Background thread that accepts TCP connections and reads JSON commands.
 * PC / Editor only — not compiled for Android/iOS.
 */
class FTcpListenerRunnable : public FRunnable {
public:
  FTcpListenerRunnable(int32 InPort,
                       TWeakObjectPtr<UNavCommandComponent> InNavComp);
  virtual ~FTcpListenerRunnable();

  virtual bool Init() override;
  virtual uint32 Run() override;
  virtual void Stop() override;

  void HandleCommand(const FString &JsonStr);
  void SendUdpMessage(const FString &Message);

private:
  int32 Port;
  TWeakObjectPtr<UNavCommandComponent> NavComp;
  FSocket *ListenSocket = nullptr;
  bool bRunning = false;
};

#endif // !(PLATFORM_ANDROID || PLATFORM_IOS)

/**
 * Actor Component that owns and manages the TCP listener thread.
 * Add this to your AMyCharacter (or any actor that has NavCommandComponent).
 *
 * On Android/iOS (native package), BeginPlay() is a no-op: the TCP server
 * is not started because there is no Python/MCP process to connect to it.
 */
UCLASS(ClassGroup = (Navigation), meta = (BlueprintSpawnableComponent))
class PROJECT001_API UTcpCommandServer : public UActorComponent {
  GENERATED_BODY()

public:
  UTcpCommandServer();

protected:
  virtual void BeginPlay() override;
  virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
  /** TCP port to listen on. Default 9001. (PC only) */
  UPROPERTY(EditAnywhere, Category = "TCP Server")
  int32 ListenPort = 9001;

  /** Reference to the NavCommandComponent on this actor. */
  UPROPERTY()
  UNavCommandComponent *NavCommandComp = nullptr;

private:
#if !(PLATFORM_ANDROID || PLATFORM_IOS)
  FTcpListenerRunnable *ListenerRunnable = nullptr;
  FRunnableThread *ListenerThread = nullptr;

  void StartServer();
  void StopServer();
#endif
};
