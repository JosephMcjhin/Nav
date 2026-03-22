// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Project001 : ModuleRules
{
	public Project001(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// ─── 公共依赖（所有平台） ───────────────────────────────────────
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"NavigationSystem",  // NavMesh path finding
			"AIModule",          // UAIBlueprintHelperLibrary
			"Json",              // JSON parsing
			"JsonUtilities",     // FJsonObjectConverter
			"AugmentedReality",  // AR public API (all platforms, safe to include)
			"Voice",             // IVoiceCapture
			"WebSockets",        // IWebSocket
			"TextToSpeech",      // TTSQueueComponent
			"UMG",               // UUserWidget / UButton / UTextBlock
			"Http",              // FHttpModule (calibration HTTP POST)
			"Slate",             // FSlateColor
			"SlateCore",         // Slate primitives
		});

		// ─── PC / Editor 专用：TCP/UDP Socket 服务器 ──────────────────
		// 移动端原生打包不需要 TCP 服务器，这些模块不会被链接
		bool bIsMobile = Target.Platform == UnrealTargetPlatform.Android
		              || Target.Platform == UnrealTargetPlatform.IOS;

		if (!bIsMobile)
		{
			PublicDependencyModuleNames.AddRange(new string[]
			{
				"Sockets",           // TCP socket server (PC / Editor)
				"Networking",        // FTcpSocketBuilder / IPv4 helpers
			});
		}

		// ─── 移动端 AR 原生后端说明 ─────────────────────────────────────
		// GoogleARCore / AppleARKit 是平台 SDK 后端插件，UBT 会在对应平台
		// 打包时自动加载（通过 uproject 的 Plugins 声明）。
		// 无需在此处显式 Add 私有依赖 —— UARBlueprintLibrary 等 AR 公共 API
		// 已经包含在 AugmentedReality 公共模块中，可直接调用。

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}

