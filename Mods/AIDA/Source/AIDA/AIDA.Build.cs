using UnrealBuildTool;
using System.IO;
using System;

public class AIDA : ModuleRules
{
	public AIDA(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		// Core engine + gameplay dependencies.
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject",
			"Engine",
			"Projects",                    // IPluginManager — locate the mod's shipped config at runtime
			"DeveloperSettings",
			"InputCore",
			"NetCore",
			"GameplayTags",
			"SlateCore", "Slate", "UMG",   // ChatWidget / ProposalUI (client-only code guarded by Content/ + WITH_EDITOR checks)
			"Json", "JsonUtilities",       // config parsing, tool schemas, adapter wire formats
			"HTTP"                         // FHttpModule — all LLM egress (server only)
		});

		// Header stubs required by FactoryGame builds.
		PublicDependencyModuleNames.AddRange(new string[] {
			"DummyHeaders"
		});

		// The game + mod loader. FactoryGame headers must only be *used* from Index/ and Actions/
		// per the game-API isolation rule (docs/DEV.md coding rule 3).
		PublicDependencyModuleNames.AddRange(new string[] {
			"FactoryGame", "SML"
		});

		PublicIncludePaths.AddRange(new string[] {
			// ... add public include paths required here ...
		});

		PrivateIncludePaths.AddRange(new string[] {
			// ... add private include paths required here ...
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			// ... add private dependencies that you statically link with here ...
		});

		DynamicallyLoadedModuleNames.AddRange(new string[] {
			// ... add any modules that your module loads dynamically here ...
		});
	}
}
