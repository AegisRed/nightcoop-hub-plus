# syntax=docker/dockerfile:1
#
# NightCoop hub — the meeting point and fallback relay of NightCoop (co-op mod for Cyberpunk 2077), your own server.
# See README.md (English) / README.ru.md (Russian) next to this file.
#
# Build context — the repository root or the hub source bundle (tools/hub/docker/bundle.sh: the same layout):
#   docker build -f tools/hub/docker/Dockerfile -t nightcoop-hub:local .
# or with Compose:  docker compose -f tools/hub/docker/docker-compose.yml up -d --build
#
# Only the hub's own sources are compiled (src/HubServer, src/NetCrypto, src/Log, tools/hub/main.cpp, Monocypher, ENet).

ARG DEBIAN=debian:13-slim

FROM ${DEBIAN} AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ git ca-certificates \
 && rm -rf /var/lib/apt/lists/*
# ENet — the exact commit the mod is built with (vendor/enet submodule of the repository)
ARG ENET_REPO=https://github.com/lsalzman/enet.git
ARG ENET_COMMIT=5a9c537fd464b3c6d3c55e1d3bd47588faf71b42
RUN git clone --quiet "${ENET_REPO}" /enet \
 && git -C /enet checkout --quiet --detach "${ENET_COMMIT}" \
 && test "$(git -C /enet rev-parse HEAD)" = "${ENET_COMMIT}"

WORKDIR /src
COPY src/HubServer.cpp src/HubServer.hpp src/NetCrypto.cpp src/NetCrypto.hpp src/Log.cpp src/Log.hpp \
     src/ProtocolCore.hpp src/HubRules.hpp src/EnetGuard.hpp src/EnetGuardSwitch.hpp src/HubCookie.hpp src/HubKnock.hpp src/
COPY vendor/monocypher/ vendor/monocypher/
COPY tools/hub/main.cpp tools/hub/

RUN set -eux; \
    mkdir -p /obj /out; \
    for f in callbacks compress host list packet peer protocol unix; do \
        gcc -c -O2 -I/enet/include \
            -DHAS_FCNTL=1 -DHAS_POLL=1 -DHAS_SOCKLEN_T=1 -DHAS_MSGHDR_FLAGS=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1 \
            -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_GETHOSTBYNAME_R=1 -DHAS_GETHOSTBYADDR_R=1 \
            "/enet/$f.c" -o "/obj/enet_$f.o"; \
    done; \
    gcc -c -O2 vendor/monocypher/monocypher.c -o /obj/monocypher.o; \
    for f in src/Log.cpp src/NetCrypto.cpp src/HubServer.cpp tools/hub/main.cpp; do \
        g++ -c -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -I/enet/include "$f" -o "/obj/$(basename "$f" .cpp).o"; \
    done; \
    g++ -o /out/nightcoop_hub /obj/*.o -pthread -static-libstdc++ -static-libgcc; \
    /out/nightcoop_hub --gen-sign-key /tmp/smoke.key >/dev/null; \
    /out/nightcoop_hub --hub-public /tmp/smoke.key | grep -q '^nightcoop-hub-public 1 '; \
    rm /tmp/smoke.key

FROM ${DEBIAN}
# Its own user, no home, no shell; the key lives in the /data volume only
RUN useradd --system --uid 10001 --user-group --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin nightcoop \
 && mkdir /data && chown nightcoop:nightcoop /data && chmod 700 /data
COPY --from=build /out/nightcoop_hub /usr/local/bin/nightcoop_hub
USER nightcoop
VOLUME ["/data"]
# Entry port; threads of the hub (--shards) also listen on 47810, 47811, … — one more port per thread
EXPOSE 47800/udp
ENTRYPOINT ["/usr/local/bin/nightcoop_hub"]
# First start: --sign-key-auto creates the hub's key in /data and logs its fingerprint (players need it)
CMD ["47800", "--shards", "auto", "--max-clients", "512", "--sign-key-auto", "/data/hub-sign.key"]
