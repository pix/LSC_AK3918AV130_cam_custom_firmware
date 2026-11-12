// README screenshots of the camera web UI (Playwright, Chromium).
// The camera's picture is never shown: live view, snapshots, zone/mask
// backgrounds and recording pictures are replaced by the given images, the
// Wi-Fi scan and recording list are fake, and personal values in every API
// answer (IPs, MAC, Wi-Fi name, overlay text, certificate fingerprints and
// names, MQTT ids) are rewritten inside the browser. Each shot is then checked:
// none of the original values may appear in the rendered page.
// Usage: node readme_shots.js <camera-ip> <picture1.jpg> <picture2.jpg> <outdir> [clip.webm]
//   (login admin/admin or CAMERA_USER / CAMERA_PASS; the camera must accept
//    the host name camera-1.local, i.e. its device name is camera-1)
// clip.webm (optional, for the player shot) is played for every recording, e.g.
//   ffmpeg -loop 1 -i picture1.jpg -t 6 -vf "scale=1280:720,drawtext=fontfile=mono.ttf:textfile=osd.txt:\
//     fontcolor=white:fontsize=26:box=1:boxcolor=black@0.55:x=w-tw-24:y=h-th-24" -c:v libvpx-vp9 clip.webm
//   (VP9: Playwright's Chromium may not play H.264)
const { chromium } = require('playwright');
const fs = require('fs');
const [IP, PIC1, PIC2, OUT, CLIP] = process.argv.slice(2);
if (!OUT) { console.error('usage: node readme_shots.js <camera-ip> <pic1.jpg> <pic2.jpg> <outdir> [clip.webm]'); process.exit(2); }
const HOST = 'camera-1.local';
const pics = [fs.readFileSync(PIC1), fs.readFileSync(PIC2)];
const clip = CLIP ? fs.readFileSync(CLIP) : null;

