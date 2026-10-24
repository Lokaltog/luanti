# fx fork: mood renderer (M1, Light & glow)

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

## Known gaps in M1

- Entities (mobs, players, dropped items) are not tinted yet. The object shader still uses white artificial light.
- Inventory and wield item meshes have no emissive maps.
- Hue can bleed through thin walls (T1 by design). The M4 RGB light volume fixes this.
- `fx_debug_view`, `fx_colored_light` and `fx_hdr_composite` are shader defines. Change them and either restart or save a shader file with hot reload on.
- The flake has not been built on NixOS yet. If `pkgs.luanti`'s recipe drifts from master's CMake options, adjust `cmakeFlags`.
