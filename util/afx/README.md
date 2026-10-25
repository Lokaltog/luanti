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
  - Punching stone (round 17: only stone; wood and earth are thin wherever you meet them) probes straight into the wall from the struck face. If air turns up within 5 nodes, the open space behind is measured (BFS, cap 240).
  - A hollow knock (`afx_knock_hollow`; round 17: a real wooden tok slowed and dulled, over the old cavity take pitched to 0.88 and cut short, so it thuds instead of gonging) plays at the face. Its level falls with wall thickness, and its pitch drops for bigger cavities.
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

## Round 11: accents

- **Real raindrops.**
  - The first drops of a shower are single drops cut from real rain recordings: soft ones on leaves and ground, taps on hard surfaces.
  - The after-rain canopy drip is rebuilt from the same drops.
  - The pitched-up wet squelch is gone.
- **Storms that approach.**
  - The mod sends `rain_front` (0..1) and its direction in `set_afx_world`.
  - The engine fades the game's rain bed in with it (any Outdoor sound with "rain" in its name), steering it from upwind while you're out in it.
  - After the rain, the mod's own tail recedes downwind over about 22 s.
- **Things land** (`landing.lua`):
  - Dropped items and arrows play the struck node's own footstep, short and light.
  - Planks over a drop add a small hollow ring.
  - Nodes without sounds fall back by group.
