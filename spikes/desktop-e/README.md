# desktop-e (throwaway, M2 slice 2b)

Candidate E from docs/07: mutter's `org.gnome.Mutter.ScreenCast` and
`org.gnome.Mutter.RemoteDesktop` directly, without the portal. `probe_e.py`
runs as the session user, checks a captured frame for the oracle's colour and
injects `f`, `j` and a click at 321,234 that `desktop-oracle` must log.
