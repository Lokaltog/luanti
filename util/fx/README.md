# fx fork: mood renderer

A patch queue on top of upstream Luanti for a Terraria-like look: coloured,
leak-free light, glowing emissives, HDR bloom, glass and water, material maps.
Everything is client-only: no map, protocol or server changes. Every effect sits
behind an `fx_*` setting (Settings → Mood Renderer), and fx only runs on the
`opengl3` video driver; GLES and the legacy OpenGL 2 driver get plain upstream
rendering.

## How it is put together

- **One renderer per world.** `fx::attach` (when the client is created) builds
  an `fx::Renderer` and `fx::detach` destroys it; there is no mutable global
  state between worlds. Code under `src/client/fx/`; the hooks in upstream
  files are a few lines each.
- **Settings registry.** `src/client/fx/params.def` lists every setting once
  (type, default, range, apply policy, tuner label). Defaults, the tuner's
  sliders, the `FxParams` uniform block and its GLSL accessors (`fxBloomKnee`
  …) are generated from it; a unit test keeps `builtin/settingtypes.txt` in
  sync. Apply policy: **LIVE** (next frame), **SHADERS** (shaders rebuild
  automatically) or **RESTART** (applies when you next join a world; the tuner
  says so).
- **Capabilities.** Decided once per world (`caps.cpp`) from the settings and
  the driver: HDR needs post-processing and float render targets and then
  forces an RGBA16F scene buffer; TAA is off by default (anti-aliasing is a
  non-goal) and off with MSAA/SSAA; AO needs HDR; material maps need array
  textures. Shaders get these as defines.
- **Uniform blocks.** Irrlicht gained uniform buffer objects
  (`setUniformBlockBinding`, `setUniformBufferData`). Binding 1 holds the
  parameters, 2 the per-frame data (matrices, camera, sun, water level, light
  volume origin), 3 the light list.
- **Colour contract.** Every fx term is computed in linear light. The scene
  buffer stores display-encoded values (`x^(1/2.2)`, HDR above 1 allowed), like
  upstream, so sky, clouds, particles and other upstream shaders keep working;
  fx shaders decode on read and encode on write (`fx_common/color.glsl`).
- **Shader includes.** `#include "fx_common/…"` with include-once guards and
  `#line` per source; `fx_generated/params.glsl` is generated.

## Features

