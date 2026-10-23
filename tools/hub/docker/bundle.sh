#!/usr/bin/env bash
# The hub's source bundle: only what the hub is built from (src/Hub*, NetCrypto, Log, ProtocolCore, Monocypher, the hub's
# main.cpp, the Dockerfile, Compose and README). For people who host their own hub without the whole mod's repository;
# publish it next to an image built from it. ENet is not inside: the Dockerfile fetches the exact commit itself. The
# project's own server scripts in tools/hub (setup.sh, systemd units, firewall) stay out. These files are published:
# comments in them are in English and only where they are needed.
#
#   bash tools/hub/docker/bundle.sh [commit]      -> nightcoop-hub-src-<commit>.tar.gz in the current directory
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
REV="${1:-HEAD}"
OUT="${OLDPWD:-.}/nightcoop-hub-src-$(git rev-parse --short "$REV").tar.gz"
# Without autocrlf: shell scripts and units must keep LF (git on Windows would give CRLF — «bash\r»)
git -c core.autocrlf=false archive --format=tar.gz -o "$OUT" "$REV" \
    src/HubServer.cpp src/HubServer.hpp src/NetCrypto.cpp src/NetCrypto.hpp src/Log.cpp src/Log.hpp src/ProtocolCore.hpp \
    src/HubRules.hpp src/EnetGuard.hpp src/EnetGuardSwitch.hpp src/HubCookie.hpp src/HubKnock.hpp \
    vendor/monocypher tools/hub/main.cpp tools/hub/docker
echo "$OUT"
