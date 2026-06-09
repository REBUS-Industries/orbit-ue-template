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
or headless (the v1.0.121 commandlet pattern -- this is the canonical bake driver):
    UnrealEditor-Cmd.exe REBUS_Visualiser.uproject -run=PythonScript ^
        -Script="import build_rebus_base_level as b; b.ensure_beam_material(force=True); b.ensure_ies_profiles(force=True)" ^
        -unattended -nop4 -nosplash -stdout -FullStdOutLogOutput

After it runs, point GameDefaultMap / EditorStartupMap at /Game/REBUS/Maps/BaseLevel
(already wired in Config/DefaultEngine.ini).
"""

import os
import re

import unreal


# v1.0.120 STOP-THE-BLEEDING editor-runtime guard. Every public entry point in
# this module (ensure_beam_material, ensure_ies_profiles, ensure_ground_materials,
# ensure_lens_material, ensure_fixture_lens_material, ensure_orbit_imported_material,
# build, ensure_base_level) calls editor-only Unreal Python APIs that live in the
# `EditorScriptingUtilities` module (unreal.EditorAssetLibrary.*,
# unreal.MaterialEditingLibrary.*, unreal.AssetToolsHelpers.get_asset_tools()).
# These APIs ONLY function inside an editor-class process. When the editor binary
# is launched in -game mode (the standard PRISM Pixel Streaming orchestrator
# command line: `UnrealEditor-Cmd.exe ... -game -PixelStreamingURL=...`), the
# editor subsystems are NOT initialised; calling EditorAssetLibrary in this
# mode dereferences uninitialised state and CRASHES the process with
# EXCEPTION_ACCESS_VIOLATION on the `EditorScriptingUtilities.dll` frame.
# Exactly the v1.0.119 user-reported crash, stack trace verbatim:
#     UnrealEditor-Engine.dll
#     UnrealEditor-EditorScriptingUtilities.dll  <-- editor-only API
#     UnrealEditor-CoreUObject.dll
#     UnrealEditor-PythonScriptPlugin.dll
#     python311.dll
# v1.0.121 CORRECTION (post-bake-attempt-1 empirical probe).
#
# The v1.0.120 guard called `unreal.SystemLibrary.is_editor()`. That symbol
# DOES NOT EXIST on `SystemLibrary` in UE 5.7's `unreal` Python bindings
# (verified: `dir(unreal.SystemLibrary)` does not contain `is_editor`, only
# `is_dedicated_server`, `is_server`, `is_standalone`, `is_split_screen`,
# `is_unattended`, etc. -- all of which require a `world_context_object`
# parameter Python doesn't have in a commandlet context). The v1.0.120 call
# raised `AttributeError("type object 'SystemLibrary' has no attribute
# 'is_editor'")`, was swallowed by the `except Exception` clause, and returned
# False every time -- meaning the v1.0.120 "stop-the-bleeding" guard has been
# unconditionally aborting EVERY entry point (editor-interactive, commandlet,
# -game, all of it) since v1.0.120 shipped. That's why both v1.0.120 and the
# v1.0.121 first-attempt commandlet bake immediately reported the abort.
#
# The CORRECT API in UE 5.7 is the module-level `unreal.is_editor()`. That
# returns True for editor-interactive sessions AND for commandlets (both have
# `GIsEditor=true`), and False for `-game` / dedicated-server / Standalone
# runtime. Empirically verified in this engine build:
#   * Commandlet (`-run=PythonScript ...`) -> `unreal.is_editor() == True`
#   * Editor-interactive -> True (by construction)
#   * `-game` -> False
#
# This mirrors the C++-side gate `URebusVisualiserSubsystem::
# CanRegenBeamMasterInProcess()` (`RebusVisualiserSubsystem.cpp`, anonymous
# namespace: `GIsEditor && !IsRunningGame()` -- v1.0.121 dropped the
# `!IsRunningCommandlet()` clause specifically so this commandlet bake path
# could land). With the right API, both editor and commandlet pass; `-game` is
# blocked. Belt-and-braces with the C++ gate keeps the v1.0.119 stack-frame-
# perfect crash impossible regardless of entry point.
def _is_editor_runtime():
    try:
        is_editor_fn = getattr(unreal, "is_editor", None)
        if is_editor_fn is None:
            # Future engine where the module-level helper moves -- best-effort
            # fallback via the editor subsystem (returns None in -game / -server
            # where editor subsystems are not registered).
            try:
                subsys = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
                return subsys is not None
            except Exception:  # noqa: BLE001
                return False
        return bool(is_editor_fn())
    except Exception:  # noqa: BLE001
        # Best-effort -- if the unreal binding moves in a future engine version,
        # treat as "not editor" so we never crash. The caller logs a warning +
        # returns the same way as the explicit-False path.
        return False


LEVEL_PACKAGE_PATH = "/Game/REBUS/Maps/BaseLevel"

MATERIALS_DIR = "/Game/REBUS/Materials"
GROUND_MASTER_PATH = MATERIALS_DIR + "/M_RebusGround"

# v1.0.93: Python-authored masters that previously had to be operator-built (or were not
# baked at all) so the user-reported "gobo not on the volumetric beam shaft" + "lens should
# be a chrome mirror" + "shaft visible inside the head" trio can be solved without anyone
# touching the editor. See the v1.0.93 README block for the full diagnosis (root cause:
# Epic's stock MI_Light light-function material does NOT have bUsedWithVolumetricFog=true,
# which is the per-material flag that gates whether a LF reaches the volumetric integrator
# -- no amount of CVar pushing fixes that; the material itself has to be authored with the
# flag flipped).
#
# FIXTURE_LENS_PATH (Fix 3) -- mirror/glass material for the GDTF <Beam> lens disc. The C++
# constructor (ConstructorHelpers::FObjectFinder for FixtureLensMaterialOverride) already
# auto-loads it from this path; v1.0.93 makes the Python builder bake it at startup so the
# operator doesn't have to author it manually. Metallic=1, Roughness=0, BaseColor=white.
FIXTURE_LENS_PATH = MATERIALS_DIR + "/M_RebusFixtureLens"

# v1.0.95 migration: legacy material assets `_cleanup_internal_beam_assets` deletes on
# first launch. See the README v1.0.95 release block for the full audit.
_LEGACY_INTERNAL_BEAM_ASSETS = (
    MATERIALS_DIR + "/M_RebusGoboLightFunction",
    MATERIALS_DIR + "/M_RebusInternalBeamShaft",
)

# Emissive "glowing lens" flare disc material (ue-plugin-build-guide.md §8.3a). Unlit, two-sided,
# ADDITIVE so a radial UV mask reads as a round soft-edged lens regardless of facing AND so the
# disc is invisible when the fixture is dark (additive adds nothing at EmissiveStrength 0) -- a
# translucent disc would instead show as a black card when off. The C++ fixture actor
# (RebusFixtureActor::BuildLensDisc) loads it by this path, makes a MID per fixture and drives
# EmissiveColor (linear fixture colour) + EmissiveStrength (dimmer x output) live.
LENS_FLARE_PATH = MATERIALS_DIR + "/M_RebusLensFlare"

# Hybrid volumetric-beam master (v1.0.31). Unlit + two-sided + ADDITIVE faux-volumetric cone
# shader: a soft on-axis core falling off toward the silhouette (1 - Fresnel), a length fade from
# the lens toward the throw (BeamFalloff), and a DepthFade so the shaft soft-clips into geometry /
# the near camera (camera-occlusion). The C++ fixture actor (RebusFixtureActor::BuildBeamCone)
# loads it by this path, makes a MID per fixture and drives BeamColor (linear fixture colour) +
# BeamIntensity (dimmer x output x SetFixtureBeamVolumetrics) live; the cone MESH carries the IES
# sizing (lens-radius base, fieldAngle far radius). Phase 2 will swap the body for a true raymarch
# + light-blocking volumetric shadows.
BEAM_PATH = MATERIALS_DIR + "/M_RebusBeam"

# v1.0.104 -- two-sided opaque master operators can re-parent Orbit-imported (glTFRuntime
# baked) materials to so thin geometry (truss cross-bars, banner cloth, sheet-metal flags)
# renders from BOTH sides. Authored alongside M_RebusGround / M_RebusBeam etc. because
# glTFRuntime's default materials are hard-baked single-sided at cook time and the engine
# offers no MID-level runtime override for the top-level Material `two_sided` flag (it's
# part of the shader-permutation key), so the only way to truly draw back faces on
# imported assets is to swap the parent for a master authored two-sided. The C++ post-
# import walker (URebusVisualiserSubsystem::EnsureImportedDoubleSided) does NOT auto-swap
# parents on glTFRuntime imports -- doing so would lose the source's PBR textures /
# vertex-colour wiring; the walker instead pushes the `bTwoSidedScalar` parameter (which
# this master exposes alongside its hard-baked `two_sided = True` for forward-compat with
# a future render-side toggle). This master is therefore a RESOURCE operators can manually
# assign to specific Orbit assets via OrbitConnector's import-material override path when
# they need genuine back-face rendering; the C++ shadow-side fix (bCastShadowAsTwoSided)
# is the always-on path that covers the v1.0.99-introduced thin-geometry shadow-flip
# regression without requiring any operator action. See the v1.0.104 README block.
ORBIT_IMPORTED_PATH = MATERIALS_DIR + "/M_RebusOrbitImported"

# v1.0.121 -- pre-baked IES profile asset family. `IESConverter.h` (the engine's runtime
# IES->UTextureLightProfile builder) is editor-only, so the v1.0.120 runtime path through
# `RebusIes::BuildLightProfile` would log
#     IESConverter.h not available in this engine build; cannot load IES at runtime.
#     Falling back to the synthesized cone.
# under any -game launch -- which is the standard PRISM Pixel Streaming orchestrator
# command line. v1.0.121 mirrors the M_RebusBeam pre-bake pattern for IES: the
# editor / commandlet bake converts every .ies file on disk into a UTextureLightProfile
# .uasset; the runtime just LoadObject<UTextureLightProfile> the pre-baked asset by name.
# Layout:
#   * IES_SOURCE_DIR        -- on-disk folder of source `.ies` files the bake walks. Add
#                              new profiles here (committed to git) to make them available.
#   * IES_INBOX_DIR_SAVED   -- absolute-disk capture folder under <ProjectSaved>/REBUS/
#                              IES_Inbox/. The runtime IES descriptor handler
#                              (`URebusVisualiserSubsystem::RegisterFixtureIes`) writes
#                              inline-pushed IES bytes here when running in editor /
#                              commandlet mode (gated by the same `GIsEditor &&
#                              !IsRunningGame()` check), so the NEXT commandlet bake
#                              picks them up automatically. Operator workflow:
#                                1. Run the visualiser in editor / commandlet mode
#                                   with the portal connected -- captures the inline
#                                   IES bytes the portal pushes per fixture library.
#                                2. Re-run the v1.0.121 bake commandlet -- bakes the
#                                   captured .ies files into /Game/REBUS/IES/.
#                                3. Commit the produced .uasset files.
#                              The capture path is NOT enabled in -game mode (would
#                              write into the wrong runtime context); the inline IES
#                              cache still drives the v1.0.91 runtime BuildLightProfile
#                              path in -game even when no pre-baked asset exists (same
#                              behaviour as v1.0.120, just with the additional pre-baked
#                              priority).
#   * IES_PACKAGE_DIR       -- /Game/-rooted package path under which the baked
#                              UTextureLightProfile assets live. The C++ runtime cache
#                              `URebusVisualiserSubsystem::GetCachedIesProfile(FName)`
#                              composes `IES_PACKAGE_DIR + "/" + SanitizeIesProfileName(id)`
#                              and LoadObject<UTextureLightProfile>'s the result.
#   * REBUS_IES_PROFILE_REVISION -- bake-cadence sentinel written as the metadata tag
#                              `IesProfileRevision` on every baked asset (visible in
#                              the editor's asset metadata + the on-disk
#                              .uasset). Bump in lockstep with REBUS_BEAM_MATERIAL_REVISION
#                              so a v1.0.121 deployment can audit "every baked IES profile
#                              is at the current revision" with one expression. Lives at
#                              the same numerical value as the beam revision so the
#                              operator only has to remember one number.
IES_PACKAGE_DIR = "/Game/REBUS/IES"
IES_SOURCE_DIR = "REBUS_Visualiser/Content/REBUS/IES/Source"
IES_INBOX_DIR_SAVED = "REBUS/IES_Inbox"  # joined under <ProjectSaved> at bake time
REBUS_IES_PROFILE_REVISION = 121

# Portal-controllable ground surface presets: name -> (ColorA, ColorB, Roughness).
# These drive the procedural M_RebusGround master (no imported image textures needed);
# the C++ scene-settings subsystem swaps between the generated instances on SetSceneProperty
# name="GroundSurface". Keep these names in sync with SetGroundSurface() in
# RebusSceneSettingsSubsystem.cpp.
GROUND_PRESETS = {
    "Concrete": (unreal.LinearColor(0.40, 0.40, 0.40, 1.0), unreal.LinearColor(0.52, 0.52, 0.50, 1.0), 0.85),
    "Tarmac":   (unreal.LinearColor(0.05, 0.05, 0.06, 1.0), unreal.LinearColor(0.12, 0.12, 0.13, 1.0), 0.70),
    "Sand":     (unreal.LinearColor(0.76, 0.66, 0.45, 1.0), unreal.LinearColor(0.87, 0.78, 0.57, 1.0), 0.90),
    "Grass":    (unreal.LinearColor(0.09, 0.20, 0.06, 1.0), unreal.LinearColor(0.18, 0.34, 0.10, 1.0), 0.95),
}
GROUND_DEFAULT_PRESET = "Concrete"


# v1.0.129 -- the OPERATOR-AUTHORED floor material presets the user dropped into
# /Game/REBUS/Materials/ (one .uasset per surface, file names matched exactly so
# git's case-sensitive filename layer + UE's case-insensitive package layer stay
# in lockstep on cross-platform clones). These are the AUTHORITATIVE floor
# materials applied to the RebusFloor static mesh at build time and re-applied
# at runtime by `Rebus.FloorSurface <surface>` / SetSceneProperty
# name="FloorSurface" (see URebusSceneSettingsSubsystem::SetFloorSurface in
# the C++ plugin source).
#
# These are SEPARATE from the legacy procedural GROUND_PRESETS / MI_RebusGround_*
# family above, which v1.0.86 generates from the M_RebusGround master. The legacy
# family is preserved byte-for-byte (still authored, still wired to the existing
# Rebus.GroundSurface command + SetSceneProperty name="GroundSurface" path) so
# any saved scene state / portal recipe that names the legacy presets keeps
# working. v1.0.129 adds the new family alongside it; the operator picks which
# surface to apply via FLOOR_DEFAULT_SURFACE (build time) or the new console
# command (runtime).
#
# Default = "Concrete" because (a) the user-authored Concrete asset is the most
# neutral / studio-grade surface for a stage visualiser, and (b) it matches
# GROUND_DEFAULT_PRESET so a v1.0.128 -> v1.0.129 rebake doesn't flip the
# visible default tier on existing scenes.
FLOOR_SURFACE_PATHS = {
    "Concrete": MATERIALS_DIR + "/Concrete",
    "Grass":    MATERIALS_DIR + "/Grass",
    "Sand":     MATERIALS_DIR + "/Sand",
    "Tarmac":   MATERIALS_DIR + "/Tarmac",
}
FLOOR_DEFAULT_SURFACE = "Concrete"


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


def _instance_path(preset):
    return "{}/MI_RebusGround_{}".format(MATERIALS_DIR, preset)


def _build_ground_master(mat):
    """Author the procedural ground master graph.

    v1.0.86 layout (1 m physical tiling):

        BaseColor = lerp(ColorA, ColorB, Noise) * BaseColorTexture.Sample(WorldUVs)
        Roughness = Roughness
        TilingMeters (scalar, default 1.0) drives WorldUVs = AbsoluteWorldPosition.xy
                                                            / (TilingMeters * 100 cm)

    Why world-position UVs and not TexCoord. The BaseLevel floor is a 100 cm engine plane
    scaled 2000x = 2 km square (see _add_floor()), so the mesh's default UV span 0..1 is
    stretched across 2000 m. Sampling a real bitmap with TexCoord makes every texture read
    as a single 2 km repeat -- the user's "stretched" symptom. Driving the UV from the
    pixel's WORLD POSITION makes the tile size depend on physical metres, not on mesh
    geometry, so the same texture sample tiles 2000 times across the same plane at
    `TilingMeters = 1`.

    BaseColorTexture defaults to /Engine/EngineResources/WhiteSquareTexture so an MI that
    doesn't wire a real texture multiplies by white (1,1,1) -- the procedural lerp output
    is preserved byte-exact. The four shipped MI presets (Concrete / Tarmac / Sand / Grass)
    therefore look identical to their pre-v1.0.86 form on a regen; only MIs that BIND a
    real bitmap to `BaseColorTexture` will see the new tiling kick in.

    The C++ runtime (URebusSceneSettingsSubsystem::SetGroundTilingMeters) pushes the
    TilingMeters scalar to a per-floor MID on the live actor, so the operator can change
    the tile size at runtime via `Rebus.SetGroundTiling <metres>` or the scene property
    of the same name -- without re-cooking the material.

    v1.0.97: the master is now double-sided ("make every Rebus-authored Python master
    double-sided" -- see README v1.0.97). The 2 km floor plane is rendered from above in
    99% of cases, but a camera that dips below the plane (sub-floor pit, off-axis
    swing-cam) or an operator-authored sub-floor mesh that re-uses an `MI_RebusGround_*`
    instance would otherwise punch through to skybox; the back-face draw makes the floor
    read as a real opaque surface from either side. Shading model + opaque blend are
    inherited from the asset factory defaults (Lit / Opaque) so this only flips two_sided.
    """
    mel = unreal.MaterialEditingLibrary

    # v1.0.97: double-sided so the back of the floor plane (and any sub-floor mesh
    # binding an MI_RebusGround_* instance) renders. Per-MI two_sided is inherited
    # from the master, so the four shipped MI presets pick this up automatically.
    _set(mat, "two_sided", True)

    # --- Procedural colour layer (back-compat with the pre-v1.0.86 master) ---
    color_a = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -1400, -240)
    color_a.set_editor_property("parameter_name", "ColorA")
    color_a.set_editor_property("default_value", unreal.LinearColor(0.40, 0.40, 0.40, 1.0))

    color_b = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -1400, 30)
    color_b.set_editor_property("parameter_name", "ColorB")
    color_b.set_editor_property("default_value", unreal.LinearColor(0.52, 0.52, 0.50, 1.0))

    noise = mel.create_material_expression(mat, unreal.MaterialExpressionNoise, -1400, 260)
    # Small scale => broad features at ground scale (world-position driven by default).
    _set(noise, "scale", 0.005)

    proc_lerp = mel.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -1080, -100)
    mel.connect_material_expressions(color_a, "", proc_lerp, "A")
    mel.connect_material_expressions(color_b, "", proc_lerp, "B")
    mel.connect_material_expressions(noise, "", proc_lerp, "Alpha")

    # --- World-position-derived UVs for 1 m physical tiling (v1.0.86) ---
    # WorldUV = AbsoluteWorldPosition.xy / (TilingMeters * 100)
    #   * /100 converts UE world units (cm) to metres,
    #   * /TilingMeters scales the metre count so TilingMeters=1 -> 1 tex/m, 0.5 -> 2 tex/m,
    #     10 -> 1 tex/10 m. Clamp on the C++ side prevents 0 from producing a div-by-zero
    #     single-texel stretch.
    tiling = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -1400, 500)
    tiling.set_editor_property("parameter_name", "TilingMeters")
    tiling.set_editor_property("default_value", 1.0)

    world_pos = mel.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -1400, 700)

    # Mask down to the XY components (the ground plane lies on Z=0; .XY is the in-plane UV).
    xy_mask = mel.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -1180, 700)
    _set(xy_mask, "r", True)
    _set(xy_mask, "g", True)
    _set(xy_mask, "b", False)
    _set(xy_mask, "a", False)
    mel.connect_material_expressions(world_pos, "", xy_mask, "")

    # tiling_cm = TilingMeters * 100 cm/m.
    tiling_cm = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -1180, 540)
    _set(tiling_cm, "const_b", 100.0)
    mel.connect_material_expressions(tiling, "", tiling_cm, "A")

    world_uvs = mel.create_material_expression(mat, unreal.MaterialExpressionDivide, -960, 640)
    mel.connect_material_expressions(xy_mask, "", world_uvs, "A")
    mel.connect_material_expressions(tiling_cm, "", world_uvs, "B")

    # --- Optional BaseColor texture sampler (defaults to white so untextured MIs no-op) ---
    base_tex = mel.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D, -700, 700)
    base_tex.set_editor_property("parameter_name", "BaseColorTexture")
    _white = unreal.EditorAssetLibrary.load_asset("/Engine/EngineResources/WhiteSquareTexture")
    if _white is not None:
        # set_editor_property on "texture" assigns the default sampler asset.
        _set(base_tex, "texture", _white)
    # Route world-derived UVs into the sampler's UV input so the texture tiles by metres.
    mel.connect_material_expressions(world_uvs, "", base_tex, "Coordinates")

    # --- Compose BaseColor = procedural * texture ---
    base_color = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -380, 0)
    mel.connect_material_expressions(proc_lerp, "", base_color, "A")
    mel.connect_material_expressions(base_tex, "", base_color, "B")  # default "" pin is RGB
    mel.connect_material_property(base_color, "", unreal.MaterialProperty.MP_BASE_COLOR)

    # --- Roughness scalar (unchanged) ---
    rough = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -380, 240)
    rough.set_editor_property("parameter_name", "Roughness")
    rough.set_editor_property("default_value", 0.85)
    mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

    mel.recompile_material(mat)


def _build_lens_flare_master(mat):
    """Author the emissive lens-flare graph: (EmissiveColor*EmissiveStrength*radialMask) ->
    Emissive, radialMask -> Opacity. Unlit + two-sided + ADDITIVE (glow that vanishes when off)."""
    mel = unreal.MaterialEditingLibrary

    # Unlit two-sided additive: facing-independent soft round glow that adds nothing when the
    # fixture is dark (no black card), and blooms when bright.
    _set(mat, "material_domain", unreal.MaterialDomain.MD_SURFACE)
    _set(mat, "shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    _set(mat, "blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    _set(mat, "two_sided", True)

    color = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -760, -200)
    color.set_editor_property("parameter_name", "EmissiveColor")
    color.set_editor_property("default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))

    strength = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -760, 40)
    strength.set_editor_property("parameter_name", "EmissiveStrength")
    strength.set_editor_property("default_value", 0.0)

    # Radial mask from the plane UVs: 1 at centre -> 0 at the inscribed circle edge (radius 0.5).
    tc = mel.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, -760, 260)
    centre = mel.create_material_expression(mat, unreal.MaterialExpressionConstant2Vector, -760, 420)
    _set(centre, "r", 0.5)
    _set(centre, "g", 0.5)
    dist = mel.create_material_expression(mat, unreal.MaterialExpressionDistance, -540, 320)
    mel.connect_material_expressions(tc, "", dist, "A")
    mel.connect_material_expressions(centre, "", dist, "B")
    twice = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -380, 320)
    _set(twice, "const_b", 2.0)
    mel.connect_material_expressions(dist, "", twice, "A")
    inv = mel.create_material_expression(mat, unreal.MaterialExpressionOneMinus, -240, 320)
    mel.connect_material_expressions(twice, "", inv, "")
    mask = mel.create_material_expression(mat, unreal.MaterialExpressionClamp, -100, 320)
    _set(mask, "min_default", 0.0)
    _set(mask, "max_default", 1.0)
    mel.connect_material_expressions(inv, "", mask, "")

    # Emissive = EmissiveColor * EmissiveStrength * radialMask.
    cs = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -420, -140)
    mel.connect_material_expressions(color, "", cs, "A")
    mel.connect_material_expressions(strength, "", cs, "B")
    csm = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -220, -80)
    mel.connect_material_expressions(cs, "", csm, "A")
    mel.connect_material_expressions(mask, "", csm, "B")
    mel.connect_material_property(csm, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.connect_material_property(mask, "", unreal.MaterialProperty.MP_OPACITY)

    mel.recompile_material(mat)


def ensure_lens_material(force=False):
    """Generate the emissive lens-flare master material. Idempotent (only creates when missing)
    unless force=True (delete + regenerate, e.g. during a full build())."""
    tools = unreal.AssetToolsHelpers.get_asset_tools()

    if force and unreal.EditorAssetLibrary.does_asset_exist(LENS_FLARE_PATH):
        unreal.EditorAssetLibrary.delete_asset(LENS_FLARE_PATH)

    if not unreal.EditorAssetLibrary.does_asset_exist(LENS_FLARE_PATH):
        mat = tools.create_asset("M_RebusLensFlare", MATERIALS_DIR, unreal.Material, unreal.MaterialFactoryNew())
        _build_lens_flare_master(mat)
        unreal.EditorAssetLibrary.save_loaded_asset(mat)


def _build_orbit_imported_master(mat):
    """Author the v1.0.104 two-sided opaque master operators can re-parent Orbit-imported
    materials to.

    The minimal PBR graph that gives glTFRuntime / OrbitConnector imports a viable
    two-sided home without losing the source material's texture / colour data once an
    operator has copied the relevant params across:

        BaseColor   = BaseColor (vector) * BaseColorTexture.Sample(TexCoord)
        Roughness   = Roughness (scalar)
        Metallic    = Metallic  (scalar)
        Emissive    = EmissiveColor * EmissiveStrength    (additive layer; default off)
        Normal      = ditto-via-texture-sample-default-(0.5, 0.5, 1)  (skipped on v1.0.104
                      -- glTFRuntime imports overwhelmingly publish baseColor + ARM /
                      metallicRoughness + emissive; normal-map preservation is on the
                      v1.0.105 roadmap so a future operator-driven swap can include it.)

    `bTwoSidedScalar` (scalar param, default 1) is exposed so the v1.0.104 C++ walker
    (URebusVisualiserSubsystem::EnsureImportedDoubleSided) can flip individual MID
    instances at runtime; the top-level `two_sided` editor property is ALSO baked True
    so a material assigned directly (no MID wrap) still renders both sides. Mirrors the
    v1.0.97 double-sided contract on every other Rebus-authored master.

    The texture sampler defaults to /Engine/EngineResources/WhiteSquareTexture so an MI
    that doesn't bind a real texture multiplies by white (1,1,1) and the BaseColor vector
    + Metallic / Roughness scalars carry the look untouched -- matches v1.0.86's
    _build_ground_master texture-default convention.
    """
    mel = unreal.MaterialEditingLibrary

    # v1.0.104: two-sided opaque -- the whole point of this master. Operators re-parent
    # Orbit-imported single-sided MIDs to this so the engine renders the back face.
    _set(mat, "two_sided", True)

    # --- BaseColor: BaseColor (vector) * BaseColorTexture.Sample(TexCoord) ---
    base_color = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -800, -240)
    base_color.set_editor_property("parameter_name", "BaseColor")
    base_color.set_editor_property("default_value", unreal.LinearColor(0.5, 0.5, 0.5, 1.0))

    base_tex = mel.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D, -800, 0)
    base_tex.set_editor_property("parameter_name", "BaseColorTexture")
    _white = unreal.EditorAssetLibrary.load_asset("/Engine/EngineResources/WhiteSquareTexture")
    if _white is not None:
        _set(base_tex, "texture", _white)

    base_mul = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -440, -120)
    mel.connect_material_expressions(base_color, "", base_mul, "A")
    mel.connect_material_expressions(base_tex, "", base_mul, "B")
    mel.connect_material_property(base_mul, "", unreal.MaterialProperty.MP_BASE_COLOR)

    # --- Roughness / Metallic (scalar params with PBR-typical defaults) ---
    rough = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -440, 160)
    rough.set_editor_property("parameter_name", "Roughness")
    rough.set_editor_property("default_value", 0.7)
    mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

    metallic = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -440, 300)
    metallic.set_editor_property("parameter_name", "Metallic")
    metallic.set_editor_property("default_value", 0.0)
    mel.connect_material_property(metallic, "", unreal.MaterialProperty.MP_METALLIC)

    # --- Optional emissive layer (additive on top; default OFF so re-parented MIDs that
    # don't bind EmissiveStrength read identical to the unlit-on-emissive baseline). ---
    em_color = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -800, 460)
    em_color.set_editor_property("parameter_name", "EmissiveColor")
    em_color.set_editor_property("default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))

    em_strength = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -800, 620)
    em_strength.set_editor_property("parameter_name", "EmissiveStrength")
    em_strength.set_editor_property("default_value", 0.0)

    em_mul = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -440, 540)
    mel.connect_material_expressions(em_color, "", em_mul, "A")
    mel.connect_material_expressions(em_strength, "", em_mul, "B")
    mel.connect_material_property(em_mul, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    # --- v1.0.104 `bTwoSidedScalar` parameter (the C++ walker probe target). The master
    # is already two_sided=True at the asset level; this scalar exists so the C++ walker
    # can detect-this-is-a-Rebus-two-sided-aware-master via GetScalarParameterValue (the
    # only runtime-safe MID parameter inspection path that doesn't load shader source).
    # The value itself is not wired into the graph -- it's a marker. A future Rebus-
    # authored master that supports BOTH single and double-sided rendering via a Static
    # Switch could wire this scalar into a StaticSwitchParameter; on v1.0.104 the master
    # is unconditionally two-sided so the marker just identifies it. ---
    two_sided_marker = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -800, 780)
    two_sided_marker.set_editor_property("parameter_name", "bTwoSidedScalar")
    two_sided_marker.set_editor_property("default_value", 1.0)

    mel.recompile_material(mat)


def _orbit_imported_master_has_two_sided(master):
    """v1.0.104: best-effort probe -- the existing on-disk M_RebusOrbitImported master
    declares the `bTwoSidedScalar` parameter (= it was baked by v1.0.104+ and carries
    the double-sided contract).

    Mirrors v1.0.86 (`_master_has_tiling_meters`) / v1.0.93
    (`_fixture_lens_master_is_current`) /
    v1.0.97 (`_master_is_two_sided`): an `or` between this probe and the top-level
    `two_sided` editor property catches BOTH "missing the scalar marker" and "master
    accidentally got single-sided" as needs-regen. Best-effort: any exception (engine-
    version rename, unfamiliar asset shape) yields False so the self-heal path treats
    the master as "needs regen" -- a redundant rebake on next launch is cheap; a false
    positive would leave the operator on an OLD single-sided master indefinitely.
    """
    try:
        info = unreal.MaterialEditingLibrary.get_scalar_parameter_names(master)
    except Exception:  # noqa: BLE001
        return False
    return any(str(n) == "bTwoSidedScalar" for n in info)


def ensure_orbit_imported_material(force=False):
    """Generate the v1.0.104 two-sided opaque master operators re-parent Orbit-imported
    materials to.

    Idempotent by default (only creates when missing); force=True deletes + regenerates
    (used by build()). v1.0.104 self-heal: a non-force call against a pre-v1.0.104 master
    -- either missing the `bTwoSidedScalar` marker or, defensively, top-level
    `two_sided = False` -- gets promoted to a force-regen with a Warning log so the
    operator can see the migration happen. Matches the v1.0.97 self-heal shape for
    `_build_ground_master` + `_build_fixture_lens_master` (combined-OR over the two
    probes triggers a single regen).
    """
    tools = unreal.AssetToolsHelpers.get_asset_tools()

    # Non-force self-heal: pre-v1.0.104 master missing the bTwoSidedScalar marker OR
    # accidentally single-sided => promote to force-regen.
    if not force and unreal.EditorAssetLibrary.does_asset_exist(ORBIT_IMPORTED_PATH):
        existing = unreal.EditorAssetLibrary.load_asset(ORBIT_IMPORTED_PATH)
        if existing is not None and (not _orbit_imported_master_has_two_sided(existing)
                                     or not _master_is_two_sided(existing)):
            unreal.log_warning("RebusBaseLevel: pre-v1.0.104 M_RebusOrbitImported detected "
                               "(missing bTwoSidedScalar marker or single-sided); regenerating.")
            force = True

    if force and unreal.EditorAssetLibrary.does_asset_exist(ORBIT_IMPORTED_PATH):
        unreal.EditorAssetLibrary.delete_asset(ORBIT_IMPORTED_PATH)

    if not unreal.EditorAssetLibrary.does_asset_exist(ORBIT_IMPORTED_PATH):
        mat = tools.create_asset("M_RebusOrbitImported", MATERIALS_DIR, unreal.Material, unreal.MaterialFactoryNew())
        _build_orbit_imported_master(mat)
        unreal.EditorAssetLibrary.save_loaded_asset(mat)

    unreal.log("RebusBaseLevel: M_RebusOrbitImported ensured (v1.0.104, two-sided opaque).")

    unreal.log("RebusBaseLevel: lens-flare material ensured.")


_BEAM_RAYMARCH_HLSL = """
// True N-step view-ray raymarch through the cone volume (Phase 2; entry/exit rework v1.0.39).
//
// v1.0.39 fix -- the beam used to vanish when the camera got close to / inside the cone. The old
// code started the march at the rasterized fragment (tFront) and marched FORWARD downrange. When
// the camera is inside, the only rasterized fragment is the far wall (back face, drawn because the
// material is two-sided); marching forward from it leaves the cone, so every sample missed and the
// shaft disappeared. The march interval is now built from the VIEW RAY vs the cone analytically:
//   ENTRY = camera itself when the camera is inside the cone (so the march always starts), else the
//           analytic near (front-wall) intersection so distant beams stay tightly sampled.
//   EXIT  = this fragment's own surface distance, also clamped by the opaque scene depth.
// Because EXIT is the fragment's own depth, a front-face fragment marches a ~zero interval (its
// surface IS the entry) and the far/back-face fragment carries the shaft -- so two-sided drawing
// never double-adds, with no per-face branching. A short near-camera soft fade avoids a hard wall
// in the lens when flying through.
//
// v1.0.40 brightness model -- the beam was faint and faded NEAR the fixture. Two fixes in the
// per-sample density: (1) a width-bias normalization (density ~ 1/radiusAt) so a ray crossing the
// wide far end no longer reads brighter than the narrow near-lens region purely from path length;
// (2) a distance-from-SOURCE softened inverse-square falloff so the shaft is BRIGHTEST at the lens
// and dims downrange (BeamFalloff = strength). The radial core starts at the lens radius (LensRadius
// at axial=0), so the shaft visibly begins as a disc of the lens diameter.
//
// v1.0.42 -- match the UE DMX-fixtures beam LOOK (the shaft is a translucent cone mesh raymarched
// front-to-back, exactly as DMXFixtures' M_LightBeam does): the radial cross-section is now a SMOOTH
// Gaussian glow (no hard mesh rim) and a soft depth fade dissolves the beam where it meets opaque
// geometry (DMX soft-particle look) instead of a hard scene-depth clip. The actual cast light stays
// IES-driven on the USpotLightComponent (more accurate than stock DMX, which uses a light-function
// cookie). Output: float4(rgb beam colour, a coverage).
//
// v1.0.110 ROLLBACK NOTE -- the v1.0.96 .. v1.0.109 screen-space self-shadow trace (per-shaft-sample
// march from `wp` toward the SpotLight, tap SceneDepth at the projected UV, attenuate when occluded)
// has been entirely REMOVED at the user's direction ("This shadow tracing,pen clip really isnt
// working, its terrible and completely wrong, remove it and we will start again."). See the README
// v1.0.110 release block for the rollback scope. v1.0.111 (below) is the redesign on a different
// architectural footing -- light-space, per-fixture, view-independent.
//
// v1.0.111 LIGHT-SPACE DEPTH-MASK BEAM OCCLUSION -- the architecturally correct replacement.
// Each fixture owns a USceneCaptureComponent2D parented to its SpotLight (so the capture rides
// the live pan/tilt aim via the existing component hierarchy) that renders a depth-only mask
// (`SCS_SceneDepth`, linear cm) into a per-fixture R16f UTextureRenderTarget2D (`BeamShadowMaskRT`).
// The raymarch below samples that texture per-step: it projects the shaft sample's world position
// into the SpotLight's local frame using the SpotLight's pushed world right / up / forward axes
// (BeamLightRight / BeamLightUp / BeamLightFwd) and the SceneCapture's tan(halfFov) (BeamShadow
// MaskTanHalfFov), reads the blocker depth at that texture UV, and attenuates the per-step
// density when the shaft sample sits FURTHER from the lens than the blocker.
//
// Why this works where the screen-space trace didn't:
//   * The capture's frustum is FIXED BY THE AIM (the SpotLight's outer cone) -- not by the
//     camera. Samples outside the capture's frustum are exactly samples where the light doesn't
//     reach geometrically, so "no occlusion change" there is semantically correct (the
//     v1.0.96..v1.0.109 false-occlusion at the screen edge was the camera-frustum mismatch
//     bleeding into a light-space comparison).
//   * Off-screen-relative-to-camera occluders still cast: the capture sees them as long as
//     they're in the LIGHT's frustum (i.e. between the fixture and the lit footprint).
//   * Reverse-Z precision crash beyond ~500m doesn't apply: the capture writes LINEAR cm
//     (SCS_SceneDepth), not non-linear NDC z, and the FarCm scalar is the SpotLight's
//     AttenuationRadius which caps the throw to something realistic.
//
// Operator-side knobs:
//   BeamShadowMaskEnabled    0/1 master gate (the shader's `[branch]` takes the per-step tap
//                            OUT of the per-pixel cost when 0; the C++ side ALSO flips the
//                            SceneCapture's bCaptureEveryFrame so the GPU capture cost is
//                            saved too)
//   BeamShadowMaskBiasCm     constant offset added to blocker depth before the comparison
//                            (default 5.0 cm) -- prevents self-occlusion against geometry
//                            that sits exactly at the lens plane
//   BeamShadowMaskFadeCm     soft-fade range in cm (default 20.0 cm) -- the shaft doesn't
//                            binary clip, it fades over a few cm so the depth-mask's
//                            discrete pixels don't read as aliased shadow edges
//   BeamShadowMaskFarCm      maximum sample-distance for the comparison (= the SpotLight's
//                            AttenuationRadius); when the capture returned a "missed all"
//                            depth (~FarCm+epsilon) we treat it as no occluder
//   BeamShadowMaskDebug      1 = paint occluded samples RED inside the shaft for visual
//                            verification; 0 = ship the regular composed beam
//   BeamShadowMaskTanHalfFov tan(0.5 * SceneCapture FOVAngle); pushed by C++ so the shader
//                            doesn't compute tan() per pixel
//   BeamLightFwd/Right/Up    SpotLight world axes; pushed by C++ each refresh; the shader
//                            uses them to project `wp - BeamOrigin` into light-local space
//   BeamShadowMaskRT         Texture2D depth-only mask; bound by C++ at BuildBeamShadowMask
//                            Capture time (never re-bound per frame)
//
float3 ro = CamPos.xyz;
float3 pp = PixelPos.xyz;
float3 toPix = pp - ro;
float tPix = length(toPix);
if (tPix < 0.001) { return float4(0,0,0,0); }
float3 rd = toPix / tPix;