- **Windows rattle:** thunder and explosions (`mcl_explosions.explode` and `tnt.boom` are wrapped) set nearby panes buzzing as the sound arrives.
- **Rain on fire:** exposed lava and fire sizzle in rain (from the game's own extinguish hisses).
- **Air pressure:** opening a sealed room to the outdoors makes a faint low whoomp (engine, `setAcoustics`).
- **Scenario:** `accents`. The `walls` listening check uses quiet settings, and its band is the bed level.

## Round 12: second review pass, nature, terrain

- **Mixer:**
  - The numbers follow the sliders (the panel redraws on every change and keeps its scroll position).
  - It scrolls in a small-screen window.
  - Layers have short names with tooltips.
- **Second review, engine:**
  - **Gain staging.** Positional sources ran at 3× and saturated at `AL_MAX_GAIN` 1, so every source sent the same level into the effects. `AL_MAX_GAIN` is now 3 while afx runs. The direct path keeps the old near-field clamp (`min(1, 1/(3g))`), and the sends scale `min(0.667, 0.5/g)`.
  - **Walls:**
    - quieter, so they no longer comb-filter;
    - the probe uses horizontal distance and needs a cluster of rays that spreads sideways, so trunks, posts and ceilings no longer count;
    - no flutter in a 1-wide tunnel;
    - the send follows the source's own distance law (send-auto off, no extra 1/d).
  - **Whoomp:** only when the listener hasn't moved, never in the first seconds.
  - **Lava:** muffles fully again.
  - **Just under the surface:** the outdoors reaches you at 0.3/0.12 and dull, and so does the world bus.
  - **Remote and next-door slots:** fed less by far sources.
  - **Storm front:** under open sky it uses the front's direction alone, and "rain" must be a word in the name.
  - **Batching:** the 30 Hz source updates are batched with `AL_SOFT_deferred_updates`.
- **Second review, mod:**
  - **Weather:**
    - snow weather is only snow where it's cold, and rain isn't rain in snowy biomes;
    - rain keeps going across biome edges.
  - **Flood control:**
    - rattles are capped and merged;
    - item impacts have a budget and per-node dedupe.
  - **Diving sounds** are non-positional and quieter.
  - **First drops** try up to 4 spots.
  - **Creatures:** zombified piglins don't startle, and a non-string voice can no longer error.
  - **Rivers** don't double underground, and flowing water is scanned by name.
  - **Frogs** only by ponds in meadows, forests and swamps.
  - **Cleanup:** discovery state is cached, and per-player state is cleaned up on leave.
  - **Review tour:** the house goes where it actually rains.
- **New (nature.lua, kept in the background):**
  - gusts sweeping through the trees from upwind (an invisible moving emitter);
  - crickets around you at night that fall silent as you come near and start again behind you;
  - singing ice on frozen lakes, more at dusk and at night;
  - your clothes rustling and your breath settling after a run, where it's really quiet.
- **New (engine):**
  - **Heightfield:** a coarse map of the land, ±192 nodes at 4-node cells, refreshed 8 rows per probe and shipped with the snapshot.
  - **Over the hills:** far sounds bend over the top of a ridge, dull and faint, where the rock in between would block them.
  - **Thunder rolls:** it echoes back off up to 3 visible high grounds 48..190 nodes away, each as late as its detour.
  - **Cave mouths moan in gusts:** pitch from the cave's size and the opening.
  - **Fly-bys:** fast objects passing within 3 nodes whizz by, with Doppler.
- **Scenario:** `nature`.

## Round 13: gain staging, recorded spaces, sound pipeline

- **Gain staging by design:**
  - Every mod sound sits at its category's reference loudness: beds -30, accents -22, your own body -26, non-positional cues -24 LUFS (short sounds by their loudest 100 ms). `sound_pipeline.py normalize` measures and rewrites the files.
  - The mix is one table, `mods/afx_ambience/levels.lua`: each sound group's level in dB on top of its files. `normalize` wrote it so the mix stayed as it was. Tune levels there, not in the code; a new or replaced sample needs no gain hunting. Every sound the mod plays goes through it (`afx_ambience.play`, `gain_for`, emitters, loops, the heartbeat).
  - The engine asks OpenAL Soft for its output limiter (`ALC_OUTPUT_LIMITER_SOFT`), also after an HRTF reset, so whatever piles up is held under full scale.
  - The listening checks measure at the level the client played (float recording, `<scenario>.raw.npz`): every run must stay under the ceiling (no sample over full scale, no 3 s louder than -12 LUFS short-term), and each segment reports its LUFS. Bands can use `lufs` and `loudest`.
- **Recorded spaces (convolution reverb):**
  - OpenAL Soft's convolution reverb plays real impulse responses: caves from Skyline Caverns and Shenandoah Caverns (passage, pool alcove, tall chamber, big cavern), a Berlin railway tunnel, a warehouse hall, a small hard room and a furnished wooden room.
  - The probe picks one: rock or built, a corridor or not, hard or soft, then the one whose decay is closest. Its fit scales its share. Under the sky or under water there is none.
  - It blends with the synthetic reverb, which keeps following the room's exact shape (its reflections' pan, its decay) and gives up 70% of what the recording takes. The recording's level is matched to the energy the synthetic reverb would have, so trading one for the other keeps the level.
  - A new space has to stay the best fit for 1.5 s. Then the old one fades out (about a second, the synthetic reverb filling in), and the new one fades in. A slot can't change its buffer once it has played, so each space gets a fresh slot, and the old one is deleted once no source sends to it.
  - It needs a send. The space next door and the space a far source sits in now share one slot (send 5), set by whichever is louder; each source's send carries its own level.
  - Setting `afx_convolution` (0..1, default 0.6), also in `/afx`. Without the effect, or without the `afx_ir_*` sounds, it's the synthetic reverb alone.
- **Sound pipeline (`sound_pipeline.py`):**
  - `report`: loudness of every mod sound, by category.
  - `normalize [--dry-run]`: as above.
  - `ir <in.wav> <name>` (or `left.wav+right.wav` for two mono takes): from the onset, at most 2.9 s (it has to fit one buffer), trimmed where it's 60 dB down, tail faded, energy normalised. Name it `afx_ir_*`.
  - `import <pack_dir> <rules.json> [--dry-run]`: cuts a sound library (Sonniss GDC bundles, say) into mod sounds. Beds get the steadiest stretch made into a seamless stereo loop; accents and foley get distinct takes, picked by onset and by how much they differ from each other. Each goes to its category's loudness. See `import_rules.example.json`. Add a level to `levels.lua` for a new group (0 dB is its category's reference).
