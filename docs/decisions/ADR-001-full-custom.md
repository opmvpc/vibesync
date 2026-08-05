---
id: ADR-001
titre: Full custom (serveur + client maison) plutôt que réutiliser Syncplay
statut: accepté
date: 2026-08-05
---

## Contexte

Le use case (fichiers locaux + VLC + sync play/pause/seek via serveur auto-hébergé) est
exactement celui de Syncplay, projet mûr et actif (1.7.6, Apache 2.0). Trois options
étudiées : déployer le serveur Syncplay existant (image `dnomd343/syncplay`), coder un
serveur maison compatible protocole Syncplay avec clients officiels, ou tout coder.

## Décision

Thibault choisit le **full custom** : serveur ET client vibesync, protocole maison.
C'est un projet de dev assumé, pas la voie de moindre effort. Le client doit être
graphique, soigné, disponible Windows + macOS Apple Silicon.

## Alternatives écartées

- **Déployer l'existant** : zéro code, 30 min — écarté par choix explicite de l'utilisateur.
- **Serveur compatible protocole Syncplay** : garde les clients officiels mais impose
  leur protocole TCP brut et leur UX — écarté, on veut la main sur tout.

## Conséquences

- On doit réimplémenter le cœur algorithmique côté client (drift, latence, ready,
  rejoin) documenté dans `research/2026-08-05-alternatives-et-sync.md`.
- Distribution du client à chaque ami (binaire à télécharger) — d'où l'exigence
  cross-platform et simplicité d'installation.
- Le protocole nous appartient : WebSocket possible (ADR-002), extensions libres.
