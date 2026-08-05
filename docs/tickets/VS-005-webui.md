---
id: VS-005
titre: Client — web UI locale (GUI)
statut: ouvert
priorité: haute
dépend-de: [VS-004]
créé: 2026-08-05
mis-à-jour: 2026-08-05
---

## Contexte

GUI = web UI embarquée (`embed.FS`) servie sur 127.0.0.1, navigateur ouvert au
lancement (ADR-004). Exigence : « pratique, bien foutu et joli » (dark, moderne).
Délégué au même agent Opus que VS-004.

## Critères d'acceptation

- [ ] Écran connexion (serveur, pseudo, salle) avec mémorisation locale
- [ ] Explorateur de fichiers backend (lecteurs Win / volumes mac) pour choisir la vidéo
- [ ] Salle : participants + ready + latence, bouton Prêt, chat, toasts, position/état
- [ ] WS local UI↔cœur ; l'UI ne parle jamais directement au serveur distant
- [ ] Design soigné (dark, accent, responsive fenêtre étroite)

## Journal du ticket

- 2026-08-05 : créé.
