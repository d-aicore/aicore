# AICore-C

The **native C++** AICORE app (JUCE 9.0.5, **no browser, no webview**). It copies the old PySide6 AICORE screen by
screen: EDIT (TRACKS, SAMPLE, STEMS, VOICE, MIDI), AI (ACE · FLAMINGO, AUDIO LORA, TEMPLATE RENDERS, DATABASE) and
SETTINGS (FOLDERS, MODELS, KEYBINDS, PLUGINS). The interface is in English. AICore is a program of its own: it
reads and runs nothing of the Core.

Universal brief: `AGENTS.md`; plan: `PLAN.md`; **state and plans: `docs/HANDOFF-2026-11-16.md`**.

## Layout

```
CMakeLists.txt             # project + version bake + JUCE 9.0.5 + modules
cmake/aicore_version.cmake # the Core's core_version.h pattern (git sha at configure)
core/                      # module WITHOUT JUCE (CLI-testable): version + self-test
app/src/                   # the app: window, pages (tracks, sample, stems, midi, ai, lora, renders, database, settings)
app/render/                # aicore-render: AICore's own VST3 renderer (headless)
services/                  # AICore's Python jobs (midi, lora, gp, database, render, vita)
contracts/                 # the aicore.service/1 contract + CPU simulator + gate
docs/evidence/             # gate logs and the old-vs-new screenshots of every page
scripts/                   # build-s0.cmd / build-s5.cmd (vcvars64 -> Ninja Release), package-main.ps1
```

## Build (Windows · VS 18 BuildTools · Ninja)

Needs CMake ≥ 3.22, Ninja, MSVC (VS 18 BuildTools), git. JUCE 9.0.5: AICore's own read-only copy
`D:/AICore/development/deps/JUCE-9.0.5` (not the Core's) when it is 9.0.5; else FetchContent with the brief's pin
(URL + SHA256 `d92207f5…`).

Automatic (low priority, logs in the given folder):

    cmd /c start "" /LOW /WAIT /B scripts\build-s5.cmd build\logs

Manual:

    call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j 4

Exes: `build\app\AICoreC_artefacts\Release\AICoreC.exe` and `build\app\aicore-render_artefacts\Release\aicore-render.exe`.

## Install (runs on its own)

    powershell -File scripts\package-main.ps1 -Promote -Shortcut

Copies the exes and `services\` to `D:\AICore\compiled\<version>`, gates that copy (self-test, render hello, grab),
then puts it in `D:\AICore\Main` (the previous Main goes to `historico`) and adds the Desktop shortcut
"AICORE (novo)".

## Command line

    AICoreC.exe --version            # 0.1.0+<git sha short=10>  (exit 0)
    AICoreC.exe --self-test          # ok: versao=... calc=42    (exit 0/1)
    AICoreC.exe --grab out.png       # opens the window, saves a PNG, quits
    AICoreC.exe --area A --section S # opens a page (0 EDIT, 1 AI, 2 SETTINGS)
    AICoreC.exe --sample-test <wav> <folder>       # SAMPLE edits, export, save (headless)
    AICoreC.exe --worker-run <kind> <fields.json>  # one audio-worker job (headless)
    AICoreC.exe                      # opens the window

The version is baked at **configure** (`cmake/aicore_version.cmake`): `0.1.0+<sha>`; a dirty tracked tree adds
`.modified`. A clean rebuild from a deleted build folder prints exactly the HEAD sha. Contract: `contracts/cli.md`.

## Where AICore keeps its data

- Settings: `%APPDATA%\AICore-C\Studio\audio-workspace.json` (the old app's keys; the audio worker reads it too).
- Plugins and roles (aicore-render): `%APPDATA%\AICore-C\plugins.json`, `roles.json`.
- Projects, training, models: `D:\AICore\projects`, `D:\AICore\training`, `D:\AICore\models` (SETTINGS > FOLDERS).
