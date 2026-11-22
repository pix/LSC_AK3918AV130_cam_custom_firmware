# ONVIF Profile G: searching and replaying the SD recordings

NVRs and ONVIF clients can list the camera's SD card recordings, search them by
time and event, and play them back over RTSP. Off by default.

| Setting | Default | Meaning |
|---|---|---|
| `onvif.replay` | off | Advertise and answer the search, recording and replay services; serve `rtsp://camera/replay/...`. |

Same login as the rest of ONVIF (WS-Security UsernameToken or HTTP Digest) and
as RTSP (Digest). Only `GetServiceCapabilities` answers without a login, as for
the other services. Nothing is ever changed or deleted through these services.

## What is a "recording"

The recorder (record.c) writes one fragmented MP4 per event clip or continuous
segment into `rec/<YYYY-MM-DD>/HH-MM-SS_<kind>.mp4`. For ONVIF:

* **Recording = one day folder**, token `2026-11-22`. It holds that day's
  motion, sound, motion+sound ("event") and continuous files in time order.
  Earliest = first file's start, latest = last file's end.
* **A single file can also be used as a token**: `2026-11-22_10-31-05_motion`
  (GetRecordingInformation, GetMediaAttributes, FindEvents scope, GetReplayUri).
  It is not listed by FindRecordings / GetRecordings (that would mean thousands
  of recordings), the replay URI of a day covers it anyway.
* Timelapse files and snapshots are left out (timelapse media time is not
  wall-clock time).
* Tracks: `VIDEO001` (H.264) and, when the files have sound, `AUDIO001`
  (stored as 8 kHz PCM, replayed as G.711 u-law).
* Times: a file starts at the local time in its name (the camera's time zone,
  `time.tz`, converted to UTC) and ends at its last modification time (the
  recorder syncs at every keyframe), or now while it is being written. Event
  clips include up to `record.pre` seconds of pre-roll before the time in the
  name: those frames carry times up to that much too late.
* Tokens are checked with record.c's own name rules (day `YYYY-MM-DD`, file
  names of `[A-Za-z0-9_-.]`, no `..`): nothing outside the recordings folder can
  be named.

## Services and operations

Endpoints (in GetServices, and in GetCapabilities under `Extension`):
`/onvif/search_service`, `/onvif/recording_service`, `/onvif/replay_service`.
GetScopes and WS-Discovery add `onvif://www.onvif.org/Profile/G`.

| Service | Implemented | Not implemented |
|---|---|---|
| Search | GetServiceCapabilities, GetRecordingSummary, GetRecordingInformation, GetMediaAttributes, FindRecordings, GetRecordingSearchResults, FindEvents, GetEventSearchResults, GetSearchState, EndSearch | FindPTZPosition / FindMetadata (no PTZ, no metadata track): ActionNotSupported |
| Recording | GetServiceCapabilities, GetRecordings, GetRecordingConfiguration, GetTrackConfiguration, GetRecordingJobs (empty) | Create/Delete/Set recordings, tracks, jobs, GetRecordingOptions, export: ActionNotSupported (the recorder is driven by `record.mode`) |
| Replay | GetServiceCapabilities, GetReplayUri, GetReplayConfiguration, SetReplayConfiguration (accepted, timeout fixed at 60 s) | reverse playback |

Search details:

* Searches are kept in a table of 4 (the oldest is dropped when a fifth
  starts), each until EndSearch or its KeepAliveTime (default 60 s, at most
  10 min) runs out. Results are computed at Find* time; Get*SearchResults pages
  them (MaxResults, default 50, at most 100 per answer). SearchState is
  `Searching` while results remain, `Completed` with the last page.
* FindRecordings: scope `IncludedRecordings` (day or file tokens) is honoured,
  `RecordingInformationFilter` (XPath) is ignored, MaxMatches caps the list.
* FindEvents: per file, at its start `tns1:RecordingHistory/Recording/State`
  (IsRecording=true), `tns1:RecordingHistory/Track/State` (IsDataPresent=true)
  and, by kind, `tns1:RuleEngine/CellMotionDetector/Motion` (IsMotion=true) /
  `tns1:AudioAnalytics/Audio/DetectedSound` (IsSoundDetected=true); the same
  with false at its end (not for the file being recorded). EndPoint before
  StartPoint searches backwards (newest first). IncludeStartState adds the
  recording state at StartPoint first. SearchFilter TopicExpression: topics or
  prefixes (`tns1:RuleEngine//.`), several with `|`; other filters are ignored.
  At most 1000 results per search (MaxMatches lowers it).
