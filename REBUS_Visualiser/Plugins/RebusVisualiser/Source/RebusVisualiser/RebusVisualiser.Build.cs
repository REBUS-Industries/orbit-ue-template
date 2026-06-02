// Copyright REBUS Industries. RebusVisualiser fixture-control + PS2 data-channel module.
using UnrealBuildTool;

public class RebusVisualiser : ModuleRules
{
	public RebusVisualiser(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"InputCore",
			"Projects",

			// HTTP / JSON for the /api/ue/* REST contract and data-channel descriptors.
			"HTTP",
			"Json",
			"JsonUtilities",

			// Rendering helpers (UTextureLightProfile / UTexture2D construction for IES + gobos).
			"RenderCore",
			"RHI",

			// Runtime fixture geometry proxies built from /api/ue/fixtures/{id}/meshes.
			"ProceduralMeshComponent",

			// Pixel Streaming 2 (UE 5.5+). The streamer + input handler live in these modules;
			// names may need a tweak per exact 5.7 layout (see Plugins/RebusVisualiser/README.md).
			"PixelStreaming2",
			"PixelStreaming2Core",
			"PixelStreaming2Input",
		});
	}
}
