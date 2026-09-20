/**
 * The one SDP fact the core needs: per m-section, its `mid` and the
 * remote direction — to answer the agent's pre-allocated audio uplink
 * (`recvonly`) with `sendonly` and nothing else (docs/21#audio-uplink-negotiation).
 * SDP stays opaque everywhere else (docs/08: never model SDP in the schema).
 */
export type SdpDirection = "sendrecv" | "sendonly" | "recvonly" | "inactive";

export interface SdpMediaSection {
  kind: string;
  mid: string | null;
  direction: SdpDirection;
}

export function parseMediaSections(sdp: string): SdpMediaSection[] {
  const out: SdpMediaSection[] = [];
  let sessionDirection: SdpDirection = "sendrecv";
  let current: SdpMediaSection | null = null;
  for (const raw of sdp.split(/\r?\n/)) {
    const line = raw.trim();
    if (line.startsWith("m=")) {
      current = { kind: line.slice(2).split(" ")[0] ?? "", mid: null, direction: sessionDirection };
      out.push(current);
      continue;
    }
    if (!line.startsWith("a=")) continue;
    const attr = line.slice(2);
    if (attr.startsWith("mid:")) {
      if (current) current.mid = attr.slice(4);
      continue;
    }
    if (attr === "sendrecv" || attr === "sendonly" || attr === "recvonly" || attr === "inactive") {
      if (current) current.direction = attr;
      else sessionDirection = attr;
    }
  }
  return out;
}
