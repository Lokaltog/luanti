# afx fork: environmental audio

A patch queue on top of the fx branch. Client-only, like fx: no map, protocol or node-definition changes. Every effect sits behind an `afx_*` setting (Settings → Audio → Environmental Audio).

## How it works

```
main thread, every afx_probe_interval (0.4 s)            sound thread (~60 Hz)
───────────────────────────────────────────              ─────────────────────────────────────────
ClientMap ─► VoxelSnapshot (64³ around the ear,           analyze(): rays + Sabine + echo cluster
             1 byte/voxel: material, sky bit)  ──msg──►              + cheapest path to the sky
             ~0.8 ms, block-wise copy                     Smoother: asymmetric glides (60 Hz)
                                                          occlusion rays per positional source (7 Hz)
F5 debug line ◄──────────── Report (4 Hz) ◄──msg──        EfxRig: EAX reverb slot + echo slot,
                                                            per-source low-pass + sends (30 Hz)
```

The probe is cheap (~0.3 ms). Probing at 2.5 Hz is enough because every parameter glides, so nothing steps audibly.

| Piece | Where | What |
| --- | --- | --- |
| Materials | `client/afx/afx.cpp` `classifyNode` | Node groups, drawtype and name give one of: air, leaky (fences, doors, panes), foliage, soft (wool), snow, glass, wood, earth, stone, water, lava. Handles both Mineclonia groups (`pickaxey`, `axey`) and Minetest Game groups (`cracky`, `choppy`). |
| Sky | snapshot builder | Sunlit air with nothing that closes the column above. Leaves and fences don't close it, so a forest floor is outdoors. A glass roof does close it, so a greenhouse is indoors. |
| Room | `afx_probe.cpp` | 64 Fibonacci rays. Sabine RT60 = 0.161·V/(S·ā), with V/S from the mean hit distance and escaped rays counted as fully absorbing. Gives enclosure, wet level (dead rooms hold less energy), HF decay ratio, early and late delays, and a reflections pan toward the nearest big reflector. |
| Echo | `afx_probe.cpp` | A coherent cluster of far (≥10 nodes), hard hits. Delay is 2d/343. Parallel walls behind you add flutter feedback. |
| Outdoors | `afx_probe.cpp` | Bucket-queue Dijkstra from the ear to the nearest sky voxel. Solids can be crossed at a per-material cost: wood 16, stone 30 (air-node equivalents). The cost sets gain and HF; the path five steps out sets the direction it comes from (the doorway). |
| Occlusion | `afx_probe.cpp` | Five rays to each positional source: direct plus four offset by a node. Averaging the two clearest gives cheap diffraction around pillars and door frames. |
| Buses | `afx_mix.h` | **world** (positional: occlusion, air absorption, reverb, echo), **local** (footsteps, digging: reverb, echo), **outdoor** (muffled and steered toward the opening, stereo angles or source radius), **indoor** (room tone), **dry** (UI, music). |
| EFX | `client/sound/efx.cpp` | All levels go through filters and send gains, never `AL_GAIN`, so the engine's fades keep working. The context asks for 4 aux sends. |

### Routing a sound to a bus

- Positional sounds are always on **world**.
- Non-positional sounds go by name. The prefixes `afx_out_`, `afx_in_` and `afx_dry_` always win. After that come the glob lists `afx_outdoor_sounds` (default `*rain*,*wind*,*weather*,*thunder*`), `afx_indoor_sounds` and `afx_dry_sounds`. Anything else is **local**.

This means Mineclonia's existing rain gets muffled indoors with no game changes.

## Settings

| Setting | Default | |
| --- | --- | --- |
| `afx_enable` | true | master switch, live |
| `afx_probe_interval` | 0.4 | seconds |
| `afx_probe_radius` | 32 | snapshot is (2r)³ |
| `afx_rays` | 64 | |
| `afx_reverb` / `afx_echo` / `afx_occlusion` / `afx_outdoor_muffle` | 1.0 | amounts |
| `afx_max_decay` | 8.0 | cap for huge caverns |
| `afx_smoothing` | 1.0 | multiplies every glide time |
| `afx_debug` | false | F5 line, and logs it every 2 s |

## Round 2: muffling, doors, echo, content

