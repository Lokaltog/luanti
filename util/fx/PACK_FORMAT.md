# fx texture pack format

The contract between the fx renderer (this fork, `vfx` branch) and the texture packs that feed it, in particular the ones jsl2mcl generates (lokaltog/mineclonia-jsl). The renderer is client-only, so everything a pack can tell it lives in the texture pack directory. Node definitions on the server can't carry fx data: custom `_fields` never reach the client.

**Ownership.** This file belongs to the renderer, because the consumer defines the format. Producers (jsl2mcl) conform to it and test against it. A change lands here first, then in the producer.

**Versions.** v1 describes what the `vfx` branch reads today (commit `8febad8`). v2 is a proposal and is not implemented yet. A pack declares its version in `fx_pack.txt`. The renderer ignores that file in v1; it's for tooling and for v2 feature gating.

```
# fx_pack.txt
format = 1
producer = jsl2mcl 0.1.0
```

## Precedence (all versions)

For every property, the first source that exists wins:

1. **Authored data in the pack:** a companion map or a table entry. A producer that generates a map offline (jsl2mcl's auto-emissive, for example) writes it as authored data. The renderer can't tell the two apart, and doesn't need to.
2. **Runtime derivation** in the renderer: `[fx_autoemissive`, the light colour from the glowing pixels, surface kind from the drawtype and name.
3. **Upstream behaviour.**

Producers should only write what they know better than the runtime heuristic. Duplicating the heuristic offline freezes it, so later renderer improvements never reach that pack.

## v1: implemented

### Emissive companion: `<stem>_e.png`

| | |
|---|---|
| Name | For every top-level atom `foo.png` in a tile's texture string, the renderer looks for `foo_e.png` among the known source images (pack, then game). Atoms inside `[combine` and similar escaped modifiers are not rewritten. |
| Missing | The atom is blackened but keeps its alpha (`foo.png^[multiply:#000000`), so overlays still composite correctly. If no atom of a light-source node has a map, the whole base goes through `[fx_autoemissive:<fx_auto_emissive_threshold>`. |
| RGB | Emitted colour in sRGB, **premultiplied by strength**: black means no glow. The shader adds `pow(rgb, 2.2) * fx_emissive_strength` in linear light, so 255 is the full strength. |
| Alpha | Ignored by the shader, which uses the base texture's alpha. It is still used when the companion texture string is composited, so a map should be transparent where it doesn't glow. |
| Size and layout | Same layout as the base: the same vertical frame strip for animated tiles. A size mismatch is resized with `[resize` (nearest), which works, but a producer should match the size. |
| Scope | Map nodes only. Entities, wield meshes and inventory images ignore it (see the known gaps in README.md). |

### Light colours: `fx_light_colors.txt`

```
# <node name> <#rrggbb> [flicker=<0..4>]
mcl_torches:torch   #ffb060 flicker=1
```

- The key is an exact node name. Every texture dir is read, and the first dir wins for a key (the same order as texture lookups).
- Only the hue is used: the colour is normalised so its brightest channel is 255. Brightness always comes from the engine's scalar light.
- **A table entry replaces the whole style**, flicker included. An entry without `flicker=` gets flicker 0, not the name heuristic (torch/fire/flame/candle/campfire 1.0, lava 0.35). Producers must write the flicker they want.
- Nodes without an entry take their colour from the average `[fx_autoemissive` colour of their first non-empty tile. That average uses the base tile, **not** an authored `_e.png`.

### Surfaces: `fx_materials.txt`

```
# <node name> glass|water|none [pattern=fluted|frosted|rippled]
mcl_core:ice   glass pattern=frosted
```

- The key is an exact node name, and an entry replaces detection for that node. `none` opts a node out.
- Detection without an entry: translucent, non-glowing liquids are water. Glasslike drawtypes, and non-`normal` drawtypes named `*glass*` without light, are glass. The pattern comes from the name: `fluted`/`reeded`, `frosted` or `ripple`.

## v2: proposal

These are listed in the order they unblock the pack side. Each item says who changes what.

### 1. Texture-keyed and glob keys in both tables (renderer)

Producers know textures, not node names: in Mineclonia, node names are often built at runtime (`"mcl_core:"..name`), so a static scan can't recover them reliably. The proposal is to accept, in both tables:

- `@foo.png`: matches any node whose first non-empty tile's base atom is `foo.png`.
- `mcl_core:glass_*`: a glob over node names.

They resolve in this order: exact name, then glob (longest pattern wins), then `@texture`.

### 2. A missing `flicker=` means the heuristic (renderer)

Only an explicit `flicker=0` turns flicker off. Then a producer can pin a colour without having to restate the flicker heuristic.

### 3. Light colour from the authored companion (renderer)

When a node's first tile has a `_e.png`, average that map instead of `[fx_autoemissive` on the base. JSL's hand-painted emissive overlays (portals, eyeblossoms, sculk) then decide the light hue as well as the glow.

### 4. Tint-aware emissive (renderer, shader)

Multiply the emissive companion by the hardware/vertex colour (`paramtype2 = "color"` palettes and `color =` tints) wherever the base is multiplied by it. For example, if a wire's power level is encoded in its palette, its glow then follows that power level. Packs need no change.

### 5. Material maps in LabPBR layout (renderer + producer)

[LabPBR](https://shaderlabs.org/wiki/LabPBR_Material_Standard) is the de facto standard among Minecraft PBR packs, so existing PBR packs could be layered under any base pack.

| File | R | G | B | A |
|---|---|---|---|---|
| `<stem>_n.png` | normal X | normal Y (+Y up, OpenGL) | AO (255 = none) | height (255 = top, 0 = deepest) |
| `<stem>_s.png` | perceptual smoothness | F0 (0–229 = linear F0 ×255; 230–255 = metals) | porosity / SSS (0–64 / 65–255) | emission (0–254; **255 = none**) |

- `_e.png` stays as the explicit-colour override of the `_s` emission channel. Emission from `_s` is `albedo × A/254`.
- **Texture slots.** All six are in use: 0 base, 1 crack, 2 emissive, 3 shadow map, 4 and 5 scene copy. Two options:
  - (a) Raise `MATERIAL_MAX_TEXTURES` to 8 and bind `_n` at 6 and `_s` at 7. GL guarantees at least 16 units. This is the simplest option.
  - (b) Append `_n` and `_s` as extra layers in the companion array: frame `i` → layers `3i`, `3i+1`, `3i+2`. This needs no new slot but is a larger change to the array build.

  Recommendation: (a).
- **Smoothness drives glass.** Frost becomes blur radius = f(1 − smoothness), and SSR strength comes from F0 and smoothness. The `pattern=frosted` name match is then just a default for packs without `_s`. `fluted`/`rippled` remain patterns, since they are normal-map shapes; a pack can also ship them as `_n`.
- **Parallax.** Texel-stepped parallax only: march in whole texels of the base texture, so 16–32 px pixel art doesn't smear. Height comes from `_n` alpha, and depth is a setting.

### 6. Bevel along the edge of connected glass (renderer, mesher)

A bevel along the line between frame and pane can't be baked into a texture for `glasslike_framed(_optional)`. The pane quad is per node, and the frame is only drawn on unconnected edges, so a baked bevel would show at every connected seam. The proposal is for the glass mesher to emit, per vertex, the distance to the nearest *drawn* frame edge in each face axis. The glass shader then builds the rounded normal from that distance (`fx_glass_bevel` = width in texels, and a profile). For `glasslike`, where the frame is painted into one texture, the producer may bake the bevel into `_n`.

### 7. Producer-side items (jsl2mcl; no renderer change)

- `_n`/`_s` derived per material: smoothness and F0 from material tags (metal blocks, glass, ice, polished stone), per-pixel ore specks, and height by colour clustering (cobble stones above mortar, plank grooves, brick mortar).
- Baked bevel for `glasslike` single-texture glass.
- An `fx` profile pack that removes the highlight streaks painted into JSL glass, since they double up with SSR and refraction. The default pack keeps them for the upstream renderer.

## Conformance

A producer's test suite checks its output against v1:

- Every `<stem>_e.png` has a base texture `<stem>.png` in the same pack, with the same size.
- `_e.png` RGB is black wherever alpha is 0. There are no RGB-plus-strength-in-alpha encodings.
- Every table line parses with the grammar above. Colours are `#rrggbb`. Every light entry states `flicker=` explicitly (v1 semantics).
- `fx_pack.txt` is present with `format = 1`.

Renderer side (proposed): `fx-smoke` loads `util/fx/testpack/`, a minimal pack with one entry of each kind, and checks with `fx_debug_view = 2` that the authored map is used.

## Open questions

1. Should `@texture` keys match any tile or only the first non-empty one? (The proposal says the first, to stay consistent with how light colours are derived.)
2. For v2 slots: is option (a) acceptable given the Irrlicht changes already in the patch queue?
3. Should `_s` emission replace `_e` when both exist, or should `_e` win? (The proposal says `_e` wins, because it's more specific.)
