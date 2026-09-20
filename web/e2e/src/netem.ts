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
  async netem(profile: ProfileName): Promise<string> {
    const spec = NETWORK_PROFILES[profile].media;
    if (!spec.netem) {
      await this.execRoot("sh", "-c", `tc qdisc del dev ${this.iface} root 2>/dev/null || true`);
      return "cleared";
    }
    await this.execRoot("tc", "qdisc", "replace", "dev", this.iface, "root", "netem", ...spec.netem.split(" "));
    return spec.netem;
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
