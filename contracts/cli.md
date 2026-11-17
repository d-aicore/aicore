# CLI contract — S0

Exe: `AICoreC` (console subsystem; readable stdout; opened from a shortcut it drops its own console). The version is
baked at **configure** from the git sha (`cmake/aicore_version.cmake`, the Core's `core_version.h` pattern):
`AICORE_BUILD_ID = <version>+<sha short=10>`, with a `.modified` suffix when the (tracked) tree is dirty at
configure. A clean rebuild at HEAD prints exactly the HEAD sha, without `.modified`.

| Command | Behaviour | Exit |
|---|---|---|
| `--version` | prints exactly `AICORE_BUILD_ID` (e.g. `0.1.0+930dc51adb`) | 0 |
| `--self-test` | checks the version format (derived from the source) + a trivial `6*7=42`; prints `ok: ...` or `fail: ...` | 0 / 1 |
| `--grab <file.png>` | opens the window, waits until it is visible, saves a PNG of it, prints `ok: grab <path> <w>x<h>` | 0 / 1 |
| *(no args)* | opens the JUCE window until it is closed | — |

The self-test can fail by construction: a missing/`unknown` sha, a build id without the `<version>+` prefix, a
non-hex sha, an inconsistent `.modified` flag or a wrong sum give `fail` (exit 1), never an empty green.
