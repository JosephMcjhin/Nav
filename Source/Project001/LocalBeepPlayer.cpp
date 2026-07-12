#include "LocalBeepPlayer.h"

#if PLATFORM_WINDOWS
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include "Windows/HideWindowsPlatformTypes.h"
#endif

// ============================================================================
// 实现：FLocalBeepPlayer::FImpl
//
// 用 Windows waveOut API 流式播放 PCM，与 Android 端 BeepPlayer.kt 逻辑镜像。
// 仅 PLATFORM_WINDOWS 下编译；其他平台 FImpl 为空结构体，所有方法为空操作。
// ============================================================================
#if PLATFORM_WINDOWS
struct FLocalBeepPlayer::FImpl {
 public:
  // ── 合成参数 ────────────────────────────────────────────────────
  static constexpr int32 SampleRate = 44100;
  static constexpr int32 BeepMs = 120;        // 每声蜂鸣时长
  static constexpr int32 AttackMs = 6;
  static constexpr int32 ReleaseMs = 30;
  static constexpr float H2Gain = 0.15f;        // 二次谐波
  static constexpr float H3Gain = 0.07f;        // 三次谐波
  static constexpr float BaseAmplitude = 0.3f;  // 基础音量（与 Android 对齐）

  // ── 三缓冲 ──────────────────────────────────────────────────────
  static constexpr int32 BufferFrames = 4410;   // 100ms @44.1kHz
  static constexpr int32 BufferSamples = BufferFrames * 2;  // 立体声

  // ── 运行时状态 ──────────────────────────────────────────────────
  FThreadSafeBool bPlaying{false};
  FRunnableThread* Thread = nullptr;

  HWAVEOUT hWaveOut = nullptr;
  WAVEHDR WaveHeaders[3];
  int16* Buffers[3] = {nullptr, nullptr, nullptr};

  // 当前播放参数（线程读取，主线程写入）
  int32 DesiredFreqHz = 0;
  float DesiredPan = 0.0f;
  float DesiredVolume = 1.0f;
  float DesiredIntervalMs = 0.0f;

  FImpl() {
    FMemory::Memzero(WaveHeaders, sizeof(WaveHeaders));
  }

  ~FImpl() { StopInternal(); }

  void SetActive(bool bActive, int32 FreqHz, float Pan, float Volume, float IntervalMs) {
    if (!bActive) {
      StopInternal();
      return;
    }
    DesiredFreqHz = FMath::Max(FreqHz, 0);
    DesiredPan = FMath::Clamp(Pan, -1.0f, 1.0f);
    DesiredVolume = FMath::Clamp(Volume, 0.0f, 1.0f);
    DesiredIntervalMs = FMath::Max(IntervalMs, 0.0f);
    if (!bPlaying) {
      bPlaying = true;
      StartThread();
    }
  }

  void StopInternal() {
    bPlaying = false;
    if (Thread) {
      Thread->Kill(true);  // UE 等效 Join（带等待）
      delete Thread;
      Thread = nullptr;
    }
    CloseDevice();
  }

 private:
  // 嵌套 Runnable 前置声明（与下方 private 段的定义访问性一致，
  // 避免 /permissive- 模式下“访问性不一致”警告）
  class FBeepRunnable;

  void StartThread();  // 类外实现（依赖 FBeepRunnable 完整定义）

  void CloseDevice() {
    if (hWaveOut) {
      waveOutReset(hWaveOut);
      for (int32 i = 0; i < 3; ++i) {
        if (WaveHeaders[i].dwFlags & WHDR_PREPARED) {
          waveOutUnprepareHeader(hWaveOut, &WaveHeaders[i], sizeof(WAVEHDR));
        }
        if (Buffers[i]) {
          delete[] Buffers[i];
          Buffers[i] = nullptr;
        }
        FMemory::Memzero(&WaveHeaders[i], sizeof(WAVEHDR));
      }
      waveOutClose(hWaveOut);
      hWaveOut = nullptr;
    }
  }

