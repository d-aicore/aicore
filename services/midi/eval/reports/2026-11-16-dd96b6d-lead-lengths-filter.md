# Durações do lead, filtro do lead e kick rápido, 2026-11-16, base `dd96b6d`

Append-only. Protocolo §11. Sem downloads, sem treino, sem GPU.

## Mudanças no serviço (branch `s2q/grelha-medicao`)
1. `tidy_lengths` (regra declarada, igual para todas as músicas): uma nota do lead acaba, no máximo, no início da nota
   seguinte da linha, e dura um número inteiro de células da grelha (mínimo 1).
2. `leadFilter` passa a **desligado por omissão** (`true` continua a ligá-lo). O pedido golden do `score_templates.py`
   continua a mandar `leadFilter: true` (golden intacto); novo `--lead-filter on|off`.
3. O limite de 2 vozes do lead aplica-se sempre (antes só com o filtro).

## Conjunto controlado (lead, 1/32): antes (`dd96b6d`, filtro on) → depois
| janela | R | P | F1 onset | F1 full | J | |Δfim| mediana |
|---|---|---|---|---|---|---|
| remember | 0,85 → 0,99 | 0,96 → 0,93 | 0,90 → 0,96 | 0,00 → 0,01 | 0,89 → 1,06 | 202 → 117 |
| wakeup | 1,00 → 1,00 | 1,00 → 0,81 | 1,00 → 0,90 | 0,00 → 0,00 | 1,00 → 1,23 | 248 → 236 |
| t1 | 0,57 → 0,75 | 0,82 → 0,79 | 0,67 → 0,77 | 0,01 → **0,42** | 0,69 → 0,94 | 188 → 56 |
| t2 | 0,32 → 0,43 | 0,68 → 0,71 | 0,43 → 0,54 | 0,00 → **0,33** | 0,47 → 0,60 | 238 → 50 |
Kick e baixo iguais (não mudaram).

## Renders reais (lead, 1/32, pitch-strict, referência que soa / escrita)
Sem mudança relevante (o limite ali é o som): remember 0,026, wakeup 0,425 → 0,419, t1 0, t2 0,231 → 0,235.
Pitch-strict + fim (regra mir_eval) sobe: remember (escrita) 0,078 → 0,225, t2 0,153 → 0,164. Kick/baixo iguais.

## Kick rápido do t2
Os kicks "perdidos" do t2 são um **rolo a 1/32** (129 de 196 kicks a 36 ms). O Demucs manda-os para o stem do
**baixo** (−11 dB no bass, −49 dB no drums). Detetar no drums+bass não ajuda (no controlado remember cai para R 0,17).
Sem solução sem um detetor de bateria dedicado. Não mudou.

## Gates (conjunto controlado, lead, 1/32)
G1 passa (mediana R 0,87; mínimo 0,43). G2: passa em 3 de 4; **wakeup J 1,23** (37 notas para 30) falha por 7 notas.
G3 passa (F1 full mediana 0,17 vs ~0 antes). G4 igual ao baseline (mesma cadeia). G5: manifesto 0, gate 0, noop 1,
MIDI 7/7 + 2/2, noop 1. → **Zona cinzenta (G2 no wakeup): decisão do Diogo.** Sem `.mid` até essa decisão.
