"""ONVIF Profile G: recording search, recording (read-only) and replay services,
with the library Home Assistant uses (onvif-zeep-async, its shipped WSDLs).

Usage: profile_g_test.py host user pass rec_dir
  rec_dir: the camera's SD rec/ folder as seen by the test (the sandbox's
  tmpfs, e.g. <build>/root/sd/rec): crafted recordings are added there
  (pg_common.make_fixture). Sets time.tz=UTC, onvif.replay=on, records one
  1-minute continuous segment with the real recorder first.
Prints PASS/FAIL lines and "profile_g: N failed"; exit status = failures.
"""
import asyncio, datetime, os, sys, time
sys.dont_write_bytecode = True                  # no __pycache__ in the source tree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pg_common as C
from pg_common import check
from onvif import ONVIFCamera
import zeep

H, U, P, REC = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
UTC = datetime.timezone.utc
TD = datetime.timedelta


def dt(day, hms):
    return datetime.datetime.strptime(f"{day} {hms}", "%Y-%m-%d %H:%M:%S").replace(tzinfo=UTC)


async def fault_of(coro):
    try:
        await coro
        return None
    except zeep.exceptions.Fault as e:
        return " ".join(getattr(q, "localname", str(q)) for q in (e.subcodes or [])) + " " + str(e.message)
    except Exception as e:                       # the library wraps some faults
        return type(e).__name__ + " " + str(e)


async def all_results(search, kind, tok, page):
    """pages through Get*SearchResults: (items, states)"""
    items, states = [], []
    for _ in range(200):
        if kind == "rec":
            r = await search.GetRecordingSearchResults({"SearchToken": tok, "MaxResults": page, "WaitTime": TD(seconds=1)})
            items += r.RecordingInformation or []
        else:
            r = await search.GetEventSearchResults({"SearchToken": tok, "MaxResults": page, "WaitTime": TD(seconds=1)})
            items += r.Result or []
        states.append(r.SearchState)
        if r.SearchState == "Completed":
            break
    return items, states


