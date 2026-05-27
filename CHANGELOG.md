# Changelog

Release notes are auto-extracted into the matching GitHub Release body on tag push.
Format: `## vX.Y.Z-suffix — Optional title` followed by bullets.

---

## v0.1.0-ue5.7-scaffold — Initial scaffold

- REBUSVis.uproject with PixelStreaming2, Interchange, PythonScriptPlugin,
  EditorScriptingUtilities, DMX plugins enabled.
- DefaultEngine.ini tuned for Lumen + VSM + TSR + Pixel Streaming 2 defaults.
- Placeholder Content/REBUS/{Maps,BP,Materials}/.gitkeep folders awaiting artist work.
- Release CI: tag pushes auto-zip the project and attach to GitHub Releases.

## Next milestone — v1.0.0-ue5.7

- Add BaseLevel.umap (artist).
- Add BP_OrbitImporter.uasset (artist + Python integration).
- Add M_DefaultLit.uasset (artist).
- Smoke-test orchestrator can clone + import a glTF cleanly.
