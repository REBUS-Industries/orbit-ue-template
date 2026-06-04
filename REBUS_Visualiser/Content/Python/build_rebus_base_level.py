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
    """Author the procedural ground master graph: lerp(ColorA, ColorB, Noise) -> BaseColor."""
    mel = unreal.MaterialEditingLibrary

    color_a = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -600, -150)
    color_a.set_editor_property("parameter_name", "ColorA")
    color_a.set_editor_property("default_value", unreal.LinearColor(0.40, 0.40, 0.40, 1.0))

    color_b = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -600, 120)
    color_b.set_editor_property("parameter_name", "ColorB")
    color_b.set_editor_property("default_value", unreal.LinearColor(0.52, 0.52, 0.50, 1.0))

    noise = mel.create_material_expression(mat, unreal.MaterialExpressionNoise, -600, 340)
    # Small scale => broad features that read at ground scale (world-position driven).
    _set(noise, "scale", 0.005)

    lerp = mel.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -260, -20)
    mel.connect_material_expressions(color_a, "", lerp, "A")
    mel.connect_material_expressions(color_b, "", lerp, "B")
    mel.connect_material_expressions(noise, "", lerp, "Alpha")
    mel.connect_material_property(lerp, "", unreal.MaterialProperty.MP_BASE_COLOR)

    rough = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -260, 260)
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
            float d = BeamDensity * core * widthNorm * srcAtten * nf * softOcc;
            float a = 1.0 - exp(-d * dt);
            trans *= (1.0 - a);
        }
    }
    t += dt;
}

float coverage = saturate(1.0 - trans);
float3 col = BeamColor.rgb * BeamIntensity * coverage;
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
    when the camera is inside (the back wall is the fragment that carries the shaft). Light-BLOCKING
    volumetric shadows on runtime-imported trusses are NOT done in this
    shader (runtime glTF meshes have no mesh distance fields, so a material can't trace them) --
    that is the native VSM fog hybrid on hero beams (see RebusFixtureActor::RefreshBeamShadowMode).
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
    sharp = _scalar("BeamSharpness", 2.5, -60)
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


def ensure_beam_material(force=False):
    """Generate the faux-volumetric beam master material. Idempotent (only creates when missing)
    unless force=True (delete + regenerate, e.g. during a full build())."""
    tools = unreal.AssetToolsHelpers.get_asset_tools()

    if force and unreal.EditorAssetLibrary.does_asset_exist(BEAM_PATH):
        unreal.EditorAssetLibrary.delete_asset(BEAM_PATH)

    if not unreal.EditorAssetLibrary.does_asset_exist(BEAM_PATH):
        mat = tools.create_asset("M_RebusBeam", MATERIALS_DIR, unreal.Material, unreal.MaterialFactoryNew())
        _build_beam_master(mat)
        unreal.EditorAssetLibrary.save_loaded_asset(mat)

    unreal.log("RebusBaseLevel: beam material ensured.")


def ensure_ground_materials(force=False):
    """Generate the procedural ground master + one instance per surface preset.

    Idempotent by default (only creates assets that don't already exist), which is what the
    startup hook wants. With force=True it deletes any existing assets first and regenerates
    them -- an unattended "always overwrite" so a full (re)bake never raises the editor's
    overwrite-confirmation dialog. Only call force=True when the level is about to be rebuilt
    (build()), so no live level actor is left referencing a deleted instance.
    """
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    mel = unreal.MaterialEditingLibrary

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
        unreal.EditorAssetLibrary.save_loaded_asset(mic)

    unreal.log("RebusBaseLevel: ground materials ensured ({} presets).".format(len(GROUND_PRESETS)))


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

    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PACKAGE_PATH):
        return False
    unreal.log("RebusBaseLevel: '{}' missing; generating it.".format(LEVEL_PACKAGE_PATH))
    build()
    return True


if __name__ == "__main__":
    build()
