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

## Round 5: sound travel (engine)

- **Around corners.** When a positional source is occluded, a distance field through open air (air, leaves, fences, edge-on doors; built on demand from the ear, 41³ window) gives its route. Walking that route back from the source, the first cell visible from the ear is where the sound turns the corner. The source is then rendered at a virtual position: in the direction of that turn, at the route's length, so distance attenuation follows the route. The bend costs up to about 10 dB and most of the highs. The position glides, and routing blends with the through-wall path.
- **Neighbouring spaces.** The probe labels every reachable cell as "this room" (route ≈ straight line) or "beyond an opening", and in one pass finds which opening each beyond-cell came through. The biggest space behind one opening is analysed from its middle. Its reverb runs on a second EAX slot (send 3) whose late tail is panned toward the opening. Coupling = aperture × size × nearness × contrast, so a ringing cave beyond a dead hut's door is what you notice.
- **Hollow floors.** Floor material, thickness and the empty space under it give `hollow`. Footsteps (any group named `*footstep*`) send into a short, low, boomy EAX "cavity" reverb (send 4).
- **Drafts** (afx_ambience): air cells in walls with sky on one side and shelter on the other whistle when the gust level is high. A pinhole whistles higher than a window. The whistle is the band-passed whistle of a real howling-wind recording.
- The context now asks for 6 aux sends: reverb, echo, wall EQ, next room, floor cavity, plus one spare.
- Probe cost is now 2–7 ms per probe on the sound thread, at 2.5 Hz.
- Scenarios `corner`, `nextdoor`, `bridge` and `draft`.

## Round 6: body and world details (afx_ambience)

- **Body** (`body.lua`, 5 Hz). Speed is measured from the actual change in position, so it also works in minecarts, boats, falls and when moved by teleport.
  - Rushing air in your ears above running speed, rising in level and pitch.
  - Bubbles each time your breath drops under water.
  - A rustle per stride through tall plants, louder when they reach your chest.
  - A squelch under each step on wet ground outdoors for two minutes after rain.
  - A held clock ticks, and a held shell plays the sea.
