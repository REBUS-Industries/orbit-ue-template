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
import unreal

try:
    import build_rebus_base_level as base
except Exception as exc:  # noqa: BLE001
    unreal.log_error("RebusBaseLevel: could not import builder ({})".format(exc))
    base = None

_post_tick_handle = None


def _ensure_and_load():
    if base is None:
        return
    try:
        if base.ensure_base_level():
            les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
            les.load_level(base.LEVEL_PACKAGE_PATH)
            unreal.log("RebusBaseLevel: generated + loaded at startup.")
    except Exception as exc:  # noqa: BLE001
        unreal.log_error("RebusBaseLevel: startup ensure failed ({})".format(exc))


def _on_post_tick(delta_seconds):
    # Fire exactly once, then unregister.
    global _post_tick_handle
    if _post_tick_handle is not None:
        unreal.unregister_slate_post_tick_callback(_post_tick_handle)
        _post_tick_handle = None
    _ensure_and_load()


# At init_unreal time the editor world / asset subsystems may not be ready to
# create + save a level, so defer one Slate tick when running the full editor.
# Fall back to running inline (e.g. a headless commandlet with no Slate loop).
if hasattr(unreal, "register_slate_post_tick_callback"):
    try:
        _post_tick_handle = unreal.register_slate_post_tick_callback(_on_post_tick)
    except Exception:  # noqa: BLE001
        _ensure_and_load()
else:
    _ensure_and_load()