- **Tests:**
  - `testRecordedSpace`: the cave picks a cave, the plank hut the wooden room, a long mine tunnel the tunnel, and the open field and the water none.
  - `TestAfxEfx.testRecordedSpace`: through a loopback device, a response plays, and a swap fades over and drops the old one.
- **hut_rain:** "door open" measured quieter than "door shut" because the storm was still building (rain ramps over 30 s). The scenario now waits for it, and checks that opening the door again isn't quieter.

## Round 14: the client owns the ambience

The ambience (what plays and where) moved from the server mod into the client. It now works on any server, and the server only sends what a client can't know.

- **A Lua runtime of its own on the client** (`src/script/scripting_afx.*`, `src/client/afx/afx_script.*`):
  - a separate Lua state, always on with afx, not client-side modding: `enable_client_modding` and a server's CSM restrictions don't apply;
  - it runs only the engine's own scripts, `builtin/afx/` (`INIT = "afx"`);
  - Lua errors are logged, not fatal; after 20 the scripts stop.
- **Its API** (`src/script/lua_api/l_afx.*`) follows the server's names where the meaning is the same, so the scripts read like a mod:
  - the loaded map: `get_node(_or_nil)`, `find_nodes_in_area(_under_air)`, `get_node_light`, `get_natural_light`, `get_timeofday`, node definitions with groups and sounds;
  - sounds of its own: `sound_play` (pos, object id, loop, start_time, max_hear_distance), `sound_fade`, `sound_stop`, `afx_sound_move` for moving emitters; objects' sounds follow them;
  - the local player, the objects around (entity name, size, velocity), and what the engine hears (`afx_hearing`, the same keys it reports to the server);
  - `afx_set_world`: the weather the engine bends distant sounds with, now set by the scripts;
  - storage (`<user>/client/afx_storage.conf`), local chat commands, formspecs named `afx:*`, chat messages.
  - `builtin/afx/compat.lua` maps the server API the layers were written against: one connected player, objects with `get_luaentity()` from the server's creature table, callbacks.
- **The layers** (`builtin/afx/*.lua`): everything the mod did, ported. The mixer (`/afx`) and `/afx_report` are now the client's, so they work on any server. Discoveries are kept per server, world and player. Gusts are moving sounds rather than entities. Items landing and arrows sticking are detected from how objects move.
- **Sounds:** the ambience's sounds moved to `sounds/afx/` (installed to `share/sounds/afx`). The client finds them there whatever the server sends, so they're no longer media, and the engine's own effects (room tone, recorded spaces, whizz, knock) look there too. The extra takes of the game's footsteps stay in the mod, because they have to merge with the game's own groups.
- **The mix** moved to `builtin/afx/levels.lua`; `sound_pipeline.py` now works on `sounds/afx`.
- **The server mod `afx_ambience` is a bridge:**
  - every second, to each afx client: rain and snow where the player is, the wind (swell and gusts, its direction), the biome with heat and humidity, the dimension, the day;
  - once: the game (id, mods, sea level, world, damage, whether it plays its own water, lava and fire) and its creatures (type, voice, size);
  - events: lightning strikes, explosions, a gust rising;
  - content: the wind chime, note stones and the music box (their sounds are played by the client, with their levels);
  - test hooks: `set_rain`, `gust_now`, `wind_override`, `thunder_at`, `poke`, `client_chat`, `client_fields`.
  - Server API: `player:set_afx_world` takes words too, and `player:send_afx_data(channel, data)` carries the data channels.
