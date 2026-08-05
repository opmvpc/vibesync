---
id: VS-005
titre: Client — web UI locale (GUI de debug)
statut: terminé
priorité: haute
dépend-de: [VS-004]
créé: 2026-08-05
mis-à-jour: 2026-08-05
---

## Contexte

GUI = web UI embarquée (`embed.FS`) servie sur 127.0.0.1 (ADR-004). Rétrogradée en
mode debug/référence par ADR-006 puis ADR-008 (clients natifs autonomes).

## Critères d'acceptation

- [x] Écran connexion (serveur, pseudo, salle) avec mémorisation locale
- [x] Explorateur de fichiers backend (lecteurs Win / volumes mac) pour choisir la vidéo
- [x] Salle : participants + ready + latence, bouton Prêt, chat, toasts, position/état
- [x] WS local UI↔cœur ; l'UI ne parle jamais directement au serveur distant
- [x] Design sombre correct (finition volontairement limitée : mode debug, ADR-008)

## Journal du ticket

- 2026-08-05 : créé.
- 2026-08-05 : livré par agent Opus B (canal /ui + token, --headless), rôle réduit à
  outil de debug par ADR-008. Vérifié et intégré par l'orchestrateur.
