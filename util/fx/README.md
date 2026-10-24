# fx fork: mood renderer (M1 Light & glow, M3 Glass & water)

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
| Debug views | 1 light chroma, 2 emissive, 3 HDR luminance (false color, blue −4 EV, green 0, red +4 EV), 4 bloom input | `fx_debug_view` |
| Shader hot reload | Polls `*.glsl` modification times and recompiles in place, keeping material IDs stable. On a compile error the old program keeps running. | `fx_shader_hot_reload` |

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

- `nix develop` gives a dev shell. `nix run .#client` runs the fork.
- `util/fx/abshot.sh out.png fx_enable=false` takes a repeatable screenshot of the test scene in `util/fx/testworld`. Set `USE_XVFB=1` to render off-screen on Mesa.

## Known gaps

- Entities (mobs, players, dropped items) are not tinted yet. The object shader still uses white artificial light.
- Inventory and wield item meshes have no emissive maps.
- Glass behind glass refracts the opaque scene only, so the nearer pane doesn't see the farther one.
- SSR has no temporal accumulation yet (M2's TAA). Reflections of thin geometry can shimmer when moving.
- Underwater detection uses the node at the camera. Caustics and shafts also apply to anything seen above the surface while submerged.
- Hue can bleed through thin walls (T1 by design). The M4 RGB light volume fixes this.
- `fx_debug_view`, `fx_colored_light` and `fx_hdr_composite` are shader defines. Change them and either restart or save a shader file with hot reload on.
- The flake has not been built on NixOS yet. If `pkgs.luanti`'s recipe drifts from master's CMake options, adjust `cmakeFlags`.