- **Engine fix:** client-side node definitions now index their groups, so `group:` lookups work on the client.
- **Settings:** the ambience's settings are the engine's now (Environmental Audio → Ambience). `afx_debug_eval` runs a line of Lua in the ambience after `afx_debug_eval_at` seconds and logs the result, for headless tests.
- **Without the bridge** (another server), the ambience still runs on the map. It has no weather, the biome comes from the nodes around, creatures go by name, and it's always the overworld.

### Regression check against round 13

The pre-refactor build (8559225) and this one ran the same 26 scenarios side by side, under the same load: 23 devtest ones plus VoxeLibre's muffle, review and life tours. Per scenario the check compared the sounds each build started and each segment's loudness (`/tmp`-style scripts: run both, diff the inventories, LUFS per segment).

- Loudness matched within the method's noise. Log timestamps are whole seconds, so short segments swing by up to ±5 dB, in both directions, from run to run.
- Event inventories matched, except for random one-shots (which bird, a gust).
- It found three real differences, now fixed:
  - **Digs and punches made on the server** (a scene, a mod, `node_dig`) didn't reach the client, so leaves broken that way flushed no birds. The bridge now forwards them (`@node`), and the client keeps one of each when it also saw its own.
  - **The biome wasn't sent with `afx_ambience_use_biomes = false`**, so there were no discovery chimes for biomes. It's always sent now; the setting only affects the beds, as before.
  - **Discoveries were kept per world name.** Two worlds called "world" shared them, so tests didn't chime twice. The key is now name plus seed, and `tour.sh` clears the ambience's storage before each run.

## Round 15: the tuning view

`/afx_view` (or "Tuning view" in `/afx`, setting `afx_debug_view`) draws what the acoustics see over the world, through walls:

- **The probe's rays**, recast from the latest snapshot, as a fan from your chest (lines from the eye itself would be seen end-on). Each is coloured by what it hit: grey stone, brown wood, green leaves, blue water, cyan glass, tan earth, white snow. Escaped rays are short and faint, yellow where they passed through open sky.
- **The way out:** a cyan arrow toward where the outdoors comes in, as long as how much of it does. A yellow line points to the space next door, and magenta lines to the wall beside you and the one across.
- **Every playing sound:**
  - a cross where it is, and a line from you: green clear, orange muffled, red cut off;
  - a dashed line to the opening when it's heard around a corner;
  - a label with its bus, level, direct gain and high-frequency loss, muffle and reverb sends, the far space, occlusion.
  - Non-positional sounds and far ones are listed under the panel.
- **A panel** with the room: enclosure, mean free path, materials, the reverb, the recorded space and its share, the outdoor paths, walls, the space next door, the far space, and the source counts.

The sound thread adds each source's applied mix to its report while the view is on; the rays are cast again on the main thread from the snapshot it built.

## Round 16: what owning the ambience on the client makes possible

- **`/afx_reload`:** the engine reloads `builtin/afx` from disk at the next step. The scripts' sounds stop, and what the server said (the game, its creatures, the latest weather) is told again. The command is the engine's own, so it works when the scripts are broken.
- **The probe's way out, used:** the path search reports where the open path gets out. That's the doorway (`opening_inner`), the first cell under the sky (`opening`), and how many open cells the doorway has across it (1 a crack, 2 a door). These come in the hearing table as `has_opening`, `opening_*`, `inner_*` and `opening_size`.
  - Gutters play at the real doorway when there's an overhang outside it.
  - Without one, rain patters on the ground just outside the door, by what it is.
  - Drafts whistle first at the crack the engine hears the outdoors through.
  - The tuning view draws the doorway as a box sized by its width.