  bool OpenDevice() {
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = SampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx,
                    0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
      return false;
    }
    for (int32 i = 0; i < 3; ++i) {
      Buffers[i] = new int16[BufferSamples];
      WaveHeaders[i].lpData = reinterpret_cast<LPSTR>(Buffers[i]);
      WaveHeaders[i].dwBufferLength = BufferSamples * sizeof(int16);
      WaveHeaders[i].dwFlags = 0;
      waveOutPrepareHeader(hWaveOut, &WaveHeaders[i], sizeof(WAVEHDR));
    }
    return true;
  }

  // 合成一段（Frames 帧）柔和立体声 PCM 数据到 OutBuf。
  // 维护跨 buffer 状态：Phase（相位）、LpPrev（低通）、Env（包络幅度 0..1）、
  // BeepPos（在当前节奏段内的位置）、bInBeep（当前是否在蜂鸣段）。
  // 实现"嘟嘟嘟"节奏：蜂鸣段（BeepMs）+ 静音段（GapMs）交替，
  // 每个蜂鸣段头尾应用 ADSR 包络消除爆破音。
  void SynthBuffer(int16* OutBuf, int32 Frames, double& Phase, float& LpPrev,
                   float& Env, int32& BeepPos, bool& bInBeep,
                   int32 FreqHz, float Pan, float Volume) {
    const double PhaseStep = 2.0 * PI * FreqHz / SampleRate;
    const float Theta = (Pan + 1.0f) * (PI / 4.0f);
    const float LeftGain = FMath::Cos(Theta);
    const float RightGain = FMath::Sin(Theta);
    const int32 AmpInt = FMath::RoundToInt(32767.0f * BaseAmplitude);
    const float LpAlpha = 0.5f;

    const int32 BeepSamples = SampleRate * BeepMs / 1000;
    const int32 AttackSamples = SampleRate * AttackMs / 1000;
    const int32 ReleaseSamples = SampleRate * ReleaseMs / 1000;
    // 间隔由 UE 下发（interval_ms），减 beep 时长即为 gap
    const int32 GapMs = FMath::Max<int32>(static_cast<int32>(DesiredIntervalMs) - BeepMs, 0);
    const int32 GapSamples = SampleRate * GapMs / 1000;

    for (int32 i = 0; i < Frames; ++i) {
      if (bInBeep) {
        // ADSR 包络
        float EnvTarget = 1.0f;
        if (BeepPos < AttackSamples) {
          EnvTarget = static_cast<float>(BeepPos) / AttackSamples;
        } else if (BeepPos > BeepSamples - ReleaseSamples) {
          const int32 RelPos = BeepPos - (BeepSamples - ReleaseSamples);
          EnvTarget =
              static_cast<float>(ReleaseSamples - RelPos) / ReleaseSamples;
        }
        EnvTarget = FMath::Clamp(EnvTarget, 0.0f, 1.0f);
        Env = Env + 0.5f * (EnvTarget - Env);

        // 基波 + 弱谐波
        const float Fundamental = FMath::Sin(Phase);
        const float H2 = FMath::Sin(2.0 * Phase);
        const float H3 = FMath::Sin(3.0 * Phase);
        const float Mixed = Fundamental + H2Gain * H2 + H3Gain * H3;
        LpPrev = LpPrev + LpAlpha * (Mixed - LpPrev);
        const int16 Sample = static_cast<int16>(FMath::Clamp(
            FMath::RoundToInt(LpPrev * AmpInt * Env * Volume),
            -32768, 32767));
        // 等功率立体声 pan
        OutBuf[i * 2] = static_cast<int16>(FMath::Clamp(
            FMath::RoundToInt(Sample * LeftGain), -32768, 32767));
        OutBuf[i * 2 + 1] = static_cast<int16>(FMath::Clamp(
            FMath::RoundToInt(Sample * RightGain), -32768, 32767));

        Phase += PhaseStep;
        if (Phase > 2.0 * PI) Phase -= 2.0 * PI;

        ++BeepPos;
        if (BeepPos >= BeepSamples) {
          bInBeep = false;
          BeepPos = 0;
          Env = 0.0f;
        }
      } else {
        // 静音段
        OutBuf[i * 2] = 0;
        OutBuf[i * 2 + 1] = 0;
        ++BeepPos;
        if (BeepPos >= GapSamples) {
          bInBeep = true;
          BeepPos = 0;
        }
      }
    }
  }

  // Runnable：后台线程循环填充 waveOut 缓冲区
  class FBeepRunnable : public FRunnable {
   public:
    explicit FBeepRunnable(FImpl& InOwner) : Owner(InOwner) {}
    virtual uint32 Run() override {
      if (!Owner.OpenDevice()) return 1;

      double Phase = 0.0;
      float LpPrev = 0.0f;
      float Env = 0.0f;
      int32 BeepPos = 0;
      bool bInBeep = true;
      int32 BufIdx = 0;
      int32 ActiveFreq = Owner.DesiredFreqHz;
      float ActivePan = Owner.DesiredPan;
      float ActiveVolume = Owner.DesiredVolume;
      float ActiveIntervalMs = Owner.DesiredIntervalMs;

      // 标记所有 buffer 为 DONE，使第一次循环就能填充
      for (int32 i = 0; i < 3; ++i) {
        Owner.WaveHeaders[i].dwFlags |= WHDR_DONE;
      }

      while (Owner.bPlaying) {
        if (Owner.DesiredFreqHz != ActiveFreq ||
            Owner.DesiredPan != ActivePan ||
            Owner.DesiredVolume != ActiveVolume) {
          ActiveFreq = Owner.DesiredFreqHz;
          ActivePan = Owner.DesiredPan;
          ActiveVolume = Owner.DesiredVolume;
          LpPrev = 0.0f;  // 参数变化时重置低通，避免瞬态
        }

        WAVEHDR& Hdr = Owner.WaveHeaders[BufIdx];
        if (!(Hdr.dwFlags & WHDR_DONE)) {
          FPlatformProcess::Sleep(0.005f);
          continue;
        }

        Owner.SynthBuffer(Owner.Buffers[BufIdx], BufferFrames, Phase, LpPrev,
                          Env, BeepPos, bInBeep,
                          ActiveFreq, ActivePan, ActiveVolume);

        Hdr.dwFlags &= ~(WHDR_DONE | WHDR_INQUEUE);
        waveOutWrite(Owner.hWaveOut, &Hdr, sizeof(WAVEHDR));

        BufIdx = (BufIdx + 1) % 3;
      }
      return 0;
    }
   private:
    FImpl& Owner;
  };
};