float3 bo = BeamOrigin.xyz;
float3 bd = normalize(BeamDir.xyz);
float blen = max(BeamLength, 1.0);
float r0 = max(LensRadius, 0.0);
float rF = max(FarRadius, r0 + 0.01);
float k = (rF - r0) / blen;          // dRadius / dAxial along the cone

// Opaque scene-depth occlusion. SceneDepth/PixelDepth are camera-Z; the ratio tPix/PixelDepth =
// 1/cos(view angle) converts the camera-Z scene depth to a distance along THIS view ray.
float tOcc = (PixelDepth > 0.001) ? (SceneDepth * tPix / PixelDepth) : (tPix + blen);

// ---- Analytic view-ray vs truncated cone (lateral surface, axial in [0, blen]) ----
// perp(t)^2 == radius(axial(t))^2  ->  quadratic qa t^2 + qb t + qc = 0.
float3 A = ro - bo;
float a0 = dot(A, bd);               // axial of camera
float ad = dot(rd, bd);              // axial rate along the ray (rd is unit)
float AA = dot(A, A);
float rr = dot(A, rd);
float p  = r0 + k * a0;
float q  = k * ad;
float qa = (1.0 - ad * ad) - q * q;
float qb = 2.0 * ((rr - a0 * ad) - q * p);
float qc = (AA - a0 * a0) - p * p;

