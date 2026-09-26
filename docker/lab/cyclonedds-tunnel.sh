#!/usr/bin/env bash
# The Cyclone DDS configuration docs/27 has promised since the spike, generated for one end of a
# link. Cyclone needs it because it binds a single interface chosen arbitrarily, so multicast
# reaching the tunnel is necessary but not sufficient — unlike Fast DDS, which needs nothing.
#
#   cyclonedds-tunnel.sh <tunnel-interface> <self-tunnel-ip> <peer-tunnel-ip>
#
# Both elements are required, and for different reasons: <Interfaces> with explicit priorities stops
# the arbitrary single-interface choice and keeps the local network usable at the same time, and the
# unicast <Peers> supply the discovery that a point-to-point link's multicast cannot bootstrap on its
# own. Use it with CYCLONEDDS_URI=file:///path/to/this/output.
# spec: docs/27-network-tunnel.md#ros2
set -euo pipefail
IFACE=${1:?tunnel interface}; SELF=${2:?self tunnel address}; PEER=${3:?peer tunnel address}
LAN=${4:-eth0}
cat <<XML
<?xml version="1.0" encoding="UTF-8"?>
<CycloneDDS xmlns="https://cdds.io/config">
  <Domain id="any">
    <General>
      <AllowMulticast>true</AllowMulticast>
      <Interfaces>
        <!-- The tunnel first, the local network still usable: without explicit priorities Cyclone
             picks one interface and logs that it chose it "arbitrarily". -->
        <NetworkInterface name="$IFACE" priority="10" multicast="true"/>
        <NetworkInterface name="$LAN" priority="1" multicast="true"/>
      </Interfaces>
    </General>
    <Discovery>
      <ParticipantIndex>auto</ParticipantIndex>
      <Peers>
        <Peer address="$SELF"/>
        <Peer address="$PEER"/>
      </Peers>
    </Discovery>
  </Domain>
</CycloneDDS>
XML