| Feature | How | Settings |
| --- | --- | --- |
| HDR composite | AO → exposure → underwater → bloom → tonemap (AgX by default, or ACES) → grade → dither. | `fx_hdr_composite`, `fx_tonemapper`, `fx_exposure` |
| AgX look | Power and saturation in AgX space, so coloured lights stay coloured. Neutral by default. | `fx_grade_saturation`, `fx_grade_contrast` |
| Threshold bloom | Soft-knee luminance threshold, added on top. | `fx_bloom_threshold`, `fx_bloom_knee`, `fx_bloom_intensity` |
| Emissive maps | `<texture>_e.png` companions in a texture array beside the base array (slot 2). Light-source tiles no unlit node uses, without a map, go through `[fx_autoemissive`. Entities glow only with an authored map. Tinted nodes (palette or `color`) glow in their tint, e.g. a wire's power level. | `fx_emissive_strength`, `fx_auto_emissive_threshold` |
| Coloured light | A 64×48×64-node RGB flood fill around the camera: the engine's light rule per channel on a worker thread, so colour only reaches where light can travel. Stained glass multiplies what passes through it. The snapshot is updated incrementally (re-meshed blocks are re-read, moving the window reads only new slabs) and recomputed only when its generation changes. Map nodes, entities and particles sample it; brightness still comes from the engine's scalar light. Beyond the window light is white. | `fx_colored_light`, `fx_light_saturation` |
| Flicker | Flickering sources (torches, fire, lava; pack table or name heuristic) go to the shader as a light list with per-light phase. | `fx_flicker`, `fx_light_gather_radius` |
| Held light | A light-emitting item in hand lights and tints the camera's surroundings. | `fx_held_light` |
| Sky light | Golden hour warmth and blue moonlight, graded by sun elevation. | `fx_sky_tint` |
| Ambient floor, rim light | Caves are dark but never pure black; mobs and items read against dark backgrounds. | `fx_ambient_floor`, `fx_rim_light` |
| Ambient occlusion | Half resolution, normal-weighted horizon sampling; the held item neither receives nor casts it. Without TAA a 4×4 bilateral blur and a depth-aware upsample denoise it. | `fx_ao`, `fx_ao_strength`, `fx_ao_radius` |
| Contact shadows | A short screen-space march towards the sun in the AO pass, applied to mid-bright pixels. | `fx_contact_shadows` |
| Glass | Glasslike drawtypes, nodes named `*glass*` and translucent solid nodes with a liquid drawtype (Mineclonia's ice) refract the opaque scene copy (slots 4 and 5), tint by their texture, reflect (SSR with a sky fallback). Drawn in the transparent pass, opaque, writing depth. Patterns: fluted, frosted, rippled. Framed glass gets a rounded bevel where a pane meets its frame (connected panes stay flat at the seams). | `fx_surfaces`, `fx_refraction`, `fx_ssr`, `fx_reflection`, `fx_glass_bevel` |
| Water | Translucent liquids: ripple normals, depth absorption, caustics on the floor, Fresnel reflection. The light behind the surface is fogged for its own distance, the surface's own light for the surface's. | `fx_water_absorption`, `fx_caustics` |
| Underwater | Per pixel in the composite, using the water surface height above the camera: absorption only along the submerged part of each ray, caustics, light shafts. Replaces upstream's flat overlay. | `fx_underwater_shafts` |
| Material maps | LabPBR 1.3 `_n`/`_s` companions: normals (cotangent frame from screen-space derivatives), texel-stepped parallax, AO, GGX sun specular, glints that fade once a texel is smaller than a pixel, `_s` emission. | `fx_materials`, `fx_normal_strength`, `fx_parallax_depth`, `fx_specular`, `fx_glints` |
| Heat haze | UV shimmer above warm, strongly blooming sources. | `fx_heat_haze` |
| TAA (off) | Kept but frozen: jitter, reprojection, variance clipping, sharpen. | `fx_taa`, `fx_taa_blend`, `fx_sharpen` |
| Moods | `client/fx_moods/*.conf` plus your own in `<user dir>/fx_moods/`. A mood is a complete overlay on the defaults; `cave.*`, `night.*` and `underwater.*` keys blend in situationally. | `fx_mood` |
| Mood tuner | Pause menu → Mood: a slider per tunable setting, "Save as mood". | |
| Debug views | 1 light chroma, 2 emissive, 3 HDR luminance (false colour), 4 bloom input, 5 scene copy, 6 scene copy depth, 7 AO. | `fx_debug_view` |
| Shader hot reload | Recompiles changed `*.glsl` in place; on a compile error the old program keeps running. | `fx_shader_hot_reload` |
| Ambient particles | `util/fx/mods/fx_ambience` (server mod, enable per world): fireflies at night under the open sky, motes around lights, embers over lava and fire, drips, bubbles, falling leaves. One grouped node scan per player per second. | `fx_particles`, `fx_particles_density`, `fx_particles_<kind>` |

Texture slots (Irrlicht now has 8): 0 base, 1 crack, 2 emissive, 3 shadow map,
4 and 5 scene copy, 6 light volume, 7 material array.

## Texture packs

The pack contract (emissive companions, light colours, surfaces, material maps,
`fx_pack.txt`, key syntax, diagnostics) is specified in
[PACK_FORMAT.md](PACK_FORMAT.md). `util/fx/testpack/` is a conformance fixture.
jsl2mcl (lokaltog/mineclonia-jsl) produces packs to it; give it a node dump from
`util/fx/mods/fx_nodedump` so it works from the real node definitions.

## Recommended config

```
enable_post_processing = true
enable_bloom = true
enable_auto_exposure = true
fx_enable = true
```

## Tooling

`devenv shell` (or direnv) gives you these commands; `fx-help` lists them. The
nixpkgs revisions in `devenv.yaml` and `flake.nix` are pinned.

| Command | What it does |
| --- | --- |
| `fx-build` | Incremental build: RelWithDebInfo, RUN_IN_PLACE, ccache, unit tests. |
| `fx-run` | Runs with `util/fx/dev.conf` (hot reload on); settings persist in `.devenv/luanti.conf`. `FX_TEXTURE_PACK=…` loads a pack. |
| `fx-play <world>` | Opens a world from `~/.minetest/worlds`. |
| `fx-upstream` | `fx-run` with `fx_enable = false`, for A/B. |
| `fx-test` / `devenv test` | The test harness on Xvfb + llvmpipe (see below). |
| `fx-test-gpu` | The same on your display and GPU, against the `gpu` goldens. |
| `fx-unit` | C++ unit tests (`TestFx`: flood fill, settings registry and settingtypes sync, pack tables and globs). |
| `fx-mcl <path>` | Links a Mineclonia checkout as `games/mineclonia` for the `mcl_*` cases. |
| `fx-renderdoc` | Frame capture. |
| `fx-install-mods` | Links `util/fx/mods/*` into this tree's `mods/` (not `~/.minetest`). |
| `fx-jsl-test` | Runs mineclonia-jsl's fx tests against this tree's test pack and node dump (`JSL2MCL_DIR`). |
| `fx-gl <cmd>` | Runs a command with the NixOS GPU driver on the library path; the GPU commands above use it, the shell itself doesn't. |

## Test harness

`util/fx/test/run.py` renders every case in `cases.py` in a deterministic test
mode (`fx_test_*` settings: fixed 1/60 s steps, capture once the scene mod says
it is ready and meshing and the light volume are idle) from a snapshot of the
build, so editing shaders mid-run can't affect it. Each case checks:

- that it rendered without Lua, shader or GL errors (and no NaN: in test mode
  NaN/Inf pixels turn magenta);
- probes: luminance, hue or saturation of screen regions (the lit floor, a red
  lamp's hue, a jack o'lantern's face glowing and its side not);
- comparisons between cases (a mood must change the image, toggling fx must
  return to the same image);
- a golden image at 480×270 (`golden/llvmpipe/`, `golden/gpu/`).

`xfail` marks a known bug: reported, not failed, and failing once it passes.
Options: `--only REGEX`, `--update-goldens`, `--gpu`, `--set KEY=VALUE`,
`--display N` (parallel runs), `--out DIR`. Report: `shots/test/report.html`.

Scenes: `util/fx/testworld` (devtest: lamps, glass, water, lava, materials,
mob) and `util/fx/test/worlds/mcl` (Mineclonia: light sources facing the camera
with their unlit twins behind, ores, glass, water and lava).

## Known gaps

- Held node items drawn as cubes and inventory icons have no emissive maps
  (held torches and other flat node items glow; craftitems with an authored
  `_e.png` glow).
- Material maps need the array-texture path; animated tiles get none.
- Auto-emissive keeps bright pixels whatever their colour, so bright stone
  parts of a light source's unique tile (the rim of a lit furnace's front)
  glow too. An authored `_e.png` fixes it.
- Contact shadows don't know whether the sun reaches a pixel, so a daylit cave
  floor next to a wall can pick up a faint shadow (limited to 0.7 nodes and
  mid-bright pixels).
- Glass behind glass refracts the opaque scene only.
- Beyond the light volume window (about 32 nodes horizontally) artificial
  light is white. The held item doesn't sample the volume; particles take
  its hue per particle (nearest node), and glowing particles keep their own
  colour.
- Cave detection reads the sky light at the camera, so a deep, roofed-over
  build counts as a cave.
- The flake and devenv have not been run on NixOS yet; run `nix flake lock` /
  `devenv shell` once to record the hashes.
