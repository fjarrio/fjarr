---
title: How-tos Over the Tunnel
description: Short, step-by-step recipes for using fjarr.net — ssh and mosh, remote desktops, file sync, remote debugging, ROS 2 and Foxglove, CAN, serial, USB/IP, industrial protocols, monitoring and more — each in a few commands.
---

Recipes for the ideas in [What you can run over the tunnel](28-tunnel-ideas.md),
in the same order. Each is a starting point: the goal, what to run on the robot,
what to run on your machine, and the usual pitfall. **✔ verified** means it has
been run over the tunnel; every other recipe is untested and follows the tool's
own documentation.

**Conventions.** Commands are for Ubuntu. `<robot>` is the robot's tunnel
address and `<you>` is yours; `fjarr-connect` prints both when the link comes
up. `user` is your account on the robot. Keep `fjarr-connect` running while you
work: the link lives as long as it does.

```console
$ fjarr-connect robot-024
  created fjarr0 100.64.0.1                        ← <you>
robot-024  100.70.118.224  mtu 1184  up in 0.8 s   ← <robot>
```

If the robot's config narrows `allow_ports`
([docs/27](27-network-tunnel.md#isolation)), add the port each recipe uses.

## Shells and administration

### ssh

**✔ verified.** A shell on the robot.

```sh
ssh user@<robot>
```

No sshd on the robot? `fjarr-connect shell robot-024` gives you its terminal
without one ([docs/27](27-network-tunnel.md#shell)). **Pitfall:** the robot needs
`openssh-server` (`sudo apt install openssh-server`).

### mosh: a shell that survives a bad link

ssh freezes when a 4G link hiccups. mosh keeps typing responsive and reconnects
by itself.

```sh
# robot
sudo apt install mosh
# you
sudo apt install mosh
mosh user@<robot>
```

**Pitfall:** mosh uses UDP ports 60000–61000 after its ssh login; allow them if
`allow_ports` is set.

### tmux: work that outlives the link

```sh
ssh user@<robot>
tmux new -A -s work        # attach to "work", or create it
```

Detach with `Ctrl-b d`. If the link drops, reconnect and run the same command:
your processes kept running on the robot.

### Cockpit: a web admin console

```sh
# robot
sudo apt install cockpit
```

Open `https://<robot>:9090` and log in with the robot's account. You get logs,
services, updates, storage and a terminal. **Pitfall:** accept the self-signed
certificate the first time.

### Ansible across the fleet

```sh
fjarr-connect list --ssh-config >> ~/.ssh/config     # Host robot-024 / HostName 100.70.…
cat > inventory.ini <<'EOF'
[robots]
robot-024 ansible_user=user
robot-031 ansible_user=user
EOF
ansible -i inventory.ini robots -m ping
```

A robot's tunnel address is derived from its id and never changes, so the
stanzas stay valid. **Pitfall:** each robot's link must be up; bring several up
with `fjarr-connect robot-024 robot-031`.

### Docker on the robot, from your CLI

```sh
DOCKER_HOST=ssh://user@<robot> docker ps
DOCKER_HOST=ssh://user@<robot> docker logs -f my-node
```

**Pitfall:** `user` must be in the robot's `docker` group.

### kubectl for k3s on the robot

```sh
# robot: install k3s so its certificate is valid for the tunnel address
curl -sfL https://get.k3s.io | sh -s - --tls-san <robot>
sudo cat /etc/rancher/k3s/k3s.yaml
# you: save it, then point it at the robot
sed -i 's/127.0.0.1/<robot>/' k3s.yaml
KUBECONFIG=k3s.yaml kubectl get pods -A
```

**Pitfall:** without `--tls-san`, kubectl rejects the certificate.

## Desktops and GUI applications

### RDP to the robot's GNOME desktop

**✔ verified** (Remmina on the mini-PC, 2026-09-30).

1. On the robot: Settings → System → Remote Desktop → turn on **Desktop
   Sharing** and **Remote Control**, and set a username and password under
   *Login Details*.
2. On your machine: Remmina → new connection → **RDP**, server `<robot>`, the
   username and password from step 1. Leave *Domain* empty.

**Pitfall:** these are the RDP credentials, not your login. The robot may ask
to create a keyring the first time.

### VNC to an X11 kiosk

```sh
# robot
sudo apt install x11vnc
x11vnc -storepasswd                                  # once
x11vnc -display :0 -rfbauth ~/.vnc/passwd -forever
# you
vncviewer <robot>:5900       # or Remmina → VNC
```

**Pitfall:** `-display :0` needs the kiosk user's X authority; run it as that user.

### One application in a window: waypipe and ssh -X

```sh
# both ends
sudo apt install waypipe
# you (Wayland desktop)
waypipe ssh user@<robot> rviz2
# you (X11 desktop)
ssh -X user@<robot> rviz2
```

The application runs on the robot and its window opens on your screen.
**Pitfall:** OpenGL-heavy apps are slow over X11 on a WAN; waypipe copes better.

### Xpra: seamless apps that survive reconnects

```sh
# robot
sudo apt install xpra
xpra start :100 --start=rviz2
# you
xpra attach ssh://user@<robot>/100
```

Drop the link and `xpra attach` again: the application kept running.

## Files and storage

### Browse the robot in your file manager

**✔ verified** (Nautilus, 2026-09-30). The robot needs `openssh-server`. In
Nautilus: *Other Locations* → *Connect to Server* → `sftp://user@<robot>/`. In
Dolphin, the address is the same. On macOS Finder, use an SFTP client such as
Cyberduck.

### Copy files, logs and bags, with resume

```sh
scp user@<robot>:/var/log/syslog .                          # ✔ verified (scp)
rsync -avP --partial user@<robot>:/data/bags/run.mcap .     # resumes where it stopped
```

**Pitfall:** over a flaky link use rsync, not scp: rerun it and it continues.

### Mount the robot's files: sshfs

```sh
sudo apt install sshfs
mkdir -p ~/robot
sshfs user@<robot>:/home/user ~/robot
# … edit with your own editor …
fusermount -u ~/robot
```

### Share a directory: NFS

```sh
# robot
sudo apt install nfs-kernel-server
echo '/data <you>(ro,no_subtree_check)' | sudo tee -a /etc/exports
sudo exportfs -ra
# you
sudo apt install nfs-common
sudo mount -t nfs4 <robot>:/data /mnt/robot
```

**Pitfall:** export to `<you>` only, never to a whole range.

### Keep a folder in sync: Syncthing

Install Syncthing on both ends and open each web UI (`http://127.0.0.1:8384`;
on the robot, use `ssh -L 8384:127.0.0.1:8384 user@<robot>` to reach it). Add
the other device by its ID, set its address to `tcp://<robot>:22000`, and share
a folder, for example maps or calibration.

### Image the robot's disk

```sh
ssh user@<robot> 'sudo dd if=/dev/nvme0n1 bs=4M status=progress' | zstd -o robot.img.zst
```

For block-level access, `nbd-server` on the robot and `nbd-client <robot> -N
<export> /dev/nbd0` on your machine expose the disk as a local device.
**Pitfall:** image a disk that is not mounted read-write, or the image is
inconsistent.

### Serve a file to the robot

```sh
# you: serve on your tunnel address only
python3 -m http.server 8000 --bind <you>
# robot
curl -O http://<you>:8000/firmware.bin
```

**Pitfall:** stop the server afterwards. Everything in that directory is
readable from the robot while it runs.

## Development and debugging

### VS Code Remote-SSH

Install the *Remote - SSH* extension, run `fjarr-connect list --ssh-config >>
~/.ssh/config`, then *Remote-SSH: Connect to Host…* → `robot-024`. The editor
is local; the terminal, the build and the debugger run on the robot.

### gdbserver: debug a process on the robot

```sh
# robot
gdbserver :2345 ./my_node                    # or: gdbserver --attach :2345 <pid>
# you
gdb ./my_node
(gdb) set sysroot remote:
(gdb) target remote <robot>:2345
```

**Pitfall:** your local binary must be the same build as the robot's, with
debug symbols.

### debugpy: debug Python on the robot from VS Code

```sh
# robot
python3 -m debugpy --listen <robot>:5678 --wait-for-client my_node.py
```

In VS Code, add an *attach* configuration with `"connect": {"host":
"<robot>", "port": 5678}` and a `pathMappings` entry from your local folder to
the robot's.

### Debug the robot's microcontroller: OpenOCD

The ST-Link or J-Link is plugged into the robot, and you debug from your desk.

```sh
# robot
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "bindto 0.0.0.0"
# you
arm-none-eabi-gdb firmware.elf -ex "target extended-remote <robot>:3333" -ex load
```

With a SEGGER J-Link, run `JLinkRemoteServerCLExe -Port 19020` on the robot and
choose *IP* with `<robot>` in J-Link's tools. Or pass the programmer itself
through with [USB/IP](#usbip-any-usb-device-on-your-machine).

### Profile on the robot

```sh
ssh user@<robot> 'sudo perf record -g -p $(pidof my_node) -- sleep 10 && sudo perf report --stdio' | less
ssh user@<robot> 'sudo py-spy record -o /tmp/prof.svg --pid $(pgrep -f my_node.py) --duration 30' \
  && scp user@<robot>:/tmp/prof.svg .
```

A program built with the Tracy client listens on port 8086; open the Tracy
profiler and connect to `<robot>`.

### Jupyter on the robot's data

```sh
# robot
jupyter lab --ip <robot> --no-browser
```

Open the URL it prints, with `<robot>` in place of the host. The data stays on
the robot.

### Wireshark: live capture from the robot's interfaces

```sh
ssh user@<robot> "sudo tcpdump -i eth0 -U -w - 'not port 22'" | wireshark -k -i -
```

Wireshark's *SSH remote capture* (sshdump) does the same from its GUI.
**Pitfall:** exclude your own ssh traffic, or the capture captures itself.

### iperf3: what the link gives you

```sh
# robot
iperf3 -s
# you
iperf3 -c <robot>            # you → robot
iperf3 -c <robot> -R         # robot → you
iperf3 -c <robot> -u -b 20M  # UDP at 20 Mbit/s: loss and jitter
```

## Robotics middleware

### ROS 2 over the link

**✔ verified** (Fast DDS and Cyclone DDS). Set the robot up once with
`sudo fjarr-agent net setup --ros yes` (it orders ROS after the tunnel and
writes the Cyclone DDS file when you choose Cyclone), then on your machine:

```sh
export ROS_DOMAIN_ID=<the robot's>
ros2 topic list
rviz2
```

The details, and why the ordering matters, are in
[docs/27](27-network-tunnel.md#ros2).

### Zenoh: ROS 2 across a slow link

DDS discovery is chatty over a WAN. Zenoh bridges the two graphs with far less
traffic.

```sh
# robot
zenoh-bridge-ros2dds -l tcp/<robot>:7447
# you
zenoh-bridge-ros2dds -e tcp/<robot>:7447
ros2 topic echo /odom
```

**Pitfall:** keep DDS itself off the tunnel when bridging (no Fjarr ROS
ordering needed), or topics arrive twice.

### ROS 1

```sh
# you
export ROS_MASTER_URI=http://<robot>:11311
export ROS_IP=<you>
rostopic list
```

**Pitfall:** the robot's nodes must advertise its tunnel address
(`ROS_IP=<robot>` in their environment), or you see topics but no data.

### Foxglove

```sh
# robot
sudo apt install ros-$ROS_DISTRO-foxglove-bridge
ros2 launch foxglove_bridge foxglove_bridge_launch.xml
```

In the Foxglove app: *Open connection* → *Foxglove WebSocket* →
`ws://<robot>:8765`. PlotJuggler works the same way with ROS 2 over the link.

### Rerun

Run the Rerun viewer on your machine and have the robot's code log to it, for
example `rr.connect_grpc("rerun+http://<you>:9876/proxy")` in the Python SDK.
**Pitfall:** the connection call changes between Rerun versions; check yours.

### rosbridge for web tools

```sh
# robot
ros2 launch rosbridge_server rosbridge_websocket_launch.xml
```

A roslibjs dashboard on your machine connects to `ws://<robot>:9090`.

### A micro-ROS board on your desk, in the robot's graph

Prototype firmware against the real robot. The board is plugged into your
laptop, the micro-ROS agent runs on your laptop, and ROS 2 crosses the link:

```sh
# you (ROS 2 over the link set up as above)
ros2 run micro_ros_agent micro_ros_agent serial --dev /dev/ttyACM0
ros2 topic list        # the board's topics, beside the robot's
```

### MQTT

```sh
# robot: let Mosquitto listen on the tunnel address
echo -e 'listener 1883 <robot>\nallow_anonymous false\npassword_file /etc/mosquitto/passwd' | sudo tee /etc/mosquitto/conf.d/fjarr.conf
sudo mosquitto_passwd -c /etc/mosquitto/passwd user && sudo systemctl restart mosquitto
# you
mosquitto_sub -h <robot> -u user -P … -t '#' -v
```

## Field buses and hardware links

### CAN: the robot's bus on your laptop (cannelloni)

```sh
# robot
cannelloni -I can0 -R <you> -r 20000 -l 20000
# you: a virtual CAN interface, bridged to the robot's
sudo modprobe vcan
sudo ip link add vcan0 type vcan && sudo ip link set vcan0 up
cannelloni -I vcan0 -R <robot> -r 20000 -l 20000
candump vcan0
```

SavvyCAN or a vendor tool on `vcan0` now sees the robot's bus, and what you send
goes onto it. **Pitfall:** sending onto a live robot's bus moves the robot.
Start with `candump` only.

### CAN with socketcand

```sh
# robot
socketcand -i can0 -l fjarr0
```

Tools that speak the socketcand protocol (SavvyCAN, python-can's `socketcand`
interface) connect to `<robot>:29536`.

### A serial port: ser2net and socat

```sh
# robot: /etc/ser2net.yaml
connection: &gps
  accepter: tcp,<robot>,3001
  connector: serialdev,/dev/ttyUSB0,115200n81,local
# then: sudo systemctl restart ser2net
# you: a local serial device that is the robot's
socat pty,link=/tmp/robot-gps,raw,echo=0 tcp:<robot>:3001
minicom -D /tmp/robot-gps          # or any tool that opens a serial port
```

For RFC 2217, which also carries the baud rate and control lines, use
`accepter: telnet(rfc2217),tcp,<robot>,3001` and
`pyserial-miniterm rfc2217://<robot>:3001 115200`.

### USB/IP: any USB device on your machine

The device is plugged into the robot and appears on your machine as if plugged
in there. It works best for programmers, USB-serial and CAN adapters, and
dongles; not for webcams or audio interfaces.

```sh
# robot
sudo apt install linux-tools-generic          # provides usbip and usbipd
sudo modprobe usbip_host
sudo usbipd -D
usbip list -l                                 # find the busid, e.g. 1-2
sudo usbip bind -b 1-2
# you
sudo apt install linux-tools-generic
sudo modprobe vhci-hcd
usbip list -r <robot>
sudo usbip attach -r <robot> -b 1-2           # it shows up in lsusb
sudo usbip detach -p 0                        # when done
```

**Pitfalls:** port 3240. While attached, the robot itself cannot use the
device. Bind only the device you mean to share ([safety](28-tunnel-ideas.md#safety)).

### Modbus/TCP to a device beside the robot

The PLC is on the robot's network, so hop through the robot:

```sh
ssh -N -L 5020:192.168.1.50:502 user@<robot>
mbpoll -a 1 -r 1 -c 10 -p 5020 127.0.0.1
```

### OPC UA

```sh
ssh -N -L 4840:192.168.1.60:4840 user@<robot>
```

Connect UaExpert to `opc.tcp://localhost:4840`. **Pitfall:** servers that return
their own hostname in the endpoint URL need that name to resolve to `127.0.0.1`
on your machine, for example in `/etc/hosts`.

### A device on the robot's network {#a-device-on-the-robots-network}

The tunnel reaches the robot, not its LAN
([why](28-tunnel-ideas.md#layer-2)). Hop through the robot deliberately:

```sh
ssh -J user@<robot> admin@192.168.1.20                    # ssh to the device
ssh -N -L 8080:192.168.1.20:80 user@<robot>               # its web page on http://localhost:8080
```

## Sensors and vendor tools

### A sensor's configuration page

Most lidars and industrial cameras serve one. If the sensor is on the robot's
network, forward its port as [above](#a-device-on-the-robots-network), then
open `http://localhost:8080`.

### A lidar's UDP stream in the vendor's viewer

A sensor sends UDP to one configured destination, usually the robot. Relay it
across the link:

```sh
# robot: forward the sensor's data port to you
socat -u UDP-RECV:7502,reuseaddr UDP-SENDTO:<you>:7502
```

Point the viewer at its local port. **Pitfall:** raw lidar data runs at 50–130
Mbit/s. That fits a LAN, not 4G.

### An RTSP camera

```sh
ffplay -rtsp_transport tcp rtsp://<robot>:8554/cam
```

TCP transport survives loss better than UDP here. To watch in a browser, use
`fjarr.camera`, which adapts to the link.

## Monitoring and observability

### Prometheus node_exporter

```sh
# robot
sudo apt install prometheus-node-exporter
# you
curl -s http://<robot>:9100/metrics | head
```

Or add `<robot>:9100` as a scrape target in your own Prometheus.

### Netdata, Glances, Grafana

```sh
# robot, one of:
sudo apt install netdata        # http://<robot>:19999
glances -w                      # http://<robot>:61208
```

A Grafana on the robot is at `http://<robot>:3000`.

### Logs

```sh
ssh user@<robot> journalctl -f -u fjarr-agent
ssh user@<robot> journalctl --since '10 min ago' -p warning
```

### The agent's pipelines

Bind the agent's introspection endpoint to the robot's tunnel address with a
token ([docs/24](24-pipeline-introspection.md)), then open `http://<robot>:7381/`
for the live pipeline graphs.

## Audio

### The robot's microphone on your machine (PipeWire, LAN)

```sh
# robot: accept audio clients from your tunnel address only
pactl load-module module-native-protocol-tcp auth-ip-acl=<you> port=4713
# you
pactl -s tcp:<robot>:4713 list short sources
parec -s tcp:<robot>:4713 -d <source> | paplay     # listen live
```

**Pitfall:** fine on a LAN, clicks over the internet. For a lossy link use ROC
(`roc-send` / `roc-recv`). In a browser, [`fjarr.audio`](17-roadmap.md#m35) is
the better path.
