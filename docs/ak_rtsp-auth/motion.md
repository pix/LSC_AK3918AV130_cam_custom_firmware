# Motion detection: zones, object size, persistence, light changes, line crossing, loitering

Motion detection works on a fixed grid of cells. Two sources feed it (`md.source`):

| source | grid | rate | change value per cell |
|---|---|---|---|
| `software` (image) | 32 x 18 luma blocks of the 640x360 sub stream | ~4/s | \|block mean - slowly adapting background\| |
| `hardware` (image processor) | 32 x 24 ISP 3D-NR motion statistic (u16) | ~10/s | the statistic itself |

`md.c` reads the grid (and builds the watched-cell masks: `md.zones` minus the burned-in overlay rows);
every decision is made in `md_logic.c`, a pure module (no hardware access, no globals, ~10 KB of
fixed state, no allocation). `md.c` turns its result into events.

## What stays as before

With every option below at its default (off), the decisions are bit-for-bit those of the original
`md.c` (motion, whole-picture motion for the adaptive frame rate, score, red cells, background):
`tests/motion` runs both against a copy of the original code on long random sequences.

- software: cell changed when its change > 40 - 0.35 x sensitivity; motion when >= 6 % .. 0.4 % of the
  watched cells changed in 2 frames in a row; more than 60 % of all cells changing re-seeds the background
  and pauses 2 s.
- hardware: cells >= max(1800, 3.5 x p90) seed regions grown over cells >= half of that (8-connected); a
  region counts when its bounding box is between min_cells(sensitivity) and 300 cells and enough of it is
  watched; at night 2 of the last 3 polls must hit; frames whose median jumps are ignored.
- day/night switch: 5 s (software) / 10 s (hardware) quiet. Motion ends `md.hold` s after the last hit.

## Options (all off by default)

| setting | values | effect |
|---|---|---|
| `md.zone_sens` | `""` or up to 8 `c0,r0,c1,r1,sens` separated by `;` (columns 0-31, rows 0-17 of the 32x18 grid, inclusive; sensitivity 1-100) | Rectangles with their own sensitivity. Software: the cell threshold and the needed share of cells use the zone's sensitivity, counted per zone (cells outside all rectangles form one more group with `md.sensitivity`). Hardware: a region counts if, for some zone, its box and its cells in that zone reach min_cells(zone sensitivity). A later rectangle wins where they overlap. Only watched cells (`md.zones`) count; the web editor marks a new rectangle as watched. |
| `md.min_size` | 0-50 cells | A changed cell counts only inside a connected blob of at least this many cells. Software: 4-neighbour flood fill over the changed cells (whole picture, so an object half inside a zone keeps its size). Hardware: the cell count of each grown region (8-connected, as the region growing). |
| `md.persist` | 0-10000 ms | The motion condition must hold continuously this long before motion starts (on top of the 2-frame rule of the software source; the hardware source in daylight otherwise fires on one poll). Also the minimum age of a tracked object for line/loiter rules. |
| `md.light` | 0, or 10-90 % | Light changes (lamps, IR cut, exposure jumps). Software: when this share of ALL cells changes at once, or the picture brightens/darkens uniformly (mean change >= 6 levels with >= 75 % of the cells moving the same way), the background is re-seeded and detection pauses 3 s. Hardware: when this share of the cells is above the quiet-scene threshold (from a slow p90 baseline), or the old median rule fires, detection pauses 3 s. 0 keeps only the original rules (60 % / median). |
| `md.line` | `""` or `x0,y0,x1,y1` (0-1000 of the picture width/height; end points >= 20 apart) | A line segment. A tracked object whose centroid moves from one side to the other through the segment gives a crossing. |
| `md.line_dir` | `both`, `left`, `right` | Crossings to report: toward the left or right side of the line as seen standing at (x0,y0) looking at (x1,y1) (image y grows downwards). The web editor writes L / R next to the line. |
| `md.loiter` | 0-600 s | An object tracked inside the loitering area for this long gives a loitering event. |
| `md.loiter_zone` | 0-8 | 0 = the watched area (`md.zones`), 1-8 = that `md.zone_sens` rectangle (a missing one = watched area). |

All are registered settings (`/api/config`, port 8091, MQTT-independent), persisted, validated
(out-of-range or malformed values are refused with a message; `md_logic` clamps again).

## Tracking (line crossing, loitering)

Only runs while `md.line` or `md.loiter` is set.

1. Changed cells of the whole picture minus the overlay (software: the per-cell hits after
   `md.min_size`; hardware: the accepted regions of the whole-picture detector) are split into blobs
   (software 4-connected, hardware 8-connected). Blobs smaller than max(2, `md.min_size`) cells are
   dropped; the 16 largest are kept. Position = centroid in 0..1000 picture coordinates.
2. Up to 8 tracks. Greedy nearest match: repeatedly the closest (track, blob) pair, if the centroid moved
   at most 8 cells. Unmatched blobs start tracks; a track without a match for 1.5 s is dropped. No
   prediction, no appearance model.
