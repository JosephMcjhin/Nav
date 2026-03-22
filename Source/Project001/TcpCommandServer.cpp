// TcpCommandServer.cpp
// PC/Editor 层 TCP 监听器实现。
// 移动端 (Android/iOS) 打包时，FTcpListenerRunnable 和相关模块不会被编译。

#include "TcpCommandServer.h"
#include "Engine/Engine.h"
#include "GameFramework/Pawn.h"
#include "NavCommandComponent.h"

// 仅 PC / Editor 平台编译 TCP/Socket 相关头文件
#if !(PLATFORM_ANDROID || PLATFORM_IOS)
#include "Async/Async.h"
#include "Common/TcpSocketBuilder.h"
#include "Common/UdpSocketBuilder.h"
#include "Dom/JsonObject.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#endif

// ===========================================================================
// FTcpListenerRunnable (PC / Editor only)
// ===========================================================================
#if !(PLATFORM_ANDROID || PLATFORM_IOS)

FTcpListenerRunnable::FTcpListenerRunnable(
    int32 InPort, TWeakObjectPtr<UNavCommandComponent> InNavComp)
    : Port(InPort), NavComp(InNavComp) {}

FTcpListenerRunnable::~FTcpListenerRunnable() { Stop(); }

bool FTcpListenerRunnable::Init() {
  ISocketSubsystem *SocketSubsystem =
      ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
  if (!SocketSubsystem)
    return false;

  ListenSocket = FTcpSocketBuilder(TEXT("NavCmdTCPServer"))
                     .AsReusable()
                     .BoundToPort(Port)
                     .Listening(1)
                     .Build();

  if (!ListenSocket) {
    UE_LOG(LogTemp, Error,
           TEXT("[TcpCommandServer] Failed to create listen socket on port %d"),
           Port);
    return false;
  }

  UE_LOG(LogTemp, Log, TEXT("[TcpCommandServer] Listening on port %d"), Port);
  bRunning = true;
  return true;
}

uint32 FTcpListenerRunnable::Run() {
  TArray<uint8> RecvBuffer;
  RecvBuffer.SetNum(4096);

  while (bRunning) {
    // Non-blocking check for incoming connection
    bool bHasPendingConnection = false;
    if (ListenSocket &&
        ListenSocket->HasPendingConnection(bHasPendingConnection) &&
        bHasPendingConnection) {
      FSocket *ClientSocket = ListenSocket->Accept(TEXT("NavCmdClient"));
      if (ClientSocket) {
        UE_LOG(LogTemp, Log, TEXT("[TcpCommandServer] Client connected."));

        // Read until connection drops
        FString AccumStr;
        while (bRunning) {
          int32 BytesRead = 0;
          bool bRecv = ClientSocket->Recv(RecvBuffer.GetData(),
                                          RecvBuffer.Num(), BytesRead);

          if (!bRecv || BytesRead <= 0)
            break;

          // Convert raw bytes to FString (UTF-8)
          FUTF8ToTCHAR Convert(
              reinterpret_cast<const char *>(RecvBuffer.GetData()), BytesRead);
          FString Chunk = FString(Convert.Length(), Convert.Get());
          AccumStr += Chunk;

          // Process newline-delimited JSON commands
          TArray<FString> Lines;
          int32 NumPushed = AccumStr.ParseIntoArray(Lines, TEXT("\n"), true);

          if (NumPushed > 0) {
            // If the text doesn't end with newline, the last segment is
            // incomplete
            if (!AccumStr.EndsWith(TEXT("\n"))) {
              AccumStr = Lines.Last();
              Lines.RemoveAt(Lines.Num() - 1);
            } else {
              AccumStr.Empty();
            }

            for (const FString &Line : Lines) {
              FString Trimmed = Line.TrimStartAndEnd();
              if (!Trimmed.IsEmpty()) {
                HandleCommand(Trimmed);
              }
            }
          }
        }

        ClientSocket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)
            ->DestroySocket(ClientSocket);
        UE_LOG(LogTemp, Log, TEXT("[TcpCommandServer] Client disconnected."));
      }
    }

    FPlatformProcess::Sleep(0.05f);
  }

  return 0;
}

