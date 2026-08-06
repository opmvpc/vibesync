# STATUS — vibesync

*Dernière mise à jour : 2026-08-06 (après-midi)*

## Où on en est

v0.1 sortie (release GitHub avec exe Windows), serveur **en prod** sur Coolify
(https://vibesync.choboai.com, mdp `onlyvibes`, auto-deploy sur push main, ~30 s
d'indispo par deploy). Repo public `github.com/opmvpc/vibesync`. Sprint **v0.2.0**
en cours : tous les retours terrain de Thibault traités côté Go et portés côté C ;
reste la convergence finale (vecteur 13, CI, sandbox, tag).

## Agents en vol

- **Agent Go** : 5 findings review terra (file de chat liée à la salle, reprise
  vierge stricte, anti-masquage buffering, régénération vecteur 13 avec
  `keepOutput` + état initial complet, injection version cmd/vibesync) + diagnostic
  du flake `TestIntegrationDebitNormalNonAffecte` (repro 1/30, vérif -count=50).
- **Agent C** : VS-026 dossiers médias + alignement sur les règles resserrées.

## Chantiers

| Ticket | Titre | Statut |
|---|---|---|
| VS-001..008, 013, 014, 016 | Socle, serveur, clients, CI, doc, release v0.1 | terminés |
| VS-009..012 | Pistes Wails/WPF/SwiftUI-façade | abandonnés (ADR-006/008) |
| VS-015 | Client macOS Swift | code livré, **build/test sur le Mac (dispo aujourd'hui)** |
| VS-017 | Faux buffering → pauses auto | **terminé** (Go+serveur déployés, port C 1d5a4ad) |
| VS-018 | UX texte + settings in-app | terminé |
| VS-019 | Icône | terminé |
| VS-020 | OSD dans VLC | ADR-009 (overlay maison) — implémentation à faire |
| VS-021 | Reprise de position (room linger) | terminé côté Go/serveur, toast C livré |
| VS-022 | UX connexion (testeur, messages, mdp) | terminé (Go + C) |
| VS-023 | Versions + invite de mise à jour | quasi — fix injection version en cours (agent Go) |
| VS-024 | Robustesse déconnexions | Go resserré en cours ; port C livré, attend vecteur 13 régénéré |
| VS-025 | Mémoriser le mdp (DPAPI) | terminé côté Windows, Keychain au polissage mac |
| VS-026 | Dossiers médias + sélection auto | **en cours (agent C)** |

## CI

Rouge attendue sur un seul point : vecteur `13-reprise-salle-vierge` côté C, en
attente de la régénération Go (champ `keepOutput` — le format golden n'encodait
pas la convention event/eventKeep du générateur). Flake Go
`TestIntegrationDebitNormalNonAffecte` en diagnostic (agent Go).

## Prochaine action

1. Intégrer le rendu de l'agent Go (QA complète, commit, push → auto-deploy).
2. Retouche C contre le vecteur 13 régénéré + règles resserrées, intégrer VS-026.
3. CI verte → `scripts/run-real-sandbox.ps1` → tag **v0.2.0** (VERSION déjà à 0.2.0).
4. Sur le Mac de Thibault : build Swift (docs/build-macos.md) + port des règles
   récentes au moteur Swift + Keychain + captures mac.
5. Ensuite : VS-020 (overlay OSD, ADR-009). Reliquat : renommage module Go
   thibsix→opmvpc quand aucun agent n'écrit.
