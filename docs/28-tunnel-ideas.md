---
title: What You Can Run Over the Tunnel
description: Ideas for fjarr.net — shells, desktops, files, debuggers, ROS, field buses, USB devices, monitoring — anything that speaks IP reaches the robot unchanged, and devices become IP through open protocols. What is verified, what is an idea, and what to watch for.
---

`fjarr.net` gives your machine and one robot a private IP link
([docs/27](27-network-tunnel.md)). Fjarr does not know or care what travels on
it. **Anything that speaks TCP, UDP or multicast works unchanged, including
things nobody at Fjarr thought of.** This page is a starting point for ideas.
Most rows are untested ideas; the ones marked **✔ verified** have been run over
the tunnel in Fjarr's own lab or on a real robot.

## How to read this page

Every row below has a short step-by-step in
[How-tos over the tunnel](29-tunnel-howtos.md), in the same order. Bring the
link up and point any tool at the robot's tunnel address:

```console
$ fjarr-connect robot-024
robot-024  100.70.118.224  mtu 1184  up in 0.8 s
  ssh <user>@100.70.118.224
```

What the link is like, so you can judge a use before trying it:

- **It reaches the robot itself, not its network.** Traffic goes only to the
  robot's own tunnel address, and the robot never forwards it
  ([docs/27](27-network-tunnel.md#isolation)). For an IP camera, a PLC or a lidar
  on the robot's LAN, hop through something on the robot that you run on
  purpose, for example `ssh -J user@<robot> user@192.168.1.20`, or `ssh -L` for
  one port.
- **The robot can reach your end too.** While the link is up, services listening
  on your machine's tunnel address, or on all interfaces, are reachable from the
  robot. That is useful for serving it a file, and something to be aware of.
- **IPv4, TCP, UDP and multicast** (so DDS and mDNS discovery work,
  [ADR-0026](adr/0026-multicast-over-the-tunnel.md)). **Layer-2 protocols do not
  cross**: EtherCAT, PROFINET RT and raw Ethernet frames need a real wire.
- **MTU 1184, and on a LAN about 200–340 Mbit/s** (measured beside and without a
  camera stream). Over the internet it is whatever the path gives, adaptive
  with the rest of the session. TCP handles loss for you; plain UDP tools see the
  real loss of the path.
- **Every port on the robot is reachable** unless the robot's config narrows it
  with `allow_ports`. Every service you run is a second door with its own login.
  The [safety notes](#safety) are at the end.

## Shells and administration

| What | Tool | Notes |
|---|---|---|
| A shell | `ssh`, **✔ verified** | also `fjarr-connect shell` with no sshd at all ([docs/27](27-network-tunnel.md#shell)) |
| A shell that survives a flaky link | `mosh` (UDP) | predictive echo and roaming; good on 4G, where ssh stalls |
| Persistent work | `tmux` / `screen` over ssh | reattach after the link drops |
| A web admin console | Cockpit (port 9090), Webmin | logs, services, updates, a terminal, in your browser |
| Fleet automation | Ansible, Salt over ssh | `fjarr-connect list --ssh-config` gives stanzas an inventory can use |
| Containers on the robot | `DOCKER_HOST=ssh://user@<robot> docker ps`, `podman --remote` | your local CLI, the robot's daemon |
| k3s on the robot | `kubectl` with the robot's API port | a cluster of one, managed from your laptop |

## Desktops and GUI applications

| What | Tool | Notes |
|---|---|---|
| The robot's desktop | RDP (Remmina, Windows Remote Desktop) to gnome-remote-desktop, **✔ verified** | a second door with its own password ([docs/27](27-network-tunnel.md#byo-remote-desktop)); Fjarr's own answer is `fjarr.desktop` in the browser |
| An X11 kiosk's desktop | VNC (`x11vnc`, port 5900) | |
| One application, not the desktop | `waypipe` (Wayland), `ssh -X` (X11) | the robot's rviz or a vendor's configuration tool in a window on your screen |
| Seamless apps over a slow link | Xpra | compresses and survives reconnects, better than raw X11 over WAN |

## Files and storage

| What | Tool | Notes |
|---|---|---|
| Browse the robot in a file manager | Nautilus/Dolphin/Finder `sftp://user@<robot>/`, **✔ verified** | Fjarr's own answer is the WebDAV drive ([ADR-0029](adr/0029-robot-files-as-a-webdav-drive.md)) |
| Copy files, logs, bags | `scp` **✔ verified**, `rsync` | rsync resumes: fetch a 20 GB rosbag over a flaky link |
| Mount the robot's disk | `sshfs`, NFS, SMB | edit robot files with your local editor |
| Sync a folder both ways | Syncthing | maps, calibration files, configs |
| A block device | NBD, iSCSI | image or restore a robot's disk from the office |
| Serve a file to the robot | `python3 -m http.server` on your machine, then `curl http://<your tunnel address>:8000/…` on the robot | a firmware image or a package, without uploading it anywhere |

## Development and debugging

| What | Tool | Notes |
|---|---|---|
| Edit and run on the robot from your IDE | VS Code Remote-SSH, JetBrains Gateway | the editor runs locally, the code runs where the hardware is |
| Debug a process on the robot | `gdbserver`, `lldb-server`, Python `debugpy` | breakpoints in your local IDE, the program on the robot |
| Debug the robot's microcontroller | OpenOCD's or pyOCD's GDB port, SEGGER `JLinkRemoteServer` | the programmer is plugged into the robot; the debugger is on your desk |
| Profile | `perf record` on the robot then `perf report` locally, the Tracy client and server, `py-spy` | |
| Notebooks on robot data | Jupyter on the robot, the browser on your side | data too big to copy stays put |
| Live packet capture from the robot | Wireshark with `sshdump` | the robot's interfaces in your local Wireshark, live |
| Measure the link | `iperf3` | what the tunnel gives on this path, today |

## Robotics middleware

| What | Tool | Notes |
|---|---|---|
| ROS 2 | `ros2 topic`, rviz2, `rqt` with DDS over the link, **✔ verified** (Fast DDS and Cyclone DDS) | the ordering rule and the Cyclone file matter ([docs/27](27-network-tunnel.md#ros2)) |
| ROS 2 across a slower link | Zenoh (`zenoh-bridge-ros2dds`) | built for WAN; far less discovery traffic than DDS |
| ROS 1 | the master's port 11311 plus the node ports; set `ROS_IP` | |
| A visualisation GUI | Foxglove with `foxglove_bridge` (WebSocket, port 8765), Rerun viewer, PlotJuggler | the robot's live topics in a desktop application |
| Web tools on ROS | `rosbridge_server` (WebSocket 9090) | roslibjs dashboards on your machine against the real robot |
| Micro-controllers on ROS 2 | the micro-ROS agent's UDP port | an MCU talking to the robot's graph |
| A message broker | MQTT (Mosquitto), NATS | the robot's broker from your tools; or yours from the robot |

## Field buses and hardware links

| What | Tool | Notes |
|---|---|---|
| The robot's CAN bus on your laptop | `cannelloni` (UDP) or `socketcand` | `candump`, SavvyCAN or a vendor tool against the real bus |
| A serial port | `ser2net` (RFC 2217), `socat` | a motor controller's or a GPS's console in your terminal |
| **Any USB device** | **USB/IP** (in the Linux kernel: `usbip` on the robot, `vhci-hcd` on your machine; usbip-win on Windows) | the device appears on your machine as if plugged in. **Excellent for bulk devices**: USB-serial adapters, ST-Link and J-Link programmers, CAN adapters, dongles, storage. Flash the robot's microcontroller with your own tools. **Poor for isochronous devices** such as webcams and audio interfaces, which expect microsecond timing and a lot of bandwidth |
| Industrial protocols | Modbus/TCP, OPC UA | a PLC's or drive's configuration software against the real device |
| A device on the robot's own network | `ssh -J` / `ssh -L` through the robot | the lidar's configuration page, an IP camera's web UI |

## Sensors and vendor tools

| What | Tool | Notes |
|---|---|---|
| A sensor's own web page | the robot's sensors often serve one (lidars, industrial cameras, IMUs); reach it on the robot or via `ssh -L` | set up the sensor from your desk |
| A vendor's desktop viewer | point it at the robot's address | many speak TCP; sensors that stream raw UDP (a lidar at 50–130 Mbit/s) fit on a LAN, not on 4G |
| An RTSP camera on the robot | VLC, ffplay | for watching in the browser, `fjarr.camera` adapts to the link and this does not |

## Monitoring and observability

| What | Tool | Notes |
|---|---|---|
| Metrics | Prometheus `node_exporter` (9100), Netdata, Glances, cAdvisor | scrape one robot from your laptop while you investigate |
| Dashboards | Grafana on the robot or on your machine | |
| Logs | `journalctl` over ssh, systemd-journal-remote, a local Loki | |
| The agent's own view | the introspection endpoint (docs/24), when bound to the tunnel address | the robot's live pipelines on the robot's own address |

## Audio

The robot's microphone or speaker as an audio device on a Linux machine works
through PipeWire's or PulseAudio's network modules, and through ROC, which was
built for lossy links. It is fine on a good LAN, and clicks and drifts over the
internet. To hear and talk to a robot from a browser, `fjarr.audio`
([M3.5](17-roadmap.md#m35)) uses WebRTC's jitter buffers and echo
cancellation, and is the better path.

## Why layer 2 does not cross {#layer-2}

Networking is layered. **Layer 2** is the local network: Ethernet frames
addressed by MAC address within one broadcast domain, where ARP, DHCP,
broadcasts, Wake-on-LAN and industrial protocols such as EtherCAT and PROFINET
RT live. **Layer 3** is IP, routed between networks, with TCP and UDP on top:
almost every application.

`fjarr.net` is a layer-3 link: a TUN device carrying IP packets between exactly
two addresses, not a TAP device carrying Ethernet frames. That is deliberate:

- **Isolation is built on IP.** A layer-2 link would plug your laptop into the
  robot's local network, with its PLC, its cameras and its broadcasts. That is
  the lateral access the tunnel exists to rule out
  ([docs/27](27-network-tunnel.md#isolation)).
- **The protocols that truly need layer 2 cannot work over the internet
  anyway.** EtherCAT and PROFINET RT need microsecond, deterministic timing on a
  dedicated wire; an internet path adds tens of milliseconds of jitter.
- **It keeps the link predictable**: one MTU, no broadcast traffic, no stranger's
  device flooding it.

What is lost is small: broadcast-based discovery in some vendor tools, and
Wake-on-LAN. Run the tool on the robot, enter the device's IP address by hand,
or hop through the robot to one device ([how-to](29-tunnel-howtos.md#a-device-on-the-robots-network)).

## What does not fit

- **Layer 2** ([above](#layer-2)): EtherCAT, PROFINET RT, raw Ethernet frames,
  broadcasts, Wake-on-LAN.
- **Timing-critical USB**: webcams, audio interfaces and anything isochronous
  over USB/IP. The camera and audio capabilities are the way to get those.
- **Reaching the robot's LAN directly**: only through a hop on the robot that
  you run on purpose. That is the isolation rule, not a limitation to work around.
- **Hard real-time control loops.** The link has an internet path's latency and
  jitter. Teleoperation belongs to Fjarr's input channels, with their deadman.

## Safety {#safety}

- **Granting `fjarr.net` grants network access to the robot from inside**
  ([docs/10](10-security.md#network-tunnel)). Everything you install on the
  robot becomes reachable to whoever holds that grant. Narrow the robot's
  `allow_ports` to what you actually use.
- **Each tool is a second door.** RDP, VNC, Cockpit and databases have their own
  logins. Fjarr audits the link, not what happens inside it.
- **USB/IP exports hardware.** A robot that can export a USB **keyboard** to an
  operator's machine can type on it. Export only named devices (vendor and
  product ids), and never automatically.
- **The robot can reach your end.** Do not leave services on your machine
  listening on all interfaces while a link is up, unless you mean the robot to
  use them.

Tried something that is not here, or that works differently from how this page
says? It belongs on this page.
