# REBUS template content

This template is consumed by the PRISM Visualiser orchestrator. The orchestrator
clones this repo, imports an ORBIT model via Interchange, and launches the editor
in `-game` mode with Pixel Streaming.

The orchestrator's Python importer expects these assets to exist:

| Path | What it is | Status |
| --- | --- | --- |
| `Content/REBUS/Maps/BaseLevel.umap` | Empty level with SkyAtmosphere, VolumetricCloud, DirectionalLight, SkyLight, ExponentialHeightFog, PostProcessVolume, 1×1km neutral landscape | **TODO — artist** |
| `Content/REBUS/BP/BP_OrbitImporter.uasset` | Blueprint with a Python-callable function `ImportGltf(gltfPath, targetFolder, levelName)` wrapping Interchange | **TODO — artist** |
| `Content/REBUS/Materials/M_DefaultLit.uasset` | Fallback master material for ORBIT meshes missing a `RenderMaterial` | **TODO — artist** |

When all three are present, tag the repo `v1.0.0-ue5.7` and the orchestrator
will start fetching this tag.