// Is the camera inside the cone volume? (then ENTRY = camera so the march always runs).
float radCam = length(A - a0 * bd);
bool camInside = (a0 >= 0.0 && a0 <= blen) && (radCam <= (r0 + k * a0));

float tEntry = 0.0;                   // default: start at the camera (covers the inside case)
if (!camInside)
{
    // First lateral hit ahead of the camera whose axial coord lies within [0, blen] = front wall.
    float tHit = 1e9;
    if (abs(qa) > 1e-6)
    {
        float disc = qb * qb - 4.0 * qa * qc;
        if (disc >= 0.0)
        {
            float sq = sqrt(disc);
            float ta = (-qb - sq) / (2.0 * qa);
            float tb = (-qb + sq) / (2.0 * qa);
            float axa = a0 + ad * ta;
            float axb = a0 + ad * tb;
            if (ta > 0.001 && axa >= 0.0 && axa <= blen) { tHit = min(tHit, ta); }
            if (tb > 0.001 && axb >= 0.0 && axb <= blen) { tHit = min(tHit, tb); }
        }
    }
    else if (abs(qb) > 1e-6)
    {
        float tl = -qc / qb;
        float axl = a0 + ad * tl;
        if (tl > 0.001 && axl >= 0.0 && axl <= blen) { tHit = tl; }
    }
    if (tHit < 1e9) { tEntry = tHit; }
}

// EXIT = this fragment's own surface, clamped by opaque scene depth. (Front-face fragments thus
// march ~zero; the far/back-face fragment carries the shaft -> no two-sided double-add.)
float tExit = min(tPix, tOcc);
if (tExit <= tEntry) { return float4(0,0,0,0); }

int steps = (int)clamp(StepCount, 2.0, 64.0);
float dt = (tExit - tEntry) / steps;
float invLen = 1.0 / blen;
float radSpan = rF - r0;
float sharp = max(BeamSharpness, 0.01);
float fall = max(BeamFalloff, 0.0);
const float NEAR_FADE_CM = 10.0;      // fade only the few cm nearest the camera (no hard wall)
const float REF_RADIUS_CM = 100.0;    // fixed length scale for the width-bias normalization
const float DEPTH_FADE_CM = 50.0;     // DMX-style soft depth fade where the beam meets geometry

float trans = 1.0;
float t = tEntry + dt * 0.5;
const int MAXSTEPS = 64;
// v1.0.111 -- shadow-mask debug accumulator. Counts shaft samples whose visibility was
// reduced by the depth-mask comparison; converted to a per-pixel RED tint at the end when
// BeamShadowMaskDebug > 0.5 so the operator can visually verify the projection lines up
// with real-world occluders (place a cube between fixture + floor -> expect a RED channel
// inside the shaft that tracks the cube through pan/tilt). The accumulator is initialised
// at 0 and stays 0 whenever BeamShadowMaskEnabled is off or the shaft sample lies outside
// the SceneCapture frustum, so the debug view degenerates to "off" cleanly.
float debugOccludedAccum = 0.0;
float debugTotalAccum = 0.0;
// Hoist the depth-mask params into locals so the compiler can keep them in registers
// (the shader plumbs them as scalar/vector parameters from the BeamMID, but accessing
// them inside the loop body lets the compiler decide; this hoisted form is the canonical
// shadow-map sampling pattern and is consistently faster on modern GPUs).
float maskEnabled = BeamShadowMaskEnabled;
float maskBias    = max(BeamShadowMaskBiasCm, 0.0);
float maskFade    = max(BeamShadowMaskFadeCm, 0.01);
float maskFar     = max(BeamShadowMaskFarCm, 100.0);
float maskTan     = max(BeamShadowMaskTanHalfFov, 0.001);
float3 lFwd       = normalize(BeamLightFwd.xyz);
float3 lRight     = normalize(BeamLightRight.xyz);
float3 lUp        = normalize(BeamLightUp.xyz);

// v1.0.128 -- beam-side gobo projection plumbing. See the master-param doc-comment
// in `_build_beam_master` for the operator contract. `goboEnable > 0.5` is the
// per-fixture gate (C++ pushes `1` while `bGoboActive` is true, `0` on clear).
// `GoboTexture` is the per-fixture `GoboRT` cookie render-target (default white
// when unbound -> sampling reads 1.0 -> no density modulation, the beam reads
// identical to pre-v1.0.128). The projection basis is the SAME light-local
// axes the depth-mask uses (`lRight` / `lUp`) so the gobo image on the floor
// pool (M_Light_Master cookie, also driven by `GoboRT` from C++) and the gobo
// image inside the cone-shaft sample EXACTLY the same source texture at
// EXACTLY the same UV for any sample on the floor surface -- the two reads
// have to agree along the rays connecting the lens to the lit footprint or the
// shaft pattern visually peels off the cookie pattern as the camera moves.
float goboEnable  = GoboEnable;
[loop]
for (int i = 0; i < MAXSTEPS; ++i)
{
    if (i >= steps) { break; }
    if (t >= tExit) { break; }
    float3 wp = ro + rd * t;
    float3 rel = wp - bo;
    float axial = dot(rel, bd);
    float aN = axial * invLen;
    if (aN >= 0.0 && aN <= 1.0)
    {
        float radiusAt = r0 + radSpan * aN;
        float3 perp = rel - axial * bd;
        float radial = length(perp);
        float rN = (radiusAt > 0.001) ? (radial / radiusAt) : 2.0;
        if (rN < 1.5)
        {
            // v1.0.42 DMX-style soft Gaussian cross-section: a smooth glow core that fades to the
            // cone edge with NO hard rim (the old pow(1-rN) had a hard cutoff at rN=1 that read as a
            // crisp mesh edge). BeamSharpness = core tightness (higher = tighter, narrower core).
            float core = exp(-rN * rN * sharp);
            // v1.0.40 width-bias normalization: optical density per unit length ~ 1/radiusAt, so a
            // view ray crossing the WIDE far end no longer reads brighter than the NARROW near-lens
            // region purely from path length (the old bug that made the beam faint at the fixture).
            // REF_RADIUS_CM is a fixed scale folded into BeamDensity; the radiusAt term cancels the
            // chord-length growth so the on-axis shaft is uniform along its length before falloff.
            float widthNorm = REF_RADIUS_CM / max(radiusAt, 1.0);
            // v1.0.40 distance-from-SOURCE falloff (softened inverse square): brightest at the lens
            // (axial=0) and dimming downrange, matching a real light shaft. BeamFalloff = strength
            // (0 = flat, higher = faster falloff); the +1 clamp keeps it finite at the lens.
            float dn = axial * invLen;               // 0 at lens .. 1 at the far throw
            float srcAtten = 1.0 / (1.0 + fall * dn * dn);
            float nf = saturate(t / NEAR_FADE_CM);              // soft near-camera fade
            float softOcc = saturate((tOcc - t) / DEPTH_FADE_CM); // DMX-style soft fade at geometry

            // v1.0.111 light-space depth-mask shadow-attenuation. Project the shaft sample
            // into the SpotLight's local frame using the pushed world axes (lFwd / lRight /
            // lUp) so the comparison is done in LIGHT space, not screen space. The capture
            // sits at the lens (parented to SpotLight, identity relative transform) so
            // `rel` (= wp - BeamOrigin = sample position relative to the lens) is the same
            // vector the capture would compute as "where is this world point relative to
            // me". axial = dot(rel, lFwd) is the linear depth (cm) from the lens along the
            // light's forward; horizDist / vertDist project the sample onto the capture's
            // image plane.
            float shadowVis = 1.0;
            [branch]
            if (maskEnabled > 0.5 && axial > 0.5)  // skip points behind the lens AND the lens-coplanar samples that would self-occlude
            {
                float lAxial = dot(rel, lFwd);
                if (lAxial > 0.5 && lAxial < maskFar)
                {
                    float horizDist = dot(rel, lRight);
                    float vertDist  = dot(rel, lUp);
                    // Perspective projection: (horizDist / lAxial) divided by tan(halfFov)
                    // gives the sample's normalised X / Y on the capture plane (range
                    // [-1, +1] inside the frustum, outside means the sample is beyond the
                    // SceneCapture's FOV -- geometrically the light doesn't reach there).
                    float ndcX = (horizDist / lAxial) / maskTan;
                    float ndcY = (vertDist  / lAxial) / maskTan;
                    if (abs(ndcX) < 1.0 && abs(ndcY) < 1.0)
                    {
                        // NDC (-1..+1) -> texture UV (0..1). V is flipped because UE
                        // SceneCapture's image plane has +Y up (UE world right convention,
                        // not GL screen y-down).
                        float2 maskUV = float2(ndcX * 0.5 + 0.5, -ndcY * 0.5 + 0.5);
                        // Sample the depth mask. SCS_SceneDepth wrote linear camera-Z in
                        // cm into R; values near maskFar (or above) mean the capture
                        // missed every occluder along that ray, which is the "infinitely
                        // far" sentinel we want to treat as unoccluded.
                        float blockerDepthCm = Texture2DSample(
                            BeamShadowMaskRT, BeamShadowMaskRTSampler, maskUV).r;
                        // Guard against the "infinite-distance" sentinel (the RT is cleared
                        // to 1e6 in C++) AND against capture passes that haven't fired yet
                        // (depth=0 on a freshly-allocated RT means "no geometry to compare
                        // against" which we treat as unoccluded too).
                        if (blockerDepthCm > 1.0 && blockerDepthCm < maskFar * 1.01)
                        {
                            // The comparison is in LINEAR cm along the light forward, not
                            // along the view ray -- so we use lAxial (the projection onto
                            // lFwd) which is the same quantity SCS_SceneDepth writes for
                            // pixels at this image-plane position. A point sitting further
                            // from the lens than its blocker depth (plus a bias) is in
                            // shadow; the soft fade hides any aliasing from the discrete
                            // R16f depth buckets.
                            float diff = lAxial - (blockerDepthCm + maskBias);
                            shadowVis = 1.0 - saturate(diff / maskFade);
                            debugTotalAccum += 1.0;
                            if (shadowVis < 0.99)
                            {
                                debugOccludedAccum += (1.0 - shadowVis);
                            }
                        }
                    }
                    // outside the capture frustum: shadowVis stays 1.0 (= no occlusion change).
                    // This is geometrically correct -- the light doesn't reach there
                    // either, so the shaft sample's density is whatever the radial Gaussian
                    // + axial falloff say. The cone-mesh raymarch will fade those samples
                    // out naturally via core / widthNorm / srcAtten.
                }
            }

            // v1.0.128 -- beam-side gobo projection (issue 1). The cone-mesh shaft
            // shares its perpendicular UV basis with the SpotLight cookie's
            // M_Light_Master sampling: at every raymarch step we project the sample's
            // perpendicular offset (relative to the cone axis) onto the SpotLight's
            // local right/up axes (lRight / lUp, the same pushed vectors the
            // depth-mask uses), normalise by the local cone radius at this axial
            // position, and map to a [0..1] UV pair. Sampling the gobo texture at
            // that UV gives a per-step transmittance multiplier (1 = fully open,
            // 0 = fully blocked) so the visible shaft modulates with the same
            // pattern projected on the lit floor. `goboEnable > 0.5` gates the
            // sampling out cleanly when no gobo is active (the per-step branch
            // keeps cost zero); the default-white texture on the master fall-
            // back means an unbound `GoboTexture` reads as 1.0 (no modulation).
            float goboMix = 1.0;
            [branch]
            if (goboEnable > 0.5)
            {
                // Project perpendicular offset into light-local right/up axes.
                // `perp` is already perpendicular to the cone axis (= rel - axial * bd);
                // dot it against the SpotLight's pushed right/up gives the
                // transverse U / V offset. Normalise by the local cone radius so
                // the UV at the cone wall is +-1 and the centre of the cone is
                // (0,0) -- matches the cookie's centred-disc convention.
                float u = (radiusAt > 0.001) ? (dot(perp, lRight) / radiusAt) : 0.0;
                float v = (radiusAt > 0.001) ? (dot(perp, lUp)    / radiusAt) : 0.0;
                // Map [-1..+1] -> [0..1] UV. V is flipped so the gobo orientation
                // matches the cookie (M_Light_Master inverts V too via the
                // standard UE SceneCapture-y-down convention).
                float2 goboUV = float2(u * 0.5 + 0.5, -v * 0.5 + 0.5);
                // Clamp to [0..1] so out-of-cone samples (which the rN<1.5 guard
                // above already mostly handles) don't wrap-sample garbage; the
                // texture's wrap mode is engine-default but explicit clamp here
                // documents the intent.
                goboUV = saturate(goboUV);
                float3 goboRGB = Texture2DSample(
                    GoboTexture, GoboTextureSampler, goboUV).rgb;
                // Treat the gobo as a monochrome transmittance mask: the rec.709
                // luma of the sample tells us how much light passes through. A
                // pure-white sample reads 1.0 (no modulation, beam unaffected);
                // a pure-black sample reads 0.0 (full block, this raymarch step
                // contributes zero density). Coloured gobos contribute their
                // tinted value -- the resulting beam shaft picks up the gobo
                // colour modulation faithfully because BeamDensity is scalar
                // and the tint comes through downstream via `col` composition.
                goboMix = dot(goboRGB, float3(0.299, 0.587, 0.114));
            }

            float d = BeamDensity * core * widthNorm * srcAtten * nf * softOcc * shadowVis * goboMix;
            float a = 1.0 - exp(-d * dt);
            trans *= (1.0 - a);
        }
    }
    t += dt;
}