void FTcpListenerRunnable::Stop() {
  bRunning = false;
  if (ListenSocket) {
    ListenSocket->Close();
    ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)
        ->DestroySocket(ListenSocket);
    ListenSocket = nullptr;
  }
}

void FTcpListenerRunnable::HandleCommand(const FString &JsonStr) {
  UE_LOG(LogTemp, Log, TEXT("[TcpCommandServer] Received: %s"), *JsonStr);

  TSharedPtr<FJsonObject> JsonObject;
  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);

  if (!FJsonSerializer::Deserialize(Reader, JsonObject) ||
      !JsonObject.IsValid()) {
    UE_LOG(LogTemp, Warning,
           TEXT("[TcpCommandServer] Failed to parse JSON: %s"), *JsonStr);
    return;
  }

  FString Action;
  if (!JsonObject->TryGetStringField(TEXT("action"), Action)) {
    UE_LOG(LogTemp, Warning,
           TEXT("[TcpCommandServer] JSON missing 'action' field."));
    return;
  }

  if (Action == TEXT("navigate_to")) {
    FString Target;
    if (JsonObject->TryGetStringField(TEXT("target"), Target)) {
      FName TargetTag = FName(*Target);

      // Dispatch to game thread to safely call UObject methods
      AsyncTask(ENamedThreads::GameThread, [this, TargetTag]() {
        if (NavComp.IsValid()) {
          NavComp->NavigateTo(TargetTag, false);
        } else {
          UE_LOG(
              LogTemp, Warning,
              TEXT("[TcpCommandServer] NavCommandComponent no longer valid."));
        }
      });
    }
  } else if (Action == TEXT("move_to")) {
    FString Target;
    if (JsonObject->TryGetStringField(TEXT("target"), Target)) {
      FName TargetTag = FName(*Target);

      AsyncTask(ENamedThreads::GameThread, [this, TargetTag]() {
        if (NavComp.IsValid()) {
          NavComp->NavigateTo(TargetTag, true);
        } else {
          UE_LOG(
              LogTemp, Warning,
              TEXT("[TcpCommandServer] NavCommandComponent no longer valid."));
        }
      });
    }
  } else if (Action == TEXT("status")) {
    FString Text;
    if (JsonObject->TryGetStringField(TEXT("text"), Text)) {
      AsyncTask(ENamedThreads::GameThread, [Text]() {
        if (GEngine) {
          GEngine->AddOnScreenDebugMessage(
              94, 3.0f, FColor::Cyan,
              FString::Printf(TEXT("[Status] %s"), *Text));
        }
      });
      // Send to TTS UDP Server
      SendUdpMessage(Text);
    }
  } else if (Action == TEXT("add_world_offset")) {
    const TArray<TSharedPtr<FJsonValue>> *DeltaArray;
    if (JsonObject->TryGetArrayField(TEXT("delta"), DeltaArray) &&
        DeltaArray->Num() >= 3) {
      float DX = (*DeltaArray)[0]->AsNumber();
      float DY = (*DeltaArray)[1]->AsNumber();
      float DZ = (*DeltaArray)[2]->AsNumber();

      AsyncTask(ENamedThreads::GameThread, [this, DX, DY, DZ]() {
        if (NavComp.IsValid()) {
          AActor *Owner = NavComp->GetOwner();
          if (Owner) {
            Owner->AddActorWorldOffset(FVector(DX, DY, DZ), true);
          }
        }
      });
    }
  } else if (Action == TEXT("stop")) {
    AsyncTask(ENamedThreads::GameThread, [this]() {
      if (NavComp.IsValid()) {
        NavComp->StopNavigation();
      }
    });
  } else if (Action == TEXT("list_destinations")) {
    AsyncTask(ENamedThreads::GameThread, [this]() {
      if (NavComp.IsValid()) {
        TArray<FName> Destinations = NavComp->GetAvailableDestinations();
        FString List;
        for (const FName &D : Destinations) {
          List += D.ToString() + TEXT(", ");
        }
        UE_LOG(LogTemp, Log,
               TEXT("[TcpCommandServer] Available destinations: %s"), *List);
        if (GEngine) {
          GEngine->AddOnScreenDebugMessage(
              96, 8.0f, FColor::White,
              FString::Printf(TEXT("[NavCmd] Destinations: %s"), *List));
        }
      }
    });
  } else {
    UE_LOG(LogTemp, Warning, TEXT("[TcpCommandServer] Unknown action: %s"),
           *Action);
  }
}