const SCRUB_INIT = () => {                       // runs in the page before its own scripts
  window.__orig = new Set();
  const rnd = (n, a) => [...Array(n)].map(() => a[Math.random() * a.length | 0]).join('');
  const keep = /"(ssid|ip|ips|mac|prefix|suffix|node|names|ap_ssid|mqtt\.host|mqtt\.topic|events\.webhook|web\.hosts|web\.proxies)":"([^"]{4,})"/g;
  const rules = [
    [/"(ssid|ap_ssid)":"[^"]*"/g, (m, k) => `"${k}":"${k === 'ssid' ? 'HomeNetwork' : 'LSC-Camera-3456'}"`],
    [/"(prefix)":"[^"]*"/g, '"prefix":"Living room"'], [/"suffix":"[^"]*"/g, '"suffix":""'],
    [/"(mqtt\.host|events\.webhook|web\.hosts|web\.proxies|names)":"[^"]+"/g, (m, k) => `"${k}":""`],
    [/\b(10|172|192)\.\d{1,3}\.\d{1,3}\.\d{1,3}\b/g, '192.168.1.50'],
    [/\b[0-9a-f]{64}\b/g, () => rnd(64, '0123456789abcdef')],
    [/\b([0-9a-f]{2}:){5}[0-9a-f]{2}\b/gi, 'd8:3e:eb:12:34:56'],
    [/(?<![0-9a-f])[0-9a-f]{12}(?![0-9a-f])/gi, 'd83eeb123456'],       // MAC without colons, also inside ids (lsc_...)
    [/CA [0-9A-F]{6}\b/g, 'CA 5A3F21'],
    [/"(ca|srv)_spki":"[^"]+"/g, (m, k) => `"${k}_spki":"${rnd(43, 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/')}="`],
    [/"default_login":true/g, '"default_login":false'],
  ];
  const orig = window.fetch.bind(window);
  window.fetch = async (input, init) => {
    const r = await orig(input, init);
    const u = typeof input === 'string' ? input : input.url;
    if (!/\/api\//.test(u) || !r.ok) return r;
    let t = await r.text();
    for (const m of t.matchAll(keep)) for (const v of m[2].split(/,\s*/)) if (v.length >= 4) {
      window.__orig.add(v);
      if (m[1] === 'mac') { window.__orig.add(v.replace(/:/g, '').toLowerCase()); window.__orig.add(v.slice(-5).replace(':', '').toUpperCase()); }
    }
    for (const m of t.matchAll(/CA ([0-9A-F]{6})\b/g)) window.__orig.add(m[1]);   // the camera CA's own id
    for (const [re, to] of rules) t = t.replace(re, to);
    return new Response(t, { status: r.status, statusText: r.statusText, headers: r.headers });
  };
};

const FAKE_SCAN = JSON.stringify([
  { ssid: 'HomeNetwork', signal: -44, secured: true }, { ssid: 'HomeNetwork-IoT', signal: -51, secured: true },
  { ssid: 'Guest', signal: -67, secured: false }]);
const day = '2026-11-12';
const FAKE_REC = JSON.stringify([
  ['18-42-10_motion.mp4', 6.1e6], ['18-42-10_motion.jpg', 48e3], ['17-05-33_sound.mp4', 3.2e6], ['17-05-33_sound.jpg', 47e3],
  ['15-20-02_event.mp4', 9.8e6], ['15-20-02_event.jpg', 51e3], ['12-00-00_timelapse.mp4', 21e6],
  ['09-12-44_motion.mp4', 4.4e6], ['09-12-44_motion.jpg', 46e3], ['08-00-00_cont.mp4', 118e6],
].map(([n, s]) => ({ d: day, n, s: Math.round(s) })));
const FAKE_DAYS = JSON.stringify({ days: [{ day, files: 10, bytes: 162.5e6 }, { day: '2026-11-11', files: 14, bytes: 241.9e6 },
  { day: '2026-11-10', files: 6, bytes: 88.4e6 }] });
const FAKE_DAY = JSON.stringify({ day, files: JSON.parse(FAKE_REC).map(f => ({ name: f.n, size: f.s })) });
const ONLY = process.env.SHOTS ? process.env.SHOTS.split(',') : null;   // e.g. SHOTS=rec_player,rec_select

(async () => {
  const b = await chromium.launch({ args: [`--host-resolver-rules=MAP ${HOST} ${IP}`] });
  let bad = 0;
  const shot = async (name, opts, fn) => {
    if (ONLY && !ONLY.includes(name)) return;
    const ctx = await b.newContext({
      httpCredentials: { username: process.env.CAMERA_USER || 'admin', password: process.env.CAMERA_PASS || 'admin' },
      ignoreHTTPSErrors: true, colorScheme: 'light', ...opts });
    await ctx.addInitScript(SCRUB_INIT);
    const p = await ctx.newPage();
    let n = 0;
    await p.route('**/*', route => {
      const path = new URL(route.request().url()).pathname;
      if (path === '/snapshot.jpg') return route.fulfill({ contentType: 'image/jpeg', body: pics[0] });
      if (path.startsWith('/rec/') && path.endsWith('.jpg')) return route.fulfill({ contentType: 'image/jpeg', body: pics[n++ % 2] });
      if (path === '/api/live/token') return route.fulfill({ status: 503, body: 'off for screenshots' });
      if (path === '/api/wifi/scan') return route.fulfill({ contentType: 'application/json', body: FAKE_SCAN });
      if (path === '/api/rec/all') return route.fulfill({ contentType: 'application/json', body: FAKE_REC });
      if (path === '/api/rec/list') return route.fulfill({ contentType: 'application/json',
        body: new URL(route.request().url()).searchParams.get('day') ? FAKE_DAY : FAKE_DAYS });
      if (path.startsWith('/rec/') && path.endsWith('.mp4'))
        return clip ? route.fulfill({ contentType: 'video/webm', body: clip }) : route.fulfill({ status: 404, body: '' });
      return route.continue();
    });
    await fn(p);
    await p.mouse.move(2, 2); await p.waitForTimeout(300);           // no hover highlight
    await p.screenshot({ path: `${OUT}/${name}.png` });
    const leaks = await p.evaluate(ip => {
      const text = document.body.innerText + ' ' + [...document.querySelectorAll('input,textarea')].map(i => i.value).join(' ') + ' ' +
        [...document.querySelectorAll('*')].flatMap(e => [...e.attributes].map(a => a.value)).join(' ');   // placeholders, titles, links
      return [...window.__orig, ip].filter(v => !['HomeNetwork', 'Living room', '192.168.1.50'].includes(v) && text.includes(v));
    }, IP);
    if (leaks.length) { bad++; console.log(`!! ${name}: original values visible: ${leaks.join(', ')}`); fs.unlinkSync(`${OUT}/${name}.png`); }
    else console.log('saved', name);
    await ctx.close();
  };
  const main = async (p, tab) => {
    await p.goto(`https://${HOST}/`, { waitUntil: 'networkidle' });
    await p.waitForTimeout(2500);
    await p.evaluate(() => {                     // picture + overlay drawn by the page, controls as when live
      document.querySelector('#badgetxt').textContent = 'Live · 1920×1080';
      document.querySelector('#badge').classList.add('live');
      document.querySelector('#livestat').textContent = '';
      const lb = document.querySelector('#livebtn');
      lb.innerHTML = '<i data-i="pause"></i><span>Pause</span>'; icon(lb.querySelector('i'), 'pause');
      osdCfg.video = 'shown-in-browser'; osdTick();
    });
    if (tab) { await p.click(`[data-tab="${tab}"]`); await p.waitForTimeout(1200); }
  };
  const desk = { viewport: { width: 1440, height: 900 }, deviceScaleFactor: 1 };
  await shot('overview', desk, p => main(p, 'image'));
  await shot('overlay', desk, p => main(p, 'overlay'));
  await shot('events', desk, async p => { await main(p, 'events'); await p.evaluate(() => document.querySelector('#zones').scrollIntoView({ block: 'end' })); await p.waitForTimeout(800); });
  await shot('integrations', desk, p => main(p, 'integrations'));
  await shot('network', desk, async p => { await main(p, 'network'); await p.click('#scan'); await p.waitForTimeout(1500); });
  await shot('security', desk, p => main(p, 'security'));
  await shot('system', desk, p => main(p, 'system'));
  await shot('dark', { ...desk, colorScheme: 'dark' }, p => main(p, 'image'));
  await shot('recordings', desk, async p => { await p.goto(`https://${HOST}/recordings`, { waitUntil: 'networkidle' }); await p.waitForTimeout(1500); });
  await shot('mobile', { viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true }, p => main(p, null));
  // recording features (settings are clicked, never saved)
  const card = (p, sel, block = 'start') => p.evaluate(([s, bl]) => document.querySelector(s).closest('.card').scrollIntoView({ block: bl }), [sel, block]);
  await shot('rec_settings', desk, async p => { await main(p, 'recordings'); await p.click('#rec_mode button[data-v="events"]'); await card(p, '#rec_mode'); await p.waitForTimeout(600); });
  await shot('rec_schedule', desk, async p => { await main(p, 'recordings'); await p.click('#sc_work'); await card(p, '#sc_grid', 'center'); await p.waitForTimeout(600); });
  await shot('rec_browse', desk, async p => {
    await main(p, 'recordings'); await p.click('#rec_days button'); await p.waitForTimeout(1500); await card(p, '#rec_files', 'center'); await p.waitForTimeout(600); });
  const recPage = async p => { await p.goto(`https://${HOST}/recordings`, { waitUntil: 'networkidle' }); await p.waitForTimeout(1200); };
  await shot('rec_select', desk, async p => {
    await recPage(p); const boxes = await p.$$('#list .row input[type="checkbox"]');
    for (const i of [0, 2, 4]) await boxes[i].click(); await p.waitForTimeout(500); });
  if (clip) await shot('rec_player', desk, async p => {
    await recPage(p); await p.click('#list .row [data-a="view"]');
    await p.waitForFunction(() => { const v = document.querySelector('#vmedia video'); return v && v.readyState >= 2; }, null, { timeout: 15000 });
    await p.evaluate(() => { const v = document.querySelector('#vmedia video'); v.pause(); v.currentTime = 2; });
    await p.waitForTimeout(800); });
  await b.close();
  process.exit(bad ? 1 : 0);
})();
