/**
 * Presentation-mode spike (M3 3.6, open question #19, docs/22#presentation-mode): can a popup window
 * show a MediaStream that lives in its opener, so presentation mode can portal one session's tracks
 * into a window per robot monitor? Measured in the lab's Chromium, under the COOP headers host apps set.
 *
 * The opener receives a real WebRTC stream (a loopback RTCPeerConnection pair carrying a canvas
 * track), then opens a popup two ways:
 *   (a) "page":   the popup loads its own same-origin page, which takes `window.opener.remoteStream`;
 *   (b) "portal": an empty popup (`window.open("")`) in which the OPENER creates the <video>, as a
 *                 React portal into the popup's document would.
 * Each popup counts rendered frames with requestVideoFrameCallback for 2 s. Findings go to
 * spikes/presentation-mode/README.md; the test only records, it does not gate.
 */
import { test } from "../../src/fixtures.ts";

const ORIGIN = "https://spike.fjarr.test";

const OPENER = /* html */ `<!doctype html><meta charset="utf-8"><title>opener</title>
<button id="go">open</button><canvas id="c" width="320" height="180"></canvas>
<script>
const c = document.getElementById("c"), g = c.getContext("2d");
let n = 0; setInterval(() => { g.fillStyle = "hsl(" + (n++ * 7 % 360) + ",80%,50%)"; g.fillRect(0, 0, 320, 180); }, 33);
async function loopback() {
  const a = new RTCPeerConnection(), b = new RTCPeerConnection();
  a.onicecandidate = (e) => e.candidate && b.addIceCandidate(e.candidate);
  b.onicecandidate = (e) => e.candidate && a.addIceCandidate(e.candidate);
  const got = new Promise((ok) => (b.ontrack = (e) => ok(e.streams[0] ?? new MediaStream([e.track]))));
  const track = c.captureStream(30).getVideoTracks()[0];
  a.addTrack(track, new MediaStream([track]));
  await a.setLocalDescription(await a.createOffer());
  await b.setRemoteDescription(a.localDescription);
  await b.setLocalDescription(await b.createAnswer());
  await a.setRemoteDescription(b.localDescription);
  return got;
}
window.ready = loopback().then((s) => (window.remoteStream = s));
window.countFrames = (video) => new Promise((ok) => {
  let frames = 0; const t0 = performance.now();
  const step = () => { frames++; if (performance.now() - t0 < 2000) video.requestVideoFrameCallback(step); else ok(frames); };
  video.requestVideoFrameCallback(step);
  setTimeout(() => ok(frames), 4000);
});
window.openTwoPortals = async () => {
  await window.ready;
  const ws = [window.open("", "p1", "width=400,height=260,left=0"), window.open("", "p2", "width=400,height=260,left=500")];
  if (ws.some((w) => !w)) return { error: "window.open returned null for " + ws.filter((w) => !w).length };
  const counts = await Promise.all(ws.map(async (w) => {
    const v = w.document.createElement("video");
    v.muted = true; v.autoplay = true; v.playsInline = true;
    w.document.body.appendChild(v);
    v.srcObject = window.remoteStream;
    await v.play().catch(() => {});
    return window.countFrames(v);
  }));
  setTimeout(() => ws.forEach((w) => w.close()), 100);
  return { frames: counts, visibility: ws.map((w) => w.document.visibilityState) };
};
window.openPortal = async () => {
  await window.ready;
  const w = window.open("", "portal", "width=400,height=260");
  if (!w) return { error: "window.open returned null" };
  try {
    const v = w.document.createElement("video");
    v.muted = true; v.autoplay = true; v.playsInline = true;
    w.document.body.appendChild(v);
    v.srcObject = window.remoteStream;
    await v.play().catch(() => {});
    const frames = await window.countFrames(v);
    return { frames, w: v.videoWidth, h: v.videoHeight };
  } catch (e) { return { error: String(e) }; } finally { setTimeout(() => w.close(), 100); }
};
document.getElementById("go").onclick = () => { window.popup = window.open("${ORIGIN}/popup.html", "page", "width=400,height=260"); };
</script>`;

const POPUP = /* html */ `<!doctype html><meta charset="utf-8"><title>popup</title><video autoplay muted playsinline></video>
<script>
window.result = (async () => {
  try {
    if (!window.opener) return { error: "no window.opener (a separate browsing context group)" };
    await window.opener.ready;
    const v = document.querySelector("video");
    v.srcObject = window.opener.remoteStream;
    await v.play().catch(() => {});
    let frames = 0; const t0 = performance.now();
    await new Promise((ok) => { const step = () => { frames++; if (performance.now() - t0 < 2000) v.requestVideoFrameCallback(step); else ok(); }; v.requestVideoFrameCallback(step); setTimeout(ok, 4000); });
    return { frames, w: v.videoWidth, h: v.videoHeight };
  } catch (e) { return { error: String(e) }; }
})();
</script>`;

