# Production safety review — resolved items

This file records the issues found in the RV1106 status-monitor review and their current resolution.

| Finding | Resolution |
|---|---|
| Full recording-directory scan every second | Replaced with one seed scan + inotify updates |
| SD create/delete write test every second | Removed; exact `/proc/mounts` check is used |
| Multiple runtime writers of `rkipc.ini` | Web writes, migration writes and S99 `sed` rewrites removed |
| Fixed-buffer Authorization overflow | Replaced with bounded in-place comparison |
| README claimed read-only while POST control existed | POST config/restart removed; docs updated |
| `/api/logs` documented but missing | Implemented |
| Shelling out to `netstat`/`ss`/`pgrep` | Replaced with non-blocking loopback RTSP probe |
| Request logging caused SD writes every 5 s | Normal request logging removed; state-change logging only |
| Single `recv()` assumed a full HTTP request | Header reader loops until terminator/limit |
| Single `send()` ignored partial writes | `send_all()` implemented |
| Mountpoint directory could be mistaken for SD | `/proc/mounts` exact-match detection |
| Init script could `rm -rf` recording data | Data-destructive removal removed; unsafe path blocks start |
| Stopping web service could affect video service | Web stop/restart now leaves `rkipc` running |
| Makefile and production build flags diverged | Unified C11/warnings-as-errors/pthread policy |
| Test plan described removed/crashing APIs | Replaced with production release-gate tests |
| Docs hard-coded stale format/duration details | Runtime docs now treat `/userdata/rkipc.ini` as source of truth |

## Remaining intentional limitations

- HTTP Basic Auth is still unencrypted. Use a trusted LAN/VLAN, VPN, or TLS reverse proxy.
- The monitor is a small single-process embedded HTTP server, not a general-purpose internet-facing web server.
- GPIO register addresses remain board-specific to the Luckfox Pico Pro Max/RV1106 layout already used by this project.
- Camera configuration changes are intentionally SSH/offline operations rather than web operations.
