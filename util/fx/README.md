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
| Effect toggles | Every effect has a checkbox in the pause-menu tuner. Off compiles it out of the shaders (`fx_use_*`), so it costs nothing; AO, glass and water, material maps and coloured light apply on the next join. | `fx_use_*` |
| Moods | `client/fx_moods/*.conf` plus your own in `<user dir>/fx_moods/`. A mood is a complete overlay on the defaults. `<situation>.fx_*` keys blend in while a situation holds: `night`, `cave`, `underwater`, `rain`, `snow`, `storm`, `nether`, `heat` (Nether near lava), `end`, `portal`, `biome.<name>`. Shared defaults for every mood live in `_situations.conf`. | `fx_mood` |
| World hints | `util/fx/mods/fx_hints` (server mod, enable per world) tells the client the dimension, biome, weather, nearby lightning and portals, as a hidden HUD text. Without it the world situations never trigger. | |
| Weather | With fx_hints: rain wets everything open to the sky (darker, glossy, sky reflections and sun glints) and puddles with rain rings form on top faces; snow settles on top faces and frost creeps in from glass panes' edges; a wind field (strength from the weather, slowly turning direction) sends gusts across foliage, even with the engine's waving off. fx_ambience adds rain dripping off leaves and breath fog in the cold. | `fx_wetness`, `fx_snow`, `fx_wind` |
| Volumetric light & height fog | Light from torches, lava and lamps glows in the air near them, coloured and stopped by walls: a half-res march through the light volume. Height fog pools below `fx_height_fog_base`, integrated analytically in the composite; thicker at dawn and in rain (moods). | `fx_volumetric`, `fx_height_fog`, `fx_height_fog_base`, `fx_use_volumetrics` |
| Bounce light | Screen-space one-bounce GI at half res: lit surfaces tint what faces them (a red wall tints the floor, lava the cave ceiling). From what's on screen only. | `fx_gi`, `fx_use_gi` |
| Soft, coloured sun shadows | The engine's dynamic shadows already do PCSS-style penumbrae and colour through translucent nodes: turn on `enable_dynamic_shadows`, `shadow_map_color`, and raise `shadow_soft_radius` (util/fx/dev.conf has a starting point). | engine settings |
| Water by biome | With fx_hints: turquoise in warm biomes, dark and choppy in cold ones, murky in swamps; swells run with the wind and grow in storms; foam along shores, around things in the water and on rough crests; water runs down the lens after you surface. | `fx_foam`, `fx_droplets_fx` |
| Sky | Physically based sky colour (single-scatter Rayleigh + Mie) over the game's sky by day in the Overworld; the game's chunky 3D clouds stay and now write depth, so sky effects and fog treat them as things in the sky. Aurora on clear nights in cold biomes. | `fx_sky_pbr`, `fx_aurora` |
| Translucency | Sunlight shines through leaves and plants, and through texels an `_s` map marks as subsurface (porosity/SSS 65..255). | `fx_translucency` |
| World effects | Fog distance blur (no halos around near objects), End stars and nebula, portal swirl and violet cast, lightning flash, lens dirt lit by bright light, warmth, vignette, red damage flash and low-health heartbeat, lava and other warm glows pulsing slowly, a violet shimmer over the End, slow idle breathing in first person (Mineclonia already widens the FOV when sprinting). No motion blur. | `fx_fog_blur`, `fx_end_sky`, `fx_portal_warp`, `fx_lightning_flash`, `fx_lens_dirt`, `fx_grade_warmth`, `fx_vignette`, `fx_damage_fx`, `fx_lava_pulse`, `fx_end_aura`, `fx_idle_breath` |
| Mood tuner | Pause menu → Mood: a slider per tunable setting, "Save as mood". | |
| Debug views | 1 light chroma, 2 emissive, 3 HDR luminance (false colour), 4 bloom input, 5 scene copy, 6 scene copy depth, 7 AO. | `fx_debug_view` |
| Shader hot reload | Recompiles changed `*.glsl` in place; on a compile error the old program keeps running. | `fx_shader_hot_reload` |
| GPU particles | Client-side, simulated on the GPU (see below): sparks and embers over lava and fire, leaves that tumble down, land, slide and get kicked up again, rain with splashes, snow that drifts and settles, drips off leaves in rain, dust motes, Nether ash and spores by biome, fireflies. All in the foliage's wind field and stirred by the player. | `fx_use_gpu_particles`, `fx_particle_budget`, `fx_particle_density`, `fx_particle_<kind>`, `fx_particle_wake` |
| Ambient particles | `util/fx/mods/fx_ambience` (server mod, enable per world): drips under water, bubbles, breath fog in the cold (seen by everyone). With `fx_ambience_legacy_particles` (server setting, off by default) also the server-sent fireflies, motes, embers, falling leaves, Nether air and rain drips, for players without GPU particles. One grouped node scan per player per second. | `fx_particles`, `fx_particles_density`, `fx_particles_<kind>`, `fx_ambience_legacy_particles` |

