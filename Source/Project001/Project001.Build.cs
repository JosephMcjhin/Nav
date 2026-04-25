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
			"WebSockets",        // IWebSocket
			"UMG",               // UUserWidget / UButton / UTextBlock
			"Http",              // FHttpModule (calibration HTTP POST)
			"Slate",             // FSlateColor
			"SlateCore",         // Slate primitives
		});

		// 依赖项清理完成，不再加载AR、Sockets和Networking

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
