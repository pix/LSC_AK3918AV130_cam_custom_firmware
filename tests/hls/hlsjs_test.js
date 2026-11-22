// hls.js in a real Chrome against the camera's HLS URL (optional part of hls_test.sh).
//   node hlsjs_test.js <playlist url> <secs>
// env: PW_DIR = a node_modules dir with playwright, HLSJS = hls.min.js (npm pack hls.js),
//      CHROME = Chrome binary (H.264 needs Google Chrome, not Playwright's Chromium).
// The page is about:blank, so every request to the camera is cross-origin: this
// also checks the CORS header. Prints PASS/FAIL with playback figures.
const path = require("path");
const { chromium } = require(path.join(process.env.PW_DIR, "playwright"));
const [url, secs] = [process.argv[2], +(process.argv[3] || 30)];

(async () => {
  const browser = await chromium.launch({ executablePath: process.env.CHROME || "/usr/bin/google-chrome-stable",
    headless: true, args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
  const page = await browser.newPage();
  const errors = [];
  page.on("pageerror", e => errors.push("page: " + e.message));
  await page.setContent("<video id=v muted playsinline></video>");
  await page.addScriptTag({ path: process.env.HLSJS });
  const r = await page.evaluate(async ([url, secs]) => {
    const v = document.getElementById("v"), ev = { frag: 0, errors: [], fatal: null, levels: null, stalls: 0 };
    if (!Hls.isSupported()) return { unsupported: true };
    const h = new Hls({ lowLatencyMode: false });
    h.on(Hls.Events.MANIFEST_PARSED, (_, d) => { ev.levels = d.levels.map(l => [l.width, l.height, l.videoCodec || l.codecSet]); });
    h.on(Hls.Events.FRAG_LOADED, () => ev.frag++);
    h.on(Hls.Events.ERROR, (_, d) => { ev.errors.push(d.details + (d.fatal ? " (fatal)" : "")); if (d.fatal) ev.fatal = d.details; });
    v.addEventListener("waiting", () => ev.stalls++);
    h.loadSource(url); h.attachMedia(v);
    const t0 = performance.now();
    let start = null;
    v.addEventListener("playing", () => { if (start === null) start = { wall: performance.now(), t: v.currentTime }; }, { once: true });
    v.play().catch(() => {});
    await new Promise(res => setTimeout(res, secs * 1000));
    const q = v.getVideoPlaybackQuality();
    const out = { ...ev, firstFrameMs: start ? Math.round(start.wall - t0) : null,
      played: start ? v.currentTime - start.t : 0, wall: start ? (performance.now() - start.wall) / 1000 : 0,
      frames: q.totalVideoFrames, dropped: q.droppedVideoFrames, w: v.videoWidth, h: v.videoHeight,
      latency: h.latency, buffered: v.buffered.length };
    h.destroy();
    return out;
  }, [url, secs]);
  await browser.close();
  const bad = [];
  if (r.unsupported) bad.push("MSE/H.264 not supported in this browser");
  else {
    if (r.fatal) bad.push("fatal error " + r.fatal);
    if (r.played < r.wall * 0.9 || r.played < secs * 0.5) bad.push(`played ${r.played.toFixed(1)} s in ${r.wall.toFixed(1)} s`);
    if (!r.w) bad.push("no picture");
    if (r.buffered > 1) bad.push(`${r.buffered} buffered ranges (gaps)`);
  }
  bad.push(...errors);
  console.log(`${bad.length ? "FAIL" : "PASS"} hls.js in Chrome: ${JSON.stringify(r)}${bad.length ? " -- " + bad.join("; ") : ""}`);
  process.exit(bad.length ? 1 : 0);
})().catch(e => { console.log("FAIL hls.js in Chrome: " + e.message); process.exit(1); });
