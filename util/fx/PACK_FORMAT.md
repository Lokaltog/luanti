# fx texture pack format

The contract between the fx renderer (this fork, `vfx` branch) and the texture packs that feed it, in particular the ones jsl2mcl generates (lokaltog/mineclonia-jsl). The renderer is client-only, so everything a pack can tell it lives in the texture pack directory. Node definitions on the server can't carry fx data: custom `_fields` never reach the client.

**Ownership.** This file belongs to the renderer, because the consumer defines the format. Producers (jsl2mcl) conform to it and test against it. A change lands here first, then in the producer.

**Versions.** v1 describes what the `vfx` branch reads as of `7a0d8b8`. v2 has been reviewed and its items are accepted unless marked otherwise, but none of it is implemented yet. A pack declares its version in `fx_pack.txt`: `key = value` lines, with `#` comments and blank lines ignored. The renderer ignores the file in v1. In v2 it gates behaviour changes: the flicker default, for example, follows `format`.

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
| Size and layout | Same layout as the base: the same vertical frame strip for animated tiles. **Animated tiles: the size must match.** Their companion is cut with `^[verticalframe:N:i` and is not resized, so a mismatch picks the wrong frames. For static tiles a mismatch falls back to `[resize`, whose filtering is unspecified, so producers should match the size there too. |
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
- `glass` and `water` work on any drawtype. A surface on a non-liquid node is alpha-blended like glass, so it gets the scene copy (fixed in `7a0d8b8`).
- Detection without an entry: translucent, non-glowing liquids are water. Glasslike drawtypes, and non-`normal` drawtypes named `*glass*` without light, are glass. The pattern comes from the name: `fluted`/`reeded`, `frosted` or `ripple`.

## v2: accepted, not yet implemented

These are listed in implementation order. Each item says who changes what. Items 1–3 are small and come first on the renderer side.

### 1. Texture-keyed and glob keys in both tables (renderer)

Producers know textures, not node names: in Mineclonia, node names are often built at runtime (`"mcl_core:"..name`), so a static scan can't recover them reliably. Both tables accept:

- **`@foo.png`** matches a node whose **base atom** is `foo.png`. The base atom is the first top-level atom of the node's first non-empty tile (`tiledef[0]` unless it's empty), with modifiers stripped. Liquids carry their texture in `tiledef[0]` too. Only the first non-empty tile counts, in both tables: matching any tile could give one node two answers.
- **Globs** over node names: `*` matches any run of characters and `?` exactly one. There are no character classes. The pattern must match the whole name, e.g. `mcl_core:glass_*`.

They resolve in this order: exact name, then glob (the longest pattern wins), then `@texture`.

### 2. A missing `flicker=` means the heuristic (renderer; `format = 2`)

Only an explicit `flicker=0` turns flicker off. Then a producer can pin a colour without restating the flicker heuristic. This changes the meaning of existing tables, so it applies only when the pack's `fx_pack.txt` says `format = 2`. Packs at v1, or without the file, keep v1 semantics.

### 3. Light colour from the authored companion (renderer)

When a node's first tile has an `_e.png`, average that map instead of running `[fx_autoemissive` on the base. The map is premultiplied, so black texels drop out after chroma normalisation. JSL's hand-painted emissive overlays (portals, eyeblossoms, sculk) then decide the light hue as well as the glow.

### 4. Material maps in LabPBR layout (renderer + producer)

