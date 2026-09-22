/**
 * The media half of a network profile: `tc netem` inside the robot's
 * container (docs/25 — CDP emulation never touches WebRTC media). Runs
 * through the docker CLI (docker-outside-of-docker from `dev`, native on
 * a CI runner). Also the door to the docs/15 fault rows that need the
 * container: SIGSTOP/SIGCONT, restart, and the robot's loopback-only
 * introspection endpoint.
 */
import { execFile } from "node:child_process";
import { promisify } from "node:util";
import { env } from "./env.ts";
import { NETWORK_PROFILES, type ProfileName } from "./profiles.ts";

const run = promisify(execFile);

/** A compose service's address on the lab network (the harness runs inside `dev`). */
export async function containerIp(service: string): Promise<string> {
  const { stdout } = await run("docker", ["compose", "exec", "-T", service, "hostname", "-i"]);
  return stdout.trim().split(/\s+/)[0]!;
}

export class RobotContainer {
  constructor(readonly service = env.robotService, readonly iface = "eth0") {}

  async exec(...cmd: string[]): Promise<string> {
    const { stdout } = await run("docker", ["compose", "exec", "-T", this.service, ...cmd], { maxBuffer: 16 * 1024 * 1024 });
    return stdout;
  }

  /** `tc` needs CAP_NET_ADMIN, which `docker compose exec` grants only to root (the image runs as `dev`). */
  async execRoot(...cmd: string[]): Promise<string> {
    const { stdout } = await run("docker", ["compose", "exec", "-T", "--user", "root", this.service, ...cmd], { maxBuffer: 16 * 1024 * 1024 });
    return stdout;
  }

  async isUp(): Promise<boolean> {
    try {
      const out = await run("docker", ["compose", "ps", "--status", "running", "--services"]);
      return out.stdout.split("\n").includes(this.service);
    } catch {
      return false;
    }
  }

  /** Apply the profile's netem spec (replacing whatever was there); `lan` clears it. */
  async netem(profile: ProfileName, iface: string = this.iface): Promise<string> {
    const spec = NETWORK_PROFILES[profile].media;
    if (!spec.netem) {
      await this.execRoot("sh", "-c", `tc qdisc del dev ${iface} root 2>/dev/null || true`);
      return "cleared";
    }
    // The impairment goes on a prio band the introspection port is filtered out of (docs/25: the
    // harness keeps observing the robot through a bad link), as docker/lab/netem.sh does.
    const port = "7381";
    await this.execRoot("tc", "qdisc", "replace", "dev", iface, "root", "handle", "1:", "prio", "bands", "3", "priomap", ..."1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1".split(" "));
    await this.execRoot("tc", "qdisc", "replace", "dev", iface, "parent", "1:2", "handle", "20:", "netem", ...spec.netem.split(" "));
    await this.execRoot("tc", "filter", "replace", "dev", iface, "parent", "1:", "protocol", "ip", "prio", "1", "handle", "800::1", "u32", "match", "ip", "dport", port, "0xffff", "flowid", "1:1");
    await this.execRoot("tc", "filter", "replace", "dev", iface, "parent", "1:", "protocol", "ip", "prio", "1", "handle", "800::2", "u32", "match", "ip", "sport", port, "0xffff", "flowid", "1:1");
    return spec.netem;
  }

  /**
   * The profile's impairment on the robot's egress toward ONE address only (docs/23 slice 6a gate 2:
   * one viewer behind a bad link while the others stay clean). The introspection port stays exempt.
   */
  async netemToward(profile: ProfileName, dstIp: string, iface: string = this.iface): Promise<string> {
    const spec = NETWORK_PROFILES[profile].media;
    if (!spec.netem) {
      await this.execRoot("sh", "-c", `tc qdisc del dev ${iface} root 2>/dev/null || true`);
      return "cleared";
    }
    const port = "7381";
    // Everything to band 1:1 (a plain queue); only packets to dstIp go to the netem on band 1:2.
    await this.execRoot("tc", "qdisc", "replace", "dev", iface, "root", "handle", "1:", "prio", "bands", "3", "priomap", ..."0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0".split(" "));
    await this.execRoot("tc", "qdisc", "replace", "dev", iface, "parent", "1:2", "handle", "20:", "netem", ...spec.netem.split(" "));
    // No explicit handles: u32 keeps one hash table per priority, and clearing deletes the root with every filter.
    await this.execRoot("tc", "filter", "add", "dev", iface, "parent", "1:", "protocol", "ip", "prio", "1", "u32", "match", "ip", "sport", port, "0xffff", "flowid", "1:1");
    await this.execRoot("tc", "filter", "add", "dev", iface, "parent", "1:", "protocol", "ip", "prio", "2", "u32", "match", "ip", "dst", `${dstIp}/32`, "flowid", "1:2");
    return `${profile} toward ${dstIp}`;
  }

  async netemStatus(): Promise<string> {
    return (await this.execRoot("tc", "qdisc", "show", "dev", this.iface)).trim();
  }

  /** docs/24 endpoint is loopback-only on the robot: fetch it from inside. `null` until slice 3b ships it. */
  async introspect(path = "/pipelines"): Promise<unknown | null> {
    try {
      const out = await this.exec("curl", "-fsS", `http://127.0.0.1:7381${path}`);
      return path.endsWith(".txt") || path.endsWith(".dot") ? out : (JSON.parse(out) as unknown);
    } catch {
      return null;
    }
  }

  /** docs/15 "whole-process hang": SIGSTOP the agent process (PID 1 of the service container). */
  async freeze(): Promise<void> {
    await run("docker", ["compose", "pause", this.service]);
  }

  async thaw(): Promise<void> {
    await run("docker", ["compose", "unpause", this.service]);
  }

  async restart(): Promise<void> {
    await run("docker", ["compose", "restart", this.service]);
  }
}
