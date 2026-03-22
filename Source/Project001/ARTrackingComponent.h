// ARTrackingComponent.h
// 跨平台 AR 追踪组件：
//   - 原生移动端 (Android/iOS)：使用 GetInputMotionState
//   读取纯净陀螺仪和加速度计，无需 ARCore
//   - PC / Editor：通过 UDP 端口 9003 接收 ARWebSensor 中继服务器数据
//   (x,y,z,yaw)

#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"

// 仅在非移动端平台编译 Socket 相关头文件
#if !(PLATFORM_ANDROID || PLATFORM_IOS)
#include "SocketSubsystem.h"
#include "Sockets.h"
#endif

#include "ARTrackingComponent.generated.h"

// ─────────────────────────────────────────────────────────────────────────────
// 追踪数据结构（统一平台接口）
// ─────────────────────────────────────────────────────────────────────────────
USTRUCT(BlueprintType)
struct FARTrackingData {
  GENERATED_BODY()

  /** 世界坐标系中的设备位置 (cm) */
  UPROPERTY(BlueprintReadOnly, Category = "AR Tracking")
  FVector Location = FVector::ZeroVector;

  /** 设备旋转 */
  UPROPERTY(BlueprintReadOnly, Category = "AR Tracking")
  FRotator Rotation = FRotator::ZeroRotator;

  /** 追踪是否有效/稳定 */
  UPROPERTY(BlueprintReadOnly, Category = "AR Tracking")
  bool bIsTracking = false;

  /**
   * 前进速度信号：0=停止，1=正常行走速度。
   */
  UPROPERTY(BlueprintReadOnly, Category = "AR Tracking")
  float ForwardSpeed = 0.f;
};

// ─────────────────────────────────────────────────────────────────────────────
// PC 端 UDP 接收线程（仅 PC / Editor 编译）
// ─────────────────────────────────────────────────────────────────────────────
#if !(PLATFORM_ANDROID || PLATFORM_IOS)

class UARTrackingComponent;

/**
 * 后台线程：监听 UDP 9003，接收 ARWebSensor 中继服务器转发的手机传感器数据。
 */
class FAR_UDPReceiver : public FRunnable {
public:
  FAR_UDPReceiver(int32 InPort, TWeakObjectPtr<UARTrackingComponent> InComp);
  virtual ~FAR_UDPReceiver();

  virtual bool Init() override;
  virtual uint32 Run() override;
  virtual void Stop() override;

private:
  int32 Port;
  TWeakObjectPtr<UARTrackingComponent> Comp;
  FSocket *RecvSocket = nullptr;
  bool bRunning = false;
};

#endif // !(PLATFORM_ANDROID || PLATFORM_IOS)

// ─────────────────────────────────────────────────────────────────────────────
// UARTrackingComponent — 公共接口
// ─────────────────────────────────────────────────────────────────────────────
UCLASS(ClassGroup = (AR), meta = (BlueprintSpawnableComponent))
class PROJECT001_API UARTrackingComponent : public UActorComponent {
  GENERATED_BODY()

public:
  UARTrackingComponent();

protected:
  virtual void BeginPlay() override;
  virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
  virtual void
  TickComponent(float DeltaTime, ELevelTick TickType,
                FActorComponentTickFunction *ThisTickFunction) override;

  // ── 对外接口（无论平台均可调用） ────────────────────────────────────

  UFUNCTION(BlueprintCallable, Category = "AR Tracking")
  FVector GetTrackedLocation() const { return CurrentData.Location; }

  UFUNCTION(BlueprintCallable, Category = "AR Tracking")
  FRotator GetTrackedRotation() const { return CurrentData.Rotation; }

  UFUNCTION(BlueprintCallable, Category = "AR Tracking")
  FARTrackingData GetTrackingData() const { return CurrentData; }

  UFUNCTION(BlueprintCallable, Category = "AR Tracking")
  bool IsTracking() const { return CurrentData.bIsTracking; }

  // ── 配置 ─────────────────────────────────────────────────────────────

  /** PC 端 UDP 监听端口 */
  UPROPERTY(EditAnywhere, Category = "AR Tracking|PC")
  int32 UDPReceivePort = 9003;

  /** PC 模式下 UE 场景的"原点偏移" */
  UPROPERTY(EditAnywhere, Category = "AR Tracking|PC")
  float MeterToCmScale = 100.f;

  /** 是否自动应用姿态到角色 */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AR Tracking")
  bool bApplyYawToOwner = true;

  /** 插值平滑速度 */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AR Tracking",
            meta = (ClampMin = "0.0", ClampMax = "30.0"))
  float YawInterpSpeed = 8.f;

  /** 移动端原生传感器 Yaw
   * 乘数。如果手机传感器读数是弧度(0-2)，可以设为 57.29578 (即 180/PI)
   * 转换为角度 */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AR Tracking|Mobile")
  float MobileYawMultiplier = 57.29578f;

  /** 模拟行走速度 */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AR Tracking",
            meta = (ClampMin = "10.0", ClampMax = "1000.0"))
  float WalkSpeed = 400.f;

  /** 移动端：判定步伐的加速度阈值 */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AR Tracking|Mobile")
  float StepAccelerationThreshold = 0.5f;

  // ── 友元（UDP 线程需写入 RawUDPData） ─────────────────────────────────
#if !(PLATFORM_ANDROID || PLATFORM_IOS)
  friend class FAR_UDPReceiver;

  /** UDP 线程写入此字段（原子字符串，使用临界区保护） */
  FCriticalSection DataLock;
  FString RawUDPData; // "X,Y,Z,Yaw"
#endif

private:
  // ── 当前追踪状态 ──────────────────────────────────────────────────────
  FARTrackingData CurrentData;

  // 用于将 HTML 相对 Yaw 转换为世界 Yaw：首次收到数据时记录角色初始朝向
  bool bHasInitialYaw = false;
  float InitialWorldYaw = 0.f;

  // 移动端原生传感器基准
  float InitialSensorYaw = 0.f;
  bool bHasInitialSensorYaw = false;

  // ── PC 端 UDP 线程 ────────────────────────────────────────────────────
#if !(PLATFORM_ANDROID || PLATFORM_IOS)
  FAR_UDPReceiver *UDPReceiver = nullptr;
  FRunnableThread *UDPReceiverThread = nullptr;
#endif

  // ── 平台专用内部函数 ──────────────────────────────────────────────────

  /** 移动端：从原生陀螺仪更新状态 (代替 ARCore) */
  void UpdateFromSensors_Mobile(float DeltaTime);

  /** PC 端：解析 UDP 字符串，更新 CurrentData */
  void UpdateFromUDP();
};
