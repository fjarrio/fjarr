# desktop-headless (throwaway, M2 slice 2b)

Robots without a display. `make_edid.py` turns a real monitor's EDID into a
distinct identity (serial `FJARRVIRT1`) for a connector forced on from the
kernel command line:

    video=HDMI-A-1:1920x1080@60e drm.edid_firmware=HDMI-A-1:edid/fjarr-1080p.bin

with the file in `/lib/firmware/edid/`. GNOME's virtual monitors need no system
change: `desktop-e/probe_e.py virtual` records one with `RecordVirtual`.
