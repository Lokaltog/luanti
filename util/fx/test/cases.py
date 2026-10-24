"""Test cases for util/fx/test/run.py.

A case:
  name, doc
  world:     key of WORLDS (default "fxscene")
  settings:  extra client settings (@ROOT@ = repo root)
  mods:      extra mods from util/fx/mods copied into the world
  probes:    [{name, region: [x0, y0, x1, y1] as fractions, lum: [lo, hi],
               hue: [deg, tolerance], sat_min, sat_max}]
  compare:   [{with: other case, min_mean | max_mean, region?, name?}]
             min_mean = must visibly differ, max_mean = must look the same
  golden:    compare with util/fx/test/golden/<backend>/<name>.png (default True)
  golden_tolerance: {backend: {mean, frac}}
  max_nan_pixels, max_black, max_white, log_must: [regex], timeout
  xfail:     on a probe or compare: the finding/phase that fixes a known bug.
             It is reported, not failed; once it passes the run fails until
             the xfail is removed.
"""

PACK = "@ROOT@/util/fx/testpack"

WORLDS = {
    # devtest room: three lamps, lava, glass wall, pool, leak wall, red glass box
    "fxscene": {"path": "util/fx/testworld", "mods": []},
    # Mineclonia, singlenode, a built scene of real light sources and materials
    "mcl": {
        "path": "util/fx/test/worlds/mcl",
        "mods": [],
        "settings": {"fxmcl_view": "lights", "mobs_spawn": "false", "mcl_doWeatherCycle": "false"},
    },
}

NIGHT = {"fxscene_time": "0"}
DAY = {"fxscene_time": "0.45"}