* GetMediaAttributes: width/height from the file at Time (else the newest),
  frame rate from its first fragment, bitrate from size / duration.

## Replay (RTSP)

`GetReplayUri` returns `rtsp://<camera>:554/replay/<token>`. The RTSP server
(rtsp.c) handles login and transports as for live video; replay.c reads the
files and sends the media:

* DESCRIBE: SDP with H.264 (sprop-parameter-sets from the file) and, if the
  files have audio, PCMU/8000; `a=range:npt=0-<length>`.
* SETUP: RTP/AVP/TCP interleaved or UDP. RTSPS (port 322, interleaved only)
  goes through the same connection code but has not been tested with replay.
* PLAY `Range`:
  * `npt=<s>-[<e>]`: seconds from the start of the recording (for a day: from
    its first file, gaps included; for a file: its media time), `npt=now-` = start;
  * `clock=<UTC>-[<UTC>]` (RFC 2326 `20261122T103010.000Z`);
  * no Range: from the start, or resume after PAUSE.
  Playback starts at the keyframe at or before the requested time (one
  fragment = one GOP); the answer's Range says where. A range end stops there.
  RTP-Info gives the first sequence number and RTP time of each track.
* `Require: onvif-replay`: every RTP packet carries the ONVIF replay header
  extension (profile 0xABAC, 3 words): NTP time of the frame (recording time,
  UTC), flags C (keyframe), E (last frame before a gap or the end), D (first
  frame after a seek or a gap), T (last frame of the replay), and the low byte
  of the PLAY request's CSeq (so a client can drop data of an earlier PLAY).
  Without the header: plain RTP (ffmpeg, VLC).
* `Scale`: 1 (default); 1 < Scale <= 16 sends keyframes only, Scale times
  faster; 0, negative (reverse) or larger: 456. `Frames: intra`: keyframes only.
  `Rate-Control: no`: as fast as the connection takes it (export).
* PAUSE, PLAY again (resume or new Range while playing), TEARDOWN.
* A day plays its files one after the other; gaps are skipped (RTP time goes on
  without the gap; the extension's NTP time jumps, E/D mark it). Files that do
  not open are skipped; a file cut short (power loss) plays up to its last
  complete fragment. Replaying the file being recorded follows it as it grows
  (tested); at the end of a day's list the folder is listed again so the
  recorder's next segment is played too (not tested yet).
* End of the recording / range: RTCP BYE on each track (ffmpeg / VLC end the
  stream cleanly), the session stays open 60 s for another PLAY or TEARDOWN.
* RTCP sender reports at the start and every 5 s (A/V sync in players).

Limits and memory: at most 2 replay sessions at once (a third DESCRIBE gets
503), each also counts towards the RTSP server's 4 connections. Per session:
the playlist (one entry per file of the day), the current fragment's sample
table (at most 4096 samples) and moof (at most 256 KB, ours are ~1-2 KB), one
frame buffer sized to the largest frame so far (at most 512 KB). Files are read
with pread one moof / one frame at a time, never whole. Live streaming is not
involved (no live hub cursor); replay competes only for Wi-Fi and SD bandwidth.

Errors: unknown, malformed or traversal tokens and timelapse files: 404;
replay off: 404; a file that is not a readable fMP4 with H.264: 415; PLAY
before SETUP: 455; bad Range or past the end: 457; bad Scale: 456.

## What was not done

* No aggregate recording spanning all days (one recording per source is what
  some NVRs expect for edge-storage import; with one recording per day they see
  several). Day recordings were chosen so lists stay short and tokens stable.
* No reverse playback, no PTZ / metadata search, no recording control (jobs,
  create / delete), no export (ExportRecordedData).
* RTP timestamps of a day replay are continuous (gaps squeezed); clients that
  place frames by RTP time instead of the extension's NTP time see the gaps
  closed.
* Times rely on the camera clock being right when the files were written and
  on the time zone setting (names are local time).
* Home Assistant's ONVIF integration does not use Profile G (it has no
  recording browser); HA can play a replay URI as a generic camera / stream.

## Tests

`tests/onvif/profile_g.sh <host build> <video.h264> <venv> [valgrind]` runs
host_server in the sandbox and both scripts, then checks the sanitizer /
valgrind verdict; see TESTING.md. Fuzzers: `tests/fuzz/fuzz_fmp4r.c` (demuxer
and a whole replay session on fuzzed files) and `tests/fuzz/fuzz_onvif_g.c`
(the SOAP requests).
