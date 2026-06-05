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
    (`_fixture_lens_master_is_current`) / v1.0.96 (`_beam_master_has_shadow_steps`) /
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
// v1.0.96 SCREEN-SPACE SHADOW TRACE -- the visible shaft now self-shadows so an occluder between
// the fixture and the floor visibly CUTS the beam. The trade-off (vs Mesh-Distance-Field tracing,
// which the runtime glTF-imported truss meshes have no SDFs for): the algorithm is SCREEN-SPACE,
// so an off-screen occluder never casts shadow into the shaft (its depth isn't in the depth
// buffer). MDF fallback is documented as future work (see README v1.0.96).
//
// v1.0.99 PROJECTION + SELF-OCCLUSION FIX (the v1.0.96 shadow trace did not visibly carve the
// beam at any occluder -- the user reported "the light beam currently goes straight through
// any object"). Two coupled bugs were the smoking gun:
//
//   (1) LWC PROJECTION. v1.0.96 computed `spRel = sp - ro` and fed that to
//       `ResolvedView.TranslatedWorldToClip`. UE 5.7's clip-space matrix expects
//       `TranslatedWorld = AbsoluteWorld + ResolvedView.PreViewTranslation`, NOT
//       `AbsoluteWorld - CamPos`: those are only equal when the view origin is exactly the
//       camera origin (and PreViewTranslation has no jitter / shadow-pass offset / LWC tile
//       term). On a stage scene the projected NDC was off by the view-origin delta, lands
//       outside [-1, 1], and the `if (any(abs(ndc) > 1.0)) { continue; }` skip then dropped
//       EVERY shadow step -- so no step ever taps SceneDepth and the trace looks "always
//       unoccluded" (the user-visible "goes straight through any object" symptom). v1.0.99
//       uses the LWC-safe formula `TranslatedWorld = sp + LWCHackToFloat(ResolvedView.
//       PreViewTranslation)`, which is the documented engine pattern for absolute-world ->
//       clip in a Custom HLSL node.
//
//   (2) FIRST-STEP SELF-OCCLUSION. The translucent cone-mesh ITSELF doesn't write to
//       SceneDepth (it's BLEND_ADDITIVE), but the shaft sample `wp` lives ON the cone's
//       projected screen pixel where the SOLID fixture body / nearby props can read AHEAD of
//       a small shadow step. v1.0.99 promotes `BeamShadowBias` (default raised 0.5 -> 5.0 cm)
//       to act as a FIRST-STEP MINIMUM DISTANCE: any candidate step with `sj < BeamShadowBias`
//       is skipped, so the trace starts at least 5 cm out from `wp` toward the light before
//       its first SceneDepth tap. If the user still sees the beam pass through occluders
//       after this, raising `BeamShadowBias` to 50 cm is the operator-side knob (set via the
//       MID directly or via a future `Rebus.BeamShadowBias` CVar).
//
// Algorithm (per shaft sample inside the existing march loop, after the density `d` is computed
// but before composing it into the transmittance):
//   1. toLight = (bo - wp); dToLight = |toLight|; sdir = toLight / dToLight.
//   2. March BeamShadowSteps steps from `wp` toward `bo` (the SpotLight position), at step
//      distance sdt = dToLight / BeamShadowSteps. The shadow march MUST NOT exceed dToLight --
//      we clamp `sj < dToLight` so a far occluder behind the light can't falsely shadow us.
//   3. At each step, world-position -> NDC -> ScreenUV via ResolvedView.TranslatedWorldToClip,
//      using `TranslatedWorld = sp + LWCHackToFloat(ResolvedView.PreViewTranslation)` (the
//      v1.0.99 LWC-safe absolute-world -> translated-world formula -- see the v1.0.99 fix
//      block above for the v1.0.96 bug it replaces).
//   4. Tap CalcSceneDepth(uv) at the projected UV. If sd + perStepBias < step.cameraZ (= clip.w
//      in UE perspective projection), the step is OCCLUDED from the camera's view -> something
//      opaque is between the camera and the step -> the step is shadowed from the light too
//      (under the screen-space assumption: the occluder is on the screen and obstructs the
//      light-ray we're tracing). v1.0.99: `BeamShadowBias` is the FIRST-STEP MINIMUM cm
//      (default 5.0) -- any candidate step within `BeamShadowBias` cm of the shaft sample is
//      skipped to avoid self-occlusion against nearby fixture / scene geometry. A tiny
//      `0.01 * sdt` per-step camera-Z tolerance is still added to the depth comparison so
//      banding doesn't appear when sd and clip.w are within float precision.
//   5. If ANY step hits an occluder we mark the sample as shadowed and attenuate the density
//      contribution by (1 - BeamShadowStrength). Strength=1 -> the shadowed sample contributes
//      nothing (full shadow). Strength=0 -> the trace runs but does nothing (master OFF).
//
// v1.0.109 PAN-EDGE / SKY / FAR-DISTANCE GUARDS (the bug the v1.0.106 default-flip finally
// made visible). Once v1.0.106 made the procedural `M_RebusBeam` cone the visible shaft,
// the user reported "the beam vs object shadowing is cutting the side of the beams when we
// pan left or right and is doing the same for all fixtures". This is the canonical
// SCREEN-SPACE-SHADOW-TRACE failure mode: the shadow march steps project to screen UVs
// where the tap is unreliable (off-screen, on a "no opaque geometry written" pixel that
// reads the sky / clear-value sentinel, or at long distance where depth precision crashes
// to sub-cm) and the `sd + bias < clipP.w` comparison fires spuriously -- the trace
// carves a black silhouette where there's actually nothing occluding the shaft.
//
// v1.0.99 already had ONE of the guards (`if (any(abs(ndc) > 1.0)) { continue; }`) which
// catches the truly off-screen case. v1.0.109 adds the THREE coupled guards the user's
// pan-edge symptom requires:
//
//   GUARD A -- SKY / NO-GEOMETRY. `CalcSceneDepth(uv)` at a sky pixel returns the engine
//   FAR-PLANE sentinel (UE 5.7: `WORLD_MAX`-class large value, ~1e10 cm). The v1.0.99
//   `sd + stepBias < clipP.w` test is well-behaved at extreme `sd`, BUT the depth buffer
//   has well-known edge-of-frustum artefacts where the rasterised sky pixel is touched
//   by a translucent-pass UV sample yet reads a SMALL near-zero value (HZB partial fill /
//   fast-clear half-evaluated tiles / TAA jitter pushing the sample one texel past the
//   real opaque silhouette into the unwritten sky region). The explicit guard
//   `if (sd >= SkyDepthSentinel) { continue; }` AND a complementary `if (sd <= 0.001) {
//   continue; }` catches both poles -- a sample that lands on "no opaque was rendered
//   here" is unambiguously UNOCCLUDED regardless of what numeric value the depth buffer
//   actually carries at that texel.
//
//   GUARD B -- FAR-DISTANCE CULL. At step camera-Z above `BeamShadowFarCullCm` (default
//   50000 cm = 500 m), the reverse-Z depth buffer has lost most of its precision -- a
//   5 cm bias is sub-LSB -- so the comparison gives essentially random answers. Beyond
//   the cull distance we `break` out of the per-pixel shadow loop entirely (the marker
//   accumulator already holds the unoccluded contribution). The default puts the cull
//   well past any realistic stage rig (the longest GDTF throws we've shipped fit inside
//   200 m); operators can raise it for arena-class throws via the new
//   `Rebus.BeamShadowFarCullCm` CVar.
//
//   GUARD C -- DISTANCE-SCALED BIAS. The v1.0.99 first-step bias `BeamShadowBias = 5.0
//   cm` was tuned at fixture-to-floor distances ~3-15 m. At 30 m the reverse-Z buffer
//   has ~3-4 cm of precision per LSB; the constant 5 cm bias survives. At 200 m the
//   precision is ~30-50 cm per LSB; 5 cm is sub-LSB and the test fires on quantisation
//   noise. v1.0.109 replaces the constant per-step bias with `stepBias = (0.01 * sdt) +
//   max(sd, 0) * BeamShadowBiasScale` (default scale 0.002 = 0.2 percent of sample
//   depth in cm, ~6 cm at 30 m, ~40 cm at 200 m) so the bias grows linearly with depth.
//   Reduces false-occlusion at the long end of the beam throw without compromising
//   the close-range trace -- at 1 m the additional term contributes 0.2 cm on top of
//   the v1.0.99 absolute floor.
//
//   GUARD D -- EXPLICIT EDGE-GUARD TOGGLE. The off-screen check is now gated on
//   `BeamShadowEdgeGuard > 0.5` (default 1.0 / ON). Flipping `Rebus.BeamShadowEdgeGuard
//   0` restores the v1.0.99 broken behaviour for A/B verification -- the operator can
//   confirm the pan-edge clipping returns, prove the diagnosis is right, then flip back.
//
// The COMPLETE fundamental fix for the screen-space-shadow failure mode is to feed the
// trace a wider-FOV neighbour depth buffer (UE 5.7 has no built-in for this) OR move the
// occlusion test to ray-traced shadows on the volumetric beam (RT-fog / Lumen volumetric
// shadows). That's a v1.0.150+ class of lift; the v1.0.109 guards are the standard
// mitigation that the OUR / Lumen / Frostbite / id-tech screen-space-shadow papers all
// document as the rate-limiting bug-class for translucent volumetric trace work.
//
// v1.0.109 DEBUG VIEW (`BeamShadowDebug` scalar param, driven by `Rebus.BeamShadowDebug
// [0|1|2]`):
//   * 0 = off (default; ship the regular composed beam).
//   * 1 = render the shadow factor as a heatmap. The beam coverage stays the same so the cone
//         shape is visible, but the colour is `lerp(green, red, shadowedFraction)` x 4.0
//         (bright). A cube placed between fixture + floor should appear as red against a
//         green beam -- if the cube is green, the trace is finding NO occluders (Cause 1
//         regression) or BeamShadowStrength is 0 (`Rebus.BeamShadow 0` master toggle).
//   * 2 = (v1.0.109 REPURPOSED) per-pixel COLOUR-BY-REASON map of which guard fired in
//         this pixel's shadow march. Replaces the v1.0.99 "first shadow step's projected
//         UV" view (the LWC projection bug has been three releases dead -- mode 2's UV
//         sanity job is done). Discrete priority (highest wins):
//           * RED    = at least one depth-occluded step (true scene-depth shadow).
//           * GREEN  = off-screen guard fired (Guard D -- v1.0.99 had this but never
//                     tagged it; v1.0.109 colours it so the pan-edge regions become
//                     visibly identifiable).
//           * BLUE   = sky / no-geometry guard fired (Guard A -- NEW v1.0.109).
//           * YELLOW = far-distance cull fired (Guard B -- NEW v1.0.109).
//           * WHITE  = nothing fired; trace ran clean to unoccluded.
//         This lets the operator visually confirm WHICH guard rescued each pixel: pan
//         a fixture left-right while `Rebus.BeamShadowDebug 2` and the screen-edge
//         regions should now show GREEN (off-screen guard saving the day -- previously
//         the carved black silhouette of the user's bug report); the sky behind the
//         beam should show BLUE; the legitimate object-shadowing inside-frame should
//         still show RED. Any pan-edge region showing RED with Edge Guard ON means the
//         off-screen guard isn't catching it (v1.0.110+ follow-up).
//
// Limitations (documented in README v1.0.96/99/109):
//   * Screen-space only -- off-screen occluders don't cast. Fundamental architectural
//     limit; the v1.0.109 guards SOFTEN the visible failure mode but don't change the
//     underlying contract.
//   * Cost ~ StepCount * BeamShadowSteps SceneDepth taps per pixel. Keep BeamShadowSteps low
//     (default 8, clamped to 16) -- 32 march * 8 shadow = 256 taps/pixel is the design point.
//   * No MDF fallback (Orbit-imported meshes lack SDFs at runtime).
//   * The shadow trace is wrapped in `[loop]` (HLSL DX-SM5+) so the compiler doesn't unroll.
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

