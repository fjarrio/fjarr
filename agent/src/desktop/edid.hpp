#pragma once
// A monitor's identity from its EDID, as mutter reports it (docs/23#desktop-x11): the PNP vendor
// ("DEL"), the monitor name descriptor ("DELL U2422H"), the serial string descriptor (or the serial
// number). So backend A, which reads EDIDs from RandR, gives a monitor the same wire id as backend E.
#include <cstddef>
#include <cstdint>
#include <string>

#include "desktop/monitor_identity.hpp"

namespace fjarr::desktop {

/// The key for `connector`'s EDID; vendor, model and serial stay empty when it is not a valid base block.
MonitorKey edid_key(const std::uint8_t* edid, std::size_t len, const std::string& connector);

} // namespace fjarr::desktop