void FTcpListenerRunnable::SendUdpMessage(const FString &Message) {
  if (Message.IsEmpty())
    return;

  ISocketSubsystem *SocketSubsystem =
      ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
  if (!SocketSubsystem)
    return;

  TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
  bool bIsValid;
  Addr->SetIp(TEXT("127.0.0.1"), bIsValid);
  Addr->SetPort(9002);

  FSocket *UdpSocket =
      FUdpSocketBuilder(TEXT("TTS_UDP_Sender")).AsNonBlocking().AsReusable();

  if (UdpSocket) {
    FTCHARToUTF8 Convert(*Message);
    int32 BytesSent = 0;
    UdpSocket->SendTo((const uint8 *)Convert.Get(), Convert.Length(), BytesSent,
                      *Addr);

    UdpSocket->Close();
    SocketSubsystem->DestroySocket(UdpSocket);
  }
}

#endif // !(PLATFORM_ANDROID || PLATFORM_IOS)

// ===========================================================================
// UTcpCommandServer (ActorComponent) — 所有平台共享部分
// ===========================================================================

UTcpCommandServer::UTcpCommandServer() {
  PrimaryComponentTick.bCanEverTick = false;
}

void UTcpCommandServer::BeginPlay() {
  Super::BeginPlay();

#if PLATFORM_ANDROID || PLATFORM_IOS
  // 移动端无需 TCP 服务器（没有 Python/MCP 连接）
  UE_LOG(LogTemp, Log,
         TEXT("[TcpCommandServer] 移动端模式：TCP 服务器已跳过。"));
#else
  // 1. Only run server on the locally controlled pawn to avoid port conflicts
  // with multiple instances
  APawn *PawnOwner = Cast<APawn>(GetOwner());
  if (PawnOwner && !PawnOwner->IsLocallyControlled()) {
    return;
  }

  // 2. Auto-find NavCommandComponent on the same actor
  NavCommandComp =
      GetOwner() ? GetOwner()->FindComponentByClass<UNavCommandComponent>()
                 : nullptr;

  if (!NavCommandComp) {
    UE_LOG(LogTemp, Warning,
           TEXT("[TcpCommandServer] No NavCommandComponent found on owner. TCP "
                "server will not start."));
    if (GEngine) {
      GEngine->AddOnScreenDebugMessage(
          -1, 15.f, FColor::Red,
          TEXT("[TcpServer] ERROR: NavCommandComponent Missing on Character!"));
    }
    return;
  }

  StartServer();
#endif
}

void UTcpCommandServer::EndPlay(const EEndPlayReason::Type EndPlayReason) {
#if !(PLATFORM_ANDROID || PLATFORM_IOS)
  StopServer();
#endif
  Super::EndPlay(EndPlayReason);
}

#if !(PLATFORM_ANDROID || PLATFORM_IOS)

void UTcpCommandServer::StartServer() {
  ListenerRunnable = new FTcpListenerRunnable(ListenPort, NavCommandComp);
  ListenerThread = FRunnableThread::Create(ListenerRunnable,
                                           TEXT("NavCmdTCPListenerThread"));

  if (GEngine) {
    GEngine->AddOnScreenDebugMessage(
        95, 20.0f, FColor::Orange,
        FString::Printf(TEXT("[TcpServer] LISTENING ON PORT %d ... Ready for "
                             "voice commands!"),
                        ListenPort));
  }
  UE_LOG(LogTemp, Log, TEXT("[TcpCommandServer] Server started on port %d"),
         ListenPort);
}

void UTcpCommandServer::StopServer() {
  if (ListenerRunnable) {
    ListenerRunnable->Stop();
  }
  if (ListenerThread) {
    ListenerThread->Kill(true);
    delete ListenerThread;
    ListenerThread = nullptr;
  }
  if (ListenerRunnable) {
    delete ListenerRunnable;
    ListenerRunnable = nullptr;
  }
  UE_LOG(LogTemp, Log, TEXT("[TcpCommandServer] Server stopped."));
}

#endif // !(PLATFORM_ANDROID || PLATFORM_IOS)