// v1.0.99 LWC translation helper. `ResolvedView.PreViewTranslation` is a `WSVector` in LWC
// builds (UE 5.4+); `LWCHackToFloat` is the engine-blessed accessor inside Custom HLSL that
// converts an LWC vector to a float3 representing translated-world. Cached once per pixel
// because every shadow step needs it.
float3 PreViewT = LWCHackToFloat(ResolvedView.PreViewTranslation);

// v1.0.99 debug-view accumulators. Track whether we found ANY occluder during the per-pixel
// march, what fraction of samples were shadowed, and the FIRST shadow step's projected UV
// so the operator can verify the LWC projection math + that the trace is actually finding
// occluders. Cheap (a few scalar ops at the end of the per-pixel work); only consulted
// when `BeamShadowDebug > 0.5` so the regular path stays unchanged.
//
// v1.0.109 extends the accumulator set with one tally per GUARD REASON (off-screen / sky /
// far-cull). The mode-2 debug view consults these to colour the per-pixel result by which
// guard rescued the trace from a false occlusion -- so the pan-edge regions the user
// reported as "cutting the side of the beams" can be visually attributed to Guard D
// (off-screen). All three are ints incremented by ONE per shadow-step that took the
// matching branch; the priority ordering in mode 2 reads "depth-occluded RED beats
// off-screen GREEN beats sky BLUE beats far-cull YELLOW beats clean WHITE", so the
// dominant failure mode bubbles to the visible colour even when multiple guards fired in
// the same per-pixel march.
int   shadowedSampleCount = 0;
int   totalSampleCount = 0;
int   offScreenSampleCount = 0;   // v1.0.109 -- Guard D off-screen NDC tap
int   skySampleCount = 0;         // v1.0.109 -- Guard A sky / no-geometry depth read
int   farCullSampleCount = 0;     // v1.0.109 -- Guard B distance > BeamShadowFarCullCm
float2 firstShadowUV = float2(-1.0, -1.0); // sentinel "no shadow step ever taken"

