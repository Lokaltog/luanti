# fx fork: mood renderer (M1 Light & glow, M2 Foundations, M3 Glass & water, M4 Light volume)

A patch queue on top of upstream Luanti. Everything is client-only: no map, protocol or server changes. Every effect sits behind an `fx_*` setting (Settings → Mood Renderer).

## What M1 adds

| Feature | How | Setting |
| --- | --- | --- |
| HDR composite | Tonemap (AgX by default) before clamping. Upstream clamped first, which wasted the 16-bit buffer. | `fx_hdr_composite`, `fx_tonemapper`, `fx_exposure` |
| AgX look | Power and saturation in AgX space, so colored lights stay colored | `fx_grade_saturation`, `fx_grade_contrast` |
| Threshold bloom | Soft-knee luminance threshold, added on top. Upstream blurred the whole frame and mixed it back in. | `fx_bloom_threshold`, `fx_bloom_knee`, `fx_bloom_intensity` |
| Emissive maps | `<texture>_e.png` packed into a companion texture array (slot 2). Light-source nodes without one get `[fx_autoemissive`. | `fx_emissive_strength`, `fx_auto_emissive_threshold` |
| Colored light (T1) | Each mesh records its light sources, clustered per 4³ cell and color. Each frame the nearest 128 go to the vertex shader, which tints the artificial-light term. The engine's scalar light still decides brightness, so color can bleed through walls but light can't. | `fx_colored_light`, `fx_light_saturation`, `fx_light_gather_radius` |
| Flicker | Per-light phase. Amplitude comes from the light table or a name heuristic (torch/fire 1.0, lava 0.35). | `fx_flicker` |
| Held light | A light-emitting item in hand adds a light at the camera | `fx_held_light` |
| Debug views | 1 light chroma, 2 emissive, 3 HDR luminance (false color, blue −4 EV, green 0, red +4 EV), 4 bloom input. Later milestones add 5, 6 (M3) and 7 (AO). | `fx_debug_view` |
| Shader hot reload | Polls `*.glsl` modification times and recompiles in place, keeping material IDs stable. On a compile error the old program keeps running. | `fx_shader_hot_reload` |

## What M2 adds

| Feature | How | Setting |
| --- | --- | --- |
| TAA | Halton jitter around the scene draw only (the held item and HUD are drawn after post-processing). Depth-based reprojection through the previous unjittered matrix, YCoCg variance clipping, and a Catmull-Rom history fetch so pixel art stays crisp. Off with MSAA or SSAA. | `fx_taa`, `fx_taa_blend` |
| Sharpening | Cross unsharp mask in the final pass | `fx_sharpen` |
| Ambient occlusion | Half resolution, normal-weighted horizon sampling (HBAO-style rather than full GTAO), noise rotated per frame. Applied inside the TAA resolve so the noise converges. Bright pixels (emissive, sunlit) keep their light. | `fx_ao`, `fx_ao_strength`, `fx_ao_radius` |
| Sky light | Golden hour warmth and blue moonlight, graded by sun elevation | `fx_sky_tint` |
| Ambient floor | A little light everywhere, so caves are dark but never pure black | `fx_ambient_floor` |
| Entity lighting | Mobs, players and items get the colored-light tint and a rim light | `fx_rim_light` |
| Moods | `client/fx_moods/*.conf` (vibrant, cozy, dark, neutral) plus your own in `<user dir>/fx_moods/`. Plain keys are applied as settings when selected. `cave.*`, `night.*` and `underwater.*` keys are targets blended in over about 1.5 s. Cave detection uses the sky light reaching the camera. | `fx_mood` |
| Mood tuner | Pause menu → Mood: sliders for every tunable `fx_*` value over the running world, plus "Save as mood". The same values are in Settings → Mood Renderer. | |

## What M4 adds

| Feature | How | Setting |
| --- | --- | --- |
| RGB light volume | A 64×48×64-node window around the camera. Node content IDs are snapshotted on the main thread (only when the block/mesh signature changes, or the camera moves 8 nodes). A worker thread runs the engine's light rule per RGB channel (−1 per step, opaque stops it) in 1–5 ms. The result is uploaded as a 2D atlas (Y slices, 8 per row) and sampled in the node vertex shader with manual trilinear filtering, half a node in front of each face. | `fx_light_volume` |
| Leak-free hue | World-light hue comes from the volume, so colour only goes where light can travel. The T1 light list still supplies flicker and the held light. Outside the volume, it falls back to T1. | |
| Stained glass tints light | Glass multiplies the light that passes through it by its texture's hue; saturation decides how strongly, so clear glass stays neutral | |

Texture slots: Irrlicht now has 8. Slot 6 holds the volume and slot 7 is free (reserved for PACK_FORMAT v2 material maps).

## What M3 adds

