# fx texture pack format

The contract between the fx renderer (this fork, `vfx` branch) and the texture
packs that feed it, in particular the ones jsl2mcl generates
(lokaltog/mineclonia-jsl). The renderer is client-only, so everything a pack can
tell it lives in the texture pack directory. Node definitions on the server
can't carry fx data: custom `_fields` never reach the client.

**Ownership.** This file belongs to the renderer, because the consumer defines
the format. Producers (jsl2mcl) conform to it and test against it. A change
lands here first, then in the producer.

**Status.** Format 1, format 2 items 1–4 and the renderer side of item 5
(glass bevel) are implemented. Items 6–7 are open.

## `fx_pack.txt`

`key = value` lines; `#` comments and blank lines are ignored.

```
# fx_pack.txt
format = 2
producer = jsl2mcl 0.2.0
```

| Key | Meaning |
|---|---|
| `format` | 1 (default when the file or key is missing) or 2. Gates behaviour that changed meaning between versions: today only the flicker default (item 2). An unsupported value warns and is clamped. |
| `producer` | Free text, for humans. |

Other keys are ignored (logged at info level). The format applies to the tables
in the same directory.

## Precedence

For every property, the first source that exists wins:

1. **Authored data in the pack:** a companion map or a table entry. A producer
   that generates a map offline writes it as authored data. The renderer can't
   tell the two apart, and doesn't need to.
2. **Runtime derivation** in the renderer: `[fx_autoemissive`, the light colour
   from the glowing pixels, the surface kind from drawtype and name.
3. **Upstream behaviour.**

Producers should only write what they know better than the runtime heuristic.
Duplicating the heuristic offline freezes it, so later renderer improvements
never reach that pack.

## Emissive companion: `<stem>_e.png`

| | |
|---|---|
| Name | For every top-level atom `foo.png` in a tile's texture string, the renderer looks for `foo_e.png` among the known source images (pack, then game). Top-level means at the start of the string or right after `^` or `(`, so atoms inside parentheses count; atoms inside `[combine` and similar escaped modifiers are not rewritten. |
| Missing | The atom is blackened but keeps its alpha (`foo.png^[multiply:#000000`), so overlays still composite. If no atom of a tile has a map **and the tile is used only by light sources** (no node with `light_source = 0` uses the same tile string), the base goes through `[fx_autoemissive:<fx_auto_emissive_threshold>`. So a lit furnace's top, shared with the unlit furnace, and a jack o'lantern's sides, shared with the pumpkin, never glow. The comparison is per tile string, the unit the runtime works on; an authored `_e.png` applies per image file, to every node whose tile string has that atom, so a producer that generates maps offline must check per file (an unlit node may use the same file with a modifier). |
| RGB | Emitted colour in sRGB, **premultiplied by strength**: black means no glow. The shader adds `pow(rgb, 2.2) * fx_emissive_strength` in linear light. |
| Alpha | Ignored by the shader, which uses the base texture's alpha. It is still used when the companion texture string is composited, so a map should be transparent where it doesn't glow. |
| Size and layout | Same layout as the base: the same vertical frame strip for animated tiles. **Animated tiles: the size must match.** Their companion is cut with `^[verticalframe:N:i` and is not resized. For static tiles a mismatch falls back to `[resize`, whose filtering is unspecified. |
| Scope | Map nodes, and entities with an authored map. Wield meshes and inventory images ignore it. |

