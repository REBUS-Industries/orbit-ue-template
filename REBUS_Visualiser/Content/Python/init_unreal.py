"""
Editor startup hook for the RebusVisualiser project.

The Unreal Python plug-in automatically discovers and runs `init_unreal.py` on
any Content/Python path at editor startup. We use it to guarantee the base level
exists, so opening the project always lands on the populated stage (fog +
post-process + sun/sky) instead of an empty fallback level when
Content/REBUS/Maps/BaseLevel.umap has not been baked yet.

Idempotent: if the level already exists this does nothing -- the EditorStartupMap
entry in Config/DefaultEngine.ini loads it. It only generates (and then opens)
the level on the first launch, or after the asset has been deleted.
"""
import re

import unreal

try:
    import build_rebus_base_level as base
except Exception as exc:  # noqa: BLE001
    unreal.log_error("RebusBaseLevel: could not import builder ({})".format(exc))
    base = None

_post_tick_handle = None


def _is_editor_authoring_context():
    """True only in a real editor session that can author/save levels + assets.

    The Python plug-in runs this start-up script in *any* launch that enables
    Python -- including the product's runtime launch, which runs the editor
    binary in `-game` mode (no editor world, GEditor == null). Calling the
    editor-scripting libraries (EditorAssetLibrary / EditorActorSubsystem /
    LevelEditorSubsystem / MaterialEditingLibrary) in that context dereferences
    a null editor world and crashes the process with an access violation
    (observed: -game Pixel Streaming launch). The C++
    URebusVisualiserSubsystem::EnsureSceneEnvironment() already backstops the
    scene at runtime, so the authoring step must simply no-op outside the editor.
    """
    try:
        cmd = unreal.SystemLibrary.get_command_line() or ""
    except Exception:  # noqa: BLE001
        cmd = ""
    # Runtime / non-authoring launches: -game, dedicated/listen -server, commandlets.
    if re.search(r"(?:^|\s)-(game|server)(?:\s|=|$)", cmd, re.IGNORECASE):
        return False
    # Need a live editor (GEditor); absent in -game even though the class exists.
    try:
        return unreal.get_editor_subsystem(unreal.LevelEditorSubsystem) is not None
    except Exception:  # noqa: BLE001
        return False


def _ensure_and_load():
    if base is None:
        return
    try:
        if base.ensure_base_level():
            les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
            les.load_level(base.LEVEL_PACKAGE_PATH)
            unreal.log("RebusBaseLevel: generated + loaded at startup.")
        # Self-heal the floor into whatever BaseLevel is now open (covers levels
        # baked before the floor feature existed).
        base.ensure_floor_in_level()
    except Exception as exc:  # noqa: BLE001
        unreal.log_error("RebusBaseLevel: startup ensure failed ({})".format(exc))


def _on_post_tick(delta_seconds):
    # Fire exactly once, then unregister.
    global _post_tick_handle
    if _post_tick_handle is not None:
        unreal.unregister_slate_post_tick_callback(_post_tick_handle)
        _post_tick_handle = None
    _ensure_and_load()


# Only author the base level in a real editor session. In the product's -game
# runtime launch GEditor is null, so running the editor-scripting libraries here
# crashes the process -- skip it (the C++ subsystem backstops the scene instead).
if not _is_editor_authoring_context():
    unreal.log("RebusBaseLevel: non-editor (runtime) launch; skipping level authoring.")
# At init_unreal time the editor world / asset subsystems may not be ready to
# create + save a level, so defer one Slate tick when running the full editor.
# Fall back to running inline (e.g. a headless commandlet with no Slate loop).
elif hasattr(unreal, "register_slate_post_tick_callback"):
    try:
        _post_tick_handle = unreal.register_slate_post_tick_callback(_on_post_tick)
    except Exception:  # noqa: BLE001
        _ensure_and_load()
else:
    _ensure_and_load()
