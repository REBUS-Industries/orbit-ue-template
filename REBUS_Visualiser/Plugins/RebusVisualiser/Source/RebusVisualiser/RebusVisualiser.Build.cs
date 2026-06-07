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

			// v1.0.79 cinematic camera pawn (UCineCameraComponent + manual exposure).
			"CinematicCamera",

			// Pixel Streaming 2 (UE 5.5+). The streamer + input handler live in these modules;
			// names may need a tweak per exact 5.7 layout (see Plugins/RebusVisualiser/README.md).
			"PixelStreaming2",
			"PixelStreaming2Core",
			"PixelStreaming2Input",
		});

		// v1.0.119 -- `FAssetCompilingManager::Get().FinishAllCompilation()` for
		// the post-regen flush in `RebuildAndVerifyBeamMaster` ships INSIDE the
		// engine module (header at `Engine/Source/Runtime/Engine/Public/Asset
		// CompilingManager.h`, declared `ENGINE_API`). No separate module
		// dependency is needed -- `Engine` is already a PublicDependencyModule
		// above. The `#if WITH_EDITOR` gate around the call site prevents the
		// flush from running in packaged builds where async compile is not a
		// concern (cooked materials skip the compile path entirely).
	}
}
