# S0 gate evidence

Clean build (build folder deleted) + CLI + grab, made by the launcher `scripts/build-s0.cmd`
(vcvars64 VS18 → `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release`).

Files:
- `s0-vcvars.log`, `s0-configure.log`, `s0-build.log` — environment / configure / build (35 edges, exit 0).
- `s0-version.log` — `--version` == the build's HEAD sha, without `.modified`.
- `s0-selftest.log` — `ok: versao=... calc=42`, exit 0.
- `s0-grab.log` + `s0-window.png` — the window opened (720x460), title + version + subtitle.

## About the sha (amend of the evidence)

This evidence was made on the clean build of commit **1b3ed63264** (full skeleton). Committing the evidence needs an
`--amend`, which necessarily changes the HEAD sha (a self-reference: the evidence holds the sha of the build that
made it). So the sha in these logs is the one of the commit **before the evidence amend**.

The gate "version == HEAD sha" is checked by a **clean rebuild at the final HEAD**: `cmake/aicore_version.cmake`
reads HEAD at configure, so a build from a deleted build folder, with a clean tree, prints exactly the HEAD sha
without `.modified`. That final rebuild was run and reported in the task report (numbers, not states).

The `ui-*-antigo-vs-novo.png` files are the old (top) and new (bottom) screenshots of every page.
