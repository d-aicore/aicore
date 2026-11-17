# services/render — rendering through plugins (S5)

`aicore-render.exe` (C++/JUCE 9.0.5, `app/render/render_main.cpp`) plays a PPQ 960 `.mid` through **one VST3
instrument**, offline, without the Core open, and returns `render.wav` (float32 stereo) + `report.json` +
`manifest.json`. It speaks the `aicore.service/1` contract, `render` profile (`contracts/CONTRACT.md` §5b), on the
same transport as S1b: the request on stdin, JSON lines on stdout, exit 0/2.

Why native and not DawDreamer (PLAN, S5 candidate): the app already carries the VST3 host
(`juce_audio_processors_headless`), with no Python, no GPL in our code and no third plugin host on the machine.
DawDreamer only comes in as an oracle, if the owner says yes.

`aicore_template_renders.py` is AI > TEMPLATE RENDERS' work: sound templates (`aicore-render-template` JSON, the
render params) play Guitar Pro MIDI through `aicore-render`, cut into 4 s pieces with their exact notes
(`audio.npy`, `notes.npy`, `catalogo.jsonl`). Example template: `D:\AICore\templates\renders\vital-init.json`.

## Running

```
scripts\build-s5.cmd                         (vcvars64 + Ninja, -j 4; logs s5-*.log)
build\app\aicore-render_artefacts\Release\aicore-render.exe hello
build\app\aicore-render_artefacts\Release\aicore-render.exe job < request.json
python services\render\gate_render.py        (exit 0)
python services\render\gate_render.py --noop (exit 1: proof that the gate fails)
```

`AICORE_RENDER_WARMUP_MS` (1500 ms by default): how long the message thread runs after `prepareToPlay`. Measured:
without it, Kick 3 rendered digital silence.

## Which plugins

The list is the owner's (2026-11-16) and lives in `app/render/supported-plugins.json`, next to the exe. AICore is a
program apart from the Core and reads nothing of it. The other plugins on the PC (Serum 2, Valhalla, Kilohearts...)
are for manual use and the scan ignores them. SETTINGS > PLUGINS shows the scan and the plugin of each role.

| role | what | where |
|---|---|---|
| bass, lead, pad, pluck | **Vital** 1.6.4 (VST3, installed by the owner): final render | `C:\Program Files\Common Files\VST3\Vital.vst3` |
| (tuning `.vital` presets) | **Vita** (`engine: "vita"`, `services/vita`) | `D:\AICore\plugins\vita-venv` (vita 0.1.0) |
| filter, drive, delay, sidechain | **Airwindows Consolidated** (VST3) | `D:\AICore\plugins\vst3\airwindows` |
| reverb | **Dragonfly Room / Hall** 3.2.10 (VST3) | `D:\AICore\plugins\vst3\dragonfly` |
| drums | **own sampler** (`engine: "sampler"`, `params.kit` = one WAV per note) or **sfizz** (`engine: "sfizz"`, `params.sfz` = an SFZ kit, played by the downloaded `sfizz_render`; owner OK 2026-11-16) | sampler inside `aicore-render`; `D:\AICore\plugins\sfizz-1.2.3-win64-lib\bin\Release\sfizz_render.exe` (or `AICORE_SFIZZ_RENDER`) |

Downloads with sha256 in `supported-plugins.json` (folder `D:\AICore\plugins\downloads`). `aicore-render scan`
looks only for these files (in `Common Files\VST3` and the list's `folders`) and writes
`%APPDATA%\AICore-C\plugins.json`. `aicore-render roles set <role> <plugin>` only takes a role the list gives
that plugin.

**A synth role's chain:** `services/vita` (.mid → dry WAV) → `aicore-render` with an `audio` input and
`effectRoles` (e.g. drive + reverb). Measured: 16 notes → Vita peak -8.8 dBFS → Airwindows + Dragonfly Room
peak -12.1 dBFS.

## What the gate proves

The host's mechanics are proven with the installed Serum 2 and Kick 3, in a test list; Serum 2 and Kick 3 render a
16-note MIDI at PPQ 960. The WAV reads back in the declared format and is not silent. The same request gives the
same WAV (with Serum 2). A PPQ 480 MIDI gives `start` → `progress` → `error UNSUPPORTED_FORMAT`, and the
`outputDir` stays empty. Bad requests give a typed error: missing MIDI, missing plugin, not a `.vst3`, hash,
sample rate, S1 fields, wrong profile, a file that is not MIDI, missing `outputDir`, an effect instead of an
instrument. The canaries (a tampered WAV, a silent WAV with the right hashes) must be caught.

## Known limits [H]

One instrument per request, with up to 8 effects in a chain, and no several tracks. The plugin state comes from a
file with the bytes of `getStateInformation`; `.fxp`/`.vstpreset`/`.SerumPreset` presets are not read yet. Plugins
that ask for a licence or a window when they load may fail or stay silent, and that gives `BACKEND_FAILED`
(never a silent success). Nobody has listened to these renders yet.
