"""
Build the RebusVisualiser base level.

Creates a blank level at /Game/REBUS/Maps/BaseLevel containing the default, portal-controllable
scene environment the visualiser runs from:

  * ExponentialHeightFog  -- global (full extent by nature), volumetric fog ENABLED.
  * PostProcessVolume     -- Infinite Extent (Unbound) = full extent, blend weight 1.
  * DirectionalLight (sun) + SkyLight + SkyAtmosphere -- so the stage renders and the
    portal's Studio-Light catalogue (KeyLightIntensity / SkyLight* / SunYaw...) has actors
    to drive.

These are exactly the actors the scene-settings subsystem (URebusSceneSettingsSubsystem)
binds the SetSceneProperty catalogue onto (ue-plugin-build-guide.md §9), so everything here is
controllable from the portal at runtime. The RebusVisualiser session subsystem also
find-or-spawns the fog + post-process volume at launch, so the stream still works even on a
truly empty level -- this script just bakes them in as the default.

Run it from the editor:
    Tools > Execute Python Script... > build_rebus_base_level.py
or headless (the pattern PRISM already uses for the importer):
    UnrealEditor REBUS_Visualiser.uproject -run=pythonscript -script="build_rebus_base_level.py"

After it runs, point GameDefaultMap / EditorStartupMap at /Game/REBUS/Maps/BaseLevel
(already wired in Config/DefaultEngine.ini).
"""

import unreal

LEVEL_PACKAGE_PATH = "/Game/REBUS/Maps/BaseLevel"


def _set(obj, name, value):
    """set_editor_property that logs instead of throwing on a renamed property."""
    try:
        obj.set_editor_property(name, value)
    except Exception as exc:  # noqa: BLE001
        unreal.log_warning("RebusBaseLevel: could not set '{}' = {} ({})".format(name, value, exc))


def _spawn(actor_class, location=None, rotation=None, label=None):
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    loc = location or unreal.Vector(0.0, 0.0, 0.0)
    rot = rotation or unreal.Rotator(0.0, 0.0, 0.0)
    actor = eas.spawn_actor_from_class(actor_class, loc, rot)
    if actor and label:
        actor.set_actor_label(label)
    return actor


def _component(actor, component_class):
    """Fetch an actor's component by class.

    Robust across engine versions -- avoids guessing the per-actor editor-property
    name for the component (which differs between UE releases and is what broke on
    5.7). Returns None and logs if the component can't be found.
    """
    comp = actor.get_component_by_class(component_class)
    if comp is None:
        unreal.log_warning("RebusBaseLevel: no {} on {}".format(
            component_class.__name__, actor.get_actor_label()))
    return comp


def _add_exponential_height_fog():
    fog = _spawn(unreal.ExponentialHeightFog, unreal.Vector(0.0, 0.0, 200.0), label="RebusHeightFog")
    if not fog:
        return
    comp = _component(fog, unreal.ExponentialHeightFogComponent)
    if not comp:
        return
    # Exponential height fog is inherently a single global actor (full extent).
    _set(comp, "fog_density", 0.02)
    _set(comp, "fog_height_falloff", 0.2)
    _set(comp, "start_distance", 0.0)
    # Volumetric fog ON so per-fixture beams scatter (§8.4).
    _set(comp, "volumetric_fog", True)
    _set(comp, "volumetric_fog_scattering_distribution", 0.2)
    _set(comp, "volumetric_fog_extinction_scale", 1.0)
    unreal.log("RebusBaseLevel: ExponentialHeightFog added (volumetric fog on).")


def _add_post_process_volume():
    ppv = _spawn(unreal.PostProcessVolume, label="RebusPostProcess")
    if not ppv:
        return
    # Infinite Extent (Unbound) = applies everywhere / full extent.
    _set(ppv, "unbound", True)
    _set(ppv, "blend_weight", 1.0)
    _set(ppv, "priority", 0.0)
    unreal.log("RebusBaseLevel: PostProcessVolume added (unbound / full extent).")


def _add_lighting():
    # Sun.
    sun = _spawn(unreal.DirectionalLight, rotation=unreal.Rotator(-45.0, 200.0, 0.0), label="RebusSun")
    if sun:
        comp = _component(sun, unreal.DirectionalLightComponent)
        if comp:
            _set(comp, "intensity", 6.0)
            _set(comp, "mobility", unreal.ComponentMobility.MOVABLE)
            _set(comp, "cast_shadows", True)

    # Sky light (real-time capture so it follows the atmosphere).
    sky = _spawn(unreal.SkyLight, location=unreal.Vector(0.0, 0.0, 300.0), label="RebusSkyLight")
    if sky:
        comp = _component(sky, unreal.SkyLightComponent)
        if comp:
            _set(comp, "real_time_capture", True)
            _set(comp, "intensity", 1.0)
            _set(comp, "mobility", unreal.ComponentMobility.MOVABLE)

    # Sky atmosphere so the sky/skylight have something to sample.
    _spawn(unreal.SkyAtmosphere, label="RebusSkyAtmosphere")
    unreal.log("RebusBaseLevel: sun + sky light + sky atmosphere added.")


def _populate():
    # Isolate each section so a failure in one (e.g. a renamed property on a
    # future engine drop) still lets the others spawn.
    for step in (_add_exponential_height_fog, _add_post_process_volume, _add_lighting):
        try:
            step()
        except Exception as exc:  # noqa: BLE001
            unreal.log_error("RebusBaseLevel: {} failed ({})".format(step.__name__, exc))


def build():
    """Force-(re)create the base level from scratch and save it.

    Replaces whatever level is currently open, so call this only for an explicit
    (re)bake (Tools > Execute Python Script / headless -run=pythonscript).
    """
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)

    # Create a fresh, empty level (replaces whatever is open) and make it current.
    les.new_level(LEVEL_PACKAGE_PATH)

    _populate()

    les.save_current_level()
    unreal.log("RebusBaseLevel: saved {}".format(LEVEL_PACKAGE_PATH))


def ensure_base_level():
    """Generate the base level only if it does not already exist.

    Returns True if it had to build the level, False if it was already present.
    This is the idempotent entry point the startup hook (init_unreal.py) uses so
    opening the project always lands on a populated stage without clobbering an
    existing BaseLevel on every launch.
    """
    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PACKAGE_PATH):
        return False
    unreal.log("RebusBaseLevel: '{}' missing; generating it.".format(LEVEL_PACKAGE_PATH))
    build()
    return True


if __name__ == "__main__":
    build()