- **What the ambience costs:** measured at about 1 ms a second in total, with a frame peak around 1 ms. It's shown in `/afx_report`. Moving the scans into the snapshot isn't worth it at this cost.
- **Your hands, heard as you act** (`hands.lua`): a blade, tool or pick swished through the air or into something, lower for heavier ones, and a soft rustle when you take something new in hand. Items the game gives a swing sound of its own are left to it. The engine calls `afx_on_node` with `swing`, `swing_hit`, `swing_node` and `wield`; the scripts register for them with `core.afx.register_on_action`.
- **Picture and sound share a clock** (the fx renderer's side):
  - `fx::addFlash`: lightning flashes the moment its strike arrives, and the engine delays the thunder by the distance. A blast flashes hot. Both tint the fx lights around them and lift the exposure by how much sky you see (`core.afx_flash`); by day lightning is a pale blink.
  - `fx::setMood`: the audio's tension greys and darkens the grade a little, and cozy warms it.
- **Smooth moving sounds:** sounds on objects take their velocity from how the object is actually drawn moving, smoothed over a tenth of a second (`afx::soundVelocity`), not from the velocity the server last sent. The Doppler shift no longer steps. This covers the server's attached sounds and the ambience's own.
- **A flock passes over:** now and then on a fair day, out in the open, a little flock crosses the sky, chirping and beating its wings, heard moving from one side to the other.
- **Hardening:**
  - The server's test hooks (`@poke`, `@chat`, `@fields`) only work with `afx_debug` on, since they reach into the client's state and settings; `@poke` only sets `next_*` timers.
  - Storage values are kept to one line.
  - No sound handle is made for a sound that isn't there.
  - Turning afx off keeps what the server said, for when it comes back on.
  - The mood leaves the picture when afx is off.

### Review fixes (three independent reviewers: the client runtime, the engine audio and visuals, the Lua and the bridge)

- **afx off and on again:**
  - The server only treats a client as afx while it's reporting (5 s, as `get_acoustics` does), so with afx off the bridge falls back to plain sounds for its content.
  - Events that arrive while afx is off are dropped, not replayed in a burst when it comes back; only what was said before the very first start waits.
  - The tuning view, `hearing()` and the report don't show stale state, and the mood leaves the picture.
  - `/afx` turns afx back on when it's off.
- **What a server sends is checked:** every number is parsed, kept finite and clamped (wind 0..3, rain 0..1, gust 0..2, play gain 0..2, pitch 0.25..4, range 0..128, positions in the world).
  - Tables are type-checked, and channel names are limited to `[A-Za-z0-9_]`.
  - The world state is replaced by each message, so a value the server stops sending is gone.
  - The mixer only sets its own sliders' settings, with their ranges.
  - Wind chimes ring at most 6 chimes × 4 notes a second.
  - Lua errors decay: a rare one is forgiven, and a run of 20 stops the scripts and their sounds, with a chat line saying so.
- **Sounds:**
  - Emitters on objects notice when their sound went with the object, or when the object they want changes (`afx_sound_alive`).
  - A fade with no step stops the sound.
  - Local sound lookup checks `.0`–`.9`.
  - A recorded space the effect won't take isn't retried every apply.
- **Doorway:** the narrowest connected cross-section over the last steps before the sky, flood-filled through what the path passes. A porch, a window two cells off, or standing in the doorway no longer fool it. It's tested with a door, a crack, an X-facing door with the ear in it, and a porch.
- **Motion:** `soundVelocity` works by the clock, not the caller's step, and starts over for an object it hasn't followed for a quarter second, so a mob that groans now and then has no Doppler blip.
- **Port regressions found:**
  - Cats purr again: a cat that's settled by you, since tameness lives on the server.
  - Clocks in item frames tick again: the frame shows the item as its texture.
  - Discoveries work on servers without the bridge, kept per server.
  - Position keys no longer collide: LuaJIT prints 14 digits of a 15-digit hash, so neighbouring nodes shared keys.
  - The wield callback runs only when the item changes.
- **Hygiene:** pending AL errors are reported rather than swallowed, the limiter's header is included on every platform, and the comments now match what the code does (flashes tint, they don't add light).
- **Test:** `afxscene_toggle_afx_at = s` turns afx off for 8 s mid-scenario.

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

