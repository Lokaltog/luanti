# afx fork: environmental audio (M1 EFX plumbing + M2 probe)

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

## Testing

- `bin/luanti --run-unittests --test-module TestAfx` runs the probe on synthetic worlds: field, stone cave, wooden hut, tunnel mouth, cliff echo, occluders, water, plus the smoother.
- `--test-module TestAfxEfx` renders through a real OpenAL Soft loopback device and measures the output. It checks the reverb tail, muffling, and that echo and bed steering land on the correct side.
- `AFX_RENDER_DIR=/tmp/afx bin/luanti --run-unittests --test-module AfxRender` writes listening clips (placeholder sounds through the real pipeline).
- `USE_XVFB=1 util/afx/tour.sh /tmp/tour` runs the real client headless through six stations of `util/afx/testworld`. It records what the player hears via OpenAL Soft's wave backend and prints a per-station table.

Measured in the engine: snapshot 0.7–1.2 ms on the main thread, probe 0.0–0.3 ms on the sound thread.

## Known gaps / next

- Doors are classed as "leaky" whether open or closed: a node's content ID doesn't say which.
- Rain-on-roof by material, procedural wind, the tension scalar and the Lua content layer (birds in leaves, insects, lava, drips) are the next milestones.
- Stereo outdoor beds need `AL_EXT_STEREO_ANGLES`, and mono beds use `AL_EXT_SOURCE_RADIUS`. Both are in OpenAL Soft.
- Echo distance is capped at about 35 nodes (the EFX echo delay limit is 0.207 s).
- A Lua API for an explicit bus (`sound_play{bus=…}`) needs a protocol field and is deferred. Use the name prefixes for now.
