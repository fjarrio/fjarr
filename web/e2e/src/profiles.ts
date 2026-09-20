/**
 * Network profiles (docs/25#the-harness): the browser half is CDP
 * `Network.emulateNetworkConditions` (HTTP + WebSocket only — never WebRTC
 * media), the media half is `tc netem` on the robot's interface.
 */
export type ProfileName = "lan" | "wifi-ok" | "4g" | "lossy" | "bad" | "offline" | "relay-only";

export interface BrowserConditions {
  offline: boolean;
  /** ms */
  latency: number;
  /** bytes/s, -1 = unlimited */
  downloadThroughput: number;
  uploadThroughput: number;
}

export interface NetemSpec {
  /** `tc qdisc … netem` arguments; empty = no qdisc. `loss 100%` is the offline media path. */
  netem: string;
}

export interface NetworkProfile {
  browser: BrowserConditions;
  media: NetemSpec;
  /** Client option to pair with the profile (relay-only). */
  client?: { iceTransportPolicy: "all" | "relay" };
}

const mbit = (n: number) => Math.round((n * 1_000_000) / 8);
const kbit = (n: number) => Math.round((n * 1_000) / 8);

export const NETWORK_PROFILES: Record<ProfileName, NetworkProfile> = {
  lan: { browser: { offline: false, latency: 0, downloadThroughput: -1, uploadThroughput: -1 }, media: { netem: "" } },
  "wifi-ok": { browser: { offline: false, latency: 20, downloadThroughput: mbit(50), uploadThroughput: mbit(20) }, media: { netem: "delay 10ms 3ms" } },
  "4g": { browser: { offline: false, latency: 80, downloadThroughput: mbit(10), uploadThroughput: mbit(3) }, media: { netem: "delay 40ms 10ms rate 8mbit" } },
  lossy: { browser: { offline: false, latency: 50, downloadThroughput: mbit(5), uploadThroughput: mbit(2) }, media: { netem: "loss 5% delay 30ms 15ms" } },
  bad: { browser: { offline: false, latency: 200, downloadThroughput: mbit(1), uploadThroughput: kbit(500) }, media: { netem: "loss 15% delay 100ms 40ms rate 1.5mbit" } },
  offline: { browser: { offline: true, latency: 0, downloadThroughput: 0, uploadThroughput: 0 }, media: { netem: "loss 100%" } },
  "relay-only": { browser: { offline: false, latency: 0, downloadThroughput: -1, uploadThroughput: -1 }, media: { netem: "" }, client: { iceTransportPolicy: "relay" } },
};

export function describeProfile(name: ProfileName): string {
  const p = NETWORK_PROFILES[name];
  const b = p.browser.offline
    ? "browser: offline"
    : `browser: ${p.browser.latency} ms / ${p.browser.downloadThroughput < 0 ? "unlimited" : `${((p.browser.downloadThroughput * 8) / 1e6).toFixed(1)} Mbps`} down / ${p.browser.uploadThroughput < 0 ? "unlimited" : `${((p.browser.uploadThroughput * 8) / 1e6).toFixed(1)} Mbps`} up`;
  const m = p.media.netem ? `media (netem): ${p.media.netem}` : "media: none";
  return `${name}: ${b}; ${m}${p.client ? `; client: iceTransportPolicy=${p.client.iceTransportPolicy}` : ""}`;
}