[LabPBR](https://shaderlabs.org/wiki/LabPBR_Materials) is the de facto standard among Minecraft PBR packs, so existing PBR packs could be layered under any base pack.

| File | R | G | B | A |
|---|---|---|---|---|
| `<stem>_n.png` | normal X, 0 = left … 255 = right | normal Y in **DirectX convention**: 0 = up … 255 = down (texture space, v down) | AO (255 = none); Z is reconstructed | height, linear: 255 = surface, 0 = 25% depth |
| `<stem>_s.png` | perceptual smoothness | F0: 0–229 = linear F0 ×255; 230–254 = predefined metals; 255 = metal with albedo as F0 | porosity 0–64 / SSS 65–255 | emission 0–254 (0–100%); **255 = none** |

This follows LabPBR v1.3, checked against the [LabPBR page](https://shaderlabs.org/wiki/LabPBR_Materials). It says explicitly: "The normal vector should be encoded in DirectX format (Y-)." The normal is in texture space. The renderer applies tile rotation, so **producers must not pre-rotate normals**.

- `_e.png` stays as the explicit-colour override of the `_s` emission channel, and wins when both exist. Emission from `_s` is `albedo × A/254`.
- **Texture slots: raise `MATERIAL_MAX_TEXTURES` to 8.** Bind `_n` at 6 and `_s` at 7. All six slots are in use today (0 base, 1 crack, 2 emissive, 3 shadow map, 4 and 5 scene copy). The limit is already patched from 4 to 6 on `vfx`, and both the GL3 and GLES2 drivers `static_assert` support for up to 8.
- **Tangent frame (renderer).** Map nodes carry no tangents, and tiles can be rotated (`facedir`, world-aligned), so world-axis tangents would be wrong. The fragment shader builds a cotangent frame from `dFdx`/`dFdy` of position and UV (Schüler 2013). No mesher change is needed.
- **Smoothness drives glass.** Frost becomes blur radius = f(1 − smoothness), and SSR strength comes from F0 and smoothness. The `pattern=frosted` name match is then just a default for packs without `_s`. `fluted`/`rippled` remain patterns, since they are normal-map shapes; a pack can also ship them as `_n`.
- **Parallax.** Texel-stepped parallax only: march in whole texels of the base texture, so 16–32 px pixel art doesn't smear. Height comes from `_n` alpha, and depth is a setting.

### 5. Bevel along the edge of connected glass (renderer, mesher)

A bevel along the line between frame and pane can't be baked into a texture for `glasslike_framed(_optional)`. The pane quad is per node, and the frame is only drawn on unconnected edges, so a baked bevel would show at every connected seam.

The mesher writes a **4-bit frame-edge mask** per pane quad: which of its four edges have a frame drawn. It fits in the spare bits of `S3DVertex::Aux`, so the vertex format doesn't change. Pane quads are exactly one node per face, so the shader computes the distance to each edge from `fract(worldPosition)`. It then builds the rounded normal where the mask says there's a frame (`fx_glass_bevel` = width in texels, plus a profile). For `glasslike`, where the frame is painted into one texture, the producer may bake the bevel into `_n`.

### 6. Tint-aware emissive (renderer, mesher; deferred)

This would scale the emissive companion by the node's palette or `color` tint, for example a wire's power level. It costs more than it looks. Tile and palette colour are baked into the vertex colour *together with* the light value (`applyTileColor()`), so the shader can't separate tint from light. Keeping the tint separately needs a vertex format change, since `Aux` has only about 5 spare bits, touching every node buffer. It's deferred until a concrete pack needs it; redstone power is the only known case.

### 7. Producer-side items (jsl2mcl; no renderer change)

- `_n`/`_s` derived per material: smoothness and F0 from material tags (metal blocks, glass, ice, polished stone), per-pixel ore specks, and height by colour clustering (cobble stones above mortar, plank grooves, brick mortar).
- Baked bevel for `glasslike` single-texture glass.
- An `fx` profile pack that removes the highlight streaks painted into JSL glass, since they double up with SSR and refraction. The default pack keeps them for the upstream renderer.

## Conformance

A producer's test suite checks its output:

- Every `<stem>_e.png` has a base texture `<stem>.png` **in the pack or in the target game**, with the same size. A pack may ship a companion for a game texture it doesn't override. For animated bases, the same size is mandatory (see the v1 size row).
- `_e.png` RGB is black wherever alpha is 0. There are no RGB-plus-strength-in-alpha encodings.
- Every table line parses with the grammar above. Colours are `#rrggbb`. At `format = 1`, every light entry states `flicker=` explicitly.
- `fx_pack.txt` is present and its `format` matches the semantics the pack was written for.
- v2 material maps: `_n`/`_s` are the same size as their base; normal Y follows the DirectX convention.

**Conformance fixture:** `util/fx/testpack/` (from `7a0d8b8`) is a hand-written v1 pack with one input of each kind. The `testpack_emissive` and `testpack_render` cases in `util/fx/smoketest.sh` (`fx-smoke`) render it: the authored magenta companion must glow, the table must turn a lamp purple, and clear glass must become rippled. Producers can also run their checker against this pack.

## Resolved in review

1. `@texture` keys match **only the first non-empty tile**, in both tables.
2. Texture slots: **raise to 8** (option a).
3. When both `_e.png` and `_s` emission exist, **`_e` wins**, because it's more specific.
4. Normal map Y: **DirectX convention**, per LabPBR v1.3.
5. Tint-aware emissive: **deferred** (v2 item 6).
