# NightCoop Hub Plus

Self-hosted rendezvous and relay hub for the [NightCoop](https://nightcoop.net) Cyberpunk 2077 co-op mod, with a Windows one-click setup, local dashboard, LAN/Radmin VPN help, and readable diagnostics.

**[Русский README](README.ru.md)** · [Windows detailed guide (RU)](tools/hub/docker/README.windows.ru.md) · [Linux/VPS guide](tools/hub/docker/README.md)

> This repository contains **the hub only**, not the Cyberpunk game mod. You still need the NightCoop mod installed and working on each player's PC. The hub cannot inspect installed mods, game files, or encrypted gameplay traffic.

## What it does

- Creates and joins sessions; helps players discover a direct UDP path, and relays encrypted game packets if direct P2P cannot be established.
- Uses a persistent hub signing key and a fingerprint so players can check they reached the intended private hub.
- Provides a **Windows host installer** and a **separate guest Firewall installer**. The two Windows Defender Firewall rules are installed locally with UAC approval.
- On Windows, publishes UDP ports through Docker Desktop, **without `network_mode`**. On Linux, the original `network_mode: host` Compose configuration remains available.
- Opens a **loopback-only**, read-only dashboard at `http://127.0.0.1:47990` with hub status, fingerprint, local IPs, copy-ready connection strings, recent Docker logs, and possible-error filtering.

## Quick start — Windows host

Requirements: Windows 10/11, internet connection for Docker/image build, Docker Desktop with Linux containers and WSL 2 backend. Docker Desktop may require enabling virtualization/WSL 2, accepting first-run terms, or restarting Windows.

1. Download the repository ZIP from the branch/release you wish to use. Extract it (do not run scripts inside the ZIP viewer).
2. In `tools/hub/docker`, double-click **`INSTALL-WINDOWS.cmd`**.
3. If Docker is not installed, the installer offers to use WinGet **only with your confirmation**. It then waits for Docker Engine, requests UAC for the two Firewall rules, builds and starts the hub, and checks local UDP publication plus the signing fingerprint.
4. A browser page opens at **[http://127.0.0.1:47990](http://127.0.0.1:47990)**. Its **Host** card has the server entry for the Cyberpunk mod: `127.0.0.1#YOUR-FINGERPRINT`. Copy it into the mod's **Server → Add an own server** menu.
5. Create a session *in the game*. The game generates the actual six-character session code. For friends, see the LAN/Radmin instructions below.

If Docker needs a restart, finish its first-run setup and run **`INSTALL-WINDOWS.cmd` again**. It reuses the existing hub key and preserves previously configured explicit Radmin peer IPs.

For ongoing management double-click **`START-WINDOWS.cmd`**. It offers start/stop, firewall setup/cleanup, logs, diagnostic export and opening/stopping the dashboard.

### On the second (guest) PC

- Install the NightCoop **game mod** there. Joining your private hub does **not** require Docker.
- From the same extracted repository, double-click **`tools/hub/docker/INSTALL-GUEST-WINDOWS.cmd`**, then approve UAC. This applies both Windows Firewall rules **on that PC**. Your host cannot remotely edit the guest's firewall.
- Join the same Radmin VPN network if you are not on the same physical LAN.
- Paste the **real invitation code from the game**, changing **only the host address** to the host's LAN or Radmin VPN IPv4.

If Radmin VPN's routing/subnet setup is unusual, each PC can use `START-WINDOWS.cmd` → **2** to explicitly allow the other PC's VPN IPv4 in the LAN rule.

## Which address goes where?

The syntax of a private invitation is:

~~~text
SESSION_CODE@HUB_IP#HUB_FINGERPRINT
~~~

Only the address between `@` and `#` should change. The **session code is produced by NightCoop inside Cyberpunk**, never by the installer or dashboard. The fingerprint comes from *your hub*, not an example from a guide.

| Player | Example (not a real invite) | Rule |
| --- | --- | --- |
| Host on the same Windows PC as Docker | `127.0.0.1#MSAT-1F70-8QYY-MP1X` | Add as own server; `127.0.0.1` is this PC |
| Guest on the same home LAN | `J3E5ZE@192.168.1.100#MSAT-1F70-8QYY-MP1X` | Use the **host's LAN IPv4** |
| Guest connected via Radmin VPN | `J3E5ZE@26.42.1.7#MSAT-1F70-8QYY-MP1X` | Use the **host's Radmin VPN IPv4**, not the guest's |

The dashboard displays your detected addresses and fingerprint. Enter the game's real session code into its optional field to enable copy-ready LAN and Radmin invitations. That field is browser-local; the dashboard does **not** create a session or store the code.

**Do not send `127.0.0.1` to a friend:** for them, it would mean their own PC.

## Local web dashboard

On the host, open `http://127.0.0.1:47990` or use management menu **8**:

- **Status:** Docker hub running/not running, UDP 47800 published, the two managed Firewall rules, fingerprint.
- **Copy helpers:** host's localhost server, LAN server/invite, Radmin server/invite. Address fields are editable if Windows detects several adapters.
- **Session code:** paste a real code/invite copied from the game; the page only formats it for the chosen address.
- **Logs:** last 250 Docker log lines, warnings/errors filter, one-click copy. Management menu **5** exports a fuller diagnostic report.

The dashboard is a local companion served by `dashboard.ps1` over a TCP listener bound to **127.0.0.1 only**. It has **read-only GET endpoints** and does not expose a Docker socket or your hub's private signing key. There is no remote admin login: friends should never access this page. Menu **9** stops the dashboard without stopping the hub.

It reports *hub-visible* problems (ports, protocol mismatch, refused joins, startup errors). It cannot see errors inside the encrypted game session or identify which gameplay mods are missing on someone else's PC.

## Network ports and Docker

| Port | Protocol | Purpose | Where it is reachable |
| --- | --- | --- | --- |
| 47800 | UDP | Hub entry / session discovery | LAN/VPN host via Docker publication |
| 47810–47824 | UDP | Redirected session shards (Windows: up to 16 total shards) | LAN/VPN host via Docker publication |
| 47810–47873 | UDP | Maximum shard range for a 64-shard Linux hub | Open as needed on Linux/VPS |
| 47990 | TCP (HTTP) | Local dashboard | **127.0.0.1 only**, not Docker-published |

Windows uses the **standalone** `tools/hub/docker/docker-compose.windows.yml`:

~~~yaml
ports:
  - "47800:47800/udp"
  - "47810-47824:47810-47824/udp"
~~~

It intentionally has **no `network_mode`**. The default is four shards; `NIGHTCOOP_WINDOWS_SHARDS` may be set to **1–16** in `.env`. A session shard uses the entry port or port `47810 + (shard number - 1)` for non-entry shards. Windows' published range cannot support more than 16 total shards. The Windows base port is fixed to 47800.

On Linux, use the original `docker-compose.yml` with `network_mode: host` (no published-port mapping). See the [Linux/VPS setup guide](tools/hub/docker/README.md).

### Firewall rules

The installers create two named and removable rules on **the local Windows PC**:

| Managed rule | Incoming UDP ports | Scope |
| --- | --- | --- |
| NightCoop Hub UDP | `47800,47810-47873` | Private profile |
| NightCoop LAN UDP | `47800,47810-47824` | LocalSubnet plus optional explicit Radmin peer IPv4, Any profile |

Use management menu **7** to remove only the helper-created rules. Existing manually created rules are not removed.

**Security:** Docker Desktop may publish on all network interfaces, and port publication can sometimes bypass host Firewall restrictions. Use a trusted LAN/VPN and avoid router-level internet port forwarding by default. These Firewall permissions and Radmin VPN membership do not replace fingerprint verification.

## Troubleshooting

1. **Host setup failed?** Check Docker Desktop shows a running Linux engine; rerun the installer after WSL 2 / licensing / reboot prompts. Menu **5** exports a diagnostic report.
2. **Cannot connect from the guest?** Both devices must be on the same LAN or Radmin network; use the **host's** IP, not guest IP or `127.0.0.1`. Install guest Firewall rules on that computer.
3. **"Server does not answer"?** Confirm UDP 47800 is published and Docker container is running. Generic TCP/HTTP port checks do **not** prove UDP reachability.
4. **Slow session creation or redirect problems?** Confirm `47810–47824/udp`, and `NIGHTCOOP_WINDOWS_SHARDS` is no more than 16.
5. **Fingerprint mismatch?** Compare the whole fingerprint to the actual hub's log. Never delete the Docker volume to "fix" network issues: it would change the key/fingerprint.
6. **Protocol mismatch / refused join?** Host and guest may be running incompatible **NightCoop game-mod builds**. Update both to compatible versions and recreate the session if needed.
7. **Missing gameplay mod or crash?** The hub has no game-mod inventory. Collect client-side NightCoop/CET/RED4ext logs; see [compatibility roadmap](docs/ROADMAP.ru.md). Do not infer missing mods solely from a hub refusal.

### Logs and key retention

- Live hub log: Windows management menu **4**, or `docker compose -f tools/hub/docker/docker-compose.windows.yml logs -f hub`.
- Diagnostic export: Windows management menu **5**. **Review and redact local/VPN IPs, session metadata and usernames before sharing it.**
- The signing key is in the Docker volume **`nightcoop-hub-data`**. Stop/restart preserves it. Do not delete the volume without a backup and a plan to give all players a new fingerprint.
- Docker logging has size/rotation limits; dashboard only displays a recent slice.

## What is not guaranteed?

The installers can verify local prerequisites, local Firewall configuration, Docker publication and a running hub, but cannot guarantee Radmin VPN routes, other PCs' network policy, third-party antivirus behavior, or gameplay mod compatibility. Docker Desktop's bridge/NAT behavior also differs from native Linux host networking and may affect direct hole punching; the hub can relay when direct transport is unavailable.

No tool in this repository automatically opens another person's Windows Firewall or discovers their installed game mods.

## Source and contributing

| Path | Purpose |
| --- | --- |
| `tools/hub/main.cpp` | C++20 hub executable |
| `src/HubServer.*`, `src/ProtocolCore.hpp` | Hub sessions, transport protocol and refusal reasons |
| `tools/hub/docker/docker-compose.yml` | Native Linux/VPS host-networked deployment |
| `tools/hub/docker/docker-compose.windows.yml` | Windows Docker Desktop UDP-published deployment |
| `tools/hub/docker/INSTALL-WINDOWS.cmd` / `INSTALL-GUEST-WINDOWS.cmd` | Local Windows host/guest installers |
| `tools/hub/docker/nightcoop-windows.ps1` | Interactive installer, Firewall and diagnostics |
| `tools/hub/docker/dashboard.ps1` / `dashboard.html` | Local read-only browser dashboard |
| `docs/ROADMAP.ru.md` | Limitations and feature proposals |

The [detailed Linux/VPS guide](tools/hub/docker/README.md) covers compilation without Docker, signing-key backup and day-to-day operation. The pinned ENet dependency is fetched during build; Monocypher's license is under `vendor/monocypher/`.

Issues and PRs are welcome. For connection bugs, include: operating system, Docker Desktop version, LAN or Radmin scenario, port settings, steps to reproduce, and **redacted** relevant logs. Do not upload signing keys or unredacted private session data.