- **World** (`places.lua`):
  - Village bells (`*:bell` nodes, using the game's bell sound) ring 3 times at dawn and 6 at noon.
  - Drips fall where water sits above the rock, more often under lakes. A dry cave only drips rarely.
  - Below y=-20, rock groans now and then, or gravel trickles somewhere far off, heard through the stone.
  - **Wind chime** node (craftable: stick, 3 string, 3 iron nuggets; `afx_ambience:wind_chime`). It rings pentatonic notes from a real singing-bowl strike when the wind gusts, and when punched.
  - Hanging lanterns creak in gusts.
- Not done, because there are no usable recordings in the archives reachable here: rooster crows, the gasp on surfacing, cold breath.
- Scenario `details` (use `AFX_DAMAGE=1` for breath).

## Round 7: cinematic layer

- **Mood score** (engine, `afx_score.h`; settings `afx_score`, `afx_score_replaces_music`). A generative score from real samples: VoxeLibre's note-block piano and bell, and the singing bowl, with a soft hall baked in. It follows the client's mood:
  - day: C major pentatonic, phrases of 3–4 notes
  - home: lower and slower, with a bell now and then
  - night: A minor pentatonic, sparser
  - dread: no melody, only low bowl strikes a semitone or tritone apart
  - dawn: a rising phrase once a day

  It plays in sessions of 2–4 minutes with 3–7 minute rests. While it's on, the game's own music (long unnamed non-positional sounds) is suppressed.
- **Ducking and deafening** (engine). Sounds named like explosions, thunder, hurt, damage or death dip the ambience beds for about a second. A blast within 12 nodes muffles everything (HF cut, gain down, reverb down) for a few seconds and starts a fading ring in the ears.
- **Heartbeat** (afx_ambience). Lub-dub from a real kick drum, below 35 % health or with a hostile mob within 10 nodes in the dark. It speeds up from 60 to 96 bpm with severity.
- **Opt-in:** `afx_ambience_follower`: deep in the dark, now and then for 20 s, footsteps a beat behind yours that stop when you stop. `afx_ambience_treasure`: rare ore within 5 nodes glints faintly through the rock.
- Scenario `cinema`.

## Round 8: real muffling, coasts, mixer, room tone, sources carry their space

- **Muffling fix (engine).** Two leaks let the outdoors through walls and rock:
  - An occluded source still excited the listener's room reverb at 40 %.
  - OpenAL's per-source low-pass is only a high shelf above 5 kHz, so birdsong and rain stayed bright however low `gainhf` went.
  - Now the room only rings with what actually arrives. A dull path is split into a bright direct share, and the rest goes through the steep muffle EQ (`dullen` in afx_mix.cpp).
  - VoxeLibre forest check (`tour.sh … voxelibre afxplay_mode=muffle`):
    - outdoors -20 dB;
    - a sealed plank house -60 dB, faint and dull;
    - a sealed pocket 4 nodes under the forest floor: silent (before the fix, all three measured the same).
- **Coasts (mod).**
  - Mineclonia/VoxeLibre name biome layers `X_ocean`/`X_underground`, which made every cave and low valley a "coast". The suffix is now read as a layer.
  - Gulls and waves need a sheet of water at sea level within 22 nodes, near sea level, in daylight.
  - Torch light no longer counts as daylight anywhere in the mod (`get_natural_light`).
- **`/afx` mixer.**
  - Live sliders for the engine settings and the mod's layer volumes (`afx_ambience_vol_<layer>`), saved on close.
  - Toggles for the follower and the treasure shimmer.
  - `/afx_report` lists what the mod found in the game: mods, biome here, the node underfoot and its footstep, entities, node groups nearby, and the detected home, bell, lantern, furnace and ore nodes. It also writes `afx_report.txt` in the world.
- **Room tone (`afx_roomtone`, engine).**
  - One looping low noise source (`afx_roomtone`) is mostly sent into the room's own reverb. Its level follows enclosure × hardness × size, and its pitch drops with size.
  - Result: a hut hums faintly, a stone hall breathes, a cavern rumbles. It is silent under the sky.
  - The mod adds settling creaks in wooden rooms, more in the evening and in wind.
- **Sources carry their space (`afx_source_space`, engine).**
  - Positional sources more than 5 nodes away get a coarse look at their own surroundings (24 rays, cached, re-looked at when they move, 4 per recast).
  - The most prominent space that rings more than the listener's own plays in a sixth EFX slot (send 5), panned toward it. Sources in a similar space feed it.
  - Example: a zombie in the cave ahead rings with the cave while you stand in the meadow.
  - Sources under water are dull from above.
  - Scenario: `voices`.
- With `afx_debug`, the log shows each new sound's occlusion and mix, every loop's mix every 2 s, and the remote slot and room tone levels.

## Round 9: one truth for "where am I", HRTF, knocks, wind, music

- **Shared state.** The client sends what it hears to the server twice a second (`TOSERVER_AFX_STATE`, 0x70). The payload is a `key=value` string: enclosure, outdoor (the open path), wall, sky, wet, decay, mfp, absorption, natural (rock/earth share of the surfaces), hollow, underwater, tension, cozy, homey.
  - Lua reads it with `player:get_acoustics()`: a table, or nil for vanilla clients or a report older than 5 s.
  - afx_ambience derives `ctx.under_sky / exposure / through / underground / indoors` from it in `where()`, and falls back to light levels without it.
  - Outdoor beds stop when nothing from outside reaches the ear. Birds only spawn where they'd be heard.
  - A dark plank room is "indoors", not a cave (the `natural` share decides).
  - The packet numbers sit far from upstream's, and vanilla peers ignore unknown commands.
- **Weather back to the client** (`TOCLIENT_AFX_WORLD`, 0x70). `player:set_afx_world({wind_x, wind_z, wind, rain})` is sent only to afx clients. The mod sends its gust model and a slowly veering wind direction.
  - In the engine (`carryWeather`), sounds more than 8 nodes away fall into the wind's shadow upwind: up to -65 % direct, duller.
  - They carry further downwind and on still nights (less air absorption).
  - Rain dulls and masks the distance.
- **HRTF** (`afx_hrtf`, live, a mixer checkbox). This resets the OpenAL Soft device with `ALC_HRTF_SOFT`, keeping the 6 aux sends. The engine only switches HRTF off again if it turned it on itself, so OpenAL Soft's own headphone auto-detection is left alone.
- **Knock to find caves.**
  - Punching stone, earth or wood probes straight into the wall from the struck face. If air turns up within 5 nodes, the open space behind is measured (BFS, cap 240).
  - A hollow knock (`afx_knock_hollow`: real hits convolved with low cavity modes) plays at the face. Its level falls with wall thickness, and its pitch drops for bigger cavities.
  - Air that connects back to your own room (a pillar) barely counts. `afx_knock.h`, tested.
- **Music in the world.**
  - Note stones: 8 pentatonic notes, sing when stepped on, right-click for the next note, so you can build a stair that plays a scale.
  - A music box that always plays its own tune, seeded by its position (mesecons work too).
  - A soft rising chime the first time you find a new kind of place: a biome, a great cavern, the sky, under water, another dimension. It's remembered in player meta.
  - All positional, so caves ring them.
- **Scenarios:** `knock` (driven by xdotool: `AFX_XDO=...`), `music` (stairs, boxes, first-cavern chime, upwind/downwind voices).

## Round 10: reviews, walls, creatures, diving, listening tests

- **Two independent reviews (mod and engine) fixed:**
  - roof rain muffled by the plank beside it;
  - a bass-heavy muffle path, not attenuated by distance;
  - hollow floors 20 dB too loud;
  - grass rustle clipping;
  - knocks on tree trunks;
  - rain sounds inside rooms and walls;
  - fake gutters;
  - emitters stuck in place;
  - jungle floors counted as indoors;
  - stale per-world state;
  - a settings race;
  - an unclean afx off.
- **Skybridges and short tunnels:**
  - the open-land reverb tail now needs terrain beyond 3 nodes;
  - the outdoors through a small opening is the smaller of path loss and the opening's share of rays.
- **Walls you can hear** (EAX slot 1): see `applyWalls`.
- **Creatures:**
  - body-sized mob footsteps (`content_cao.cpp`);
  - animals startling (`creatures.lua`).
- **Diving:** `water.lua`, plus `water_depth` in the probe.
- **Per-place mix:** `place_target()` in init.lua.
- **Listening regression tests:** `python3 util/afx/listen_check.py [--only NAME] [--keep DIR]`.
  - It records the scenarios in `listen_bands.json` in the real client, each on its own display and port, so they can run in parallel.
  - It checks per-segment loudness bands, absolute or relative to another segment, and fails on Lua errors.
  - Run it before committing mix changes.

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
