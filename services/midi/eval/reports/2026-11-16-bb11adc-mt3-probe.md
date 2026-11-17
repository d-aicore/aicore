# Sonda P4 — YourMT3 (MT3 polifónico), 2026-11-16, base `bb11adc`

Append-only. Protocolo: `docs/MIDI-MAKER.md` §11 (Diogo). Nada promovido, nada no serviço.

## Modelo e ambiente
- Código: Hugging Face space `mimbres/YourMT3` @ `5e66c1ea173a8186e0d20432b841d3180cc015b5`, licença Apache-2.0, em
  `D:\AICore\models\mt3-src`.
- Pesos: `YPTF.MoE+Multi (noPS)` = `mc13_256_g4_all_v7_mt3f_sqr_rms_moe_wf4_n8k2_silu_rope_rp_b36_nops/last.ckpt`,
  sha256 `ae38e415c79efd5592dcb9b658cdb99ddb11d4c4e1eaa364cab04a052473fc25`, 562 MB. Pré-treinado, sem treino.
- Venv `D:\AICore\models\mt3-env`: python 3.11.18, torch 2.15.0+cpu, torchaudio 2.11.1+cpu, lightning 2.6.6,
  transformers 4.45.1, numpy 1.26.4. CPU, 4 threads, prioridade baixa, offline. Precisão 32.
- Sem instalar: `mir_eval` e `wandb` (só treino) substituídos por módulos vazios; WAV lido com leitor próprio
  (o torchaudio novo pede torchcodec). Verificado: os 655 tensores do checkpoint carregam (0 em falta, 0 a mais).
- Corredor: `services/midi/mt3_notes.py`; A/B: `services/midi/eval/p4_ab.py`.

## Entradas (iguais em todos os braços)
Stems de `D:\AI Dev\midi-maker\avaliacao\stems\<janela>` (uma corrida de Demucs, só leitura), sha256 em
`_medidas/p4-ab.json`. Fase da grelha da corrida P1. Quantize/descodificador = serviço. Referência calibrada ao pitch
que soa: FM answer −12; baixo remember/t1 −1 (medido, ver §10).

Regra do lead declarada antes de correr (thread, 07:36): não-bateria e não-baixo, registo ±19 da mediana ponderada,
skyline por célula de 1/32.

## Resultado (lead, 1/32, referência calibrada)
| janela | Basic Pitch (filtro on) R / P | MT3 melhor braço R / P | notas MT3 (mix / other / bass) |
|---|---|---|---|
| remember | 0,026 / 0,026 | 0,000 / 0,000 | 5 / 115 / 0 |
| wakeup | 0,900 / 0,287 | 0,000 / 0,000 | 0 / 0 / 0 |
| t1 | 0,000 / 0,000 | 0,000 / 0,000 | 0 / 19 / 271 |
| t2 | 0,144 / 0,621 | 0,016 / 0,136 | 96 / 39 / 103 |

Baixo: MT3 no stem bass t1 R 0,042 (Basic Pitch 0,763); t2 R 0,199 (Basic Pitch 0,565).

Controlo: com áudio limpo o MT3 funciona — render Vita do baixo do t1: 312 notas, a nota 34 certa (132×);
mistura controlada (Vita + kick sintético): 349 notas. Nos renders dos templates (mistura e stems) quase não dá notas.

Tempo (CPU, por janela de 25–30 s): carga 1,3 s; 7–30 s por ficheiro (mix ~10 s, other 9–30 s, bass 8–18 s).
O baseline completo (Demucs + Basic Pitch) é ~12–15 s por janela.

## Gates
G1 (R lead ≥ 0,60 mediana, ≥ 0,40 cada): **falha** (melhor braço: mediana 0,000). G2–G4 não se aplicam.
G5: o serviço não mudou; gates do contrato verdes.

## Decisão
O MT3 **não passa G1**. Pela especificação do Diogo, o próximo braço é o **MuScriptor** (precisa do Vulkan SDK,
autorizado só neste caso). Se o MuScriptor também não passar G1 → parar modelos; próximo lever = separação melhor / P5,
com novo OK. Sem `.mid` para ouvir (nada passou os gates).