const variants: Array<{ name: string; opener: Record<string, string>; popup: Record<string, string> }> = [
  { name: "no COOP", opener: {}, popup: {} },
  { name: "COOP same-origin (both)", opener: { "cross-origin-opener-policy": "same-origin" }, popup: { "cross-origin-opener-policy": "same-origin" } },
  { name: "COOP same-origin (opener only)", opener: { "cross-origin-opener-policy": "same-origin" }, popup: {} },
  { name: "COOP same-origin-allow-popups", opener: { "cross-origin-opener-policy": "same-origin-allow-popups" }, popup: {} },
  {
    name: "COOP same-origin + COEP require-corp (cross-origin isolated)",
    opener: { "cross-origin-opener-policy": "same-origin", "cross-origin-embedder-policy": "require-corp" },
    popup: { "cross-origin-opener-policy": "same-origin", "cross-origin-embedder-policy": "require-corp" },
  },
];

const within = <T,>(p: Promise<T>, ms: number, what: string): Promise<T | { error: string }> =>
  Promise.race([p, new Promise<{ error: string }>((ok) => setTimeout(() => ok({ error: `${what}: no answer within ${ms} ms` }), ms))]);

for (const v of variants) {
  test(`presentation spike: ${v.name}`, async ({ page }) => {
    const context = page.context();
    await context.route(`${ORIGIN}/**`, (route) => {
      const path = new URL(route.request().url()).pathname;
      const isPopup = path === "/popup.html";
      void route.fulfill({ status: 200, contentType: "text/html", headers: isPopup ? v.popup : v.opener, body: isPopup ? POPUP : OPENER });
    });
    await page.goto(`${ORIGIN}/opener.html`);
    await page.evaluate(() => (window as unknown as { ready: Promise<unknown> }).ready);

    // (b) the opener builds the <video> inside an empty popup (a portal).
    const b = await within(page.evaluate(() => (window as unknown as { openPortal: () => Promise<unknown> }).openPortal()), 8000, "portal");
    console.log(`SPIKE ${v.name} | portal: ${JSON.stringify(b)}`);

    // (a) the popup loads its own page and takes the opener's stream.
    const popupEvent = context.waitForEvent("page", { timeout: 5000 }).catch(() => null);
    await page.click("#go");
    const popup = await popupEvent;
    let a: unknown = { error: "no popup page" };
    if (popup) {
      await popup.waitForLoadState("load", { timeout: 5000 }).catch(() => undefined);
      a = await within(popup.evaluate(() => (window as unknown as { result: Promise<unknown> }).result), 8000, "popup result");
      await popup.close().catch(() => undefined);
    }
    console.log(`SPIKE ${v.name} | page: ${JSON.stringify(a)}`);
  });
}

test("presentation spike: two portal windows at once, one stream", async ({ page }) => {
  const context = page.context();
  await context.route(`${ORIGIN}/**`, (route) => void route.fulfill({ status: 200, contentType: "text/html", headers: { "cross-origin-opener-policy": "same-origin" }, body: OPENER }));
  await page.goto(`${ORIGIN}/opener.html`);
  await page.evaluate(() => (window as unknown as { ready: Promise<unknown> }).ready);
  const r = await within(page.evaluate(() => (window as unknown as { openTwoPortals: () => Promise<unknown> }).openTwoPortals()), 12000, "two portals");
  console.log(`SPIKE two portals (COOP same-origin), no permission | ${JSON.stringify(r)}`);
  // Chrome lets a page with the Window Management permission open several popups from one gesture.
  const granted = await context.grantPermissions(["window-management"], { origin: ORIGIN }).then(() => "granted", (e) => `not grantable: ${e.message.split("\n")[0]}`);
  const state = await page.evaluate(() => navigator.permissions.query({ name: "window-management" as PermissionName }).then((p) => p.state, (e) => String(e)));
  const screens = await page.evaluate(async () => {
    const w = window as unknown as { getScreenDetails?: () => Promise<{ screens: unknown[] }> };
    return w.getScreenDetails ? (await w.getScreenDetails()).screens.length : "no getScreenDetails";
  }).catch((e) => String(e));
  const r2 = await within(page.evaluate(() => (window as unknown as { openTwoPortals: () => Promise<unknown> }).openTwoPortals()), 12000, "two portals");
  console.log(`SPIKE two portals, window-management ${granted} (state ${state}, screens ${screens}) | ${JSON.stringify(r2)}`);
});