float trans = 1.0;
float t = tEntry + dt * 0.5;
const int MAXSTEPS = 64;
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
            // v1.0.96 screen-space shadow trace: march toward the SpotLight at `bo` and tap
            // SceneDepth at each step's projected screen UV. The sample is shadowed when any
            // step finds an opaque feature in front of it. See the file-header doc-comment for
            // the full algorithm. The trace is gated on BeamShadowStrength > 0 so the master
            // toggle (Rebus.BeamShadow 0) takes the branch out of the per-pixel cost.
            // v1.0.99: also gated on BeamShadowDebug > 0 so the heatmap visualisation runs
            // even when strength is near zero (operator can confirm the trace finds
            // occluders independently of the strength dial).
            float shadowAtten = 1.0;
            bool  sOccluded   = false;
            ++totalSampleCount;
            [branch]
            if (BeamShadowStrength > 0.001 || BeamShadowDebug > 0.5)
            {
                float3 toLight = bo - wp;
                float dToLight = length(toLight);
                if (dToLight > 0.001)
                {
                    float3 sdir = toLight / dToLight;
                    int sSteps = (int)clamp(BeamShadowSteps, 1.0, 16.0);
                    float sdt = dToLight / (float)sSteps;
                    // v1.0.99: BeamShadowBias is the FIRST-STEP MINIMUM cm (default 5.0).
                    // Any candidate step within `biasBase` cm of the shaft sample is
                    // skipped, so the very first SceneDepth tap is at least biasBase cm
                    // out from `wp` toward the light. This avoids the trace reading a
                    // nearby opaque pixel (fixture body / prop edge near the cone) as
                    // its own occluder. See the file-header v1.0.99 fix doc-comment.
                    float biasBase = max(BeamShadowBias, 0.0);
                    // v1.0.109 -- guard knobs cached once per shaft sample so the inner
                    // loop runs them as scalar compares (the shader compiler hoists
                    // these). FarCullCm with the `BeamShadowEdgeGuard > 0.5` master
                    // toggle is intentionally evaluated INSIDE the loop on `clipP.w`
                    // (camera-Z of THIS step), not on `dToLight`, because a long shadow
                    // ray can start in-frame and walk into the far-precision-trash
                    // region; the guard kicks in step-by-step as the trace progresses.
                    // SkySentinel uses the engine's documented `WORLD_MAX` floor
                    // (1e9 cm) clamped to a per-shader literal so the comparison is
                    // shader-portable -- the CalcSceneDepth at sky pixels in UE 5.7
                    // returns a value FAR larger than any plausible occluder camera-Z
                    // (>1e5 cm). 65000 cm = 650 m is the v1.0.109-spec sentinel; the
                    // CVar can NOT make this any larger than the engine's actual sky
                    // sentinel without losing the guard, so the value is hard-coded
                    // here rather than wired as another scalar parameter (the
                    // BeamShadowFarCullCm guard already provides the operator-tunable
                    // long-distance escape hatch).
                    float farCullCm = max(BeamShadowFarCullCm, 100.0);
                    bool  bEdgeOn   = (BeamShadowEdgeGuard > 0.5);
                    float biasScale = max(BeamShadowBiasScale, 0.0);
                    const float SkyDepthSentinel = 65000.0; // cm; "no opaque written" pixels
                    [loop]
                    for (int j = 0; j < 16; ++j)
                    {
                        if (j >= sSteps) { break; }
                        // Linear step distance from wp toward bo. The first viable step
                        // is the smallest j with `sj >= biasBase` (skip everything
                        // closer to wp than the first-step minimum).
                        float sj = sdt * ((float)j + 1.0);
                        if (sj < biasBase) { continue; }
                        // Clamp the march distance so a far-away occluder BEHIND the light
                        // can't falsely shadow the shaft (per the v1.0.96 spec).
                        if (sj >= dToLight) { break; }
                        float3 sp = wp + sdir * sj;
                        // v1.0.99: LWC-safe absolute-world -> translated-world. The
                        // v1.0.96 `sp - ro` shortcut was wrong (TranslatedWorldToClip
                        // expects `AbsoluteWorld + PreViewTranslation`, not
                        // `AbsoluteWorld - CamPos`) and projected every step OUTSIDE
                        // the [-1,1] NDC range -- the off-screen guard then dropped
                        // every tap so the trace concluded "always unoccluded" and the
                        // beam visibly carved nothing. See the file-header v1.0.99
                        // fix doc-comment for the full diagnosis.
                        float3 TranslatedWorldPos = sp + PreViewT;
                        float4 clipP = mul(float4(TranslatedWorldPos, 1.0), ResolvedView.TranslatedWorldToClip);
                        if (clipP.w <= 0.001) { continue; }
                        // v1.0.109 Guard B -- far-distance cull. Above
                        // `BeamShadowFarCullCm` (default 50000 cm = 500 m) the
                        // reverse-Z depth buffer has lost most of its precision and
                        // the `sd + stepBias < clipP.w` comparison becomes noise-
                        // driven. Break out of the per-step loop (rather than
                        // `continue`-ing into more equally-untrustworthy steps); the
                        // accumulator already holds the contribution of the closer
                        // steps that DID succeed.
                        if (clipP.w > farCullCm)
                        {
                            ++farCullSampleCount;
                            break;
                        }
                        float2 ndc = clipP.xy / clipP.w;
                        // v1.0.109 Guard D -- off-screen toggle. v1.0.99 had this guard
                        // unconditionally; v1.0.109 puts a master switch behind it so
                        // `Rebus.BeamShadowEdgeGuard 0` restores the broken behaviour
                        // for diagnostic A/B (the operator can verify the pan-edge
                        // clipping returns, prove the diagnosis, then flip back).
                        // Tag the sample for the mode-2 colour-by-reason view so the
                        // operator can see WHICH steps the guard saved.
                        if (any(abs(ndc) > 1.0))
                        {
                            ++offScreenSampleCount;
                            if (bEdgeOn) { continue; }
                        }
                        // NDC -> UV (UE Y is flipped relative to NDC).
                        float2 uv = ndc * float2(0.5, -0.5) + 0.5;
                        // v1.0.99 debug capture: remember the very first projected UV
                        // we actually evaluate (any sample, any step). The mode-2
                        // visualisation paints this on the beam so the operator can
                        // see at a glance whether the LWC projection lands sane UVs.
                        if (firstShadowUV.x < 0.0)
                        {
                            firstShadowUV = saturate(uv);
                        }
                        // SceneDepth at uv is linear camera-Z in cm; clip.w in UE perspective
                        // projection is also camera-Z, so the comparison is consistent.
                        float sd = CalcSceneDepth(uv);
                        // v1.0.109 Guard A -- sky / no-geometry tap. At a sky pixel UE
                        // 5.7's `CalcSceneDepth` returns the FAR-plane sentinel (very
                        // large camera-Z); a sample that lands on "no opaque was
                        // rendered here" is unambiguously UNOCCLUDED regardless of
                        // what numeric value the depth buffer actually carries. The
                        // complementary near-zero check catches the dual failure mode
                        // where a partial HZB fill / fast-clear half-evaluated tile
                        // reads back as ~0 -- an obviously-bogus camera-Z that the
                        // depth comparison would otherwise read as "occluder right
                        // at the camera" and false-shadow every step inside it.
                        if (sd >= SkyDepthSentinel || sd <= 0.001)
                        {
                            ++skySampleCount;
                            continue;
                        }
                        // v1.0.109 Guard C -- distance-scaled bias. The v1.0.99 absolute
                        // `0.01 * sdt` floor stays (close-range robust-mode tolerance);
                        // the multiplicative term `sd * BeamShadowBiasScale` (default
                        // scale 0.002 = 0.2 percent of sample depth) grows the bias
                        // linearly with depth so the test stays meaningful in the
                        // reverse-Z low-precision regime at the long end of the beam
                        // throw (~6 cm bias at 30 m, ~40 cm at 200 m). The constant
                        // 5 cm `biasBase` from v1.0.99 is the FIRST-STEP MINIMUM and
                        // is separately checked above on `sj`; this `stepBias` is
                        // the per-step depth-comparison tolerance.
                        float stepBias = (0.01 * sdt) + sd * biasScale;
                        if (sd + stepBias < clipP.w) { sOccluded = true; break; }
                    }
                    if (sOccluded)
                    {
                        shadowAtten = max(1.0 - BeamShadowStrength, 0.0);
                        ++shadowedSampleCount;
                    }
                }
            }
            float d = BeamDensity * core * widthNorm * srcAtten * nf * softOcc * shadowAtten;
            float a = 1.0 - exp(-d * dt);
            trans *= (1.0 - a);
        }
    }
    t += dt;
}