// StartThread 类外实现（此时 FBeepRunnable 已完整定义）
void FLocalBeepPlayer::FImpl::StartThread() {
  Thread = FRunnableThread::Create(
      new FBeepRunnable(*this), TEXT("LocalBeepPlayer"),
      0, TPri_BelowNormal);
}
#else  // !PLATFORM_WINDOWS
struct FLocalBeepPlayer::FImpl {};  // 非 Windows 平台空实现
#endif  // PLATFORM_WINDOWS

// ============================================================================
// FLocalBeepPlayer 公开接口（转发到 PImpl）
// ============================================================================
FLocalBeepPlayer& FLocalBeepPlayer::Get() {
  static FLocalBeepPlayer Instance;
  return Instance;
}

FLocalBeepPlayer::FLocalBeepPlayer() : Impl(new FImpl()) {}

FLocalBeepPlayer::~FLocalBeepPlayer() {
  delete Impl;
  Impl = nullptr;
}

void FLocalBeepPlayer::SetActive(bool bActive, int32 FreqHz, float Pan,
                                 float Volume, float IntervalMs) {
#if PLATFORM_WINDOWS
  Impl->SetActive(bActive, FreqHz, Pan, Volume, IntervalMs);
#else
  // 非 Windows 平台无操作（debug 调试主要在 Windows 编辑器进行）
#endif
}

void FLocalBeepPlayer::Stop() {
#if PLATFORM_WINDOWS
  Impl->StopInternal();
#endif
}