`[fx_autoemissive` keeps pixels whose brightest channel is above the
threshold. It can't tell a bright grey stone pixel from a white flame, so a
light source whose unique tile has bright non-glowing parts (the stone rim of
a lit furnace's front) glows there too. Ship an `_e.png` for those.

## Tables: `fx_light_colors.txt`, `fx_materials.txt`

```
# fx_light_colors.txt: <key> <#rrggbb> [flicker=<0..4>]
mcl_torches:torch        #ffb060 flicker=1
mcl_lanterns:lantern_*   #ffc080
@default_lava.png        #ff6020 flicker=0.35

# fx_materials.txt: <key> glass|water|none [pattern=fluted|frosted|rippled]
mcl_core:ice             glass pattern=frosted
mcl_core:glass_*         glass
```

### Keys

- **Exact node name.**
- **Glob** over node names: `*` matches any run of characters and `?` exactly
  one. No character classes. The pattern must match the whole name.
- **`@foo.png`** matches a node whose **base atom** is `foo.png`: the first atom
  of its first non-empty tile (`tiledef[0]` unless empty), modifiers stripped.
  Only the first non-empty tile counts, so a node can't get two answers.

A node resolves to: exact name, then glob (the longest pattern wins), then
`@texture`. Every texture dir is read; the first dir wins for a key (the same
order as texture lookups).

### Light colours

- Only the hue is used: the colour is normalised so its brightest channel is
  255. Brightness always comes from the engine's scalar light.
- `flicker` is clamped to 0..4 and scaled by `fx_flicker`; the product is
  capped at 0.99.
- **Format 1:** an entry replaces the whole style; an entry without `flicker=`
  gets flicker 0.
- **Format 2:** an entry without `flicker=` keeps the name heuristic
  (torch/fire/flame/candle 1.0, lava 0.35). Only an explicit `flicker=0` turns
  it off.
- Keys only resolve for nodes with `light_source > 0`.
- **Without an entry** the colour is the average glowing colour of the node's
  first tile that is unique to light sources (see the companion's "Missing"
  row), else of its first non-empty tile. When that tile has an authored
  `_e.png`, the map is averaged (it is premultiplied, so black texels drop
  out); otherwise `[fx_autoemissive` of the base.

### Surfaces

- An entry replaces detection for that node. `none` opts a node out.
- `glass` and `water` work on any drawtype. A surface on a non-liquid node is
  drawn in the transparent pass, after the opaque scene copy, writing depth.
- Detection without an entry: translucent, non-glowing liquids (a
  `liquidtype` other than `none`, liquid drawtype) are water. Translucent,
  non-glowing nodes with a liquid drawtype that are not liquids (Mineclonia's
  ice) are glass. Glasslike drawtypes, and non-`normal` drawtypes named
  `*glass*` without light, are glass. The pattern comes from the name: `fluted`/`reeded`,
  `frosted` or `ripple`.

### Diagnostics

Malformed lines and unknown options warn with `file:line`. After the node
definitions load, every key that matched no node warns too (light keys count
only light sources; surface keys only when surfaces are on).

## Material maps: LabPBR 1.3

| File | R | G | B | A |
|---|---|---|---|---|
| `<stem>_n.png` | normal X, 0 = left … 255 = right | normal Y, **DirectX**: 0 = up … 255 = down (texture space, v down) | AO (255 = none); Z is reconstructed | height, linear: 255 = surface, 0 = 25% depth |
| `<stem>_s.png` | perceptual smoothness | F0: 0–229 = linear F0 ×255; 230–254 = predefined metals; 255 = metal with albedo as F0 | porosity / SSS (unused) | emission 0–254 (0–100%); **255 = none** |

- Looked up for the base atom (the first atom at the start of the tile string)
  and resized to the base size. Missing maps get neutral fills.
- Every tile slot gets maps: base, overlay and special tiles alike. Only
  tiles that go into array textures get them, though. The maps of a size bunch
  form one interleaved array (layer 2i = `_n`, 2i+1 = `_s`), so with materials
  on, a bunch holds at most half of `array_texture_max` tiles.
- The normal is in texture space. The renderer builds the tangent frame per
  pixel from screen-space derivatives, so tile rotation and world alignment
  are handled; **producers must not pre-rotate normals**.
- Emission from `_s` is `albedo × A/254`. Where a tile has both an `_e.png`
  and `_s` emission, the renderer takes the brighter of the two per texel
  (it can't tell an authored black `_e` texel from "no map"), so a producer
  that ships `_e.png` should leave `_s` alpha at 255.
- Parallax is texel-stepped (whole texels of the base), on `_n` alpha, depth
  set by `fx_parallax_depth`.

## Open items (accepted)

5. **Bevel along the edge of connected glass** (renderer: done). The mesher
   puts a 4-bit frame-edge mask per pane quad in bits 12..15 of
   `S3DVertex::Aux` (the texture layer index keeps bits 0..11, so array
   textures are capped at 4096 layers with fx on); the glass shader bends its
   normal into a quarter-round bevel where a frame is drawn
   (`fx_glass_bevel`, in 1/16 node). Single-texture `glasslike` glass has no
   frame edges: producers bake its bevel into `_n`.
6. **Tint-aware emissive** (deferred). Tile and palette colour are baked into
   the vertex colour together with light, so this needs a vertex format change.
   Redstone power is the only known case.
7. **Producer-side** (jsl2mcl): `_n`/`_s` per material class; baked bevel for
   single-texture glass; an `fx` profile without the painted glass highlight
   streaks.

## Conformance

A producer's test suite checks its output:

- Every `<stem>_e.png` has a base `<stem>.png` in the pack or the target game,
  with the same size (mandatory for animated bases).
- `_e.png` RGB is black wherever alpha is 0.
- Every table line parses with the grammar above; colours are `#rrggbb`.
- `fx_pack.txt` is present and its `format` matches the semantics the pack was
  written for. At format 1, every light entry states `flicker=`.
- `_n`/`_s` are the same size as their base; normal Y is DirectX.
- Every table key matches a node. The fork's `fx_nodedump` mod
  (`util/fx/mods/fx_nodedump`) writes the client-visible node definitions of a
  game (names, tiles, drawtype, `light_source`, groups) as JSON, so a producer
  can resolve keys against the real definitions instead of parsing Lua.

**Fixture:** `util/fx/testpack/` is a hand-written format 2 pack with one input
of each kind. The `testpack_emissive`, `testpack_render` and `materials` cases
of `util/fx/test/run.py` render it.
