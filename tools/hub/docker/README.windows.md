# Windows / Docker Desktop quick setup (LAN and Radmin VPN)

On the **host PC**, double-click **`INSTALL-WINDOWS.cmd`** in this folder. It checks Docker Desktop, offers a `winget` installation if missing (with your consent), requests UAC for firewall rules, starts the hub and checks local UDP publication. Docker may require a separate first-run agreement, WSL 2 configuration or restart; rerun the installer afterward.

Use **`START-WINDOWS.cmd`** for the ongoing management menu, or start it manually:

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\nightcoop-windows.ps1
~~~

1. **On the host:** use `INSTALL-WINDOWS.cmd` (or management menu item 1). Approve any necessary Docker installation/first-run/UAC prompts. The helper starts the hub and prints its fingerprint plus colored localhost/LAN/Radmin examples.
2. **On the second PC:** Docker is not needed unless that PC also hosts a hub. Double-click **`INSTALL-GUEST-WINDOWS.cmd`** to install the two Firewall rules **on that PC** (UAC, no Docker). For optional exact Radmin peer addresses use `START-WINDOWS.cmd` → item **2**. The script cannot remotely change another computer.
3. **In the game:** host enters `127.0.0.1#FINGERPRINT` when adding the own server. Guests use the real session invite from Cyberpunk but replace only the host address between `@` and `#` with the **host's LAN IPv4** or **host's Radmin VPN IPv4**. Keep the session code and fingerprint unchanged. Examples shown in the console are examples, not generated invites.

## Local read-only host dashboard

After `INSTALL-WINDOWS.cmd`, the host's browser opens **http://127.0.0.1:47990**. Menu item **8** reopens it; **9** stops only the dashboard.

It shows hub/Docker status, UDP publishing, managed firewall rule status, the fingerprint, LAN and Radmin VPN address cards, copy buttons, recent Docker logs and explanations for common hub errors. Paste a **real game session code** in the browser to enable copy-ready per-network invitations; the dashboard does not generate sessions or save that code.

The companion HTTP server binds only to **127.0.0.1**, has read-only endpoints and does not read the signing key or expose a Docker socket. Do not expose TCP 47990 to LAN/VPN/WAN. Installed Cyberpunk gameplay mods cannot be compared by this repository's hub alone; that requires client-side support. [Roadmap (RU)](../../../docs/ROADMAP.ru.md).

The standalone Windows Compose file has **no `network_mode` field** and publishes `47800/udp` and `47810-47824/udp`. It defaults to four shards. Set `NIGHTCOOP_WINDOWS_SHARDS` to 1–16 if necessary; 47800 is fixed for this profile. The original Linux Compose file remains unchanged. Run Windows with `docker compose -f docker-compose.windows.yml up -d --build`.

The installer creates:
- **NightCoop Hub UDP (managed)**: inbound UDP 47800,47810–47873 on Private networks.
- **NightCoop LAN UDP (managed)**: inbound UDP 47800,47810–47824 from LocalSubnet and optional exact Radmin peer IPs, on Any profile.

Menu **3** displays addresses and examples, **4** follows Docker logs, **5** writes a diagnostic text report (review private IPs before sharing), and **6** stops the hub without deleting its signing key. Item **7** removes only the two helper-managed firewall rules (with UAC).

**Caveats:** Radmin VPN peers must be on the same Radmin network. Reachability depends on VPN routes, Windows Firewall and Docker Desktop; it is not guaranteed. Docker Desktop's bridge NAT can interfere with direct hole punching (the hub can relay when needed). Published ports may be exposed on all host interfaces and can bypass some host firewall restrictions: use on trusted LAN/VPN only; do not forward these ports from the public internet casually. See the complete [Russian Windows guide](README.windows.ru.md) or [Linux/VPS guide](README.md).
