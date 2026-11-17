# services/lora — memory → dataset → text LoRA

A separable service that speaks the `aicore.service/1` contract (`contracts/CONTRACT.md` §5c): a JSON request on
stdin, `start` → `progress`… → `done` | `error` lines on stdout, exit 0/2. CPU only, BELOW_NORMAL priority, no
downloads, no GPU. Everything comes out `validation: "unreviewed"`.

## Where it comes from

It is the chain that already existed in the AICORE Python repo (`D:\AIcore-new` @ 500bec8, READ-ONLY), now wired to
the AICore-C contract. The code **is imported, not copied** (root `AICORE_PY_SRC`, by default `D:\AICore\app`,
which equals 500bec8):

| Step | AICORE code |
|---|---|
| C04 memory package (ZIP) | `services/memory/.../package.py` (exports), `aicore_contracts.package` (reads) |
| Import, attest, one example per episode, review | `apps/studio/.../project.py` |
| Dedup, families, 80/10/10 splits without leaks | `apps/studio/.../dataset.py` |
| Step plan, `MockTrainer`, atomic checkpoints | `workers/trainer/.../train.py` |

Each job's `report.json` keeps the sha256 of these files.

## Operations

- **`dataset`** — `memory_package` (≥1) → `dataset-manifest.json`, `examples.jsonl`, `report.json`.
  Params: `datasetId`, `approvedBy`, `attestedBy`, `seed`, `allowSmall`, `allowSyntheticSources`.
  Each example's target = the episode's last assistant message. Episodes without a valid target go to
  `report.json` (`skipped`), blocked examples to `exclusions`.
- **`train`** — `dataset_manifest` + `examples` + `recipe` → `train-report.json`.
  A real request ends in **`BACKEND_FAILED`** after reading and planning: the real QLoRA trainer does not exist yet.
  With `synthetic: true` it runs the `MockTrainer` (proof of the plumbing, fake loss, no weights).

## Running (on this machine)

```
D:\AICore\python\Scripts\python.exe services\lora\aicore_lora_service.py hello
D:\AICore\python\Scripts\python.exe services\lora\aicore_lora_service.py job < request.json
D:\AICore\python\Scripts\python.exe services\lora\gate_service.py          # exit 0
D:\AICore\python\Scripts\python.exe services\lora\gate_service.py --noop   # exit 1 (proof that it fails)
```

AICORE's interpreter already has `aicore_contracts`, `aicore_studio` (editable in `D:\AICore\app`), `jinja2`,
`pyarrow` and `tokenizers`. `aicore_worker` comes in through `AICORE_PY_SRC`.

## Gate (2026-11-16)

`gate_service.py`: 8/8 scenarios, canaries 2/2, `--noop` → exit 1. A synthetic package of 20 episodes
(`make_fixture.py`, from `fixtures/elastic-bass.events.jsonl`, a byte-for-byte copy of AICORE's fixture).
Evidence: `docs/evidence/lora-gate-service.log`.

- hello: `lora` profile, `dataset` and `train` operations;
- dataset: 20 episodes → 20 examples → train 16 / validation 2 / test 2, no family in two splits,
  `examples.jsonl` equal to the manifest;
- real training → `BACKEND_FAILED` after `read` + plan, empty `outputDir` (typed error on the happy path);
- synthetic training → `MockTrainer` 2 steps, checkpoint checked, report marked synthetic;
- 15 bad requests → typed error, including "synthetic sources never make a real dataset" (`INSUFFICIENT_DATA`);
- same package → same `datasetHash`;
- canaries: a tampered `examples.jsonl` and a polluted stdout must be caught.

## What is missing (in order)

1. **A real QLoRA trainer** behind the same interface as the `MockTrainer` (`step`, `validation_loss`,
   `save_states`), from `probe.py`'s `TorchQLoRABackend` (Transformers + PEFT + bitsandbytes, in WSL). It needs to
   compile the dataset with the profile's tokenizer (`compile.py`, Arrow) and **only train/validation** must reach the
   trainer. Blocked: the GPU is busy with treino-3 until ~16:00 on 2026-11-16, and the Qwen3-4B weights/tokenizer are
   not on disk (download only with the owner's permission).
2. **A real memory source**: there are no C04 packages on disk today. The Core→memory bridge was removed in AICORE
   commit `cdb9157`.
3. **Automatic memory selection** (AICORE's `docs/plans/AUTOMATIC-MEMORY-SELECTION.md`): today the target is always
   each episode's last answer.
4. A/B/C/D evaluation (`evaluate.py`) as an `evaluate` operation.