- **Two outdoor paths.** The sky search runs on two layers. The *open* path goes only through porous voxels (air, leaves, fences, water) and stays airy, with a mild low-pass. The *wall* path crosses solids and is sent to an EFX equalizer slot that acts as a steep "behind a wall" low-pass: about -10 dB at 700 Hz and -36 dB above 4 kHz. Occluded positional sources use the same muffled path for whatever gets through the blocker. Gains are `exp(-cost/6)`. A plank wall lets about -26 dB through, stone about -58 dB.
- **Thin nodes.** Collision boxes, rotated by param2, are sampled per (content, param2). A node that covers only one axis is stored as "thin" (cell bits 6-7) and blocks only travel along that axis. A shut door seals the doorway, while the same door swung open, or a slab you're walking over, doesn't block.
- **Echo.** A single echo now rides on the EAX early reflections, which carry a delay and a direction (verified by the loopback test). The echo slot only adds flutter between parallel walls. Echo is scaled by (1 - enclosure)², so caverns give reverb, not slap-back.
- **Content mod** `util/afx/mods/afx_ambience` (Mineclonia/VoxeLibre aware, works anywhere):
  - Rain on the roof above you, by material, and on nearby windows exposed to the sky.
  - Birds from real leaves at exponentially spaced intervals (`afx_ambience_bird_interval`, mean 25 s, owls at night).
  - Lava bubbling at lava surfaces.
  - Drips from cave ceilings.
  - Real recordings, built by `import_sounds.py`. Credits are in the mod.
  - `/afx_rain <0..1|off>` forces rain for testing.

## Round 3: mood, biomes, proximity

- **Mood** (`client/afx/afx_mood.h`). Two slow scalars computed on the client:
  - **tension**: rises with darkness (weighted by how shut-in you are), being underground (no path to the sky), large ringing caverns, and night outdoors.
  - **cozy**: signs of home within 8 nodes (beds, chests, furnaces, lamps) combined with light.
  - **Timing**: dread creeps in over about 20 s, relief arrives in about 3 s.
  - **Layers**: the engine plays the `afx_mood_dread` drone and the `afx_mood_cozy` hearth layers itself, whenever a mod provides those sounds.
  - **Stingers**: when deep in dread, an occasional `afx_mood_stinger` plays far off in the dark (positional, so it gets occluded and reverberated).
  - **Relief**: `afx_mood_relief` (chimes) plays when you come back out under the sky after real dread.
  - `afx::mood()` exposes both scalars to other client code, so the fx renderer can key grading or exposure on the same numbers.
  - Settings: `afx_mood` (level) and `afx_doppler` (OpenAL Doppler, default 0.5).
- **Biome beds** (afx_ambience). Wind, wind in trees, distant birds, crickets at night, frogs, jungle and waves. They are mixed from biome names plus local evidence (leaves, open water, flowers) and fade with time of day. Rain silences the birds. Height above y=40 adds howling wind, so floating islands howl.
- **Proximity sources**: rivers (flowing water), waterfalls (flow over a drop), insects over flowers by day, bees at nests, crackle at fires and lit furnaces. Under water you get an underwater bed, and the engine routes everything else through the wall EQ.
- Scenarios `descent`, `meadow`, `beach` and `sky_island` in `tour.sh`.

## Round 4: weather, water, life, places

