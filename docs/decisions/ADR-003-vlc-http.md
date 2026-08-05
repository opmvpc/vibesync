---
id: ADR-003
titre: Contrôle de VLC via son interface HTTP locale, lancée par le client
statut: accepté
date: 2026-08-05
---

## Contexte

Trois interfaces possibles pour piloter VLC : script Lua intf (choix de Syncplay,
in-process, précis mais fragile aux versions et installation manuelle du script),
RC/telnet (limité), HTTP `/requests/status.json` (JSON, stable, accessible depuis
n'importe quel langage). Détails : `research/2026-08-05-alternatives-et-sync.md`.

## Décision

Le client vibesync **lance lui-même VLC** avec `--extraintf http --http-host 127.0.0.1
--http-port <aléatoire> --http-password <aléatoire>` et le pilote via l'interface HTTP :
poll de `status.json` (~200 ms), commandes `pl_pause`/`pl_forceresume`/`seek`/`rate`.
Position fine lue via `position × length` (le champ `time` n'est qu'à la seconde).
Le driver est isolé dans `internal/vlc` derrière une interface Go, remplaçable.

## Alternatives écartées

- Script Lua intf : plus précis mais fragile (issues récurrentes chez Syncplay),
  installation d'un fichier dans le dossier VLC de chaque ami.
- RC/telnet : pas conçu pour du polling haute fréquence, sécurisation pénible.

## Conséquences

- Précision du seek HTTP ≈ 1 s → stratégie « seek grossier puis affinage par rate »
  obligatoire (voir `docs/protocol.md` §sync).
- Le client doit localiser VLC (chemins standards Windows/macOS + config manuelle).
- Un faux VLC HTTP (httptest) rend le moteur de sync testable sans VLC réel.
