# #1799 campaign record: plan, attempts, host state

Companion to [campaign-summary.md](campaign-summary.md) and the 2026-11-28 section of
[integrated-acceptance-1259.md](../../../../analysis/integrated-acceptance-1259.md). It lists every
attempt of the campaign, counted or not, with what the harness recorded about the host. The raw run
directories (captures, `measurement.csv`, editor logs) are not tracked (#1610).

## Host state per attempt

`nvidia-smi` before launch and after exit: graphics / memory clock, board power, temperature. They
are taken just before launch and just after exit (a launch right after another run can still read boost
clocks: 3 of 29 attempts did, `gl-deferred` round 1, `gl-forward-repeat` round 1 and `vk-deferred`
round 2 attempt 3), not under the run's own load; the harness does not sample clocks during a run, and it records
no CPU clock. During the run it samples GPU utilisation and the process list every
30 s: the samples column counts them, and *foreign* lists any competing or new GPU process.

| cell | attempt | status | started | wall s | before: clocks, power, temp | after: clocks, power, temp | samples | GPU util during run % | foreign |
|---|---|---|---|---:|---|---|---:|---|---|
| gl-deferred | round-01-attempt-1 | counted | 11:28:04 | 140 | 2655 MHz / 10501 MHz, 29.29 W, 44.13 °C | 225 MHz / 405 MHz, 22.73 W, 43.96 °C | 6 | 1–100 | none |
| gl-deferred | round-02-attempt-2 | counted | 12:11:39 | 134 | 210 MHz / 405 MHz, 16.15 W, 42.02 °C | 330 MHz / 405 MHz, 21.96 W, 43.90 °C | 6 | 1–100 | none |
| gl-deferred | round-03-attempt-3 | counted | 12:49:09 | 134 | 210 MHz / 405 MHz, 16.66 W, 42.70 °C | 270 MHz / 810 MHz, 22.54 W, 44.13 °C | 6 | 1–100 | none |
| gl-forward-csm | round-01-attempt-1 | counted | 11:30:33 | 132 | 210 MHz / 405 MHz, 18.02 W, 42.28 °C | 360 MHz / 810 MHz, 22.25 W, 42.68 °C | 6 | 0–100 | none |
| gl-forward-csm | round-02-attempt-2 | counted | 12:14:02 | 123 | 210 MHz / 405 MHz, 16.46 W, 42.07 °C | 315 MHz / 810 MHz, 23.23 W, 42.70 °C | 5 | 0–100 | none |
| gl-forward-csm | round-03-attempt-3 | counted | 12:51:31 | 123 | 210 MHz / 405 MHz, 16.54 W, 42.33 °C | 375 MHz / 810 MHz, 23.54 W, 42.76 °C | 5 | 0–100 | none |
| gl-forward-plus | round-01-attempt-1 | counted | 11:25:30 | 145 | 210 MHz / 405 MHz, 17.47 W, 42.50 °C | 345 MHz / 810 MHz, 24.62 W, 44.16 °C | 6 | 5–100 | none |
| gl-forward-plus | round-02-attempt-2 | counted | 12:09:08 | 143 | 210 MHz / 405 MHz, 16.41 W, 41.16 °C | 210 MHz / 405 MHz, 16.25 W, 43.56 °C | 6 | 1–100 | none |
| gl-forward-plus | round-03-attempt-3 | counted | 12:46:39 | 141 | 210 MHz / 405 MHz, 16.60 W, 43.19 °C | 315 MHz / 810 MHz, 24.95 W, 44.59 °C | 6 | 0–100 | none |
| gl-forward-repeat | round-01-attempt-1 | counted | 11:22:59 | 142 | 2565 MHz / 10501 MHz, 30.35 W, 43.45 °C | 675 MHz / 810 MHz, 26.01 W, 44.42 °C | 6 | 2–100 | none |
| gl-forward-repeat | round-02-attempt-2 | counted | 12:06:17 | 162 | 210 MHz / 405 MHz, 14.61 W, 36.02 °C | 345 MHz / 810 MHz, 23.68 W, 42.85 °C | 7 | 3–100 | none |
| gl-forward-repeat | round-03-attempt-3 | counted | 13:11:56 | 141 | 210 MHz / 405 MHz, 16.03 W, 41.96 °C | 285 MHz / 810 MHz, 23.27 W, 44.22 °C | 6 | 1–100 | none |
| gl-forward | round-01-attempt-1 | counted | 11:20:28 | 142 | 210 MHz / 405 MHz, 16.01 W, 37.56 °C | 285 MHz / 810 MHz, 23.93 W, 43.65 °C | 6 | 1–100 | none |
| gl-forward | round-02-attempt-2 | counted | 12:41:03 | 142 | 210 MHz / 405 MHz, 16.08 W, 38.39 °C | 315 MHz / 810 MHz, 22.46 W, 43.48 °C | 6 | 1–100 | none |
| gl-forward | round-03-attempt-3 | counted | 13:09:25 | 142 | 210 MHz / 405 MHz, 15.64 W, 38.56 °C | 270 MHz / 810 MHz, 22.44 W, 43.68 °C | 6 | 0–89 | none |
| vk-deferred | round-01-attempt-1 | counted | 11:39:08 | 184 | 570 MHz / 810 MHz, 38.92 W, 44.11 °C | 510 MHz / 810 MHz, 29.20 W, 45.79 °C | 7 | 1–96 | none |
| vk-deferred | round-02-attempt-2 | failed | 12:22:34 | 709 | 210 MHz / 405 MHz, 15.66 W, 42.82 °C | 210 MHz / 405 MHz, 16.45 W, 41.42 °C | 24 | 0–96 | none |
| vk-deferred | round-02-attempt-3 | counted | 12:43:33 | 176 | 2565 MHz / 10501 MHz, 35.88 W, 43.39 °C | 540 MHz / 5001 MHz, 34.54 W, 45.62 °C | 7 | 0–96 | none |
| vk-deferred | round-03-attempt-4 | counted | 12:59:49 | 176 | 210 MHz / 405 MHz, 16.43 W, 43.56 °C | 555 MHz / 5001 MHz, 33.66 W, 45.96 °C | 7 | 0–96 | none |
| vk-forward-plus | round-01-attempt-1 | counted | 11:36:02 | 177 | 210 MHz / 405 MHz, 18.18 W, 43.30 °C | 885 MHz / 5001 MHz, 35.25 W, 45.99 °C | 7 | 1–96 | none |
| vk-forward-plus | round-02-attempt-2 | counted | 12:19:17 | 186 | 210 MHz / 405 MHz, 16.93 W, 43.33 °C | 570 MHz / 810 MHz, 31.27 W, 45.68 °C | 7 | 0–96 | none |
| vk-forward-plus | round-03-attempt-3 | counted | 12:56:46 | 174 | 210 MHz / 405 MHz, 16.19 W, 43.42 °C | 525 MHz / 5001 MHz, 37.42 W, 46.16 °C | 7 | 0–97 | none |
| vk-forward | round-01-attempt-1 | counted | 11:32:54 | 178 | 210 MHz / 405 MHz, 16.75 W, 41.08 °C | 780 MHz / 810 MHz, 30.14 W, 45.85 °C | 7 | 1–96 | none |
| vk-forward | round-02-attempt-2 | counted | 12:16:14 | 174 | 210 MHz / 405 MHz, 16.52 W, 41.08 °C | 465 MHz / 5001 MHz, 40.42 W, 45.90 °C | 7 | 0–97 | none |
| vk-forward | round-03-attempt-3 | counted | 12:53:43 | 174 | 210 MHz / 405 MHz, 16.28 W, 41.13 °C | 495 MHz / 5001 MHz, 45.64 W, 46.07 °C | 7 | 0–96 | none |
| vk-hybrid | round-01-attempt-1 | failed | 11:42:22 | 1007 | 210 MHz / 405 MHz, 16.61 W, 43.48 °C | 210 MHz / 405 MHz, 15.24 W, 36.93 °C | 34 | 0–70 | none |
| vk-hybrid | round-01-attempt-2 | counted | 11:59:18 | 409 | 210 MHz / 405 MHz, 15.74 W, 36.91 °C | 375 MHz / 810 MHz, 20.82 W, 37.08 °C | 15 | 1–69 | none |
| vk-hybrid | round-02-attempt-3 | counted | 12:34:31 | 382 | 225 MHz / 405 MHz, 17.16 W, 41.50 °C | 435 MHz / 5001 MHz, 39.51 W, 39.96 °C | 14 | 0–71 | none |
| vk-hybrid | round-03-attempt-4 | counted | 13:02:53 | 383 | 210 MHz / 405 MHz, 16.19 W, 43.48 °C | 405 MHz / 5001 MHz, 31.95 W, 40.08 °C | 14 | 0–75 | none |

The two `failed` attempts are the editor capture stalls (#1871).

## ShadowPass sub-passes

Every `ShadowPass/<Family>` and `ShadowPass/<Family>/<Class>` bracket (#1590) above 0.05 ms, for the
GL Forward VSM cell, the GL Forward CSM control and the Vulkan Forward cell, from the campaign's
`summary.json`. `campaign-summary.md` lists only each camera's ten largest sub-passes; the report's
caster-class comparison reads this table. A bracket missing from a camera did not run there.

| cell | camera | sub-pass | p50 ms (range over counted runs) |
|---|---|---|---:|
| gl-forward | head-closeup | `ShadowPass/VsmLocal` | 23.0–23.3 |
| gl-forward | head-closeup | `ShadowPass/VsmLocal/FoliageCasters` | 0.1 |
| gl-forward | head-closeup | `ShadowPass/VsmLocal/FoliageCull` | 1.1–1.3 |
| gl-forward | head-closeup | `ShadowPass/VsmLocal/GroomCasters` | 21.0–21.5 |
| gl-forward | head-closeup | `ShadowPass/VsmLocal/MeshCasters` | 0.5 |
| gl-forward | head-closeup | `ShadowPass/VsmPages` | 0.3–0.4 |
| gl-forward | head-closeup | `ShadowPass/VsmSun` | 6.1–6.2 |
| gl-forward | head-closeup | `ShadowPass/VsmSun/FoliageCasters` | 0.9 |
| gl-forward | head-closeup | `ShadowPass/VsmSun/FoliageCull` | 2.0 |
| gl-forward | head-closeup | `ShadowPass/VsmSun/GroomCasters` | 1.9–2.0 |
| gl-forward | head-closeup | `ShadowPass/VsmSun/MeshCasters` | 0.3 |
| gl-forward | head-closeup | `ShadowPass/VsmSun/SkinnedCasters` | 0.3 |
| gl-forward | head-closeup | `ShadowPass/VsmSun/TerrainCasters` | 0.5 |
| gl-forward | head-backlit | `ShadowPass/VsmLocal` | 24.2–25.5 |
| gl-forward | head-backlit | `ShadowPass/VsmLocal/FoliageCull` | 0.5 |
| gl-forward | head-backlit | `ShadowPass/VsmLocal/GroomCasters` | 23.1–24.4 |
| gl-forward | head-backlit | `ShadowPass/VsmLocal/MeshCasters` | 0.5 |
| gl-forward | head-backlit | `ShadowPass/VsmPages` | 0.5 |
| gl-forward | head-backlit | `ShadowPass/VsmSun` | 5.4–5.5 |
| gl-forward | head-backlit | `ShadowPass/VsmSun/FoliageCasters` | 0.9 |
| gl-forward | head-backlit | `ShadowPass/VsmSun/FoliageCull` | 2.0 |
| gl-forward | head-backlit | `ShadowPass/VsmSun/GroomCasters` | 1.4 |
| gl-forward | head-backlit | `ShadowPass/VsmSun/MeshCasters` | 0.3 |
| gl-forward | head-backlit | `ShadowPass/VsmSun/TerrainCasters` | 0.5–0.6 |
| gl-forward | coats-closeup | `ShadowPass/VsmLocal` | 5.4–5.5 |
| gl-forward | coats-closeup | `ShadowPass/VsmLocal/FoliageCasters` | 2.3 |
| gl-forward | coats-closeup | `ShadowPass/VsmLocal/FoliageCull` | 0.6 |
| gl-forward | coats-closeup | `ShadowPass/VsmLocal/GroomCasters` | 2.5 |
| gl-forward | coats-closeup | `ShadowPass/VsmPages` | 0.4 |
| gl-forward | coats-closeup | `ShadowPass/VsmSun` | 15.9–16.0 |
| gl-forward | coats-closeup | `ShadowPass/VsmSun/FoliageCasters` | 8.7–8.8 |
| gl-forward | coats-closeup | `ShadowPass/VsmSun/FoliageCull` | 2.0–2.1 |
| gl-forward | coats-closeup | `ShadowPass/VsmSun/GroomCasters` | 4.1 |
| gl-forward | coats-closeup | `ShadowPass/VsmSun/SkinnedCasters` | 0.3 |
| gl-forward | coats-closeup | `ShadowPass/VsmSun/TerrainCasters` | 0.7 |
| gl-forward | population-dolly | `ShadowPass/VsmPages` | 0.5 |
| gl-forward | population-dolly | `ShadowPass/VsmSun` | 19.3–19.4 |
| gl-forward | population-dolly | `ShadowPass/VsmSun/FoliageCasters` | 11.1–11.4 |
| gl-forward | population-dolly | `ShadowPass/VsmSun/FoliageCull` | 2.1 |
| gl-forward | population-dolly | `ShadowPass/VsmSun/GroomCasters` | 4.7–4.8 |
| gl-forward | population-dolly | `ShadowPass/VsmSun/SkinnedCasters` | 0.3 |
| gl-forward | population-dolly | `ShadowPass/VsmSun/TerrainCasters` | 0.8 |
| gl-forward | rapid-turn | `ShadowPass/VsmLocal` | 1.0 |
| gl-forward | rapid-turn | `ShadowPass/VsmLocal/FoliageCasters` | 0.1 |
| gl-forward | rapid-turn | `ShadowPass/VsmLocal/FoliageCull` | 0.6 |
| gl-forward | rapid-turn | `ShadowPass/VsmLocal/GroomCasters` | 0.1 |
| gl-forward | rapid-turn | `ShadowPass/VsmPages` | 0.6 |
| gl-forward | rapid-turn | `ShadowPass/VsmSun` | 16.6–16.8 |
| gl-forward | rapid-turn | `ShadowPass/VsmSun/FoliageCasters` | 12.4–12.6 |
| gl-forward | rapid-turn | `ShadowPass/VsmSun/FoliageCull` | 2.1 |
| gl-forward | rapid-turn | `ShadowPass/VsmSun/GroomCasters` | 0.9 |
| gl-forward | rapid-turn | `ShadowPass/VsmSun/SkinnedCasters` | 0.1 |
| gl-forward | rapid-turn | `ShadowPass/VsmSun/TerrainCasters` | 0.9 |
| gl-forward-csm | head-closeup | `ShadowPass/Atlas` | 7.9–8.2 |
| gl-forward-csm | head-closeup | `ShadowPass/Atlas/FoliageCasters` | 0.1 |
| gl-forward-csm | head-closeup | `ShadowPass/Atlas/FoliageCull` | 1.1 |
| gl-forward-csm | head-closeup | `ShadowPass/Atlas/GroomCasters` | 6.3 |
| gl-forward-csm | head-closeup | `ShadowPass/Atlas/SkinnedCasters` | 0.2 |
| gl-forward-csm | head-closeup | `ShadowPass/Atlas/TerrainCasters` | 0.1 |
| gl-forward-csm | head-closeup | `ShadowPass/Csm` | 2.1 |
| gl-forward-csm | head-closeup | `ShadowPass/Csm/FoliageCasters` | 0.6 |
| gl-forward-csm | head-closeup | `ShadowPass/Csm/FoliageCull` | 0.5 |
| gl-forward-csm | head-closeup | `ShadowPass/Csm/GroomCasters` | 0.2 |
| gl-forward-csm | head-closeup | `ShadowPass/Csm/SkinnedCasters` | 0.6 |
| gl-forward-csm | head-closeup | `ShadowPass/Csm/TerrainCasters` | 0.1 |
| gl-forward-csm | head-backlit | `ShadowPass/Atlas` | 7.0–7.1 |
| gl-forward-csm | head-backlit | `ShadowPass/Atlas/FoliageCasters` | 0.1 |
| gl-forward-csm | head-backlit | `ShadowPass/Atlas/FoliageCull` | 0.5 |
| gl-forward-csm | head-backlit | `ShadowPass/Atlas/GroomCasters` | 6.2 |
| gl-forward-csm | head-backlit | `ShadowPass/Atlas/SkinnedCasters` | 0.1 |
| gl-forward-csm | head-backlit | `ShadowPass/Csm` | 1.6 |
| gl-forward-csm | head-backlit | `ShadowPass/Csm/FoliageCasters` | 0.6 |
| gl-forward-csm | head-backlit | `ShadowPass/Csm/FoliageCull` | 0.5 |
| gl-forward-csm | head-backlit | `ShadowPass/Csm/GroomCasters` | 0.2 |
| gl-forward-csm | head-backlit | `ShadowPass/Csm/TerrainCasters` | 0.1 |
| gl-forward-csm | coats-closeup | `ShadowPass/Atlas` | 5.1–5.3 |
| gl-forward-csm | coats-closeup | `ShadowPass/Atlas/FoliageCasters` | 2.1–2.3 |
| gl-forward-csm | coats-closeup | `ShadowPass/Atlas/FoliageCull` | 0.6 |
| gl-forward-csm | coats-closeup | `ShadowPass/Atlas/GroomCasters` | 2.1–2.4 |
| gl-forward-csm | coats-closeup | `ShadowPass/Atlas/SkinnedCasters` | 0.1 |
| gl-forward-csm | coats-closeup | `ShadowPass/Csm` | 9.1–9.6 |
| gl-forward-csm | coats-closeup | `ShadowPass/Csm/FoliageCasters` | 7.0–7.5 |
| gl-forward-csm | coats-closeup | `ShadowPass/Csm/FoliageCull` | 0.5 |
| gl-forward-csm | coats-closeup | `ShadowPass/Csm/GroomCasters` | 0.5 |
| gl-forward-csm | coats-closeup | `ShadowPass/Csm/SkinnedCasters` | 0.9 |
| gl-forward-csm | coats-closeup | `ShadowPass/Csm/TerrainCasters` | 0.2 |
| gl-forward-csm | population-dolly | `ShadowPass/Csm` | 12.6–13.4 |
| gl-forward-csm | population-dolly | `ShadowPass/Csm/FoliageCasters` | 10.5–11.2 |
| gl-forward-csm | population-dolly | `ShadowPass/Csm/FoliageCull` | 0.6 |
| gl-forward-csm | population-dolly | `ShadowPass/Csm/GroomCasters` | 0.5 |
| gl-forward-csm | population-dolly | `ShadowPass/Csm/SkinnedCasters` | 0.9 |
| gl-forward-csm | population-dolly | `ShadowPass/Csm/TerrainCasters` | 0.2 |
| gl-forward-csm | rapid-turn | `ShadowPass/Atlas` | 1.1–1.2 |
| gl-forward-csm | rapid-turn | `ShadowPass/Atlas/FoliageCasters` | 0.1–0.2 |
| gl-forward-csm | rapid-turn | `ShadowPass/Atlas/FoliageCull` | 0.6 |
| gl-forward-csm | rapid-turn | `ShadowPass/Atlas/GroomCasters` | 0.1 |
| gl-forward-csm | rapid-turn | `ShadowPass/Atlas/SkinnedCasters` | 0.1 |
| gl-forward-csm | rapid-turn | `ShadowPass/Csm` | 12.3–13.2 |
| gl-forward-csm | rapid-turn | `ShadowPass/Csm/FoliageCasters` | 10.8–11.8 |
| gl-forward-csm | rapid-turn | `ShadowPass/Csm/FoliageCull` | 0.6 |
| gl-forward-csm | rapid-turn | `ShadowPass/Csm/GroomCasters` | 0.2 |
| gl-forward-csm | rapid-turn | `ShadowPass/Csm/SkinnedCasters` | 0.4 |
| gl-forward-csm | rapid-turn | `ShadowPass/Csm/TerrainCasters` | 0.2 |
| vk-forward | head-closeup | `ShadowPass/VsmLocal` | 18.9–20.0 |
| vk-forward | head-closeup | `ShadowPass/VsmLocal/FoliageCull` | 1.4–1.5 |
| vk-forward | head-closeup | `ShadowPass/VsmLocal/GroomCasters` | 17.0–18.0 |
| vk-forward | head-closeup | `ShadowPass/VsmLocal/MeshCasters` | 0.3 |
| vk-forward | head-closeup | `ShadowPass/VsmLocal/TerrainCasters` | 0.1 |
| vk-forward | head-closeup | `ShadowPass/VsmPages` | 0.3 |
| vk-forward | head-closeup | `ShadowPass/VsmSun` | 5.9–6.0 |
| vk-forward | head-closeup | `ShadowPass/VsmSun/FoliageCasters` | 0.8 |
| vk-forward | head-closeup | `ShadowPass/VsmSun/FoliageCull` | 2.7 |
| vk-forward | head-closeup | `ShadowPass/VsmSun/GroomCasters` | 1.3–1.4 |
| vk-forward | head-closeup | `ShadowPass/VsmSun/MeshCasters` | 0.3 |
| vk-forward | head-closeup | `ShadowPass/VsmSun/SkinnedCasters` | 0.0–0.1 |
| vk-forward | head-closeup | `ShadowPass/VsmSun/TerrainCasters` | 0.5 |
| vk-forward | head-backlit | `ShadowPass/VsmLocal` | 18.4–19.6 |
| vk-forward | head-backlit | `ShadowPass/VsmLocal/FoliageCull` | 0.9–1.0 |
| vk-forward | head-backlit | `ShadowPass/VsmLocal/GroomCasters` | 17.0–18.4 |
| vk-forward | head-backlit | `ShadowPass/VsmLocal/MeshCasters` | 0.3 |
| vk-forward | head-backlit | `ShadowPass/VsmPages` | 0.4–0.6 |
| vk-forward | head-backlit | `ShadowPass/VsmSun` | 5.6–5.7 |
| vk-forward | head-backlit | `ShadowPass/VsmSun/FoliageCasters` | 0.8 |
| vk-forward | head-backlit | `ShadowPass/VsmSun/FoliageCull` | 2.7 |
| vk-forward | head-backlit | `ShadowPass/VsmSun/GroomCasters` | 1.1 |
| vk-forward | head-backlit | `ShadowPass/VsmSun/MeshCasters` | 0.3 |
| vk-forward | head-backlit | `ShadowPass/VsmSun/TerrainCasters` | 0.5 |
| vk-forward | coats-closeup | `ShadowPass/VsmLocal` | 7.2–7.6 |
| vk-forward | coats-closeup | `ShadowPass/VsmLocal/FoliageCasters` | 3.7–3.9 |
| vk-forward | coats-closeup | `ShadowPass/VsmLocal/FoliageCull` | 0.8–0.9 |
| vk-forward | coats-closeup | `ShadowPass/VsmLocal/GroomCasters` | 2.5–2.6 |
| vk-forward | coats-closeup | `ShadowPass/VsmLocal/TerrainCasters` | 0.2 |
| vk-forward | coats-closeup | `ShadowPass/VsmPages` | 0.5 |
| vk-forward | coats-closeup | `ShadowPass/VsmSun` | 15.5–15.6 |
| vk-forward | coats-closeup | `ShadowPass/VsmSun/FoliageCasters` | 8.4–8.6 |
| vk-forward | coats-closeup | `ShadowPass/VsmSun/FoliageCull` | 2.7 |
| vk-forward | coats-closeup | `ShadowPass/VsmSun/GroomCasters` | 3.3 |
| vk-forward | coats-closeup | `ShadowPass/VsmSun/TerrainCasters` | 0.8 |
| vk-forward | population-dolly | `ShadowPass/VsmPages` | 0.5–0.7 |
| vk-forward | population-dolly | `ShadowPass/VsmSun` | 15.3–15.4 |
| vk-forward | population-dolly | `ShadowPass/VsmSun/FoliageCasters` | 9.3–9.6 |
| vk-forward | population-dolly | `ShadowPass/VsmSun/FoliageCull` | 3.1–3.2 |
| vk-forward | population-dolly | `ShadowPass/VsmSun/GroomCasters` | 1.3–1.4 |
| vk-forward | population-dolly | `ShadowPass/VsmSun/TerrainCasters` | 0.8 |
| vk-forward | rapid-turn | `ShadowPass/VsmLocal` | 1.2 |
| vk-forward | rapid-turn | `ShadowPass/VsmLocal/FoliageCasters` | 0.2 |
| vk-forward | rapid-turn | `ShadowPass/VsmLocal/FoliageCull` | 0.8 |
| vk-forward | rapid-turn | `ShadowPass/VsmLocal/GroomCasters` | 0.1 |
| vk-forward | rapid-turn | `ShadowPass/VsmPages` | 0.5 |
| vk-forward | rapid-turn | `ShadowPass/VsmSun` | 14.6–15.0 |
| vk-forward | rapid-turn | `ShadowPass/VsmSun/FoliageCasters` | 10.0–10.3 |
| vk-forward | rapid-turn | `ShadowPass/VsmSun/FoliageCull` | 2.7 |
| vk-forward | rapid-turn | `ShadowPass/VsmSun/GroomCasters` | 0.7 |
| vk-forward | rapid-turn | `ShadowPass/VsmSun/TerrainCasters` | 0.9 |

## Plan

Run as `python scripts/perf/acceptance-benchmark.py --plan plan.json --output campaign2` under
`build-lock.ps1 -MaxConcurrent 1 -Jobs -1`. The test host binary was copied, with its DLLs, into
`build-cached/scratch/b1799/bin/` so a rebuild could not change it mid-campaign.

```json
{
 "repo": "C:/repos/OloEngine-integrated-renderer-release-baseline-1799",
 "buildConfig": "Release",
 "compiler": "clang-cl",
 "gateMs": 16.667,
 "repeats": 3,
 "maxRetries": 2,
 "timeoutSeconds": 2400,
 "mcpPort": 7391,
 "shaderCacheDir": null,
 "shaderRoot": "C:/repos/OloEngine-integrated-renderer-release-baseline-1799/OloEditor/assets/shaders",
 "assetRoot": "C:/repos/OloEngine-integrated-renderer-release-baseline-1799/OloEditor/SandboxProject/Assets",
 "exe": "C:/repos/OloEngine-integrated-renderer-release-baseline-1799/build-cached/scratch/b1799/bin/OloEngine-Tests.exe",
 "editorExe": "C:/repos/OloEngine-integrated-renderer-release-baseline-1799/bin/Release/OloEditor/OloEditor.exe",
 "extraCompetitors": [
  "WowB",
  "Wow"
 ],
 "ignoreGpuProcesses": [],
 "newGpuProcessUtilMax": 5,
 "idleGpuUtilMax": 30,
 "testHostArgs": [],
 "contentionWaitSeconds": 1800,
 "cells": [
  {
   "id": "gl-forward",
   "host": "gl",
   "manifest": "C:/repos/OloEngine-integrated-renderer-release-baseline-1799/OloEditor/assets/benchmark/manifests/acceptance-forward.yaml"
  },
  {
   "id": "gl-forward-repeat",
   "host": "gl",
   "manifest": "C:/repos/OloEngine-integrated-renderer-release-baseline-1799/OloEditor/assets/benchmark/manifests/acceptance-forward.yaml"
  },
  {
   "id": "gl-forward-plus",
   "host": "gl",
   "manifest": "C:/repos/OloEngine-integrated-renderer-release-baseline-1799/OloEditor/assets/benchmark/manifests/acceptance-forward-plus.yaml"
  },
  {
   "id": "gl-deferred",
   "host": "gl",
   "manifest": "C:/repos/OloEngine-integrated-renderer-release-baseline-1799/OloEditor/assets/benchmark/manifests/acceptance-deferred.yaml"
  },
  {
   "id": "gl-forward-csm",
   "host": "gl",
   "manifest": "C:/repos/OloEngine-integrated-renderer-release-baseline-1799/OloEditor/assets/benchmark/manifests/acceptance-forward-csm.yaml"
  },
  {
   "id": "vk-forward",
   "host": "vulkan",
   "manifest": "C:/repos/OloEngine-integrated-renderer-release-baseline-1799/OloEditor/assets/benchmark/manifests/acceptance-forward.yaml"
  },
  {
   "id": "vk-forward-plus",
   "host": "vulkan",
   "manifest": "C:/repos/OloEngine-integrated-renderer-release-baseline-1799/OloEditor/assets/benchmark/manifests/acceptance-forward-plus.yaml"
  },
  {
   "id": "vk-deferred",
   "host": "vulkan",
   "manifest": "C:/repos/OloEngine-integrated-renderer-release-baseline-1799/OloEditor/assets/benchmark/manifests/acceptance-deferred.yaml"
  },
  {
   "id": "vk-hybrid",
   "host": "vulkan",
   "manifest": "C:/repos/OloEngine-integrated-renderer-release-baseline-1799/OloEditor/assets/benchmark/manifests/acceptance-hybrid.yaml"
  }
 ]
}
```