async def main():
    http = C.Http(H, U, P)
    # ---- set up: replay on, UTC, one real recording, crafted ones
    http.config(**{"time.tz": "UTC", "onvif.replay": "on", "record.audio": "on", "record.segment": "1", "record.keep_free": "100", "record.mode": "continuous"})
    t0 = time.time()
    day, name = C.wait_recording(http)
    check(day is not None, f"the recorder made a 1-minute segment ({day}/{name}, {time.time() - t0:.0f} s)")
    if not day:
        return
    http.config(**{"record.mode": "off"})
    time.sleep(2)
    real = os.path.join(REC, day, name)
    files, dur = C.make_fixture(REC, real)
    print(f"   fixture: {len(files)} crafted files, real segment {dur:.2f} s")

    cam = ONVIFCamera(H, 80, U, P)
    await cam.update_xaddrs()
    ns = {k: v for k, v in cam.xaddrs.items()}
    check(all(any(s in k for k in ns) for s in ("ver10/search", "ver10/replay", "ver10/recording")),
          "GetServices advertises search, replay and recording")
    dev = await cam.create_devicemgmt_service()
    caps = await dev.GetCapabilities({"Category": "All"})
    ext = caps.Extension
    # zeep lets the schema's leading <xs:any namespace="##other"> take tt: elements too: look in both places
    xel = {e.tag.split("}")[1]: e for e in (ext._value_1 or [])} if ext is not None else {}
    xs = xel.get("Search")
    xaddr = ext.Search.XAddr if ext is not None and ext.Search is not None else (
        xs.find("{http://www.onvif.org/ver10/schema}XAddr").text if xs is not None else "")
    check(ext is not None and {"Recording", "Search", "Replay"} <= (set(xel) | {k for k in ("Recording", "Search", "Replay") if ext[k] is not None})
          and xaddr.endswith("/onvif/search_service"), f"GetCapabilities: Extension Recording/Search/Replay, search XAddr {xaddr}")
    scopes = await dev.GetScopes()
    check(any("Profile/G" in s.ScopeItem for s in scopes), "GetScopes has onvif://www.onvif.org/Profile/G")

    search = await cam.create_search_service()
    rec = await cam.create_recording_service()
    rpl = await cam.create_replay_service()
    sc = await search.GetServiceCapabilities()
    check(sc.MetadataSearch is False, "search GetServiceCapabilities")
    rc = await rec.GetServiceCapabilities()
    check(rc.DynamicRecordings is False and rc.MaxRecordingJobs == 0, "recording GetServiceCapabilities (read-only)")
    pc = await rpl.GetServiceCapabilities()
    check(pc.ReversePlayback is False and pc.RTP_RTSP_TCP is True, "replay GetServiceCapabilities")

    # ---- summary
    days = sorted({day, C.FIX_DAY, C.FIX_DAY2})
    s = await search.GetRecordingSummary()
    check(s.NumberRecordings == len(days), f"GetRecordingSummary: {s.NumberRecordings} recordings (days {days})")
    check(s.DataFrom == dt(C.FIX_DAY2, "23:59:30") and s.DataUntil >= s.DataFrom,
          f"summary span {s.DataFrom} .. {s.DataUntil}")

    # ---- FindRecordings + paging
    tok = await search.FindRecordings({"Scope": {}, "KeepAliveTime": TD(seconds=30)})
    items, states = await all_results(search, "rec", tok, 1)
    got = [i.RecordingToken for i in items]
    check(sorted(got) == days and states[-1] == "Completed" and states[:-1] == ["Searching"] * (len(states) - 1),
          f"FindRecordings paged 1 by 1: {got}, states {states}")
    st = await search.GetSearchState({"SearchToken": tok})
    check(st == "Completed", "GetSearchState Completed")
    ep = await search.EndSearch({"SearchToken": tok})
    check(isinstance(ep, datetime.datetime), f"EndSearch -> {ep}")
    f = await fault_of(search.GetRecordingSearchResults({"SearchToken": tok}))
    check(f is not None and "InvalidToken" in f, f"results after EndSearch refused ({f})")
    tok = await search.FindRecordings({"Scope": {"IncludedRecordings": [C.FIX_DAY, "../../etc", "2025-01-99"]}, "KeepAliveTime": TD(seconds=30)})
    items, _ = await all_results(search, "rec", tok, 10)
    check([i.RecordingToken for i in items] == [C.FIX_DAY], "FindRecordings scope: only the valid, existing token")
    await search.EndSearch({"SearchToken": tok})

    # ---- RecordingInformation of the crafted day
    ri = await search.GetRecordingInformation({"RecordingToken": C.FIX_DAY})
    tracks = {t.TrackType: t for t in ri.Track}
    check(ri.EarliestRecording == dt(C.FIX_DAY, "10:00:00"), f"EarliestRecording {ri.EarliestRecording}")
    check(ri.LatestRecording >= dt(C.FIX_DAY, "11:30:00"), f"LatestRecording {ri.LatestRecording}")
    check(set(tracks) == {"Video", "Audio"} and tracks["Video"].TrackToken == "VIDEO001", f"tracks {sorted(tracks)}")
    check(ri.RecordingStatus == "Stopped" and "2 motion, 1 sound, 1 motion+sound, 1 continuous" in ri.Content,
          f"status {ri.RecordingStatus}, content '{ri.Content}' (timelapse, jpg, odd names left out)")
    fi = await search.GetRecordingInformation({"RecordingToken": C.FIX_DAY + "_10-30-00_sound"})
    check(fi.EarliestRecording == dt(C.FIX_DAY, "10:30:00") and abs((fi.LatestRecording - fi.EarliestRecording).total_seconds() - dur) <= 1.5,
          f"file token: {fi.EarliestRecording} .. {fi.LatestRecording}")
    for bad in ("2025-01-16", "../../../etc/passwd", C.FIX_DAY + "_12-00-00_timelapse", C.FIX_DAY + "_..", "x" * 70, ""):
        f = await fault_of(search.GetRecordingInformation({"RecordingToken": bad}))
        check(f is not None and ("NoRecording" in f or "InvalidArgVal" in f), f"GetRecordingInformation({bad[:20]!r}) refused")

    # ---- media attributes
    ma = await search.GetMediaAttributes({"RecordingTokens": [C.FIX_DAY, day], "Time": dt(C.FIX_DAY, "10:30:30")})
    v = [t for t in ma[0].TrackAttributes if t.VideoAttributes is not None]
    a = [t for t in ma[0].TrackAttributes if t.AudioAttributes is not None]
    check(len(ma) == 2 and v and v[0].VideoAttributes.Encoding == "H264" and v[0].VideoAttributes.Width > 0
          and v[0].VideoAttributes.Framerate > 0, f"GetMediaAttributes video {v[0].VideoAttributes.Width}x{v[0].VideoAttributes.Height} "
          f"{v[0].VideoAttributes.Framerate} fps {v[0].VideoAttributes.Bitrate} kbps" if v else "GetMediaAttributes video")
    check(a and a[0].AudioAttributes.Encoding == "G711" and a[0].AudioAttributes.Samplerate == 8, "GetMediaAttributes audio G711 8 kHz")
    f = await fault_of(search.GetMediaAttributes({"RecordingTokens": ["nope"], "Time": dt(C.FIX_DAY, "10:00:00")}))
    check(f is not None, "GetMediaAttributes unknown token refused")

    # ---- FindEvents: forward, paging, backward, start state, MaxMatches
    q = {"StartPoint": dt(C.FIX_DAY, "09:00:00"), "EndPoint": dt(C.FIX_DAY, "13:00:00"), "Scope": {},
         "SearchFilter": {}, "IncludeStartState": False, "KeepAliveTime": TD(seconds=30)}
    tok = await search.FindEvents(q)
    ev, states = await all_results(search, "ev", tok, 7)
    await search.EndSearch({"SearchToken": tok})
    times = [e.Time for e in ev]
    topics = [e.Event.Topic._value_1 for e in ev]
    check(len(ev) == 30 and times == sorted(times) and states[-1] == "Completed" and len(states) == 5,
          f"FindEvents forward: {len(ev)} events in {len(states)} pages, in time order")
    motion = [e for e, t in zip(ev, topics) if "CellMotion" in t]
    check(len(motion) == 6 and motion[0].Time == dt(C.FIX_DAY, "10:00:00"), f"motion events: {len(motion)}, first at {motion[0].Time if motion else None}")
    first = ev[0]
    item = first.Event.Message._value_1.Data.SimpleItem[0]
    check(topics[0] == "tns1:RecordingHistory/Recording/State" and item.Name == "IsRecording" and item.Value == "true"
          and first.RecordingToken == C.FIX_DAY and first.StartStateEvent is False, "first event: Recording/State IsRecording=true")
    q2 = dict(q, StartPoint=dt(C.FIX_DAY, "13:00:00"), EndPoint=dt(C.FIX_DAY, "09:00:00"))
    tok = await search.FindEvents(q2)
    evb, _ = await all_results(search, "ev", tok, 100)
    await search.EndSearch({"SearchToken": tok})
    check([e.Time for e in evb] == sorted(times, reverse=True), "FindEvents backward (EndPoint < StartPoint): newest first")
    q3 = dict(q, StartPoint=dt(C.FIX_DAY, "10:00:30"), IncludeStartState=True)
    tok = await search.FindEvents(q3)
    evs, _ = await all_results(search, "ev", tok, 100)
    await search.EndSearch({"SearchToken": tok})
    check(len(evs) >= 2 and evs[0].StartStateEvent and evs[1].StartStateEvent and evs[0].Time == dt(C.FIX_DAY, "10:00:30")
          and not any(e.StartStateEvent for e in evs[2:]) and all(e.Time >= dt(C.FIX_DAY, "10:00:30") for e in evs),
          f"IncludeStartState: {sum(1 for e in evs if e.StartStateEvent)} start-state events first")
    tok = await search.FindEvents(dict(q, MaxMatches=5))
    ev5, _ = await all_results(search, "ev", tok, 100)
    await search.EndSearch({"SearchToken": tok})
    check(len(ev5) == 5, "FindEvents MaxMatches=5")
    tok = await search.FindEvents(dict(q, Scope={"IncludedRecordings": [C.FIX_DAY + "_10-31-30_event"]}))
    evf, _ = await all_results(search, "ev", tok, 100)
    await search.EndSearch({"SearchToken": tok})
    check(len(evf) == 8 and all(e.RecordingToken == C.FIX_DAY + "_10-31-30_event" for e in evf),
          "FindEvents scoped to one file: motion+sound start/end")

    # ---- recording service (read-only)
    recs = await rec.GetRecordings()
    check(sorted(r.RecordingToken for r in recs) == days and all(r.Tracks.Track[0].TrackToken == "VIDEO001" for r in recs),
          f"GetRecordings: {len(recs)} items")
    cfg = await rec.GetRecordingConfiguration({"RecordingToken": C.FIX_DAY})
    check(cfg.Source.Address.endswith("/onvif/device_service") and "motion" in cfg.Content, "GetRecordingConfiguration")
    tc = await rec.GetTrackConfiguration({"RecordingToken": C.FIX_DAY, "TrackToken": "AUDIO001"})
    check(tc.TrackType == "Audio", "GetTrackConfiguration AUDIO001")
    f = await fault_of(rec.GetTrackConfiguration({"RecordingToken": C.FIX_DAY, "TrackToken": "META001"}))
    check(f is not None and "NoTrack" in f, "GetTrackConfiguration unknown track refused")
    jobs = await rec.GetRecordingJobs()
    check(not jobs, "GetRecordingJobs: none (the recorder follows record.mode)")
    f = await fault_of(rec.DeleteRecording({"RecordingToken": C.FIX_DAY}))
    check(f is not None and os.path.exists(os.path.join(REC, C.FIX_DAY, "10-00-00_motion.mp4")), "DeleteRecording refused, nothing deleted")

    # ---- replay service
    setup = {"Stream": "RTP-Unicast", "Transport": {"Protocol": "RTSP"}}
    u = await rpl.GetReplayUri({"StreamSetup": setup, "RecordingToken": C.FIX_DAY})
    check(u == f"rtsp://{H}:554/replay/{C.FIX_DAY}", f"GetReplayUri day -> {u}")
    u = await rpl.GetReplayUri({"StreamSetup": setup, "RecordingToken": f"{day}_{name[:-4]}"})
    check(u.endswith(f"/replay/{day}_{name[:-4]}"), f"GetReplayUri file -> {u}")
    for bad in ("2025-01-16", "../x", C.FIX_DAY + "_12-00-00_timelapse", C.FIX_DAY + "/10-00-00_motion.mp4"):
        f = await fault_of(rpl.GetReplayUri({"StreamSetup": setup, "RecordingToken": bad}))
        check(f is not None, f"GetReplayUri({bad!r}) refused")
    rcfg = await rpl.GetReplayConfiguration()
    check(rcfg.SessionTimeout == TD(seconds=60), "GetReplayConfiguration PT60S")

    # ---- raw SOAP: topic filter, malformed requests, login
    body = (f'<tse:FindEvents><tse:StartPoint>{C.FIX_DAY}T09:00:00Z</tse:StartPoint><tse:EndPoint>{C.FIX_DAY}T13:00:00Z</tse:EndPoint>'
            '<tse:Scope/><tse:SearchFilter><wsnt:TopicExpression Dialect="http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet">'
            'tns1:RuleEngine//.</wsnt:TopicExpression></tse:SearchFilter><tse:IncludeStartState>false</tse:IncludeStartState>'
            '<tse:KeepAliveTime>PT10S</tse:KeepAliveTime></tse:FindEvents>')
    code, txt = C.soap(H, "/onvif/search_service", body, U, P)
    stok = txt.split("SearchToken>")[1].split("<")[0] if "SearchToken>" in txt else ""
    code, txt = C.soap(H, "/onvif/search_service", f"<tse:GetEventSearchResults><tse:SearchToken>{stok}</tse:SearchToken>"
                       "<tse:MaxResults>100</tse:MaxResults></tse:GetEventSearchResults>", U, P)
    check(code == 200 and txt.count("<tt:Result>") == 6 and "RecordingHistory" not in txt, f"TopicExpression tns1:RuleEngine//. -> {txt.count('<tt:Result>')} motion events")
    code, txt = C.soap(H, "/onvif/search_service", "<tse:FindRecordings><tse:Scope/><tse:KeepAliveTime>PT5S</tse:KeepAliveTime></tse:FindRecordings>")
    check(code == 400 and "NotAuthorized" in txt, "FindRecordings without login: NotAuthorized")
    code, txt = C.soap(H, "/onvif/replay_service", f"<trp:GetReplayUri><trp:RecordingToken>{C.FIX_DAY}</trp:RecordingToken></trp:GetReplayUri>", U, P + "x")
    check(code == 400 and "NotAuthorized" in txt, "GetReplayUri with a wrong password: NotAuthorized")
    code, txt = C.soap(H, "/onvif/search_service", "<tse:GetServiceCapabilities/>")
    check(code == 200 and "MetadataSearch" in txt, "GetServiceCapabilities without login (as the other services)")
    for b in ("<tse:FindEvents><tse:StartPoint>garbage</tse:StartPoint></tse:FindEvents>",
              "<tse:FindEvents/>", "<tse:GetEventSearchResults><tse:SearchToken>search-00000000</tse:SearchToken></tse:GetEventSearchResults>",
              "<tse:GetMediaAttributes><tse:RecordingTokens>" + "A" * 5000 + "</tse:RecordingTokens></tse:GetMediaAttributes>",
              "<tse:FindRecordings><tse:Scope><tt:IncludedRecordings>" + "</tt:IncludedRecordings><tt:IncludedRecordings>".join(["2025-01-15"] * 200)
              + "</tt:IncludedRecordings></tse:Scope></tse:FindRecordings>",
              "<tse:FindEvents><tse:StartPoint>2025-01-15T09:00:00Z</tse:StartPoint><tse:EndPoint>9999-99-99T99:99:99Z</tse:EndPoint></tse:FindEvents>",
              "<tse:FindPTZPosition/>", "<trc:CreateRecordingJob/>"):
        code, txt = C.soap(H, "/onvif/search_service", b, U, P)
        check(code in (200, 400) and "Envelope" in txt, f"malformed / unsupported request answered ({code}): {b[:50]}")
    # searches are bounded: 6 more than the table holds, the oldest go
    toks = []
    for i in range(6):
        toks.append(await search.FindRecordings({"Scope": {}, "KeepAliveTime": TD(seconds=120)}))
    f = await fault_of(search.GetRecordingSearchResults({"SearchToken": toks[0]}))
    r = await search.GetRecordingSearchResults({"SearchToken": toks[-1]})
    check(f is not None and r.SearchState == "Completed", "search table bounded (oldest search dropped)")
    for t in toks[2:]:
        await search.EndSearch({"SearchToken": t})

    # ---- time zone: names are local time
    http.config(**{"time.tz": "Europe/Paris"})
    ri = await search.GetRecordingInformation({"RecordingToken": C.FIX_DAY})
    check(ri.EarliestRecording == dt(C.FIX_DAY, "09:00:00"), f"Europe/Paris: 10:00:00 local = {ri.EarliestRecording} UTC")
    http.config(**{"time.tz": "UTC"})

    # ---- switched off
    http.config(**{"onvif.replay": "off"})
    cam2 = ONVIFCamera(H, 80, U, P)
    await cam2.update_xaddrs()
    check(not any("ver10/search" in k for k in cam2.xaddrs), "onvif.replay=off: search service not advertised")
    code, txt = C.soap(H, "/onvif/search_service", "<tse:GetRecordingSummary/>", U, P)
    check(code == 400 and "ActionNotSupported" in txt, "onvif.replay=off: search refused")
    http.config(**{"onvif.replay": "on"})
    await cam2.close()
    await cam.close()


asyncio.run(main())
print(f"profile_g: {C.fails} failed")
sys.exit(C.fails)