float coverage = saturate(1.0 - trans);
float3 col = BeamColor.rgb * BeamIntensity * coverage;

// v1.0.99 + v1.0.109 debug visualisation.
//   Mode 1 (v1.0.99, unchanged): per-pixel heatmap of `shadowedSampleCount /
//     totalSampleCount` (green = no shadow steps found an occluder, red = every
//     visible sample was shadowed).
//   Mode 2 (v1.0.109 REPURPOSED): colour samples by which GUARD fired during the
//     per-pixel shadow march. Discrete priority (highest wins) so the dominant
//     failure mode bubbles to the visible colour:
//       RED    = at least one depth-occluded step  (true scene-depth shadow).
//       GREEN  = off-screen guard fired             (Guard D -- v1.0.99 + v1.0.109).
//       BLUE   = sky / no-geometry guard fired      (Guard A -- NEW v1.0.109).
//       YELLOW = far-distance cull fired            (Guard B -- NEW v1.0.109).
//       WHITE  = clean -- trace ran to unoccluded.
//     The pre-v1.0.109 mode-2 "first projected UV" view is retired: the LWC
//     projection bug has been three releases dead and the UV-sanity job is done.
// The 4.0 multiplier keeps the debug output bright even at low coverage so dim cone
// edges stay visible.
[branch]
if (BeamShadowDebug > 0.5)
{
    float shadowedFrac = (totalSampleCount > 0)
        ? saturate((float)shadowedSampleCount / (float)totalSampleCount) : 0.0;
    float3 dbgCol;
    if (BeamShadowDebug > 1.5)
    {
        // Mode 2 -- colour by GUARD REASON. The priority ordering matters: a
        // pixel where (say) ONE step found a true occluder AND a different step
        // fell off-screen should colour RED, not GREEN, because RED represents
        // genuine scene shadow while GREEN represents the guard rescuing a
        // false-positive -- the operator cares MORE about identifying real
        // shadows than about counting guard rescues.
        const float3 dbgRed    = float3(1.0, 0.0, 0.0);
        const float3 dbgGreen  = float3(0.0, 1.0, 0.0);
        const float3 dbgBlue   = float3(0.0, 0.0, 1.0);
        const float3 dbgYellow = float3(1.0, 1.0, 0.0);
        const float3 dbgWhite  = float3(1.0, 1.0, 1.0);
        if      (shadowedSampleCount > 0) { dbgCol = dbgRed; }
        else if (offScreenSampleCount > 0) { dbgCol = dbgGreen; }
        else if (skySampleCount > 0)       { dbgCol = dbgBlue; }
        else if (farCullSampleCount > 0)   { dbgCol = dbgYellow; }
        else                                { dbgCol = dbgWhite; }
    }
    else
    {
        // Mode 1 -- shadow-factor heatmap.
        const float3 dbgGreen = float3(0.0, 1.0, 0.0);
        const float3 dbgRed   = float3(1.0, 0.0, 0.0);
        dbgCol = lerp(dbgGreen, dbgRed, shadowedFrac);
    }
    col = dbgCol * 4.0 * coverage;
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

    v1.0.96: the shader now self-shadows via a SCREEN-SPACE shadow trace per shaft sample (march
    toward the SpotLight, tap SceneDepth at each step's projected UV; if the depth buffer shows
    an occluder in front of the step the sample is shadowed -> density attenuated by
    `1 - BeamShadowStrength`). See the `_BEAM_RAYMARCH_HLSL` doc-comment for the full algorithm
    + limitations. Mesh-Distance-Field tracing on the runtime glTF-imported truss meshes is
    documented future work (the imported meshes have no SDFs at runtime, so a material can't
    DF-trace them). The pre-v1.0.96 hero-beam native VSM fog hybrid (
    RebusFixtureActor::RefreshBeamShadowMode) is preserved and unchanged -- it carves the
    SEPARATE soft per-light fog halo, on top of which the v1.0.96 trace carves the crisp
    cone-mesh shaft.
    """
    mel = unreal.MaterialEditingLibrary

    _set(mat, "material_domain", unreal.MaterialDomain.MD_SURFACE)
    _set(mat, "shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    _set(mat, "blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    _set(mat, "two_sided", True)

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
    # v1.0.96 screen-space shadow trace parameters. See the _BEAM_RAYMARCH_HLSL doc-comment for
    # the full algorithm. BeamShadowSteps is clamped to [1, 16] inside the shader so a runaway
    # CVar can't blow the per-pixel cost; default 8 is the design point paired with StepCount=32
    # (32 * 8 = 256 SceneDepth taps per beam pixel). BeamShadowStrength = 1 means "fully shadow
    # any sample whose shadow ray hits an occluder" -- 0 disables the trace via the
    # `if (BeamShadowStrength > 0.001) [branch]` gate in the shader.
    #
    # v1.0.99: BeamShadowBias now means "FIRST-STEP MINIMUM cm" (default raised 0.5 -> 5.0)
    # to prevent self-occlusion against nearby fixture / prop geometry on the first SceneDepth
    # tap. See the v1.0.99 PROJECTION + SELF-OCCLUSION FIX block in the _BEAM_RAYMARCH_HLSL
    # doc-comment for the rationale.
    #
    # v1.0.99 BeamShadowDebug visualisation mode (default 0 = off). Set to 1 to render the
    # per-pixel shadow-factor heatmap (green = unshadowed, red = shadowed); set to 2 to render
    # the v1.0.109 colour-by-GUARD-REASON view (RED depth-occluded, GREEN off-screen guard,
    # BLUE sky / no-geometry, YELLOW far-cull, WHITE clean). Driven by the
    # `Rebus.BeamShadowDebug [0|1|2]` console CVar (RebusFixtureActor.cpp).
    shadowsteps = _scalar("BeamShadowSteps", 8.0, 500)
    shadowstrength = _scalar("BeamShadowStrength", 1.0, 580)
    shadowbias = _scalar("BeamShadowBias", 5.0, 660)
    shadowdebug = _scalar("BeamShadowDebug", 0.0, 740)
    # v1.0.109 pan-edge / sky / far-distance guards on the screen-space shadow trace.
    # See the v1.0.109 PAN-EDGE GUARDS block in the _BEAM_RAYMARCH_HLSL doc-comment for
    # the full diagnosis + per-guard rationale. All three are CVar-driven (Rebus.Beam
    # ShadowFarCullCm / Rebus.BeamShadowEdgeGuard / Rebus.BeamShadowBiasScale via
    # RebusFixtureActor.cpp) and surfaced in `Rebus.DumpBeamShadow` with the same
    # EXISTS/MISSING flag the v1.0.103 scalars carry. Defaults paired with the
    # `GRebusBeamShadow*` C++ globals so a fresh project / master regen agrees with a
    # fresh-spawn fixture without any portal push.
    #   * BeamShadowFarCullCm  = 50000.0 cm = 500 m -- shadow march steps with camera-Z
    #     above this are skipped (reverse-Z precision is sub-LSB beyond ~500 m, the
    #     comparison becomes random; the rig fits comfortably inside 200 m).
    #   * BeamShadowEdgeGuard  = 1.0 (ON) -- master toggle for the off-screen NDC
    #     guard. 0 restores the v1.0.99 broken behaviour for diagnostic A/B; keep at
    #     1 in production.
    #   * BeamShadowBiasScale  = 0.002 -- 0.2 percent of sample depth in cm added on
    #     top of the v1.0.99 absolute `BeamShadowBias` cm. ~6 cm bias at 30 m, ~40
    #     cm at 200 m -- grows linearly with depth so the comparison stays meaningful
    #     in the long-throw low-precision regime.
    # Y positions placed BELOW the existing engine-node row (campos / pixelpos /
    # scenedepth / pixeldepth -- last sits at 1220) so the v1.0.109 scalars don't
    # collide with the v1.0.96 vector params (BeamOrigin / BeamDir at 820 / 900)
    # in the editor graph. Purely cosmetic -- the wire connections through
    # `input_names` + `custom_inputs` + `src_for` below are the source of truth.
    shadowfarcull = _scalar("BeamShadowFarCullCm", 50000.0, 1300)
    shadowedgeguard = _scalar("BeamShadowEdgeGuard", 1.0, 1380)
    shadowbiasscale = _scalar("BeamShadowBiasScale", 0.002, 1460)

    beamorigin = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -1100, 820)
    beamorigin.set_editor_property("parameter_name", "BeamOrigin")
    beamorigin.set_editor_property("default_value", unreal.LinearColor(0.0, 0.0, 0.0, 0.0))

    beamdir = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -1100, 900)
    beamdir.set_editor_property("parameter_name", "BeamDir")
    beamdir.set_editor_property("default_value", unreal.LinearColor(1.0, 0.0, 0.0, 0.0))

    # ---- Scene/view inputs (engine nodes) ----
    campos = mel.create_material_expression(mat, unreal.MaterialExpressionCameraPositionWS, -1100, 980)
    pixelpos = mel.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -1100, 1060)
    scenedepth = mel.create_material_expression(mat, unreal.MaterialExpressionSceneDepth, -1100, 1140)
    pixeldepth = mel.create_material_expression(mat, unreal.MaterialExpressionPixelDepth, -1100, 1220)

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
        # v1.0.96 screen-space shadow trace inputs (v1.0.99 added BeamShadowDebug).
        "BeamShadowSteps", "BeamShadowStrength", "BeamShadowBias", "BeamShadowDebug",
        # v1.0.109 pan-edge / sky / far-distance guards (see the v1.0.109 PAN-EDGE
        # GUARDS block in the _BEAM_RAYMARCH_HLSL doc-comment).
        "BeamShadowFarCullCm", "BeamShadowEdgeGuard", "BeamShadowBiasScale",
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
        "BeamShadowSteps": shadowsteps, "BeamShadowStrength": shadowstrength,
        "BeamShadowBias": shadowbias, "BeamShadowDebug": shadowdebug,
        # v1.0.109 -- pan-edge / sky / far-distance guard scalars.
        "BeamShadowFarCullCm": shadowfarcull, "BeamShadowEdgeGuard": shadowedgeguard,
        "BeamShadowBiasScale": shadowbiasscale,
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


def _beam_master_has_shadow_steps(master):
    """v1.0.96 self-heal probe -- True when the on-disk master is the v1.0.96 version that
    exposes the screen-space shadow trace parameter set (`BeamShadowSteps` + `BeamShadowStrength`
    + `BeamShadowBias` scalars). Pre-v1.0.96 masters lack these scalars and need a force-regen
    so the per-fixture MID picks up the new Custom HLSL node + parameter contract on the next
    editor launch. Mirrors v1.0.86's `_master_has_tiling_meters` pattern exactly.
    """
    try:
        info = unreal.MaterialEditingLibrary.get_scalar_parameter_names(master)
    except Exception:  # noqa: BLE001
        return False
    names = [str(n) for n in info]
    return ("BeamShadowSteps" in names
            and "BeamShadowStrength" in names
            and "BeamShadowBias" in names)


def _beam_master_has_shadow_debug(master):
    """v1.0.99 self-heal probe -- True when the on-disk master is the v1.0.99 version that
    exposes the new `BeamShadowDebug` scalar (the per-pixel shadow-factor / first-UV
    visualisation knob driven by `Rebus.BeamShadowDebug`). Pre-v1.0.99 masters carry the
    v1.0.96 shadow-trace contract but the projection-bug-and-self-occlusion fix in the
    Custom HLSL is bundled with this scalar; flagging the master as STALE on missing
    BeamShadowDebug forces a regen with the corrected HLSL on the next editor launch.
    Mirrors `_beam_master_has_shadow_steps` exactly.
    """
    try:
        info = unreal.MaterialEditingLibrary.get_scalar_parameter_names(master)
    except Exception:  # noqa: BLE001
        return False
    names = [str(n) for n in info]
    return "BeamShadowDebug" in names


def _beam_master_has_pan_edge_guard(master):
    """v1.0.109 self-heal probe -- True when the on-disk master carries the v1.0.109
    pan-edge / sky / far-distance guard scalar contract (`BeamShadowFarCullCm` +
    `BeamShadowEdgeGuard` + `BeamShadowBiasScale`). Pre-v1.0.109 masters carry the
    v1.0.99 shadow trace but lack the three new guards AND -- critically -- run the
    pre-v1.0.109 Custom HLSL that doesn't have the sky / far-cull / distance-scaled-bias
    branches at all. The fix lives in the HLSL itself, so flagging the master STALE on
    missing v1.0.109 scalars forces a regen that replaces the entire Custom node code
    on the next editor launch -- catches the symptom the user reported against v1.0.106:
    "the beam vs object shadowing is cutting the side of the beams when we pan left or
    right". See the v1.0.109 PAN-EDGE GUARDS block in `_BEAM_RAYMARCH_HLSL` for the
    diagnostic + per-guard rationale. Mirrors `_beam_master_has_shadow_debug` exactly.
    """
    try:
        info = unreal.MaterialEditingLibrary.get_scalar_parameter_names(master)
    except Exception:  # noqa: BLE001
        return False
    names = [str(n) for n in info]
    return ("BeamShadowFarCullCm" in names
            and "BeamShadowEdgeGuard" in names
            and "BeamShadowBiasScale" in names)


def ensure_beam_material(force=False):
    """Generate the faux-volumetric beam master material. Idempotent (only creates when missing)
    unless force=True (delete + regenerate, e.g. during a full build()).

    v1.0.96 / v1.0.99 / v1.0.109 self-heal cascade: when an EXISTING master is on disk but is
    missing any of the per-release scalar contracts, promote the call to a force-regen so the
    C++ MID push picks up the new shape on the next launch -- log a Warning per release so the
    change is auditable. Each cascade step targets the bug-class the release fixed:
      * pre-v1.0.96 -- missing `BeamShadowSteps`/`Strength`/`Bias` -> regen w/ v1.0.96 shadow
        trace itself.
      * pre-v1.0.99 -- missing `BeamShadowDebug` -> regen w/ v1.0.99 LWC-projection + first-
        step-bias fix.
      * pre-v1.0.109 -- missing `BeamShadowFarCullCm`/`EdgeGuard`/`BiasScale` -> regen w/
        v1.0.109 pan-edge / sky / far-distance / distance-scaled-bias guards (the
        screen-space-shadow failure mode the v1.0.106 default-flip finally made visible).

    Mirrors v1.0.86 (`_master_has_tiling_meters`) and v1.0.93 (`_fixture_lens_master_is_current`).
    """
    tools = unreal.AssetToolsHelpers.get_asset_tools()

    if not force and unreal.EditorAssetLibrary.does_asset_exist(BEAM_PATH):
        existing = unreal.EditorAssetLibrary.load_asset(BEAM_PATH)
        if existing is not None and not _beam_master_has_shadow_steps(existing):
            unreal.log_warning("RebusBaseLevel: pre-v1.0.96 M_RebusBeam detected "
                               "(missing BeamShadowSteps/BeamShadowStrength/BeamShadowBias); "
                               "regenerating with screen-space shadow trace.")
            force = True
        elif existing is not None and not _beam_master_has_shadow_debug(existing):
            # v1.0.99 self-heal -- existing v1.0.96/97 master is missing BeamShadowDebug
            # AND (more importantly) the v1.0.99 LWC-projection / first-step-bias fix in
            # the _BEAM_RAYMARCH_HLSL Custom node. Force-regen so the per-fixture MID picks
            # up the corrected shader (operator-visible bug: shadow trace runs but every
            # step lands off-screen, so the beam looks unshadowed at any occluder -- the
            # exact symptom the v1.0.99 release block calls out).
            unreal.log_warning("RebusBaseLevel: pre-v1.0.99 M_RebusBeam detected "
                               "(missing BeamShadowDebug + carries the v1.0.96 LWC "
                               "projection bug); regenerating with the v1.0.99 fix.")
            force = True
        elif existing is not None and not _beam_master_has_pan_edge_guard(existing):
            # v1.0.109 self-heal -- existing v1.0.99..v1.0.108 master is missing the
            # pan-edge / sky / far-distance guard scalar contract AND -- critically --
            # runs the pre-v1.0.109 _BEAM_RAYMARCH_HLSL without the sky / far-cull /
            # distance-scaled-bias / edge-toggle branches at all. Operator-visible bug
            # (the v1.0.109 user report against v1.0.106): "the beam vs object shadowing
            # is cutting the side of the beams when we pan left or right and is doing
            # the same for all fixtures" -- the canonical screen-space-shadow failure
            # mode v1.0.106 finally exposed by making the procedural cone the visible
            # shaft. Force-regen so the per-fixture MID picks up the v1.0.109 HLSL with
            # the four guards (A sky, B far-cull, C distance-scaled bias, D edge-toggle).
            unreal.log_warning("RebusBaseLevel: pre-v1.0.109 M_RebusBeam detected "
                               "(missing BeamShadowFarCullCm/BeamShadowEdgeGuard/"
                               "BeamShadowBiasScale + carries the pan-edge / sky / "
                               "far-distance failure-mode bug); regenerating with "
                               "the v1.0.109 pan-edge guards.")
            force = True

    if force and unreal.EditorAssetLibrary.does_asset_exist(BEAM_PATH):
        unreal.EditorAssetLibrary.delete_asset(BEAM_PATH)

    if not unreal.EditorAssetLibrary.does_asset_exist(BEAM_PATH):
        mat = tools.create_asset("M_RebusBeam", MATERIALS_DIR, unreal.Material, unreal.MaterialFactoryNew())
        _build_beam_master(mat)
        unreal.EditorAssetLibrary.save_loaded_asset(mat)

    unreal.log("RebusBaseLevel: beam material ensured.")


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
    v1.0.86 (`_master_has_tiling_meters`) / v1.0.93 (`_fixture_lens_master_is_current`) /
    v1.0.96 (`_beam_master_has_shadow_steps`).

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


def _add_floor():
    plane = unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/Plane.Plane")
    floor = _spawn(unreal.StaticMeshActor, unreal.Vector(0.0, 0.0, 0.0), label="RebusFloor")
    if not floor:
        return
    # Tag so URebusSceneSettingsSubsystem can find it for GroundSurface / bGroundVisible.
    floor.set_editor_property("tags", ["RebusFloor"])
    # ~2 km plane reads as an effectively infinite ground at stage scale.
    floor.set_actor_scale3d(unreal.Vector(2000.0, 2000.0, 1.0))

    comp = _component(floor, unreal.StaticMeshComponent)
    if comp and plane:
        comp.set_static_mesh(plane)
        mic = unreal.EditorAssetLibrary.load_asset(_instance_path(GROUND_DEFAULT_PRESET))
        if mic:
            comp.set_material(0, mic)
    unreal.log("RebusBaseLevel: infinite floor plane added (default surface: {}).".format(GROUND_DEFAULT_PRESET))


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


def build():
    """Force-(re)create the base level from scratch and save it.

    Replaces whatever level is currently open, so call this only for an explicit
    (re)bake (Tools > Execute Python Script / headless -run=pythonscript).
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
	# v1.0.95 migration -- see the helper docstring + README v1.0.95.
	_cleanup_internal_beam_assets()

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
	# Ground + lens materials self-heal independently of the map (cheap if already present).
	ensure_ground_materials()
	ensure_lens_material()
	ensure_beam_material()
	# v1.0.93 mirror lens self-heal (drives every Epic-beam lens object).
	ensure_fixture_lens_material()
	# v1.0.104 Orbit-imported two-sided opaque master self-heal.
	ensure_orbit_imported_material()
	# v1.0.95 migration -- see the helper docstring + README v1.0.95.
	_cleanup_internal_beam_assets()

	if unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PACKAGE_PATH):
        return False
    unreal.log("RebusBaseLevel: '{}' missing; generating it.".format(LEVEL_PACKAGE_PATH))
    build()
    return True


if __name__ == "__main__":
    build()
