# The services a tunnel exists to reach, as they look from the far end of a link: an ssh daemon on
# the robot's own network stack. It runs as a SIDECAR sharing the demo robot's network namespace
# (`network_mode: service:demo-robot`), not inside the agent's image, because that is the real
# shape — on a robot, sshd is a package the integrator already has and `fjarr-agent` is a separate
# process that never knows about it. It also keeps an ssh daemon out of the container whose job is
# to be the agent.
#
# Dev-only fixture: the key pair below is generated at build time and goes nowhere near a release.
# spec: docs/27-network-tunnel.md · docs/12-development-environment.md
FROM ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    openssh-server ca-certificates coreutils \
    && rm -rf /var/lib/apt/lists/*

# The account an operator lands in. No password: key only, which is also how docs/10 wants a robot.
RUN useradd -m -s /bin/bash robot && mkdir -p /run/sshd /home/robot/.ssh && chmod 700 /home/robot/.ssh

# One throwaway key pair, baked in so both ends agree without any host wiring. The private half is
# copied out by `make tun-up` for whoever plays operator.
RUN ssh-keygen -t ed25519 -N "" -f /etc/fjarr-lab-key -C "fjarr lab (dev only)" \
    && cp /etc/fjarr-lab-key.pub /home/robot/.ssh/authorized_keys \
    && chown -R robot:robot /home/robot/.ssh && chmod 600 /home/robot/.ssh/authorized_keys

# A file with known content and a known hash, for the scp half of the docs/27 gate. Made at start,
# not baked, so its size follows FJARR_LAB_FILE_MB.
COPY start.sh /usr/local/bin/start.sh
RUN chmod +x /usr/local/bin/start.sh

CMD ["/usr/local/bin/start.sh"]
