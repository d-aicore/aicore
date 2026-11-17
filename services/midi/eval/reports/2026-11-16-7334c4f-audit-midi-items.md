# Audit 2026-11-16 — the four services/midi items, base `7334c4f`

Append-only. Each item was first confirmed against the code, then fixed, then covered by a test that is proven to
fail when the fix is removed (`tests/mutations.py`).

| item | confirmed in code | fix | test / mutation |
|---|---|---|---|
| (a) window past the end of the WAV cut silently | yes: `stereo[start*rate:(start+length)*rate]` just returned fewer samples | `check_window()`: start outside the WAV or end past it (1 ms slack) → `LIMIT_EXCEEDED` before Demucs; nothing published | `test_window_past_the_end_is_an_error` runs `Job.run` on a 1 s WAV with 0.5+5 s; mutation "window past the end cut silently" caught |
| (b) `parts=vocals` accepted, not transcribed | yes: no vocals track; with `includeStems` a vocals stem was even written | `check_parts()` in `validate()`: parts without a transcriber → `UNSUPPORTED_OPERATION` before `start` | `test_vocals_part_is_refused_before_work` (and the normal request still passes); mutation caught |
| (c) hello `maxOutputBytes` 8 MiB while stems are WAVs | yes: stems were written with no size check | hello `maxOutputBytes` = 128 MiB (largest output); `check_output_size()`: .mid ≤ 8 MiB, each stem ≤ 128 MiB | `test_output_caps`; mutations "stem size unchecked / midi uncapped" and "hello says 8 MiB output" caught |
| (d) `score_templates.py --out <relative>` fails | yes: the request's `outputDir` must be absolute (schema pattern) → `SCHEMA_INVALID` | `resolve_out()` makes `--out` absolute | `test_eval_out_dir_is_absolute`; mutation caught; also run for real with `--out rel-test` (exit 0) |

Not touched here: the 128 vs 8 MiB WAV cap in `contracts/` (the app thread owns it).

Gates: unit tests 16/16; mutations 14/14 caught; manifest verify 0; gate 0; gate --noop 1; MIDI gate 7/7 + 2/2; --noop 1.