| Feature | How | Setting |
| --- | --- | --- |
| Scene copy | At the start of ClientMap's transparent pass, the opaque color and depth are blitted into a copy bound to texture slots 4 and 5. Irrlicht now has 6 slots instead of 4. | `fx_surfaces` |
| Glass | Glasslike drawtypes and nodes named `*glass*` get a glass shader variant and are drawn alpha-blended, so clear pixels still refract. Translucent texels tint (stained glass), opaque texels stay the lit frame. | `fx_refraction` |
| Glass patterns | `fluted` (vertical ridges), `frosted` (disk blur) and `rippled`, picked from the node name (`*fluted*`, `*frosted*`, `*ripple*`) or from `fx_materials.txt`. A texture's own luminance also bends light slightly. | |
| Water | Translucent non-glowing liquids: moving ripple normals, depth absorption (red goes first), scattering toward the water's lit color, caustics on the floor seen through it | `fx_water_absorption`, `fx_caustics` |
| Reflections | Screen-space ray march against the depth copy (28 steps plus 5 refine steps), Fresnel-weighted, falling back to a sky color from the fog color | `fx_ssr`, `fx_reflection` |
| Underwater | Post pass: absorption by distance, caustics on everything visible, and light shafts marched along the view ray through a soft caustic pattern. It replaces upstream's flat `post_effect_color` overlay. | `fx_underwater_shafts` |
| Debug views | 5 raw scene copy, 6 scene copy depth | `fx_debug_view` |

`fx_materials.txt` in a texture pack overrides the detection:

```
# <node> glass|water|none [pattern=fluted|frosted|rippled]
mcl_core:glass_light_blue  glass pattern=fluted
mcl_core:ice               glass pattern=frosted
```

Two more Irrlicht fixes came with M3. `blitRenderTarget` now flushes a target's lazy attachments first, because a never-bound blit destination was an empty, incomplete FBO. `IVideoDriver::getCurrentRenderTarget()` is now public.

The engine also gets an Irrlicht fix: `sampler2DArray` uniforms were never bound to their texture unit. Upstream only worked because the base texture uses unit 0.

## Recommended config

```
enable_post_processing = true
enable_bloom = true
enable_auto_exposure = true
fx_enable = true
```

## Light color table

Put `fx_light_colors.txt` in a texture pack. The jsl2mcl `fx` step generates it.

```
# <node> <#rrggbb> [flicker=<0..1>]
mcl_torches:torch      #ffb060 flicker=1
mcl_nether:glowstone   #ffd890
```

Nodes not listed get their color from their texture's glowing pixels.

## Tooling

`devenv shell` (or direnv with the bundled `.envrc`) gives you these commands. `fx-help` lists them.

| Command | What it does |
| --- | --- |
| `fx-build` | Incremental build: RelWithDebInfo, RUN_IN_PLACE, ccache. Configures on the first run. |
| `fx-run` | Runs with `util/fx/dev.conf` (hot reload on). In-game settings persist in `.devenv/luanti.conf`. Set `FX_TEXTURE_PACK=…` to load a pack. |
| `fx-play <world>` | Opens a world from `~/.minetest/worlds` directly. Your games and mods are found through `LUANTI_*_PATH`. |
| `fx-upstream` | Same as `fx-run` with `fx_enable = false`, for A/B |
| `fx-smoke` / `devenv test` | Headless smoke test on Mesa: every config and debug view must render without shader or GL errors |
| `fx-shots [dir]` | A/B screenshots of the test scene on your GPU |
| `fx-renderdoc` | Frame capture |

The scripts behind these are `util/fx/abshot.sh` and `util/fx/smoketest.sh`. The client saves screenshots itself (`fx_auto_screenshot = <path>`, after `fx_auto_screenshot_delay` seconds) and quits, so they work on X11, Wayland or headless.

`flake.nix` also works: `nix develop` gives a shell, `nix run .#client` runs the fork.

## Known gaps

- Inventory and wield item meshes have no emissive maps.
- Glass behind glass refracts the opaque scene only, so the nearer pane doesn't see the farther one.
- SSR runs before TAA's resolve but is not itself accumulated. Thin reflected details can still shimmer slightly.
- Underwater detection uses the node at the camera. Caustics and shafts also apply to anything seen above the surface while submerged.
- Entities still use T1 hue: the light volume is only bound to map materials.
- Beyond the volume window (about 32 nodes horizontally), hue falls back to T1.
- `fx_debug_view`, `fx_colored_light` and `fx_hdr_composite` are shader defines. Change them and either restart or save a shader file with hot reload on.
- The flake and devenv have not been run on NixOS yet. If `pkgs.luanti`'s recipe drifts from master's CMake options, adjust the flake's `cmakeFlags`.
- Cave detection reads the sky light at the camera, so a deep, roofed-over build counts as a cave.
- TAA does not reproject moving entities (no motion vectors). Variance clipping keeps ghosting short.