3. A track counts for rules after 3 matches and `md.persist` ms.
4. Line: the side of the line is decided with a dead band of 1.5 % of the picture around the line (no
   flapping when someone stands on it). When a counted track is on the other side than where it was last
   confirmed, and the path between the two points intersects the segment, that is one crossing, with the
   direction of the new side. Movement around the end of the segment is not a crossing.
5. Loitering: a counted track whose centroid stays in the area; the timer restarts when it leaves the area
   or the track is lost.

State: `line` is on from a crossing until `md.hold` s without one; `loiter` is on while someone loiters
and `md.hold` s after.

### Limits (by design of a 32x18 / 32x24 grid at 4-10 frames/s)

- One cell is 20x20 px of the sub stream (60x60 px of 1080p). Small or distant people are 1-4 cells;
  set `md.min_size` with that in mind.
- Two people walking close together are one blob, and one person can split into two blobs (legs/torso):
  counts of crossings are approximate. Crossing tracks can swap identities.
- Fast objects (more than 8 cells between two frames: crossing the whole picture width in less than
  ~1 s with the software source, ~0.4 s with the hardware one) are lost between frames and no crossing
  is seen.
- Tracking does not need the motion area threshold: a blob of a few cells can cross the line without
  being "motion" (use `md.min_size` against small things like pets or leaves).
- The software source compares with a slowly adapting background: something that stands still is
  absorbed in roughly 10-20 s (it disappears), and when it leaves, the uncovered background shows as a
  "ghost" for ~10 s. Loitering therefore needs some movement, and a parked object that leaves can look
  like a short loiterer. The hardware statistic only shows movement at all: a still person is invisible.
- The light rules cannot tell a very large object (filling > `md.light` % of the picture) from a light
  change.
- Tracks are dropped during the quiet times (day/night switch, light change).

## Events

| | motion (unchanged) | line crossing | loitering |
|---|---|---|---|
| MQTT state | `<base>/motion` ON/OFF | `<base>/line` ON (every crossing) / OFF; `<base>/line_direction` left/right (retained) | `<base>/loiter` ON/OFF |
| HA discovery | binary_sensor motion | binary_sensor "Line crossed" + sensor "Line crossing direction" | binary_sensor "Loitering" (occupancy) |
| webhook JSON | `"event":"motion"` | `"event":"line","direction":"left"` | `"event":"loiter","zone":n` |
| ONVIF | CellMotionDetector/Motion, VideoSource/MotionAlarm | `tns1:RuleEngine/LineDetector/Crossed` (ObjectId = track id, Rule LineRule) | `tns1:RuleEngine/FieldDetector/ObjectsInside` (IsInside, Rule FieldRule) |
| SD card | event snapshot `..._motion.jpg` | `..._line.jpg` | `..._loiter.jpg` |

The HA entities for line/loitering are announced at MQTT connect (and when HA comes online) while the
feature is set up, and removed otherwise (so after changing `md.line` / `md.loiter` they appear at the
next MQTT reconnect). The SD recorder still starts on motion only; the new events save an event
snapshot. Schedules / privacy mode suppress the new events like motion. ONVIF Crossed messages carry
`PropertyOperation="Changed"` like every message of this camera (a pure event would omit it).

## Cost

Per analysis frame, every option on, worst case (checkerboard of changes): O(cells) work (two
connected-component passes, the region growing, one histogram) plus O(8 tracks x 16 blobs) matching.
On the PC this is under 0.25 ms even in the ASan build. On the ARM926 (soft float) estimated <= ~150-250 k
cycles for the software source (the per-cell float work existed before; the new integer passes add
~50-80 k) and ~100-150 k cycles for the hardware source: well under 1 ms per frame, i.e. < 0.5 % CPU at
4 resp. 10 frames/s. The block averaging of the frame (unchanged) costs more than the decision. Memory:
`mdl_t` 10 KB + output 0.9 KB, static; stack use < 2 KB.

## Tuning on the camera

`GET /api/events` -> `md.tracks` ([id, x, y] per track, drawn as yellow dots in the zone editor),
`md.line`, `md.loiter`, `md.light`. Record the hardware statistic and replay it on the PC:

    # on a PC in the camera's network (user:password = the camera login)
    while :; do printf '%s ' "$(date +%s%3N)"; curl -s -u user:password http://CAMERA/api/md/raw; echo; sleep 0.1; done > md.log
    # replay with any settings (tests/motion/md_replay.c)
    md_replay md.log sensitivity=60 line=500,0,500,1000 min_size=3 light=30 -v

Things to validate on real scenes: day and IR night, lamps switched on/off, IR cut switching, clouds /
sun on walls, trees and rain, people at several distances walking across the line in both directions,
someone waiting at a door (loitering), cars parking (software source ghosts).

## Tests

`tests/motion/run.sh` (unit test with ASan+UBSan and valgrind, replay), `tests/fuzz/fuzz_md_logic.c`
(libFuzzer: random settings + grid sequences, parsers; per-frame time bound). See TESTING.md.
