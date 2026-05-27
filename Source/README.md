# Source

**No C++ in this template — keep everything in Blueprints + Python.**

This directory exists only to satisfy the standard Unreal project layout. Adding
C++ source here would force a per-engine, per-platform compile step into the
PRISM Visualiser orchestrator pipeline, which we explicitly want to avoid:

- The orchestrator clones this repo and launches `UnrealEditor.exe -game` against
  the project as-is. No `Build.bat`, no Visual Studio, no UBT.
- Artists can iterate on the level, master material, and the Python-callable
  importer Blueprint without ever opening a code IDE.
- The release zip stays small (no `Binaries/`, no per-platform DLLs).

If you ever genuinely need native code — for example, a custom Pixel Streaming
input handler that can't be expressed in Blueprints — please raise it as a
discussion in [REBUS-ORBIT/orbit-ue-template](https://github.com/REBUS-ORBIT/orbit-ue-template/discussions)
**before** adding a `Source/` module. We'd rather extend the Python plugin or
ship a sibling helper plugin than break the no-compile property of this template.
