# Teste controlado — notas conhecidas pelos nossos plugins, 2026-11-16, base `f2293ac`

Append-only. Protocolo §11. OK do Diogo (07:54): só medir, sem treino, sem GPU.
Conjunto: `eval/controlled_set.py build`. Para cada janela: notas ESCRITAS de kick, baixo e lead do template (sem bend,
sem automação de FX) → baixo e lead no Vita (preset init) + kick sintético → `mix.wav`. Verdade = notas escritas = notas
que soam. Serviço completo (Demucs seed 42 → Basic Pitch → descodificador), BPM dado, 1/32.

## Resultado, 1/32 (P / R / F1 onset / F1 full / J / |Δfim| mediana em ticks)
| janela | parte | serviço (filtro on) | Basic Pitch filtro off | MT3 mix (lead-select) |
|---|---|---|---|---|
| remember | kick | 0,93 / 0,99 / 0,96 / 0,00 / 1,06 / 77 | | |
| remember | baixo | 0,81 / 0,95 / 0,87 / 0,81 / 1,18 / 3 | | 0,10 / 0,05 |
| remember | lead | 0,96 / 0,85 / 0,90 / 0,00 / 0,89 / 202 | 0,93 / 0,99 / 0,96 / 0,00 / 1,06 | 0,01 / 0,01 |
| wakeup | kick | 0,93 / 0,97 / 0,95 / 0,00 / 1,03 / 76 | | |
| wakeup | baixo | 0,83 / 0,97 / 0,89 / 0,89 / 1,18 / 4 | | 0,02 / 0,01 |
| wakeup | lead | 1,00 / 1,00 / 1,00 / 0,00 / 1,00 / 248 | 0,81 / 1,00 / 0,90 / 0,00 / 1,23 | 0 / 0 |
| t1 | kick | 1,00 / 0,95 / 0,97 / 0,00 / 0,94 / 76 | | |
| t1 | baixo | 0,95 / 0,81 / 0,88 / 0,00 / 0,85 / 62 | | 0,70 / 0,34 |
| t1 | lead | 0,82 / 0,57 / 0,67 / 0,01 / 0,69 / 188 | 0,79 / 0,79 / 0,79 / 0,01 / 1,00 | 0,31 / 0,18 |
| t2 | kick | 1,00 / 0,33 / 0,50 / 0,50 / 0,33 / 36 | | |
| t2 | baixo | 0,94 / 0,32 / 0,47 / 0,46 / 0,34 / 42 | | 0,26 / 0,08 |
| t2 | lead | 0,68 / 0,32 / 0,43 / 0,00 / 0,47 / 238 | 0,70 / 0,43 / 0,53 / 0,00 / 0,62 | 0,33 / 0,14 |

MT3 no CPU: 39–51 s por janela (o serviço completo: ~13–15 s).

## Leitura
- **Com som limpo, o caminho atual funciona**: lead R 0,85 / 1,00 / 0,57 / 0,32 e baixo R 0,95 / 0,97 / 0,81 / 0,32.
  O fracasso nos renders dos templates vem do som (FX, bend, tremolo, motor FM uma oitava abaixo, sampler
  desafinado) e de verdades que não são o que soa — não da cadeia em si.
- **leadFilter off** sobe o recall do lead (t1 0,57 → 0,79, t2 0,32 → 0,43, remember 0,85 → 0,99) com pouca perda de
  precisão. Nos renders reais a ablação já mostrava que o filtro não ajuda.
- **Durações do lead estão erradas**: |Δfim| ~190–250 ticks → F1 full ≈ 0. É o próximo problema "tipo Guitar Pro"
  (durações por run-length na grelha, Plano B fase 2).
- **t2**: kick e baixo só 33 % de recall mesmo com som limpo (kicks a 1/16 encostados) → detetor do kick (N4).
- **MT3** é pior que o Basic Pitch também com som limpo, e 3× mais lento. Fica descartado.

## Gates (lead, 1/32), melhor braço = Basic Pitch filtro off
G1 passa (mediana 0,89; mínimo 0,43). G2 falha por pouco (wakeup J 1,23; t2 P 0,70). G3 falha (F1 full ≈ 0: durações).
Nada promovido. Sem `.mid` (regra: só com gates).