float coverage = saturate(1.0 - trans);
float3 col = BeamColor.rgb * BeamIntensity * coverage;
// v1.0.111 debug visualisation: when BeamShadowMaskDebug > 0.5, tint shaft pixels RED in
// proportion to how much of their density got carved by the depth-mask comparison. Lets
// the operator visually verify the projection -- a cube placed between fixture + floor
// should paint a RED channel inside the shaft that follows the cube through pan/tilt.
// When debug is off (default) this branch is dead code on modern GPUs.
if (BeamShadowMaskDebug > 0.5 && debugTotalAccum > 0.5)
{
    float occlFrac = saturate(debugOccludedAccum / max(debugTotalAccum, 1.0));
    col = lerp(col, float3(1.0, 0.0, 0.0), occlFrac * coverage);
}
return float4(col, coverage);
"""


def _build_beam_master(mat):
    """Author the TRUE raymarched beam graph (Phase 2, v1.0.33).

    A single Custom HLSL node marches the view ray through the cone volume (front-to-back with
    transmittance), with a radial on-axis core -> soft edge profile (BeamSharpness) and a length
    attenuation (BeamFalloff), additive output. v1.0.39: the march interval is built analytically
    from the view ray vs the cone (ENTRY = camera when the camera is inside the cone, else the
    analytic front-wall hit; EXIT = the fragment's own surface clamped by scene depth) so the shaft
    stays visible when the camera is near / at the mouth of / fully inside the cone, and the
    two-sided draw never double-adds. Camera scene-depth occlusion (the shaft hidden behind opaque
    geometry) and a short near-camera soft fade are kept. The cone geometry is fed as params
    (BeamOrigin/BeamDir world from the component, BeamLength, LensRadius, FarRadius) so the shader
    math matches the procedural mesh exactly. StepCount + BeamDensity tune the march.

    Unlit + two-sided + ADDITIVE so it never shows a black card when dark and back faces still draw
    when the camera is inside (the back wall is the fragment that carries the shaft).

    v1.0.110: the v1.0.96..v1.0.109 screen-space self-shadow trace + its seven `BeamShadow*` scalar
    parameters have been REMOVED (operator decision -- the trace's pan-edge / sky / far-distance
    failure modes were unacceptable even with the v1.0.109 guards in place). The cone-mesh shaft
    now renders unoccluded again, matching the pre-v1.0.96 baseline. The pre-v1.0.96 hero-beam
    native VSM fog hybrid (RebusFixtureActor::RefreshBeamShadowMode) is unchanged and unrelated --
    it carves the SEPARATE soft per-light fog halo with Unreal's native volumetric-shadow path.
    See the README v1.0.110 release block for the rollback scope + the open architectural
    question for v1.0.111+.
    """
    mel = unreal.MaterialEditingLibrary

    _set(mat, "material_domain", unreal.MaterialDomain.MD_SURFACE)
    _set(mat, "shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    _set(mat, "blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    _set(mat, "two_sided", True)
    # v1.0.117 -- DISABLE FIXED-FUNCTION DEPTH TEST on the shaft material. This is the
    # single biggest fix for the v1.0.117 user-reported "the beam clips against an
    # invisible bounding box when I pan; AND `Rebus.BeamShadowMask 0` doesn't change
    # it" symptom. With the depth-mask ruled out by the operator-toggle A/B
    # (described in the README v1.0.117 release block), the most common remaining
    # cause for additive translucent cones reading as "clipped behind invisible
    # geometry" is FIXED-FUNCTION depth occlusion against OTHER translucent surfaces
    # (sibling beam cones during a pan-cross, the chrome lens disc, LED-matrix
    # `IsBeamLensComponents` PMCs -- anything that lands in the depth buffer
    # between the camera and the cone fragment kills the cone's per-pixel depth
    # test against an AABB-aligned silhouette).
    #
    # Crucially, the HLSL raymarch ALREADY does its own scene-depth occlusion via
    # `tExit = min(tPix, tOcc)` in `_BEAM_RAYMARCH_HLSL` (the v1.0.39 entry/exit
    # math, never removed). `tOcc` is computed from `SceneDepth` -- so the floor /
    # wall / opaque-occluder fade is shader-driven and continues to work correctly
    # even without the fixed-function depth test. The ONLY thing v1.0.117's
    # `disable_depth_test = True` removes is the depth test against OTHER translucent
    # surfaces -- exactly the failure surface the user reports.
    #
    # Pair this with the v1.0.117 C++ `RefreshBeamConeCullingFlags` (also v1.0.117 --
    # sets `bRenderInDepthPass=false` on the cone primitive, the read+write
    # complement to this material-side `disable_depth_test=True`).
    _set(mat, "disable_depth_test", True)

    # v1.0.117 -- explicit "the cone shaft is NOT a volumetric fog participant"
    # assertion. The volumetric fog participant is the SpotLight's light function
    # material (`M_RebusGoboLightFunction`, v1.0.93 -- its `bUsedWithVolumetricFog
    # = True` is what carves cookie patterns into the fog volume); the shaft
    # cone is a separate visible-additive surface that has no business landing
    # in the volumetric fog froxel grid. Setting this explicitly false guards
    # against a class of pipeline-state surprises where UE 5.7's translucent
    # pass interacts with the volumetric fog tile grid (8x8x128 froxels) and
    # can produce axis-aligned clip artefacts on cones that straddle a tile
    # boundary -- a known UE failure mode the v1.0.117 user-symptom shape
    # could otherwise be confused for.
    _set(mat, "used_with_volumetric_fog", False)

    # v1.0.117 -- BeamMaterialRevision sentinel scalar. Bumped by every release
    # cycle that touches the master so the v1.0.112 auto-purge probe in
    # `URebusVisualiserSubsystem::ProbeAndAutoPurgeStaleBeamMaster` can detect
    # a stale on-disk master and force a regen WITHOUT relying on the .uasset
    # md5 (which depends on the cooked editor state and isn't deterministic
    # across operator machines). The C++ probe reads this scalar via
    # `UMaterial::GetScalarParameterDefaultValue` and compares against the
    # expected revision constant (`RebusExpectedBeamMaterialRevision = 117`
    # in `RebusVisualiserSubsystem.cpp`); mismatch -> regen via the same
    # Python script. The runtime BeamMID inherits the master's default and
    # the v1.0.117 `Rebus.DumpBeamCulling` prints it back so the operator
    # can confirm the regen actually landed on their machine.
    # v1.0.119 -- bumped 117.0 -> 119.0 (the v1.0.118 build-fix did NOT bump the
    # revision; v1.0.119 ALSO did not change `_build_beam_master`'s graph but does
    # carry the SHOWSTOPPER Python tabs/spaces fix in `build()` / `ensure_base_level()`
    # without which the auto-purge could never actually invoke this function on the
    # operator's machine -- so v1.0.119 is the FIRST release where the v1.0.117
    # `disable_depth_test=True` flag is GUARANTEED to land on a stale master. Bumping
    # the revision sentinel forces the v1.0.112 auto-purge probe in
    # `URebusVisualiserSubsystem::ProbeAndAutoPurgeStaleBeamMaster` to treat every
    # pre-v1.0.119 master as stale and re-regen it via the freshly-importable Python
    # module, so operators who already booted v1.0.117/v1.0.118 (and got a master
    # baked through some OTHER path -- e.g. they manually ran `ensure_beam_material`
    # via Tools > Execute Python Script in a console that happened to handle the
    # tabs/spaces error differently) get a clean regen too. Keep in lockstep with the
    # C++ mirror `RebusExpectedBeamMaterialRevision` in `RebusVisualiserSubsystem.cpp`.
    rev = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -1100, -340)
    rev.set_editor_property("parameter_name", "BeamMaterialRevision")
    # v1.0.120 -- bumped 119.0 -> 120.0 in lockstep with REBUS_BEAM_MATERIAL_REVISION
    # above + the C++ mirror `RebusExpectedBeamMaterialRevision` in
    # RebusVisualiserSubsystem.cpp. See the REBUS_BEAM_MATERIAL_REVISION docstring
    # for the v1.0.120 stop-the-bleeding -game-mode crash diagnosis.
    rev.set_editor_property("default_value", float(REBUS_BEAM_MATERIAL_REVISION))

    # ---- Driveable parameters (per-fixture MID) ----
    color = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -1100, -260)
    color.set_editor_property("parameter_name", "BeamColor")
    color.set_editor_property("default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))

    def _scalar(name, default, y):
        s = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -1100, y)
        s.set_editor_property("parameter_name", name)
        s.set_editor_property("default_value", default)
        return s

    intensity = _scalar("BeamIntensity", 0.0, -140)
    # v1.0.108 -- raised authored default 2.5 -> 6.0 so a freshly-regenerated
    # `M_RebusBeam` master inherits the sharper Gaussian core that pinches the visible
    # shaft to the bright floor disc edge (the v1.0.108 "cone size doesn't match the
    # spotlight footprint" fix). The per-fixture MID seeds this scalar from the
    # `Rebus.BeamSharpness` CVar via `RefreshBeamRadialParams` (RebusFixtureActor.cpp)
    # at every spawn AND every CVar refresh -- so this authored default only matters
    # for fixtures that bypass `BuildBeamCone`'s seed (none in the current pipeline)
    # OR for the master's editor preview before any MID is created. Kept in sync with
    # the constexpr `RebusBeamSharpness = 6.0f` in RebusFixtureActor.cpp so master-level
    # tools (live shader tweaks in the editor) read the same default the runtime fixture
    # would seed at spawn.
    sharp = _scalar("BeamSharpness", 6.0, -60)
    falloff = _scalar("BeamFalloff", 1.6, 20)
    stepcount = _scalar("StepCount", 32.0, 100)
    density = _scalar("BeamDensity", 0.015, 180)
    beamlen = _scalar("BeamLength", 6000.0, 260)
    lensrad = _scalar("LensRadius", 2.0, 340)
    farrad = _scalar("FarRadius", 1000.0, 420)

    beamorigin = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -1100, 500)
    beamorigin.set_editor_property("parameter_name", "BeamOrigin")
    beamorigin.set_editor_property("default_value", unreal.LinearColor(0.0, 0.0, 0.0, 0.0))

    beamdir = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -1100, 580)
    beamdir.set_editor_property("parameter_name", "BeamDir")
    beamdir.set_editor_property("default_value", unreal.LinearColor(1.0, 0.0, 0.0, 0.0))

    # v1.0.111 -- light-space depth-mask scalar / vector / texture parameters. See the
    # v1.0.111 release block in `_BEAM_RAYMARCH_HLSL` for the architecture; ALL six
    # scalars + three vectors + one texture are pushed by RefreshBeamShadowMaskParams
    # (RebusFixtureActor.cpp) on the per-fixture MID. The master defaults below are
    # the "operationally safe" fallback (enabled=0 -> shader's [branch] gates the
    # mask sampling out, NO per-step cost, the master previews as a plain unoccluded
    # cone) for the editor-preview path that doesn't go through a fixture spawn.
    # Live operator tuning happens via the `Rebus.BeamShadowMask*` CVars which call
    # the refresh sink which calls RefreshBeamShadowMaskParams on every fixture.
    mask_enabled = _scalar("BeamShadowMaskEnabled", 0.0, 660)
    mask_bias    = _scalar("BeamShadowMaskBiasCm",  5.0, 740)
    mask_fade    = _scalar("BeamShadowMaskFadeCm", 20.0, 820)
    mask_far     = _scalar("BeamShadowMaskFarCm", 6000.0, 900)
    mask_tan     = _scalar("BeamShadowMaskTanHalfFov", 0.5, 980)
    mask_debug   = _scalar("BeamShadowMaskDebug", 0.0, 1060)

    beamlightfwd = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -1100, 1140)
    beamlightfwd.set_editor_property("parameter_name", "BeamLightFwd")
    beamlightfwd.set_editor_property("default_value", unreal.LinearColor(1.0, 0.0, 0.0, 0.0))

    beamlightright = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -1100, 1220)
    beamlightright.set_editor_property("parameter_name", "BeamLightRight")
    beamlightright.set_editor_property("default_value", unreal.LinearColor(0.0, 1.0, 0.0, 0.0))

    beamlightup = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -1100, 1300)
    beamlightup.set_editor_property("parameter_name", "BeamLightUp")
    beamlightup.set_editor_property("default_value", unreal.LinearColor(0.0, 0.0, 1.0, 0.0))

    # Texture-object parameter (not a regular Texture Sample). The Custom HLSL node
    # takes a Texture2D via the TextureObjectParameter pin; the HLSL body samples
    # it through `Texture2DSample(BeamShadowMaskRT, BeamShadowMaskRTSampler, uv)`
    # -- the engine auto-generates the SamplerState companion at compile time.
    # Default texture is /Engine/EngineResources/WhiteSquareTexture (full-white) so
    # an unbound RT samples as "no occluder" everywhere (matches the in-shader
    # "blockerDepthCm > maskFar" sentinel which falls through to shadowVis = 1.0).
    mask_rt = mel.create_material_expression(mat, unreal.MaterialExpressionTextureObjectParameter, -1100, 1380)
    mask_rt.set_editor_property("parameter_name", "BeamShadowMaskRT")
    try:
        default_white = unreal.EditorAssetLibrary.load_asset("/Engine/EngineResources/WhiteSquareTexture")
        if default_white is not None:
            mask_rt.set_editor_property("texture", default_white)
    except Exception:
        # Best-effort -- if the engine asset path moves the master still cooks with
        # a null default, and the per-fixture C++ push wires the real RT on spawn.
        pass

    # v1.0.128 -- BEAM-SIDE GOBO PROJECTION. Issue 1 of the v1.0.128 brief: when DMX
    # selects a gobo the user sees the gobo silhouette projected on the floor pool
    # (via the SpotLight's M_Light_Master cookie path) and on the lens face (via
    # M_RebusFixtureLens' v1.0.102 emissive layer) -- but NOT inside the visible
    # volumetric beam shaft. Pre-v1.0.128 the procedural cone material (this master)
    # had no gobo sampler at all, so a fixture running `bPreferProceduralBeam=1`
    # (the default since v1.0.106) showed the gobo on the floor + lens but the
    # raymarched additive shaft stayed unmodulated -- visually the cookie pattern
    # appeared on the lit pool but the beam between the fixture and the pool was
    # uniformly bright. v1.0.128 adds:
    #   * `GoboTexture` (TextureObjectParameter, default white) -- per-fixture cookie
    #     bitmap (the same `GoboRT` per-fixture render target the M_Light_Master
    #     cookie path samples, so cone shaft + floor pool show the same pattern in
    #     lockstep).
    #   * `GoboEnable` (Scalar, default 0.0) -- per-fixture enable gate (driven by
    #     C++ `bGoboActive` + `Rebus.LensFollowGobo` style global toggle). 0 ->
    #     shader skips the sampling entirely (the `[branch]` keeps cost zero); 1
    #     -> per-raymarch-step density is modulated by the gobo sample.
    # Sampled in `_BEAM_RAYMARCH_HLSL` via a planar projection: at every raymarch
    # step the perpendicular component of the sample position (relative to the
    # cone's local axis through the apex) is normalised against the local cone
    # radius to produce a [-1, +1] UV pair, sample the gobo texture there, and
    # multiply the per-step density. The projection is planar (= the gobo lives
    # in a transverse plane at the lens, extruded down the beam) which matches
    # the cookie's behaviour on the floor pool when the projection lands flat
    # (vertical projection cone onto a flat floor) -- they trace the same UV
    # math from the lens out. See the v1.0.128 README release block for the
    # full derivation. Default texture white-square so the un-bound state
    # (`GoboEnable=0` OR no texture pushed) collapses to "fully transmissive",
    # matching the pre-v1.0.128 visual exactly when no gobo is active.
    gobo_tex_obj = mel.create_material_expression(mat, unreal.MaterialExpressionTextureObjectParameter, -1100, 1460)
    gobo_tex_obj.set_editor_property("parameter_name", "GoboTexture")
    try:
        if default_white is not None:
            gobo_tex_obj.set_editor_property("texture", default_white)
    except Exception:
        # Same best-effort posture as BeamShadowMaskRT above; the C++ push
        # wires the real per-fixture GoboRT on every gobo apply.
        pass

    gobo_enable = _scalar("GoboEnable", 0.0, 1540)

    # ---- Scene/view inputs (engine nodes) ----
    campos = mel.create_material_expression(mat, unreal.MaterialExpressionCameraPositionWS, -1100, 680)
    pixelpos = mel.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -1100, 760)
    scenedepth = mel.create_material_expression(mat, unreal.MaterialExpressionSceneDepth, -1100, 840)
    pixeldepth = mel.create_material_expression(mat, unreal.MaterialExpressionPixelDepth, -1100, 920)

    # ---- Custom raymarch node ----
    custom = mel.create_material_expression(mat, unreal.MaterialExpressionCustom, -500, 100)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT4)
    custom.set_editor_property("description", "RebusBeamRaymarch")
    custom.set_editor_property("code", _BEAM_RAYMARCH_HLSL)

    input_names = [
        "CamPos", "PixelPos", "SceneDepth", "PixelDepth",
        "BeamColor", "BeamIntensity", "BeamSharpness", "BeamFalloff",
        "StepCount", "BeamDensity", "BeamOrigin", "BeamDir",
        "BeamLength", "LensRadius", "FarRadius",
        # v1.0.111 light-space depth-mask plumbing -- six scalars + three vectors +
        # one texture-object. The texture pin (`BeamShadowMaskRT`) doesn't need a
        # special pin-type tag: the engine infers `Texture2D` from the connected
        # `MaterialExpressionTextureObjectParameter` and auto-generates the companion
        # `SamplerState BeamShadowMaskRTSampler` symbol the HLSL body samples through.
        "BeamShadowMaskEnabled", "BeamShadowMaskBiasCm", "BeamShadowMaskFadeCm",
        "BeamShadowMaskFarCm", "BeamShadowMaskTanHalfFov", "BeamShadowMaskDebug",
        "BeamLightFwd", "BeamLightRight", "BeamLightUp",
        "BeamShadowMaskRT",
        # v1.0.128 -- beam-side gobo projection. Same texture-object + scalar
        # plumbing shape as `BeamShadowMaskRT` / `BeamShadowMaskEnabled` above;
        # the engine auto-generates `SamplerState GoboTextureSampler` for the
        # `Texture2DSample(GoboTexture, GoboTextureSampler, uv)` call inside
        # `_BEAM_RAYMARCH_HLSL`. See the master-param doc-comment above for the
        # projection contract.
        "GoboTexture", "GoboEnable",
    ]
    custom_inputs = []
    for n in input_names:
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", unreal.Name(n))
        custom_inputs.append(ci)
    custom.set_editor_property("inputs", custom_inputs)

    src_for = {
        "CamPos": campos, "PixelPos": pixelpos, "SceneDepth": scenedepth, "PixelDepth": pixeldepth,
        "BeamColor": color, "BeamIntensity": intensity, "BeamSharpness": sharp, "BeamFalloff": falloff,
        "StepCount": stepcount, "BeamDensity": density, "BeamOrigin": beamorigin, "BeamDir": beamdir,
        "BeamLength": beamlen, "LensRadius": lensrad, "FarRadius": farrad,
        "BeamShadowMaskEnabled": mask_enabled, "BeamShadowMaskBiasCm": mask_bias,
        "BeamShadowMaskFadeCm": mask_fade, "BeamShadowMaskFarCm": mask_far,
        "BeamShadowMaskTanHalfFov": mask_tan, "BeamShadowMaskDebug": mask_debug,
        "BeamLightFwd": beamlightfwd, "BeamLightRight": beamlightright, "BeamLightUp": beamlightup,
        "BeamShadowMaskRT": mask_rt,
        # v1.0.128 -- beam-side gobo projection (issue 1).
        "GoboTexture": gobo_tex_obj, "GoboEnable": gobo_enable,
    }
    for n in input_names:
        mel.connect_material_expressions(src_for[n], "", custom, n)

    # Custom returns float4(rgb beam colour, a coverage): rgb -> Emissive, a -> Opacity.
    rgb_mask = mel.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -180, 40)
    _set(rgb_mask, "r", True)
    _set(rgb_mask, "g", True)
    _set(rgb_mask, "b", True)
    _set(rgb_mask, "a", False)
    mel.connect_material_expressions(custom, "", rgb_mask, "")
    mel.connect_material_property(rgb_mask, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    a_mask = mel.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -180, 200)
    _set(a_mask, "r", False)
    _set(a_mask, "g", False)
    _set(a_mask, "b", False)
    _set(a_mask, "a", True)
    mel.connect_material_expressions(custom, "", a_mask, "")
    mel.connect_material_property(a_mask, "", unreal.MaterialProperty.MP_OPACITY)

    mel.recompile_material(mat)


REBUS_BEAM_MATERIAL_REVISION = 122
"""v1.0.117 -- sentinel revision baked into the on-disk M_RebusBeam master via the
`BeamMaterialRevision` scalar parameter. Bumped by every release that touches the
master so the v1.0.112 auto-purge probe in
`URebusVisualiserSubsystem::ProbeAndAutoPurgeStaleBeamMaster` (C++) can detect a
stale on-disk master deterministically (instead of relying on a cooked-editor-state
md5 which differs across operator machines / build configs). Bump this AND the C++
mirror `RebusExpectedBeamMaterialRevision` in lockstep when the master changes.

v1.0.119 -- bumped 117 -> 119. The v1.0.117 / v1.0.118 builds shipped a Python
file whose `build()` / `ensure_base_level()` functions mixed TAB and SPACE
indentation -- Python 3 raises `TabError` at PARSE time so `import build_rebus
_base_level` failed unconditionally, the editor-startup level-author no-opped,
AND the C++ auto-purge's `py import build_rebus_base_level; build_rebus_base
_level.ensure_beam_material(force=True)` Exec failed at the import line.
Operator effect: the v1.0.117 PRIMARY ROOT-CAUSE FIX (`disable_depth_test = True`
on the master + matching `bRenderInDepthPass = false` on the cone primitive) only
applied to the COMPONENT side; the MASTER side never re-baked because the regen
path was non-functional. v1.0.119 fixes the Python file AND bumps this constant
so the auto-purge will recognise any v1.0.117/v1.0.118 baked master (revision
117) as STALE and force-regen it via the now-importable Python module. The
v1.0.117 master graph itself is unchanged in v1.0.119 (no shader / parameter
changes); the revision bump exists purely to invalidate cached masters cooked
during the broken window.

v1.0.120 -- bumped 119 -> 120 as part of the stop-the-bleeding release. v1.0.119
shipped an auto-regen path that CRASHED the editor binary when launched with
`-game` (the standard PRISM Pixel Streaming orchestrator command line:
`UnrealEditor-Cmd.exe ... -game -PixelStreamingURL=...`), because this Python
function calls `unreal.EditorAssetLibrary.*` / `unreal.MaterialEditingLibrary.*`
/ `unreal.AssetToolsHelpers.get_asset_tools()` -- ALL editor-only APIs that
dereference uninitialised editor subsystem state in `-game` mode and raise
EXCEPTION_ACCESS_VIOLATION on the `EditorScriptingUtilities.dll` frame.
v1.0.120 gates the regen at TWO chokepoints (C++ `URebusVisualiserSubsystem::
CanRegenBeamMasterInProcess()` and Python `_is_editor_runtime()`) so the crash
is impossible regardless of which entry point is taken. The revision bump
itself ensures the v1.0.112 auto-purge probe correctly recognises pre-v1.0.120
cooked masters as stale when a developer next opens the project in a real
editor session and re-bakes (the operator workflow documented in README
v1.0.120 release block). The master graph itself is UNCHANGED in v1.0.120
(no shader / parameter changes); the bump exists purely to invalidate cached
masters from the broken v1.0.119 window so the next editor-session bake re-
stamps them at the new revision.

v1.0.121 -- bumped 120 -> 121 alongside the commandlet-driven offline bake.
v1.0.120 left the master STALE on disk because the only valid bake host was
an interactive editor session and the user (correctly) refused to bake by
hand. v1.0.121 relaxes the C++ + Python gates to also allow COMMANDLETS
(`-run=PythonScript ...`), so the bake can be driven end-to-end from CI / a
shell via `UnrealEditor-Cmd.exe REBUS_Visualiser.uproject -run=PythonScript
-Script="import build_rebus_base_level as b; b.ensure_beam_material(force
=True); b.ensure_ies_profiles(force=True)" -unattended -nop4 -nosplash`. The
revision bump invalidates every pre-v1.0.121 cooked master so the first
commandlet bake against an existing checkout produces a fresh .uasset at
the new sentinel. The master GRAPH itself is unchanged in v1.0.121; only
the sentinel + the gate.