Texture slots (Irrlicht now has 8): 0 base, 1 crack, 2 emissive, 3 shadow map,
4 and 5 scene copy, 6 light volume, 7 material array. Uniform block 4 is the
GPU particles' FxParticles.

## GPU particles

`src/client/fx/particles.{h,cpp}`, `particle_emit.cpp`, shaders
`fx_particles_{update,spawn,draw}` and `fx_common/particles.glsl`. Plain
OpenGL 3.3 (GLSL 1.50): no compute, no SSBOs, no instancing.

- **State** in RGBA32F textures, one texel per slot (512 wide):
  S0 position (nodes relative to the camera offset) + age, S1 velocity +
  kind, S2 life, seed, kind parameter, rest age; S3 tint, leaf layer, size,
  glow. S0..S2 ping-pong between two sets; S3 is written at spawn only.
  100 000 slots are 196 rows: 11 MB of state.
- **Update**: one full-target fragment pass per frame (`fx_particles_update`,
  three render targets): gravity, heat rise, drag towards the air velocity
  (the foliage's wind field from `nodes_shader`, the same gusts; weaker where
  the sky is closed), analytic curl turbulence, the player's wake (position
  and velocity from LocalPlayer; air near a moving player moves with them).
  Collision is block-exact against the **world texture**, axis by axis and
  sub-stepped. Rain and drips turn into splashes where they land, leaves and
  snow come to rest on top faces (leaves float on water), sparks bounce and
  die in water. Resting leaves slide in gusts, fall off edges, get kicked up
  by strong gusts and by the player running past, and fade after 10-20 s.
- **Spawn**: the CPU decides what spawns where and draws one GL point per new
  particle into the four targets (`fx_particles_spawn`). Slots come from a
  ring that skips slots the CPU knows are still alive (it knows each
  particle's longest life), so nothing is read back from the GPU; when the
  budget is full, spawns are dropped.
- **World texture**: an RGBA8 atlas laid out like the light volume's
  (96×48×96 nodes): r solid, g open to the sky, b liquid (water / lava).
  Rebuilt from the light volume's node snapshot at most 4× a second when it
  changes; the same scan collects the emitters.
- **Emitters** (game agnostic, by groups, drawtype and names): lava with air
  above, fire, leaves with air below (`leaves` group, `waving = 2` or
  allfaces named `*leaves*`), plants under the open sky (fireflies at
  night), light sources (dust), plus the weather and world from fx_hints:
  rain or snow (snow when it rains in a cold biome) in a disc around the
  camera where the column is open to the sky, drips under canopies in rain,
  Nether air by biome (as fx_ambience's table), daylight dust.
- **Draw**: at the end of ClientMap's transparent pass, a static mesh of
  16384 quads per chunk of slots (chunk index via `materialColor`); the
  vertex shader fetches the state with `texelFetch` and builds streaks along
  the velocity (sparks, rain, drips), tumbling or flat leaves (the real leaf
  tile from an array texture, tinted by the node's palette colour), flat
  splash rings, or camera-facing dots. Premultiplied alpha, depth-tested,
  no depth writes; glowing kinds add HDR emission (cooling sparks: white-
  yellow, orange, dull red) and bloom. Light: sky (open to the sky × the
  day-night ratio, graded like the sun) and the light volume's coloured
  artificial light. Soft edges against the opaque scene copy's depth.

Settings: `fx_use_gpu_particles` (restart), `fx_particle_budget` (restart,
default 100 000), `fx_particle_density`, `fx_particle_sparks`,
`fx_particle_leaves`, `fx_particle_weather`, `fx_particle_motes`,
`fx_particle_wake` (tuner sliders and mood keys). Needs `fx_colored_light`
(the light volume's snapshot) and float render targets.

**fx_ambience with GPU particles**: the client can't tell the server mod what
it draws, so the mod's replaced spawners are off unless the server sets
`fx_ambience_legacy_particles = true` (for players without GPU particles;
those with them then see both). Drips under water, bubbles and breath fog
stay: other players see breath fog.

Limits: collision and emitters only inside the 96×48×96 window around the
camera (beyond it particles fly free and fade out by 46 nodes); node boxes
count as full blocks if walkable (slabs, stairs), plants and air as empty;
sky light is "open to the sky" per column, not the engine's spread sunlight
(dust indoors near windows is lit by the ambient term); particles drawn
after the transparent pass are hidden behind glass and water surfaces, which
write depth; no sorting between particles (leaves and snow are cut out or
nearly opaque, the rest adds).

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