- **Sound takes time to travel** (engine). Positional one-shots farther than 34 nodes start after distance / 343 s, so thunder arrives after the flash and far explosions arrive late. Loops are exempt. Stop, fade and move requests for a sound still in flight are honoured.
- **Unnamed long sounds stay dry** (engine). A non-positional sound longer than 45 s that no rule names is treated as music, so the game's music doesn't reverberate in caves.
- **Weather** (`weather.lua`):
  - Gusts: wind rises over a few seconds, holds and dies away, on top of slow swells. Exposure (height above the surrounding ground) scales it, so ridges and peaks blow harder.
  - Rain starts with a few fat drops, then the surface layers swell in over 30 s.
  - Gutters run off the eaves while it rains, and the canopy keeps dripping for 90 s afterwards. Birds come back gradually.
  - Thunder: far strikes get a low-passed roll; near strikes get a crack, unless the game plays its own (VoxeLibre's lightning mod).
- **Water** (`water.lua`):
  - Waves lap at the actual edge between water and land.
  - Underground rivers are found within 22 nodes and heard through the rock.
  - Wading and falling into water make splashes.
- **Life** (`life.lua`):
  - A dawn chorus (birds four times as frequent, songbirds only, louder bed) and gulls on the coast by day.
  - Breaking leaves, or sometimes punching a trunk, flushes 2–4 birds with alarm calls and leaf rustle, and the tree goes quiet afterwards. Birds don't call from right above you.
  - Frogs croak from the far bank at night. As you reach the water's edge they plop in and the chorus stops for 25 s.
  - Villages: day and night beds when villagers are around.
- **Places** (`places.lua`):
  - Home (3+ signs of home, under a roof): a clock (item frame or `group:clock` node), a purring tamed cat, and lit furnaces humming.
  - Mineshafts (rails + timber): creaks. Dungeons (spawner or mossy stonework): chains.
  - The Nether and the End have their own beds, plus distant chimes in the End.
- **Game-aware.** The mod defers to the game's own water, lava, fire and thunder sounds where it has them, so nothing doubles. Node lists (furnaces, campfires, bee nests, rails, spawners) are built by name when the game loads, which also covers Mineclonia's naming.
- Scenarios `storm`, `shore`, `home` and `mineshaft`, plus `tour.sh <out> voxelibre`: a real-game tour of VoxeLibre. Point `games/voxelibre` at a VoxeLibre checkout. The tour visits forest, rain, plains at dusk, a cave, snow, a thunderstorm, the Nether and the End.

## Variety for short sounds

- **Engine** (`afx_variation`, default 1.0): every short (< 2.5 s) non-dry one-shot gets a pitch/speed factor of about ±4 % (hard limit ±7 %) and about ±1.5 dB of level, drawn from a triangle distribution so most changes are subtle. A group's samples are never picked twice in a row. This covers the engine's own footsteps, digging and placing, which mods can't reach.
- **Extra takes** (`make_variants.py <game_dir> <out>`): for each footstep, dig, dug and place group in a game, it writes three new takes into free `.N` slots (from .9 downward). Each take is one of the game's own samples with pitch and speed shifted independently, a ±2 dB high-shelf tilt and ±1 dB of level. Luanti merges `name.N.ogg` across mods, so the game draws from 5–7 takes instead of 2–4. The shipped takes are made from VoxeLibre. Rerun the script against your Mineclonia install to match its samples exactly.
- The home clock is quieter now.

## Testing

- `bin/luanti --run-unittests --test-module TestAfx` runs the probe on synthetic worlds: field, stone cave, wooden hut, tunnel mouth, cliff echo, occluders, water, plus the smoother.
- `--test-module TestAfxEfx` renders through a real OpenAL Soft loopback device and measures the output. It checks the reverb tail, muffling, and that echo and bed steering land on the correct side.
- `AFX_RENDER_DIR=/tmp/afx bin/luanti --run-unittests --test-module AfxRender` writes listening clips (placeholder sounds through the real pipeline).
- `USE_XVFB=1 util/afx/tour.sh /tmp/afx <scenario>` records a scripted scenario in the real client, with afx_ambience running, through OpenAL Soft's wave backend. Scenarios: `hut_rain`, `tunnel_wind`, `forest`, `cavern`, `cliff`. It writes .wav, .mp3 and the client log with acoustic state every 2 s. Add `afx_enable=false` for an A/B baseline.

Measured in the engine: snapshot 0.7–1.2 ms on the main thread, probe 0.0–0.3 ms on the sound thread.

## Known gaps / next

- Rain-on-roof by material, procedural wind, the tension scalar and the Lua content layer (birds in leaves, insects, lava, drips) are the next milestones.
- Stereo outdoor beds need `AL_EXT_STEREO_ANGLES`, and mono beds use `AL_EXT_SOURCE_RADIUS`. Both are in OpenAL Soft.
- Echo distance is capped at about 35 nodes (the EFX echo delay limit is 0.207 s).
- A Lua API for an explicit bus (`sound_play{bus=…}`) needs a protocol field and is deferred. Use the name prefixes for now.