CASES = [
    # --- baselines ------------------------------------------------------------
    {"name": "upstream", "doc": "fx off: the engine as upstream renders it",
     "settings": {"fx_enable": "false"}},
    {"name": "night", "doc": "default fx at night: lamps, lava, light volume",
     "probes": [
         {"name": "floor lit", "region": [0.1, 0.85, 0.9, 0.98], "lum": [0.08, 0.6]},
         {"name": "red lamp wall is red", "region": [0.32, 0.28, 0.38, 0.36], "hue": [345, 30], "sat_min": 0.3},
         {"name": "blue lamp wall is blue", "region": [0.47, 0.28, 0.52, 0.36], "hue": [225, 35]},
     ]},
    {"name": "day", "doc": "default fx at noon", "settings": DAY},
    {"name": "underwater", "doc": "camera in the pool at noon",
     "settings": {**DAY, "fxscene_view": "underwater"},
     "probes": [{"name": "water is blue-green (not a red death screen)", "region": [0.1, 0.1, 0.9, 0.9],
                 "hue": [185, 35]}]},

    # --- features off / variants -------------------------------------------
    {"name": "no_taa_ao", "settings": {"fx_taa": "false", "fx_ao": "false"}},
    {"name": "night_white", "doc": "fx_colored_light off: white artificial light",
     "settings": {"fx_colored_light": "false"},
     "compare": [{"with": "night", "min_mean": 2.0, "name": "colour changes the image"}]},
    {"name": "tint_glow", "doc": "tint-aware glow: the magenta _e map takes the palette tint",
     "settings": {"texture_path": PACK, "fxscene_view": "materials", "fx_debug_view": "2"},
     "golden": False, "max_black": 1.0,
     "probes": [
         {"name": "red tint: red glow", "region": [0.34, 0.17, 0.45, 0.33], "hue": [355, 30]},
         {"name": "green tint on magenta: dim violet", "region": [0.55, 0.17, 0.66, 0.33], "hue": [270, 35]},
     ]},
    {"name": "materials", "doc": "M5 material maps in daylight",
     "settings": {**DAY, "texture_path": PACK, "fxscene_time": "0.35", "fxscene_view": "materials"},
     "compare": [{"with": "materials_off", "min_mean": 2.0, "name": "maps change the image"}]},
    {"name": "materials_off",
     "settings": {**DAY, "texture_path": PACK, "fxscene_time": "0.35", "fxscene_view": "materials",
                  "fx_materials": "false"}},
    {"name": "specular_hot", "doc": "regression #2: extreme specular must not overflow fp16",
     "settings": {**DAY, "texture_path": PACK, "fxscene_time": "0.35", "fxscene_view": "materials",
                  "fx_specular": "3", "fx_glints": "3", "fx_exposure": "4"},
     "golden": False},
    {"name": "no_contact_shadows",
     "settings": {**DAY, "fx_contact_shadows": "0"}},
    {"name": "contact_shadows", "doc": "regression #3: contact shadows change little overall",
     "settings": DAY,
     "compare": [{"with": "no_contact_shadows", "max_mean": 4.0,
                  "name": "contact shadows are local, not a global darkening"}]},

    # --- debug views -----------------------------------------------------------
    {"name": "debug1_chroma", "settings": {"fx_debug_view": "1"}},
    {"name": "debug2_emissive", "settings": {"fx_debug_view": "2"}, "max_black": 1.0},
    {"name": "debug3_luminance", "settings": {"fx_debug_view": "3"}},
    {"name": "debug5_scenecopy", "settings": {"fx_debug_view": "5"}},
    {"name": "debug7_ao", "settings": {"fx_debug_view": "7"}, "max_white": 1.0},

    # --- moods, tuner ----------------------------------------------------------
    {"name": "tuner", "settings": {"fx_open_tuner_on_start": "true"}},
    {"name": "mood_vibrant", "doc": "regression #20: a mood set in the config applies at startup",
     "settings": {**DAY, "fx_mood": "vibrant"},
     "compare": [{"with": "day", "min_mean": 1.5, "name": "vibrant differs from neutral"}]},
    {"name": "mood_dark", "settings": {"fx_mood": "dark"},
     "compare": [{"with": "night", "min_mean": 1.5, "name": "dark differs from neutral"}]},

    # --- pack format ---------------------------------------------------------
    {"name": "testpack_emissive", "settings": {"texture_path": PACK, "fx_debug_view": "2"}, "max_black": 1.0},
    {"name": "testpack_render", "settings": {"texture_path": PACK}},

    # --- runtime behaviour ----------------------------------------------------
    {"name": "rejoin", "doc": "regression #1: join, leave, join again in one process",
     "settings": {"fx_test_rejoin": "1", "texture_path": PACK}, "golden": False},
    {"name": "toggle_fx", "doc": "fx_enable off and on again at runtime",
     "settings": {"fx_test_set": "5:fx_enable=false;20:fx_enable=true"},
     "compare": [{"with": "night", "max_mean": 3.0, "name": "back to the same image"}]},

    # --- screen space (R3) ---------------------------------------------------
    {"name": "hand", "doc": "the wielded item in front of a wall", "settings": {**DAY, "fxscene_view": "hand"}},
    {"name": "hand_ao", "doc": "regression #7: the held item gets no AO and casts none",
     "settings": {**DAY, "fxscene_view": "hand", "fx_debug_view": "7"}, "max_white": 1.0,
     "probes": [{"name": "no AO on the hand", "region": [0.72, 0.72, 0.9, 0.95], "lum": [0.9, 1.0]}]},
    {"name": "hand_night", "doc": "night, holding cobble (reference for held_light)",
     "settings": {"fxscene_view": "hand"}, "golden": False},
    {"name": "held_light", "doc": "a held light source lights the dark wall in front",
     "settings": {"fxscene_view": "hand", "fxscene_wield": "testnodes:light14"},
     "compare": [{"with": "hand_night", "min_mean": 3.0, "name": "the held light brightens the scene"}]},
    {"name": "underwater_up", "doc": "under water looking up: absorption stops at the surface",
     "settings": {**DAY, "fxscene_view": "underwater_up"},
     "probes": [{"name": "water is blue-green", "region": [0.1, 0.5, 0.9, 0.95], "hue": [185, 40]}]},

    {"name": "toggles_off", "doc": "every hard toggle off: shaders still build, the scene renders",
     "settings": {"fx_use_parallax": "false", "fx_use_specular": "false", "fx_use_glints": "false", "fx_use_contact_shadows": "false", "fx_use_caustics": "false", "fx_use_shafts": "false", "fx_use_heat_haze": "false", "fx_use_fog_blur": "false", "fx_use_lens_dirt": "false", "fx_use_end_sky": "false", "fx_use_portal_warp": "false", "fx_use_lightning": "false", "fx_use_damage_fx": "false", "fx_use_wet": "false", "fx_use_snow": "false", "fx_use_wind": "false"}, "golden": False},
    # --- world moods (wave 1); hints from fx_test_hints instead of the server mod
    {"name": "end_sky", "doc": "the End: stars and nebula over the sky, cold grade",
     "settings": {"fx_test_hints": "fx1;dim=end;biome=End;weather=none;flash=0;portal=none",
                  "fxscene_view": "underwater_up", "fxscene_time": "0.0"}, "golden": False},
    {"name": "nether_crimson", "doc": "Nether, crimson forest: warm, blurred distance, vignette",
     "settings": {"fx_test_hints": "fx1;dim=nether;biome=CrimsonForest;weather=none;flash=0;portal=none"},
     "compare": [{"with": "night", "min_mean": 3.0, "name": "the Nether mood changes the image"}]},
    {"name": "storm", "doc": "thunderstorm grade and fog blur",
     "settings": {"fx_test_hints": "fx1;dim=overworld;biome=Plains;weather=thunder;flash=0;portal=none", **DAY},
     "compare": [{"with": "day", "min_mean": 2.0, "name": "the storm mood changes the image"}]},
    {"name": "rain", "doc": "rain: wet, darker, glossy ground with puddles",
     "settings": {"fx_test_hints": "fx1;dim=overworld;biome=Plains;weather=rain;flash=0;portal=none", **DAY},
     "compare": [{"with": "day", "min_mean": 2.0, "name": "rain changes the ground"}]},
    {"name": "snow", "doc": "snow settles on top faces, frost on glass",
     "settings": {"fx_test_hints": "fx1;dim=overworld;biome=IcePlains;weather=snow;flash=0;portal=none", **DAY},
     "compare": [{"with": "day", "min_mean": 3.0, "name": "snow covers the ground"}]},
    {"name": "portal_warp", "doc": "standing in a nether portal: swirl and violet cast",
     "settings": {"fx_test_hints": "fx1;dim=overworld;biome=Plains;weather=none;flash=0;portal=nether"},
     "compare": [{"with": "night", "min_mean": 3.0, "name": "the portal warps the image"}]},
    {"name": "low_health", "doc": "damage FX off unless hurt: same image as night",
     "settings": {}, "golden": False,
     "compare": [{"with": "night", "max_mean": 1.0, "name": "no damage, no red"}]},

    # --- content -----------------------------------------------------------------
    {"name": "particles", "mods": ["fx_ambience"],
     "settings": {"fx_particles_density": "4", "fxscene_view": "particles"}, "golden": False},
    {"name": "particles_day", "mods": ["fx_ambience"],
     "settings": {**DAY, "fx_particles_density": "4", "fxscene_view": "particles"}, "golden": False},

    # --- Mineclonia -------------------------------------------------------------
    {"name": "mcl_night", "world": "mcl", "doc": "real Mineclonia light sources at night",
     "timeout": 600,
     "probes": [{"name": "scene drawn (not a loading screen)", "region": [0.1, 0.85, 0.9, 0.98],
                 "lum": [0.1, 0.8]}]},
    {"name": "mcl_emissive", "world": "mcl", "doc": "regression #9: only real light sources glow",
     "settings": {"fx_debug_view": "2"}, "timeout": 600, "max_black": 1.0,
     "probes": [
         {"name": "jack o'lantern face glows", "region": [0.385, 0.52, 0.45, 0.64], "lum": [0.2, 1.0]},
         {"name": "glowstone glows", "region": [0.06, 0.52, 0.17, 0.62], "lum": [0.3, 1.0]},
         {"name": "lit furnace fire glows", "region": [0.57, 0.6, 0.6, 0.65], "lum": [0.2, 1.0]},
         {"name": "plain pumpkin does not glow", "region": [0.447, 0.475, 0.468, 0.50], "lum": [0.0, 0.03]},
         {"name": "unlit furnace does not glow", "region": [0.525, 0.455, 0.565, 0.478], "lum": [0.0, 0.03]},
         {"name": "jack o'lantern side does not glow", "region": [0.63, 0.48, 0.67, 0.54], "lum": [0.0, 0.03]},
     ]},
    {"name": "mcl_upstream", "world": "mcl", "settings": {"fx_enable": "false"}, "timeout": 600},
]
