// End-to-end push-to-talk: headless Chrome + fake mic (440 Hz file) on the camera page over HTTPS.
// usage: node talktest.js <dir> <url> <user> <pass> <hold_ms>
const { spawn } = require("child_process");
const sleep = ms => new Promise(r => setTimeout(r, ms));
const [dir, url, user, pass, hold, micfile] = process.argv.slice(2);
(async () => {
  const chrome = spawn("google-chrome-stable", ["--headless=new", "--no-sandbox", "--disable-gpu", "--remote-debugging-port=9334",
    "--ignore-certificate-errors", "--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream",
    "--use-file-for-fake-audio-capture=" + (micfile || dir + "/tone440.wav"), "--autoplay-policy=no-user-gesture-required",
    "--user-data-dir=" + dir + "/prof", "about:blank"], { stdio: "ignore" });
  let ver; for (let i = 0; i < 40 && !ver; i++) { await sleep(250); try { ver = await (await fetch("http://127.0.0.1:9334/json/version")).json(); } catch (e) {} }
  const tgt = await (await fetch("http://127.0.0.1:9334/json/new?about:blank", { method: "PUT" })).json();
  const ws = new WebSocket(tgt.webSocketDebuggerUrl); let id = 0; const pend = {};
  const send = (method, params = {}) => new Promise(r => { const i = ++id; pend[i] = r; ws.send(JSON.stringify({ id: i, method, params })); });
  ws.onmessage = e => { const m = JSON.parse(e.data);
    if (m.id && pend[m.id]) { pend[m.id](m.result || m.error); delete pend[m.id]; return; }
    if (m.method === "Fetch.authRequired") send("Fetch.continueWithAuth", { requestId: m.params.requestId, authChallengeResponse: { response: "ProvideCredentials", username: user, password: pass } });
    else if (m.method === "Fetch.requestPaused") send("Fetch.continueRequest", { requestId: m.params.requestId });
    else if (m.method === "Runtime.consoleAPICalled") console.log("console:", m.params.args.map(a => a.value).join(" "));
  };
  await new Promise(r => ws.onopen = r);
  const ev = async x => { const r = await send("Runtime.evaluate", { expression: x, returnByValue: true, awaitPromise: true }); return r.result ? r.result.value : r; };
  await send("Fetch.enable", { handleAuthRequests: true, patterns: [{ urlPattern: "*" }] });
  await send("Runtime.enable"); await send("Page.enable");
  await send("Page.addScriptToEvaluateOnNewDocument", { source: `(()=>{window.__sent=[];const o=WebSocket.prototype.send;
    WebSocket.prototype.send=function(d){if(d instanceof ArrayBuffer)window.__sent.push(...new Int16Array(d));return o.call(this,d)}})()` });
  await send("Page.navigate", { url });
  for (let i = 0; i < 20; i++) { await sleep(500); const b = await ev(`document.querySelector("#badgetxt")?.textContent||""`); if (/Live/.test(b)) break; }
  console.log("state:", JSON.stringify(await ev(`({secure:window.isSecureContext,badge:document.querySelector("#badgetxt").textContent,talkVisible:!document.querySelector("#talkbtn").hidden})`)));
  const r = await ev(`(()=>{const b=document.querySelector("#talkbtn");b.scrollIntoView({block:"center"});const r=b.getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()`);
  const mouse = (type) => send("Input.dispatchMouseEvent", { type, x: r.x, y: r.y, button: "left", buttons: type === "mousePressed" ? 1 : 0, clickCount: 1 });
  await send("Input.dispatchMouseEvent", { type: "mouseMoved", x: r.x, y: r.y });
  const mode = process.env.CLICK ? "click" : "hold";
  await mouse("mousePressed"); if (mode === "click") await mouse("mouseReleased");
  console.log("talk down (" + mode + ")");
  await sleep(1000);
  console.log("during:", JSON.stringify(await ev(`({active:document.querySelector("#talkbtn").classList.contains("active"),label:document.querySelector("#talkbtn span").textContent,toast:document.querySelector("#toast").textContent})`)));
  await sleep(Number(hold) - 1000);
  if (mode === "click") { await mouse("mousePressed"); await mouse("mouseReleased"); } else await mouse("mouseReleased");
  await sleep(200);
  console.log("talk up:", await ev(`document.querySelector("#talkbtn").classList.contains("active")?"still active":"released"`));
  const pcm = await ev(`(()=>{const a=Int16Array.from(window.__sent);let s="";const u=new Uint8Array(a.buffer);for(let i=0;i<u.length;i+=8192)s+=String.fromCharCode(...u.subarray(i,i+8192));return btoa(s)})()`);
  require("fs").writeFileSync(dir + "/sent.raw", Buffer.from(pcm || "", "base64"));
  console.log("talk-back samples sent:", Buffer.from(pcm || "", "base64").length / 2);
  await sleep(500); ws.close(); chrome.kill();
})().catch(e => { console.error(e); process.exit(1); });