v1.0.128 -- bumped 121 -> 122. UNLIKE v1.0.120 / v1.0.121 (sentinel-only
bumps), v1.0.128 ACTUALLY changes the master graph -- adds the v1.0.128
beam-side gobo projection plumbing: `GoboTexture` (TextureObjectParameter,
default white) + `GoboEnable` (Scalar, default 0.0) parameters wired into
the `_BEAM_RAYMARCH_HLSL` Custom node, with a per-step planar projection
that modulates the raymarch density by the gobo sample in the SpotLight's
local right/up axes (same basis as the v1.0.111 depth-mask). Fixes the
"gobo selected via DMX but no pattern visible inside the volumetric beam
shaft" symptom (issue 1 of the v1.0.128 brief): pre-v1.0.128 the procedural
cone master had no gobo sampler at all, so a fixture running the default
`bPreferProceduralBeam=1` (since v1.0.106) showed the cookie on the lit
floor pool (M_Light_Master's MF_DMXGobo) + on the lens face
(M_RebusFixtureLens' v1.0.102 emissive layer) but the volumetric shaft
between the lens and the floor stayed uniformly bright. v1.0.128 adds the
same `GoboRT` per-fixture render target the cookie path samples as the
texture source, projected planarly through the cone interior so the shaft
modulation matches the floor pattern in lockstep. C++ side: new
`RefreshBeamGoboParams()` method pushes `GoboTexture` + `GoboEnable` onto
BOTH the cone slot-0 MID AND the cached `BeamMID` (the v1.0.125 double-
push pattern) from every gobo-state apply site (`ApplyCurrentGoboToEpic
Beam` tail, `ClearGoboToOpen`, `OnGoboRTUpdate`, `BuildBeamCone` seed).
The bump invalidates every pre-v1.0.128 cooked master so the v1.0.112
auto-purge probe recognises them as stale and the (commandlet-driven)
regen produces a current master with the gobo plumbing baked in. Until
that bake runs the on-disk master will report `BeamMaterialRevision=121`
and the C++ `SelfHealBeamMaterialRevisionIfMismatched` will log a warning
on every fixture spawn (the existing post-spawn self-heal probe, see
v1.0.119); the gobo push itself silently no-ops against the stale master
because the parameter doesn't exist -- pre-v1.0.128 behaviour preserved
exactly (cone visible, no gobo modulation) until the rebake lands.
"""


def _beam_master_has_shadow_mask(master):
    """v1.0.111 (extended v1.0.117): best-effort probe that the existing on-disk
    M_RebusBeam master declares (a) the v1.0.111 light-space depth-mask parameter
    set (`BeamShadowMaskEnabled`, `BeamShadowMaskBiasCm`, `BeamShadowMaskFadeCm`,
    `BeamShadowMaskFarCm`, `BeamShadowMaskTanHalfFov`, `BeamShadowMaskDebug` scalars
    + `BeamLightFwd`, `BeamLightRight`, `BeamLightUp` vectors + `BeamShadowMaskRT`
    texture) AND (b) the v1.0.117 `BeamMaterialRevision` sentinel scalar at the
    current revision constant `REBUS_BEAM_MATERIAL_REVISION` (117). When ANY of
    those are missing OR the revision sentinel reads as a different number, the
    on-disk master pre-dates v1.0.117 (or has been clobbered by a side-quest
    edit) and the C++ per-fixture state push will silently no-op against the
    stale parameter contract / missing material flags (`disable_depth_test`,
    `bUsedWithVolumetricFog`, ...) -- so the v1.0.117 root-cause fix never
    actually engages.

    Mirrors the v1.0.104 (`_orbit_imported_master_has_two_sided`) / v1.0.97
    (`_master_is_two_sided`) self-heal probes -- best-effort, ANY exception
    yields False so the self-heal treats the master as needs-regen (a redundant
    rebake is cheap; missing the v1.0.117 contract would leave the operator on
    an old un-fixed master indefinitely).

    Returns True when the master DOES declare the full v1.0.117 parameter set
    AT the current revision AND therefore needs no migration; False otherwise.
    """
    try:
        scalars = unreal.MaterialEditingLibrary.get_scalar_parameter_names(master)
        vectors = unreal.MaterialEditingLibrary.get_vector_parameter_names(master)
        textures = unreal.MaterialEditingLibrary.get_texture_parameter_names(master)
    except Exception:  # noqa: BLE001
        return False
    scalar_names = {str(n) for n in scalars}
    vector_names = {str(n) for n in vectors}
    texture_names = {str(n) for n in textures}
    required_scalars = {
        "BeamShadowMaskEnabled", "BeamShadowMaskBiasCm", "BeamShadowMaskFadeCm",
        "BeamShadowMaskFarCm", "BeamShadowMaskTanHalfFov", "BeamShadowMaskDebug",
        # v1.0.117 -- the revision sentinel must exist AND read the expected value
        # below for the master to count as "matching".
        "BeamMaterialRevision",
        # v1.0.128 -- beam-side gobo enable scalar. Missing here means the master
        # pre-dates the v1.0.128 gobo-plumbing rebake -- the C++ push will silently
        # no-op against the stale parameter contract and the volumetric beam shaft
        # will read uniformly bright (the pre-v1.0.128 visual). Probe failure here
        # routes through the same auto-purge / commandlet bake path the v1.0.111
        # shadow-mask plumbing uses.
        "GoboEnable",
    }
    required_vectors = {"BeamLightFwd", "BeamLightRight", "BeamLightUp"}
    required_textures = {
        "BeamShadowMaskRT",
        # v1.0.128 -- beam-side gobo texture parameter (per-fixture cookie RT).
        "GoboTexture",
    }
    if not (required_scalars.issubset(scalar_names)
            and required_vectors.issubset(vector_names)
            and required_textures.issubset(texture_names)):
        return False
    # v1.0.117 -- revision sentinel check. A master that has the parameter but
    # at a different revision is stale (e.g. an old v1.0.117-dev build whose
    # constants haven't been bumped yet, or a hand-edit that rewrote the
    # default to a sentinel test value). Best-effort -- ANY exception in the
    # default-read path means we fall through to "stale".
    try:
        rev_val = unreal.MaterialEditingLibrary.get_material_default_scalar_parameter_value(
            master, "BeamMaterialRevision")
        if int(round(float(rev_val))) != REBUS_BEAM_MATERIAL_REVISION:
            return False
    except Exception:  # noqa: BLE001
        return False
    return True


def ensure_beam_material(force=False):
    """Generate the faux-volumetric beam master material. Idempotent (only creates when missing)
    unless force=True (delete + regenerate, e.g. during a full build()).

    v1.0.111 self-heal: a non-force call against a pre-v1.0.111 master that's missing the new
    light-space depth-mask parameter set (`_beam_master_has_shadow_mask` returns False) gets
    promoted to a force-regen with a Warning log -- without that the operator would silently
    keep an old master where the C++ per-fixture `RefreshBeamShadowMaskParams` push reaches
    no-op-against-unknown-name and the new v1.0.111 occlusion never engages. Matches the
    v1.0.104 (`_orbit_imported_master_has_two_sided`) / v1.0.97 (`_master_is_two_sided`)
    cascade shape -- one probe -> one promotion -> one regen log line.

    v1.0.120 STOP-THE-BLEEDING: hard guard at the top -- returns False without touching
    any editor-only API when `_is_editor_runtime()` is false. v1.0.119 crashed the user's
    UE session because the C++ auto-purge invoked this function via `GEngine->Exec("py
    import build_rebus_base_level; build_rebus_base_level.ensure_beam_material(force=True)")`
    from a `-game` mode session. `unreal.EditorAssetLibrary.does_asset_exist(BEAM_PATH)`
    (the FIRST editor-only call below) dereferences uninitialised editor subsystem state
    in `-game` mode and raises EXCEPTION_ACCESS_VIOLATION on the `EditorScriptingUtilities
    .dll` frame, taking the entire UE process down. The C++ side now also gates this
    (see `URebusVisualiserSubsystem::CanRegenBeamMasterInProcess()`); this guard is the
    belt-and-braces complement so even an out-of-band `py` invocation can't crash.
    Returns False (caller-observable signal that nothing happened) instead of raising,
    consistent with the early-return shape of the other self-heal probes in this module.
    """
    if not _is_editor_runtime():
        unreal.log_warning(
            "RebusBaseLevel: ensure_beam_material(force={}) ABORTING -- not in an editor "
            "runtime (`unreal.is_editor() == False`; -game / -server / Standalone "
            "mode). Editor-only asset APIs (EditorAssetLibrary / "
            "MaterialEditingLibrary / AssetToolsHelpers) would crash the process "
            "with EXCEPTION_ACCESS_VIOLATION here -- exactly the v1.0.119 user-"
            "reported crash. The on-disk /Game/REBUS/Materials/M_RebusBeam.uasset "
            "(if present) will be used as-is by the runtime. To regenerate: drive "
            "the v1.0.121 commandlet bake "
            "`UnrealEditor-Cmd.exe REBUS_Visualiser.uproject -run=PythonScript "
            "-Script=\"import build_rebus_base_level as b; b.ensure_beam_material("
            "force=True); b.ensure_ies_profiles(force=True)\" -unattended -nop4 "
            "-nosplash -stdout -FullStdOutLogOutput` from a shell, then commit the "
            "regenerated .uasset (+ .uexp) to source control. Future -game sessions "
            "will then load the pre-baked master without needing this rebuild.".format(force))
        return False

    tools = unreal.AssetToolsHelpers.get_asset_tools()

    # v1.0.111 non-force self-heal: pre-v1.0.111 master missing the light-space depth-mask
    # contract => promote to force-regen so the new parameter set is bound on disk.
    if not force and unreal.EditorAssetLibrary.does_asset_exist(BEAM_PATH):
        existing = unreal.EditorAssetLibrary.load_asset(BEAM_PATH)
        if existing is not None and not _beam_master_has_shadow_mask(existing):
            unreal.log_warning(
                "RebusBaseLevel: pre-v1.0.111 M_RebusBeam detected (missing one or more of "
                "BeamShadowMask*/BeamLight* parameters); regenerating so the v1.0.111 "
                "light-space depth-mask beam-occlusion path can bind."
            )
            force = True

    if force and unreal.EditorAssetLibrary.does_asset_exist(BEAM_PATH):
        unreal.EditorAssetLibrary.delete_asset(BEAM_PATH)

    if not unreal.EditorAssetLibrary.does_asset_exist(BEAM_PATH):
        mat = tools.create_asset("M_RebusBeam", MATERIALS_DIR, unreal.Material, unreal.MaterialFactoryNew())
        _build_beam_master(mat)
        unreal.EditorAssetLibrary.save_loaded_asset(mat)

    unreal.log("RebusBaseLevel: beam material ensured.")
    return True


# ---------------------------------------------------------------------------------------
# v1.0.121 -- IES profile pre-bake. Walks IES_SOURCE_DIR + the runtime capture
# inbox (Saved/REBUS/IES_Inbox/), converts every .ies file into a
# UTextureLightProfile .uasset under /Game/REBUS/IES/, and tags each baked asset
# with the v1.0.121 IesProfileRevision metadata so the runtime cache can verify
# (and a future bump can invalidate). Mirrors `ensure_beam_material`: idempotent
# by default (skip when the asset is already on disk AND at the current
# revision), force=True deletes + re-imports.
# ---------------------------------------------------------------------------------------

def _sanitize_ies_profile_name(profile_id):
    """Sanitize an arbitrary profile id (UUID, free-form display name, file
    basename) into a UE-asset-safe name. Keeps [A-Za-z0-9_-]; everything else
    becomes underscore. Matches the C++ `SanitizeIesProfileName` helper in
    `RebusVisualiserSubsystem.cpp` so the bake-time path and the runtime
    LoadObject<UTextureLightProfile> path agree byte-exact on the asset name.

    Falls back to "ies_profile" when the result would be empty (extremely
    short / all-punctuation ids).
    """
    if not profile_id:
        return "ies_profile"
    safe = re.sub(r"[^A-Za-z0-9_\-]", "_", str(profile_id))
    # Collapse consecutive underscores so a UUID that turned into "abc__def" reads
    # cleanly; idempotent against names that are already collapsed.
    safe = re.sub(r"_+", "_", safe).strip("_-")
    return safe or "ies_profile"


def _ies_package_path(profile_name):
    """Compose the /Game-rooted package path for a baked IES profile asset."""
    return "{}/{}".format(IES_PACKAGE_DIR, profile_name)


def _saved_inbox_dir():
    """Resolve the absolute disk path of the inline-IES capture inbox at runtime.
    Uses `unreal.Paths.project_saved_dir()` so the path is whatever Saved/ the
    running project resolved (works whether the bake commandlet was launched
    from the workspace repo, the PRISM deployment dir, or a CI scratch).
    Returns None on best-effort failure so the caller can skip the inbox walk
    silently (logged).
    """
    try:
        saved = unreal.Paths.project_saved_dir()
    except Exception:  # noqa: BLE001
        return None
    if not saved:
        return None
    return os.path.normpath(os.path.join(saved, IES_INBOX_DIR_SAVED))


def _list_ies_source_files(workspace_root_hint=None):
    """Return a list of absolute paths to every `.ies` file the bake should
    consider. Walks IES_SOURCE_DIR (committed source) and the Saved inbox
    (runtime captures). The source folder is relative to the project root --
    we use `unreal.Paths.project_dir()` at runtime (since the bake commandlet
    might be running against the workspace OR a deployment dir), with an
    optional `workspace_root_hint` override for headless callers that know
    better.

    Returns a list of (absolute_disk_path, profile_id) tuples. `profile_id` is
    the basename without `.ies` (and gets sanitized downstream into the asset
    name) -- the source folder convention is to name files by the portal's
    profile id (e.g. `96d62ffd-faf6-4bf5-a551-c4c774aa066c.ies`) so the runtime
    cache hits on the same id the portal pushes via RegisterFixtureIes.
    """
    out = []

    # Source dir (committed)
    try:
        project_dir = workspace_root_hint or unreal.Paths.project_dir()
    except Exception:  # noqa: BLE001
        project_dir = workspace_root_hint or ""
    if project_dir:
        # IES_SOURCE_DIR is repo-relative (rooted at the workspace root, ABOVE the
        # project's `REBUS_Visualiser/` folder). Try both layouts: repo-root + the
        # explicit prefix, and project-root with the trailing slice (Content/REBUS/
        # IES/Source) so the convention works from either reference frame.
        candidates = []
        # Project-relative form (canonical for a commandlet bake against an
        # in-place .uproject): <ProjectDir>/Content/REBUS/IES/Source
        candidates.append(os.path.normpath(
            os.path.join(project_dir, "Content", "REBUS", "IES", "Source")))
        # Workspace-relative form (repo root): <ProjectDir>/../IES_SOURCE_DIR
        candidates.append(os.path.normpath(
            os.path.join(project_dir, "..", IES_SOURCE_DIR)))
        # Bare absolute hint
        candidates.append(os.path.normpath(IES_SOURCE_DIR))
        for src_dir in candidates:
            if not os.path.isdir(src_dir):
                continue
            for fname in sorted(os.listdir(src_dir)):
                if fname.lower().endswith(".ies"):
                    abs_path = os.path.join(src_dir, fname)
                    pid = os.path.splitext(fname)[0]
                    out.append((abs_path, pid))
            break  # only the first existing candidate; don't double-bake

    # Saved inbox (runtime captures from the IES descriptor handler).
    inbox_dir = _saved_inbox_dir()
    if inbox_dir and os.path.isdir(inbox_dir):
        for fname in sorted(os.listdir(inbox_dir)):
            if fname.lower().endswith(".ies"):
                abs_path = os.path.join(inbox_dir, fname)
                pid = os.path.splitext(fname)[0]
                out.append((abs_path, pid))

    return out


def _import_one_ies(src_abs_path, profile_id):
    """Drive the engine's IES factory to import `src_abs_path` into
    `/Game/REBUS/IES/<sanitized profile_id>` as a UTextureLightProfile asset.
    Uses `unreal.AssetImportTask` so the import goes through the SAME path the
    editor's Import button uses (no need to manually build a factory + walk
    asset registry). Idempotent in conjunction with the caller's existing-asset
    delete/skip pre-pass; the task is always set to `replace_existing=True` for
    forced re-imports.

    Returns the created/updated UObject (UTextureLightProfile) on success, None
    on failure (logged).
    """
    safe_name = _sanitize_ies_profile_name(profile_id)
    package_path = _ies_package_path(safe_name)

    task = unreal.AssetImportTask()
    task.filename = src_abs_path
    task.destination_path = IES_PACKAGE_DIR
    task.destination_name = safe_name
    task.automated = True
    task.save = True
    task.replace_existing = True

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    try:
        tools.import_asset_tasks([task])
    except Exception as exc:  # noqa: BLE001
        unreal.log_warning(
            "RebusBaseLevel v1.0.121: import_asset_tasks failed for IES '{}' ({}): {}".format(
                profile_id, src_abs_path, exc))
        return None

    # The task carries `imported_object_paths` after a successful import; resolve
    # the asset via EditorAssetLibrary so we can write the revision metadata tag.
    asset = unreal.EditorAssetLibrary.load_asset(package_path)
    if not asset:
        unreal.log_warning(
            "RebusBaseLevel v1.0.121: IES import for '{}' produced no asset at {} "
            "(check that the .ies file is well-formed: {}).".format(
                profile_id, package_path, src_abs_path))
        return None

    # v1.0.121 revision tag -- writable user metadata on the asset's package, queryable
    # at runtime via the asset registry. A future bump invalidates everything baked
    # under this revision automatically (the C++ cache reads + compares).
    try:
        unreal.EditorAssetLibrary.set_metadata_tag(
            asset, "IesProfileRevision", str(REBUS_IES_PROFILE_REVISION))
        unreal.EditorAssetLibrary.set_metadata_tag(
            asset, "IesSourcePath", src_abs_path)
        unreal.EditorAssetLibrary.save_loaded_asset(asset)
    except Exception as exc:  # noqa: BLE001
        unreal.log_warning(
            "RebusBaseLevel v1.0.121: could not write IesProfileRevision metadata "
            "tag on {} ({}); asset is still usable, just lacks the bake-revision "
            "sentinel.".format(package_path, exc))
    return asset


def _ies_asset_is_current(profile_name):
    """True iff /Game/REBUS/IES/<profile_name> exists AND its IesProfileRevision
    metadata tag matches REBUS_IES_PROFILE_REVISION. Used by the non-force
    idempotent path so a steady-state bake skips already-current assets.
    """
    pkg = _ies_package_path(profile_name)
    if not unreal.EditorAssetLibrary.does_asset_exist(pkg):
        return False
    try:
        asset = unreal.EditorAssetLibrary.load_asset(pkg)
        if not asset:
            return False
        rev = unreal.EditorAssetLibrary.get_metadata_tag(asset, "IesProfileRevision")
        if rev is None or rev == "":
            return False
        return int(rev) == REBUS_IES_PROFILE_REVISION
    except Exception:  # noqa: BLE001
        return False


def ensure_ies_profiles(force=False):
    """Bake every available `.ies` file into a `/Game/REBUS/IES/<sanitized id>`
    UTextureLightProfile asset. Mirrors `ensure_beam_material`:
      * Same v1.0.120 editor-runtime guard (`_is_editor_runtime`) -- no-op + warn
        in -game / -server.
      * Idempotent by default (force=False) -- skips assets already at the
        current revision; bakes anything missing or stale.
      * force=True deletes existing baked assets first and re-imports them all,
        re-stamping the revision tag.

    Source files come from two folders:
      1. `REBUS_Visualiser/Content/REBUS/IES/Source/` (committed source files,
         the canonical place to add new profiles). Add files named `<profile id
         from portal>.ies` here.
      2. `<ProjectSaved>/REBUS/IES_Inbox/` (runtime captures written by the C++
         `RegisterFixtureIes` handler when running in editor / commandlet mode
         -- so a one-time visualiser session with the portal connected captures
         every inline-pushed profile, then the bake commandlet picks them up).

    Returns the list of (profile_id, package_path) tuples actually baked
    (skipped-current entries are NOT in the list). Empty list on full-skip or
    when no source files are present.
    """
    if not _is_editor_runtime():
        unreal.log_warning(
            "RebusBaseLevel: ensure_ies_profiles(force={}) ABORTING -- not in an editor "
            "runtime (`unreal.is_editor() == False`; -game / -server / Standalone mode). "
            "Editor-only asset APIs (AssetImportTask / EditorAssetLibrary) would crash "
            "the process with EXCEPTION_ACCESS_VIOLATION here. Already-baked IES assets "
            "under /Game/REBUS/IES/ (if present) will be used as-is by the runtime; any "
            "profile id without a pre-baked asset falls back to the synthesized cone. "
            "To regenerate: drive the v1.0.121 commandlet bake `UnrealEditor-Cmd.exe "
            "REBUS_Visualiser.uproject -run=PythonScript -Script=\"import "
            "build_rebus_base_level as b; b.ensure_beam_material(force=True); "
            "b.ensure_ies_profiles(force=True)\" -unattended -nop4 -nosplash -stdout "
            "-FullStdOutLogOutput`.".format(force))
        return []

    sources = _list_ies_source_files()
    if not sources:
        unreal.log_warning(
            "RebusBaseLevel v1.0.121: no source IES files found under "
            "REBUS_Visualiser/Content/REBUS/IES/Source/ or Saved/REBUS/IES_Inbox/. "
            "If you expected profiles to be present, either (a) commit source .ies "
            "files to Content/REBUS/IES/Source/, OR (b) run the visualiser once in "
            "editor / commandlet mode with the portal connected so the v1.0.121 "
            "inline-IES capture path writes captured profiles to the Saved inbox, "
            "then re-run this bake. Nothing to do.")
        return []

    baked = []
    # Pre-pass: in force mode wipe existing IES assets so deletes happen before
    # re-imports (mirrors the M_RebusBeam.uasset force-delete pattern).
    seen_safe_names = set()
    if force:
        for _src, pid in sources:
            safe = _sanitize_ies_profile_name(pid)
            if safe in seen_safe_names:
                continue
            seen_safe_names.add(safe)
            pkg = _ies_package_path(safe)
            if unreal.EditorAssetLibrary.does_asset_exist(pkg):
                try:
                    unreal.EditorAssetLibrary.delete_asset(pkg)
                except Exception as exc:  # noqa: BLE001
                    unreal.log_warning(
                        "RebusBaseLevel v1.0.121: could not delete stale IES "
                        "asset {} ({}); the re-import will try to overwrite "
                        "in place.".format(pkg, exc))

    seen_safe_names = set()
    for src_abs_path, pid in sources:
        safe = _sanitize_ies_profile_name(pid)
        if safe in seen_safe_names:
            # The same id appeared in both Source/ AND IES_Inbox/. Prefer the
            # FIRST entry encountered (Source folder is walked first, so a
            # committed source wins over a captured inbox copy).
            continue
        seen_safe_names.add(safe)

        pkg = _ies_package_path(safe)
        if not force and _ies_asset_is_current(safe):
            # Already at the current revision; skip.
            continue

        asset = _import_one_ies(src_abs_path, pid)
        if asset is None:
            continue
        baked.append((pid, pkg))

    unreal.log(
        "RebusBaseLevel v1.0.121: IES profiles ensured ({} source files seen, {} "
        "baked at revision {}). Profiles: {}".format(
            len(sources), len(baked), REBUS_IES_PROFILE_REVISION,
            ", ".join(_sanitize_ies_profile_name(pid) for pid, _ in baked) or "(none)"))
    return baked


# ---------------------------------------------------------------------------------------
# v1.0.93 -- Fix 3: M_RebusFixtureLens (mirror/glass lens disc).
# ---------------------------------------------------------------------------------------
#
# Previously operator-authored (v1.0.71 added the asset PATH the C++ constructor's
# FObjectFinder looks for, but never the Python builder). The runtime fallback MID
# (FixtureLensMID off /Engine/BasicShapes/BasicShapeMaterial, Metallic=1 Roughness=0 since
# v1.0.89) handles the case when the .uasset doesn't exist, but the user wants the asset
# itself baked by the startup script so a fresh checkout / project regenerate lands the
# polished-mirror lens the first time without the "missing asset" runtime fallback path.

def _build_fixture_lens_master(mat):
    """Mirror/glass for the GDTF <Beam> lens disc (v1.0.93 -- Python-authored,
    v1.0.102 -- emissive layer follows the fixture's live dimmer x colour x gobo).

    BaseColor / Metallic / Roughness stay as v1.0.93: Metallic=1.0, Roughness=0.0,
    BaseColor=white -- approximates a polished chrome lens (true dielectric translucent
    glass is beyond the surface/lit domain; the dark fixture interior absorbs the back
    so the metallic-mirror approximation reads visually correct against the chrome
    accents on a moving head). The PBR layer is unchanged so a fixture with dimmer=0
    reads identical to pre-v1.0.102 -- chrome mirror, no glow.

    v1.0.102 -- additive EMISSIVE chain. User request (verbatim, v1.0.102):
        "can the lens material be emiissive as well and follow the dimmer, colour and
        gobo of the fixture its part of."
    The lens is now ALSO emissive: a fixture at full intensity with a red colour reads
    as a glowing red disc, and with a leaf gobo loaded the gobo silhouette is visible
    directly on the lens face. The emissive layer is ADDITIVE on top of the existing
    chrome PBR so dimmer=0 still reads as chrome (the emissive output is `Emissive *
    EmissiveIntensity * GoboMix`, and `EmissiveIntensity` defaults to 0.0 so an editor
    preview reads as chrome mirror; the live C++ MID push from
    `ARebusFixtureActor::RefreshLensEmissive` overrides at runtime).
    The four new parameters and their combine formula:
        * `Emissive` (vector, default white) -- the fixture's CURRENT live colour
          (`ColorR/G/B.Current`).
        * `EmissiveIntensity` (scalar, default 0.0) -- live dimmer x shutter-gate x
          `Rebus.LensEmissiveScale`. Default 0 so the asset's editor preview reads as
          a chrome mirror with no glow.
        * `GoboTexture` (Texture2D, default `/Engine/EngineResources/WhiteSquareTexture`
          -- same default the v1.0.86 ground BaseColorTexture uses so an untextured MI
          no-ops to white) -- the per-fixture cookie render-target so the lens face
          shows the gobo pattern.
        * `bUseGobo` (scalar, default 0.0) -- 0 = lens glows UNIFORM colour (gobo
          sample ignored, GoboMix == 1.0), 1 = lens face is masked by the gobo sample.
          Driven from the per-fixture `bGoboActive` flag in C++.
    Combine formula (matches the v1.0.102 task spec):
        GoboSample = TextureSample(GoboTexture, TexCoord0)
        GoboMix    = Lerp(One, GoboSample.rgb, bUseGobo)
        EmissiveOut = Emissive * EmissiveIntensity * GoboMix -> MP_EMISSIVE_COLOR

    v1.0.97: double-sided ("make every Rebus-authored Python master double-sided" --
    see README v1.0.97). The procedural <Beam> lens disc is a thin two-faced ring
    sitting at the cone apex; once the camera passes inside the head shell (or the
    fixture is physically wide enough that the lens reads from a glancing angle) the
    lens read as a black hole because the back face was culled. Double-siding makes
    the chrome mirror visible from inside the head, matching the v1.0.95 deliverable's
    intent that the lens object is "always visible" in Epic-beam mode.
    """
    mel = unreal.MaterialEditingLibrary

    _set(mat, "material_domain", unreal.MaterialDomain.MD_SURFACE)
    _set(mat, "shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    _set(mat, "blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    # v1.0.97: see docstring + README v1.0.97.
    _set(mat, "two_sided", True)

    # ---- v1.0.93 PBR chain (unchanged) ----
    color = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -600, -150)
    color.set_editor_property("parameter_name", "Color")
    color.set_editor_property("default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))
    mel.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)

    metal = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -600, 40)
    metal.set_editor_property("parameter_name", "Metallic")
    metal.set_editor_property("default_value", 1.0)
    mel.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)

    rough = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -600, 200)
    rough.set_editor_property("parameter_name", "Roughness")
    rough.set_editor_property("default_value", 0.0)
    mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

    # ---- v1.0.102 EMISSIVE chain (additive on top of the chrome PBR) ----
    # Emissive vector + EmissiveIntensity scalar carry the live dimmer x colour from
    # `ARebusFixtureActor::RefreshLensEmissive` (the C++ MID push). Default 0 intensity
    # so the editor preview reads as chrome mirror only.
    emissive = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -1200, 420)
    emissive.set_editor_property("parameter_name", "Emissive")
    emissive.set_editor_property("default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))

    emissive_intensity = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -1200, 580)
    emissive_intensity.set_editor_property("parameter_name", "EmissiveIntensity")
    emissive_intensity.set_editor_property("default_value", 0.0)

    # GoboTexture: per-fixture cookie render-target so the gobo silhouette shows on the
    # lens face. Default `/Engine/EngineResources/WhiteSquareTexture` matches the v1.0.86
    # ground BaseColorTexture pattern -- a white default sampler so an untextured MI
    # no-ops to 1.0 and the lens glows uniform colour (matching bUseGobo=0).
    gobo_tex = mel.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D, -1200, 740)
    gobo_tex.set_editor_property("parameter_name", "GoboTexture")
    _white = unreal.EditorAssetLibrary.load_asset("/Engine/EngineResources/WhiteSquareTexture")
    if _white is not None:
        _set(gobo_tex, "texture", _white)
    # Sample the gobo across the disc's UV0 -- both the real <Beam> procedural-mesh
    # lens disc (`MeshComponents` built from `/meshes`) and the synthetic LensDisc
    # (engine `/Engine/BasicShapes/Plane`) carry sensible 0..1 disc UVs, so the gobo
    # sample lands on the lens face exactly the way the cookie projects onto the floor.
    tc = mel.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, -1500, 740)
    _set(tc, "coordinate_index", 0)
    mel.connect_material_expressions(tc, "", gobo_tex, "Coordinates")

    # bUseGobo: 0 -> ignore the gobo sample (lens glows uniform colour), 1 -> mask the
    # emissive by the gobo. Combined via Lerp(One, GoboSample, bUseGobo) so bUseGobo=0
    # passes Emissive * EmissiveIntensity through unchanged, bUseGobo=1 multiplies by
    # the gobo RGB. C++ drives this from `bGoboActive` -- 1.0 while a gobo is live,
    # 0.0 when cleared to Open.
    use_gobo = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -1200, 900)
    use_gobo.set_editor_property("parameter_name", "bUseGobo")
    use_gobo.set_editor_property("default_value", 0.0)

    one = mel.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -900, 740)
    _set(one, "constant", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))

    gobo_mix = mel.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -700, 800)
    mel.connect_material_expressions(one, "", gobo_mix, "A")
    mel.connect_material_expressions(gobo_tex, "", gobo_mix, "B")  # default "" pin is RGB
    mel.connect_material_expressions(use_gobo, "", gobo_mix, "Alpha")

    # EmissiveOut = Emissive * EmissiveIntensity * GoboMix
    em_x_int = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -500, 500)
    mel.connect_material_expressions(emissive, "", em_x_int, "A")
    mel.connect_material_expressions(emissive_intensity, "", em_x_int, "B")

    em_out = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -300, 600)
    mel.connect_material_expressions(em_x_int, "", em_out, "A")
    mel.connect_material_expressions(gobo_mix, "", em_out, "B")
    mel.connect_material_property(em_out, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    mel.recompile_material(mat)


def _fixture_lens_master_is_current(master):
    """v1.0.93 self-heal probe -- True when the master exposes the expected parameter set
    (`Color` vector + `Metallic` + `Roughness` scalars). False when an older shape (or an
    operator hand-authored placeholder) is on disk, in which case the non-force ensure
    promotes itself to a force regen.
    """
    try:
        mel = unreal.MaterialEditingLibrary
        vector_names = [str(n) for n in mel.get_vector_parameter_names(master)]
        scalar_names = [str(n) for n in mel.get_scalar_parameter_names(master)]
    except Exception:  # noqa: BLE001
        return False
    return ("Color" in vector_names
            and "Metallic" in scalar_names
            and "Roughness" in scalar_names)


def _fixture_lens_master_has_emissive(master):
    """v1.0.102 self-heal probe -- True when the master exposes the v1.0.102 emissive
    parameter set (`Emissive` vector + `EmissiveIntensity` / `bUseGobo` scalars +
    `GoboTexture` texture param). Mirrors v1.0.97's `_master_is_two_sided` /
    v1.0.93's `_fixture_lens_master_is_current` self-heal pattern: a False here
    promotes the non-force `ensure_fixture_lens_material` path to a force-regen so the
    operator picks up the v1.0.102 chain on the next editor launch without manual
    action. The C++ MID push from `ARebusFixtureActor::RefreshLensEmissive` is a
    silent no-op against a master that lacks these params (UMaterialInstanceDynamic
    setters skip missing parameter names) -- the probe is the only guard against the
    "operator booted on a pre-v1.0.102 baked master, lens stays chrome" failure mode.
    Best-effort: any reflection-API exception (engine-version rename of the texture-
    param enumerator, etc.) yields False so the self-heal treats the master as "needs
    regen". False-negatives are cheap (one redundant rebake on next launch);
    false-positives would leave the operator on the OLD chrome-only master
    indefinitely, which is the failure mode this probe exists to prevent.
    """
    try:
        mel = unreal.MaterialEditingLibrary
        vector_names = [str(n) for n in mel.get_vector_parameter_names(master)]
        scalar_names = [str(n) for n in mel.get_scalar_parameter_names(master)]
        texture_names = [str(n) for n in mel.get_texture_parameter_names(master)]
    except Exception:  # noqa: BLE001
        return False
    return ("Emissive" in vector_names
            and "EmissiveIntensity" in scalar_names
            and "bUseGobo" in scalar_names
            and "GoboTexture" in texture_names)


def ensure_fixture_lens_material(force=False):
    """Generate the mirror/glass lens master material. Idempotent (only creates when
    missing) unless force=True (delete + regenerate, e.g. during a full build()).

    v1.0.93 self-heal: when an EXISTING master is on disk but is missing the v1.0.93
    parameter contract (e.g. an operator-authored placeholder from before v1.0.93 baked
    the asset), promote the call to a force-regen so the C++ FObjectFinder picks up the
    correct shape on the next launch -- log a Warning so the change is auditable.

    v1.0.97 self-heal: same path also detects a pre-v1.0.97 master (still single-sided)
    and promotes to a force-regen so the new double-sided master ships on the next
    launch. Combined via OR with the v1.0.93 contract check so EITHER upgrade triggers a
    single regen.

    v1.0.102 self-heal: same path ALSO detects a pre-v1.0.102 master (missing the new
    `Emissive` / `EmissiveIntensity` / `bUseGobo` / `GoboTexture` params) and promotes
    to a force-regen so the new emissive chain ships on the next launch. Without this
    probe an operator who booted on a v1.0.93-v1.0.101 baked master would silently keep
    a chrome-only lens because the C++ MID push from `ARebusFixtureActor::
    RefreshLensEmissive` no-ops against missing parameter names. The three probes
    combine via OR so any one upgrade triggers a single regen (no triple-bake on a
    project that pre-dates v1.0.93 + v1.0.97 + v1.0.102 simultaneously).
    """
    tools = unreal.AssetToolsHelpers.get_asset_tools()

    if not force and unreal.EditorAssetLibrary.does_asset_exist(FIXTURE_LENS_PATH):
        existing = unreal.EditorAssetLibrary.load_asset(FIXTURE_LENS_PATH)
        if existing is not None and (not _fixture_lens_master_is_current(existing)
                                     or not _master_is_two_sided(existing)
                                     or not _fixture_lens_master_has_emissive(existing)):
            unreal.log_warning("RebusBaseLevel: pre-v1.0.102 M_RebusFixtureLens detected "
                               "(missing Color/Metallic/Roughness parameter contract, "
                               "single-sided, or missing v1.0.102 Emissive/EmissiveIntensity/"
                               "bUseGobo/GoboTexture); regenerating.")
            force = True

    if force and unreal.EditorAssetLibrary.does_asset_exist(FIXTURE_LENS_PATH):
        unreal.EditorAssetLibrary.delete_asset(FIXTURE_LENS_PATH)

    if not unreal.EditorAssetLibrary.does_asset_exist(FIXTURE_LENS_PATH):
        mat = tools.create_asset("M_RebusFixtureLens", MATERIALS_DIR, unreal.Material, unreal.MaterialFactoryNew())
        _build_fixture_lens_master(mat)
        unreal.EditorAssetLibrary.save_loaded_asset(mat)

    unreal.log("RebusBaseLevel: M_RebusFixtureLens ensured (Python-authored mirror/glass).")


# ---------------------------------------------------------------------------------------
# v1.0.95 migration helper. See README v1.0.95 release block for context.
# ---------------------------------------------------------------------------------------

def _cleanup_internal_beam_assets():
    """v1.0.95: delete the legacy material masters in `_LEGACY_INTERNAL_BEAM_ASSETS` if
    they exist. Idempotent: silently no-ops when nothing is present. Logs a Warning the
    first time it deletes anything so the operator can see the migration happen on first
    v1.0.95 launch. Safe to re-run. Mirrors `_master_has_tiling_meters` self-heal (v1.0.86).
    """
    deleted = 0
    for path in _LEGACY_INTERNAL_BEAM_ASSETS:
        if unreal.EditorAssetLibrary.does_asset_exist(path):
            try:
                if unreal.EditorAssetLibrary.delete_asset(path):
                    deleted += 1
                    unreal.log_warning(
                        "RebusBaseLevel v1.0.95: deleted legacy asset {0} (see README "
                        "v1.0.95 release block).".format(path))
            except Exception as e:  # noqa: BLE001
                unreal.log_warning(
                    "RebusBaseLevel v1.0.95: failed to delete legacy asset {0}: {1}. "
                    "Re-run build_rebus_base_level after restarting the editor, or "
                    "delete it manually from the Content Browser.".format(path, e))
    if deleted == 0:
        unreal.log("RebusBaseLevel v1.0.95: no legacy assets found (clean checkout, or migration already ran).")


def _master_has_tiling_meters(master):
    """v1.0.86: detect whether the on-disk master is the new (1 m world-driven tiling) version.

    Returns True if the master exposes the `TilingMeters` scalar parameter; False otherwise
    (pre-v1.0.86 master). We probe via MaterialEditingLibrary's scalar-parameter API which
    enumerates the static parameter set without touching the graph.
    """
    try:
        info = unreal.MaterialEditingLibrary.get_scalar_parameter_names(master)
    except Exception:  # noqa: BLE001
        return False
    return any(str(n) == "TilingMeters" for n in info)


def _master_is_two_sided(master):
    """v1.0.97: best-effort read of a master material's top-level `two_sided` editor property.

    Used by the v1.0.97 self-heal so an EXISTING on-disk master that pre-dates v1.0.97 (and is
    therefore still single-sided) gets force-regenerated on the next editor launch -- mirrors
    v1.0.86 (`_master_has_tiling_meters`) / v1.0.93 (`_fixture_lens_master_is_current`).

    Best-effort: any exception (engine-version rename of the property, asset shape we don't
    recognise) yields False so the self-heal path treats the master as "needs regen". False-
    negatives are cheap (one redundant rebake on next launch); false-positives would leave the
    operator on the OLD single-sided master indefinitely, which is the failure mode this
    probe exists to prevent.

    `two_sided` is a top-level editor property on `unreal.Material`; reading it does not
    require loading the shader graph.
    """
    try:
        return bool(master.get_editor_property("two_sided"))
    except Exception:  # noqa: BLE001
        return False


def ensure_ground_materials(force=False):
    """Generate the procedural ground master + one instance per surface preset.

    Idempotent by default (only creates assets that don't already exist), which is what the
    startup hook wants. With force=True it deletes any existing assets first and regenerates
    them -- an unattended "always overwrite" so a full (re)bake never raises the editor's
    overwrite-confirmation dialog. Only call force=True when the level is about to be rebuilt
    (build()), so no live level actor is left referencing a deleted instance.

    v1.0.86 self-heal: the non-force path detects a pre-v1.0.86 master (missing the
    `TilingMeters` parameter) and force-regenerates the master + instances so the next
    editor launch picks up the new world-driven UV pipeline automatically. Anyone who
    customised the master in the editor will lose those edits on first v1.0.86 startup --
    they should re-apply them after the regen (acceptable for an additive parameter
    upgrade; the regen logs a Warning so the change isn't silent).

    v1.0.97 self-heal: the same path ALSO detects a pre-v1.0.97 master (top-level
    `two_sided` reads False) and promotes the call to a force-regen so the new double-
    sided master ships on the next launch. The two probes combine via OR so EITHER
    upgrade triggers a single regen (no double-bake on a project that pre-dates v1.0.86
    and v1.0.97 simultaneously).
    """
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    mel = unreal.MaterialEditingLibrary

    # Non-force self-heal: if the existing master is the pre-v1.0.86 version (no TilingMeters
    # parameter) OR the pre-v1.0.97 version (single-sided), promote the call to a force-regen
    # so the new master ships on the next launch without the user manually running build().
    if not force and unreal.EditorAssetLibrary.does_asset_exist(GROUND_MASTER_PATH):
        existing = unreal.EditorAssetLibrary.load_asset(GROUND_MASTER_PATH)
        if existing is not None and (not _master_has_tiling_meters(existing)
                                     or not _master_is_two_sided(existing)):
            unreal.log_warning("RebusBaseLevel: pre-v1.0.97 ground master detected "
                               "(missing TilingMeters or single-sided); "
                               "regenerating master + instances.")
            force = True

    if force:
        # Delete instances before the master so removing the master can't dangle their parent.
        for preset in GROUND_PRESETS:
            path = _instance_path(preset)
            if unreal.EditorAssetLibrary.does_asset_exist(path):
                unreal.EditorAssetLibrary.delete_asset(path)
        if unreal.EditorAssetLibrary.does_asset_exist(GROUND_MASTER_PATH):
            unreal.EditorAssetLibrary.delete_asset(GROUND_MASTER_PATH)

    if not unreal.EditorAssetLibrary.does_asset_exist(GROUND_MASTER_PATH):
        master = tools.create_asset("M_RebusGround", MATERIALS_DIR, unreal.Material, unreal.MaterialFactoryNew())
        _build_ground_master(master)
        unreal.EditorAssetLibrary.save_loaded_asset(master)

    master = unreal.EditorAssetLibrary.load_asset(GROUND_MASTER_PATH)
    if not master:
        unreal.log_error("RebusBaseLevel: failed to load ground master material; skipping instances.")
        return

    for preset, (color_a, color_b, rough) in GROUND_PRESETS.items():
        path = _instance_path(preset)
        if unreal.EditorAssetLibrary.does_asset_exist(path):
            continue
        mic = tools.create_asset("MI_RebusGround_{}".format(preset), MATERIALS_DIR,
                                 unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
        mel.set_material_instance_parent(mic, master)
        mel.set_material_instance_vector_parameter_value(mic, "ColorA", color_a)
        mel.set_material_instance_vector_parameter_value(mic, "ColorB", color_b)
        mel.set_material_instance_scalar_parameter_value(mic, "Roughness", rough)
        # v1.0.86 TilingMeters defaults to 1.0 on the master; preset overrides could be
        # added here later if e.g. Sand should naturally tile coarser than Concrete. For now
        # all four shipped presets inherit the 1 m default so the C++ runtime push is
        # authoritative.
        unreal.EditorAssetLibrary.save_loaded_asset(mic)

    unreal.log("RebusBaseLevel: ground materials ensured ({} presets, v1.0.86 world tiling).".format(len(GROUND_PRESETS)))


def _resolve_floor_surface_material(preset):
    """Return (UMaterialInterface, asset_path) for the requested v1.0.129 surface.

    v1.0.129 PRIMARY PATH -- look up `preset` in FLOOR_SURFACE_PATHS and
    EditorAssetLibrary.does_asset_exist+load_asset the operator-authored
    /Game/REBUS/Materials/<Preset> .uasset. Returns it on hit; the four .uasset
    files the user shipped (Concrete / Grass / Sand / Tarmac) ARE the
    authoritative floor surfaces.

    v1.0.129 FALLBACK PATH -- if the user-authored asset is missing (partial-
    checkout clone, brand-new operator workstation, asset deleted by accident),
    fall back to the legacy procedural MI_RebusGround_<Preset> instance that
    v1.0.86 generates from the M_RebusGround master. This keeps the build
    idempotent and non-catastrophic on partial-state machines -- the visible
    surface still tracks the requested preset, it's just the v1.0.85 procedural
    flavour instead of the v1.0.129 user-authored one. Logs a Warning so the
    operator sees they need to re-pull the .uasset files.

    Unknown preset -> default to FLOOR_DEFAULT_SURFACE and recurse (single
    level: the default is always in the table). This guards against operator
    typos in `Rebus.FloorSurface <typo>` without aborting the call.
    """
    if preset not in FLOOR_SURFACE_PATHS:
        unreal.log_warning(
            "RebusBaseLevel v1.0.129: unknown floor surface '{}' (valid: {}); "
            "falling back to default '{}'.".format(
                preset, sorted(FLOOR_SURFACE_PATHS.keys()), FLOOR_DEFAULT_SURFACE))
        preset = FLOOR_DEFAULT_SURFACE

    primary = FLOOR_SURFACE_PATHS[preset]
    if unreal.EditorAssetLibrary.does_asset_exist(primary):
        try:
            mat = unreal.EditorAssetLibrary.load_asset(primary)
        except Exception as exc:  # noqa: BLE001
            mat = None
            unreal.log_warning(
                "RebusBaseLevel v1.0.129: load_asset('{}') raised ({}); "
                "falling back to legacy MI_RebusGround_{}.".format(
                    primary, exc, preset))
        if mat is not None:
            return mat, primary

    legacy = _instance_path(preset)
    unreal.log_warning(
        "RebusBaseLevel v1.0.129: user-authored floor material '{}' not found "
        "(re-pull missing /Game/REBUS/Materials/{}.uasset to enable the new "
        "surface); falling back to legacy procedural '{}'.".format(
            primary, preset, legacy))
    try:
        mat = unreal.EditorAssetLibrary.load_asset(legacy)
    except Exception as exc:  # noqa: BLE001
        mat = None
        unreal.log_warning(
            "RebusBaseLevel v1.0.129: legacy fallback load_asset('{}') also "
            "raised ({}); floor will use the engine default material.".format(
                legacy, exc))
    return mat, legacy if mat is not None else None


def _add_floor():
    plane = unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/Plane.Plane")
    floor = _spawn(unreal.StaticMeshActor, unreal.Vector(0.0, 0.0, 0.0), label="RebusFloor")
    if not floor:
        return
    # Tag so URebusSceneSettingsSubsystem can find it for GroundSurface /
    # bGroundVisible / v1.0.129 FloorSurface.
    floor.set_editor_property("tags", ["RebusFloor"])
    # ~2 km plane reads as an effectively infinite ground at stage scale.
    floor.set_actor_scale3d(unreal.Vector(2000.0, 2000.0, 1.0))

    comp = _component(floor, unreal.StaticMeshComponent)
    if comp and plane:
        comp.set_static_mesh(plane)
        # v1.0.129 -- floor material is now the operator-authored
        # /Game/REBUS/Materials/<FLOOR_DEFAULT_SURFACE> .uasset, with safe
        # fallback to the legacy MI_RebusGround_<preset> if the new asset is
        # missing (handled inside _resolve_floor_surface_material).
        mat, resolved_path = _resolve_floor_surface_material(FLOOR_DEFAULT_SURFACE)
        if mat is not None:
            comp.set_material(0, mat)
            unreal.log(
                "RebusBaseLevel v1.0.129: floor surface set to '{}' (asset: {}).".format(
                    FLOOR_DEFAULT_SURFACE, resolved_path))
    unreal.log("RebusBaseLevel: infinite floor plane added (default surface: {}).".format(FLOOR_DEFAULT_SURFACE))


def _has_floor(actor):
    try:
        return actor.actor_has_tag("RebusFloor")
    except Exception:  # noqa: BLE001
        return any(str(t) == "RebusFloor" for t in actor.get_editor_property("tags"))


def ensure_floor_in_level():
    """Add the floor to the currently-open level if it's missing, then save.

    Self-heals an existing BaseLevel that was baked before the floor feature
    existed (the floor is part of the level, not a separate asset, so the
    map-missing check in ensure_base_level won't add it on its own). Idempotent.
    """
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for actor in eas.get_all_level_actors():
        if isinstance(actor, unreal.StaticMeshActor) and _has_floor(actor):
            return False

    _add_floor()
    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()
    unreal.log("RebusBaseLevel: floor added to existing level and saved.")
    return True


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
    # Volumetric fog ON so per-fixture beams scatter (§8.4). Tuned for haze/beam visibility:
    # a far volumetric range with subtle extinction + mild forward scatter (matches the C++
    # EnsureSceneEnvironment fallback so a fresh spawn renders the same).
    _set(comp, "volumetric_fog", True)
    _set(comp, "volumetric_fog_distance", 35000.0)             # cm; far enough for stage beams
    _set(comp, "volumetric_fog_extinction_scale", 0.3)         # subtle haze, not a wall of fog
    _set(comp, "volumetric_fog_scattering_distribution", 0.4)  # mild forward scatter
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
    for step in (_add_exponential_height_fog, _add_post_process_volume, _add_lighting, _add_floor):
        try:
            step()
        except Exception as exc:  # noqa: BLE001
            unreal.log_error("RebusBaseLevel: {} failed ({})".format(step.__name__, exc))


def _base_level_world_is_valid():
    """True iff /Game/REBUS/Maps/BaseLevel resolves to a UWorld with the matching name.

    The pre-v1.0.129 idempotency gate in ensure_base_level() used
    `unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PACKAGE_PATH)` -- which
    returns True for ANY package file on disk at that path, regardless of what's
    inside. That is too lax: a stub package containing no UWorld (a partial save
    aborted before the world export was written; a `new_level()` followed by an
    interrupted `save_current_level()`; a redirector left behind by a rename;
    a wrong-class asset accidentally created at that path) satisfies
    `does_asset_exist` but causes UE 5.7's startup-map loader to emit
    `LogUObjectGlobals: Warning: Failed to find world in already loaded world
    package /Game/REBUS/Maps/BaseLevel!` and fall back to an empty map. The
    Python builder would then short-circuit and never repair the stub.

    v1.0.129 promotes the gate from "package exists" to "package resolves to a
    UWorld with the expected leaf name". Anything else -- missing package,
    package present but contains no UWorld (the v1.0.129 user-reported root
    cause), redirector / wrong class, PackageName-vs-WorldName divergence
    (umap renamed without rewriting the inner UWorld's name) -- returns False
    so the caller deletes the stub and re-authors from scratch via build().

    Returns False on any internal Python exception so the caller errs on the
    side of repair; the subsequent build() is itself idempotent and safe to
    re-run, even when no stub is actually present.
    """
    if not unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PACKAGE_PATH):
        return False
    try:
        loaded = unreal.EditorAssetLibrary.load_asset(LEVEL_PACKAGE_PATH)
    except Exception as exc:  # noqa: BLE001
        unreal.log_warning(
            "RebusBaseLevel: load_asset('{}') raised ({}); treating as broken "
            "and rebuilding.".format(LEVEL_PACKAGE_PATH, exc))
        return False
    # The package may be a UObjectRedirector (umap was renamed/moved and only
    # the redirector remains), a UDataAsset created by mistake at this path, or
    # None when the package is loadable but contains no top-level asset (the
    # v1.0.129 stub case -- a 644-byte truncated save with a package header but
    # no UWorld export blob, which UE 5.7's load_asset surfaces as None).
    if loaded is None:
        return False
    try:
        is_world = isinstance(loaded, unreal.World)
    except Exception:  # noqa: BLE001
        is_world = False
    if not is_world:
        return False
    # PackageName-vs-WorldName divergence: the umap was renamed via Content
    # Browser but the inner UWorld's UObject name stayed at the old leaf. UE's
    # FindWorldInPackage matches by name, so divergence emits the same "Failed
    # to find world" warning even though there IS a UWorld inside. Re-author
    # to converge the names.
    try:
        leaf = LEVEL_PACKAGE_PATH.rsplit("/", 1)[-1]
        return loaded.get_name() == leaf
    except Exception:  # noqa: BLE001
        return False


def build():
    """Force-(re)create the base level from scratch and save it.

    Replaces whatever level is currently open, so call this only for an explicit
    (re)bake (Tools > Execute Python Script / headless -run=pythonscript).

    v1.0.119 NOTE -- this function (and `ensure_base_level()` below) PREVIOUSLY
    mixed TAB and SPACE indentation in v1.0.117 / v1.0.118, which causes Python 3
    to raise `TabError: inconsistent use of tabs and spaces in indentation` at
    PARSE time. The fallout was severe: `init_unreal.py`'s top-level
    `import build_rebus_base_level as base` line caught the exception and set
    `base = None`, so the editor-startup level-author path silently no-opped;
    and the C++ auto-purge's `py import build_rebus_base_level;
    build_rebus_base_level.ensure_beam_material(force=True)` Exec failed at the
    import line, so a stale on-disk `M_RebusBeam.uasset` was never regenerated.
    v1.0.119 converts every line in BOTH functions to 4-space indentation
    (matching every other function in this module) so the import succeeds. See
    the v1.0.119 README release block for the full diagnosis + fix.

    v1.0.129 NOTE -- if a stub / wrong-class / redirector asset is already
    sitting at LEVEL_PACKAGE_PATH (the v1.0.129 root cause: the package loads
    but contains no UWorld), `les.new_level()` below will replace the open
    world with a fresh empty UWorld and `save_current_level()` will write a
    valid umap on top of the stub. No explicit delete is needed because the
    save-current-level path overwrites the package file in place. As a final
    cleanup, `fix_up_referencers_for_redirectors_in_folder` resolves any
    redirector pairs left over in `/Game/REBUS/Maps/` so subsequent loads
    never touch a stale redirector.
    """
    # A full bake always overwrites the generated materials (no confirmation dialog);
    # safe because new_level below replaces the open level, so nothing references the
    # instances we just deleted/recreated.
    ensure_ground_materials(force=True)
    ensure_lens_material(force=True)
    ensure_beam_material(force=True)
    # v1.0.93 mirror lens (drives every Epic-beam lens object).
    ensure_fixture_lens_material(force=True)
    # v1.0.104 two-sided opaque master operators can re-parent Orbit-imported materials
    # to so thin geometry renders both sides (see ORBIT_IMPORTED_PATH doc + README v1.0.104).
    ensure_orbit_imported_material(force=True)
    # v1.0.121 -- pre-bake every IES profile under /Game/REBUS/IES/ so the -game
    # runtime doesn't depend on the editor-only IESConverter.h at fixture spawn.
    ensure_ies_profiles(force=True)
    # v1.0.95 migration -- see the helper docstring + README v1.0.95.
    _cleanup_internal_beam_assets()

    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)

    # Create a fresh, empty level (replaces whatever is open) and make it current.
    # Overwrites any existing package at this path including v1.0.129 stubs that
    # contain no UWorld -- new_level + save_current_level always produce a valid
    # umap with a UWorld of the matching leaf name.
    les.new_level(LEVEL_PACKAGE_PATH)

    _populate()

    les.save_current_level()
    unreal.log("RebusBaseLevel: saved {}".format(LEVEL_PACKAGE_PATH))

    # v1.0.129 -- resolve any UObjectRedirector pairs that may have been left in
    # the Maps folder by an earlier rename / move. Without this, a redirector
    # at /Game/REBUS/Maps/BaseLevel that ALSO satisfies does_asset_exist would
    # keep re-triggering the "world not found" warning even though the actual
    # UWorld now lives at the resolved target path. Best-effort: a missing
    # AssetTools API on a future engine drop is treated as a soft warning, not
    # an abort -- the just-written valid umap is the primary fix anyway.
    try:
        asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
        asset_tools.fix_up_referencers_for_redirectors_in_folder(["/Game/REBUS/Maps"])
        unreal.log(
            "RebusBaseLevel v1.0.129: fix_up_referencers_for_redirectors_in_folder"
            "('/Game/REBUS/Maps') ran -- any stale Map redirectors have been resolved.")
    except Exception as exc:  # noqa: BLE001
        unreal.log_warning(
            "RebusBaseLevel v1.0.129: fix_up_referencers_for_redirectors_in_folder "
            "raised ({}); the just-written umap is still valid -- this only matters "
            "if a prior rename left a redirector in /Game/REBUS/Maps/.".format(exc))


def ensure_base_level():
    """Generate the base level only if it is missing OR contains no valid UWorld.

    Returns True if it had to build/rebuild the level, False if the existing
    asset already resolved to a UWorld with the matching leaf name. This is the
    idempotent entry point the startup hook (init_unreal.py) uses so opening
    the project always lands on a populated stage without clobbering an
    existing BaseLevel on every launch.

    v1.0.119 NOTE -- see `build()`'s docstring; same tabs/spaces fix lives here.

    v1.0.129 ROOT-CAUSE FIX -- the pre-v1.0.129 gate was
    `unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PACKAGE_PATH)`, which is
    a pure "file at this path?" check. A package file CAN exist on disk while
    containing no UWorld -- a stub left by an interrupted save, a redirector
    after a rename, a wrong-class asset created by mistake. UE 5.7's startup-
    map loader then emits the operator-confusing warning
    `LogUObjectGlobals: Warning: Failed to find world in already loaded world
    package /Game/REBUS/Maps/BaseLevel!` and falls back to an empty map; the
    pre-v1.0.129 builder short-circuited and never repaired it.

    v1.0.129 promotes the gate to `_base_level_world_is_valid()`, which loads
    the asset and verifies it resolves to a `unreal.World` with the matching
    leaf name. On miss, it logs an Error line (so the operator sees the repair
    happening) and calls `build()` which `new_level` + `save_current_level`'s
    a valid umap on top of whatever was there -- making the builder idempotent
    regardless of starting state. See the v1.0.129 README release block for the
    four corruption-shape scenarios this covers.
    """
    # Ground + lens materials self-heal independently of the map (cheap if already present).
    ensure_ground_materials()
    ensure_lens_material()
    ensure_beam_material()
    # v1.0.93 mirror lens self-heal (drives every Epic-beam lens object).
    ensure_fixture_lens_material()
    # v1.0.104 Orbit-imported two-sided opaque master self-heal.
    ensure_orbit_imported_material()
    # v1.0.121 IES profile self-heal (idempotent on missing/stale; no-op on current).
    ensure_ies_profiles()
    # v1.0.95 migration -- see the helper docstring + README v1.0.95.
    _cleanup_internal_beam_assets()

    if _base_level_world_is_valid():
        return False

    # The gate above already distinguishes "missing" from "present-but-broken"
    # via `does_asset_exist`; reuse the same probe here so the log line makes
    # the operator's diagnosis explicit.
    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PACKAGE_PATH):
        unreal.log_error(
            "RebusBaseLevel v1.0.129: '{}' exists on disk but does NOT contain a "
            "valid UWorld with the matching leaf name (stub package / redirector "
            "/ wrong asset class / PackageName-vs-WorldName divergence). This is "
            "the root cause of the UE 5.7 'Failed to find the world in already "
            "loaded world package' engine warning. Rebuilding the level in place "
            "via new_level + save_current_level so the package always converges "
            "to a valid umap; see README v1.0.129 + Troubleshooting block.".format(
                LEVEL_PACKAGE_PATH))
    else:
        unreal.log(
            "RebusBaseLevel: '{}' missing; generating it.".format(LEVEL_PACKAGE_PATH))
    build()
    return True


if __name__ == "__main__":
    build()
