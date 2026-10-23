# Monocypher 4.0.2 — where it comes from

- `monocypher.c`, `monocypher.h` and `LICENCE.md` are taken unchanged from the author's release archive
  `https://monocypher.org/download/monocypher-4.0.2.tar.gz` (directory `src/`, downloaded on 2026-10-16).
- Checked against the tag archive on the author's GitHub (`https://github.com/LoupVaillant/Monocypher/archive/refs/tags/4.0.2.tar.gz`):
  the code is the same; only the first line differs (`// Monocypher version 4.0.2` instead of `__git__` — the release script
  fills in the version).
- License: BSD-2-Clause or CC0 1.0, at your choice (`LICENCE.md`).
- What for: transport encryption (X25519, XChaCha20-Poly1305, EdDSA). The library is compiled into the mod and the hub; players
  download nothing separately.