## Round 17: a knock, not a gong

- **Hollow knock.** The old takes rang 80 Hz cavity modes for ~0.8 s (a gong), ~15 dB over the game's hit. New takes: a real tok/thunk (slowed 0.8, lowpassed 1.8 kHz) over the old take pitched 0.88 with a 85 ms body decay; -30 dB in 0.2-0.33 s, centroid ~600 Hz. Rock over a void sounds "drummy" (dull), solid rock rings: this is the physical direction too.
- **Stone only.** Wood and earth are thin almost everywhere (plank walls, trunks, topsoil over stone), so hollowness there is noise. Built stone still counts: a roofed stone house knocks hollow from outside, which is right.
- **Test scenario:** `knock` now mines `afxscene:rock` with a dull pick, so every swing plays the game's stone hit too (as in a real game).
- **Pipeline:** `normalize` no longer drops the table's `return {` when it keeps hand-tuned levels, and leaves peak-limited files alone instead of re-encoding them every run.

## Round 17: the ambience breathes (`builtin/afx/variation.lua`)

A place no longer sounds the same every minute. All of it moves slowly (10 s and up), so it reads as life rather than as the mix fidgeting.

- **Phases.** A shared "world breath" moves between three phases: lull, normal and lively. Each lasts 30–120 s, with 15 s smooth transitions.
  - A lull takes life beds down 5 dB and thins one-shots to about 20%. The wind breathes with it (0.6–0.8) because the stillness is the eerie part. The sea mostly doesn't (0.3).
  - Context makes lulls likelier: night, the hush at dusk, the wind swelling, and the minute after thunder or a blast.
- **Startle.** Thunder or a blast (anything that rattles windows) stops the birds at once. Life beds dip, and the birds come back over about 35 s.
- **Drift.** Inside a phase, each bed wanders by ±1 dB on its own, and one-shot rates by ±25%.
- **Style.**
  - The balance between a place's beds wanders (±25% of each one's share, total power kept).
  - The air's brightness drifts: the beds' direct-path highs go down to 0.75, via the engine's new per-sound tone, `core.afx_sound_tone`.
  - Beds swap takes every 90–240 s over an 18 s crossfade. Second takes come from other stretches or recordings: `*_b`, `*_c`, made in import_sounds.py.
  - Three bird species are about at a time; one leaves and another arrives every 1–3 min.
- **One-shots.** The phase scales birds, croaks, flocks, drips, creaks, chains and ice. It uses `V.thin` (Poisson thinning, so it works the moment the rate drops) and `V.interval` (shorter waits when lively). Life one-shots get the full effect; place one-shots get the square root.
- **Exempt.** Anything that is information rather than ambience is left alone: water you're next to, lava, fires, creatures, your own actions, rain and music.
- **Biome blend.**
  - The bridge samples the biomes on two rings around the player (12 and 24 nodes, `afx_ambience_biome_radius`) and sends weights (`@biomes`).
  - The client mixes the place from them, then rate-limits how fast what you hear may change (`biome.blend_rate`, 0.07 of a place per second). A fast flight over biomes glides, and an edge sits half and half instead of flipping.
- **Tuning.**
  - Everything lives in the second table in `levels.lua`; the pipeline keeps it.
  - The mixer's Variation sliders scale the main knobs: depth, how often, pace, wander, tone and blend speed. The mixer also has an on/off switch.
  - `/afx_var <key> <value>` sets any tunable live; `/afx_var phase lull|normal|lively` forces a phase.
  - The tuning view shows the phase, rates, tone, birds about, takes and the place/biome mix.
- **Tests.**
  - Tours turn variation off by default, so runs compare.
  - The `breath` scenario (normal → lull → lively → near thunder) is a listening check: in the forest the lull is about −2.7 dB on the whole bed, lively +3 dB.
  - The `biomes` scenario switches the biomes around at once; the jungle comes up over about 14 s.
