// Copyright REBUS Industries. Minimal primary game module for the visualiser
// template. Exists only to give the project a compilable code module so the
// in-tree ORBIT/glTFRuntime plugins build in-context.
using UnrealBuildTool;

public class REBUS_Visualiser : ModuleRules
{
	public REBUS_Visualiser(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
