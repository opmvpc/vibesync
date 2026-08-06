---
id: VS-020
titre: Messages OSD dans VLC (« X a mis pause », « X a seek à 12:34 »)
statut: ouvert
priorité: normale
dépend-de: [VS-017]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

Demande de Thibault, comme dans Syncplay original (qui passe par son script Lua).
Notre driver est HTTP-only : l'interface HTTP n'expose pas d'OSD. Piste : activer
en plus l'interface RC (`--extraintf`) et pousser le filtre marquee (`marq`) par le
socket RC — à valider par une recherche avant d'implémenter.

## Critères d'acceptation

- [x] Recherche : RC/marq et HTTP infaisables sur VLC 3.x (testé en réel) —
      `research/2026-08-06-recherche-osd-vlc.md`
- [x] Décision documentée : ADR-009 — OSD par fenêtre overlay maison sur VLC
- [ ] Implémentation Windows (ui/win32) : overlay layered topmost cliquable-au-travers,
      messages « X a mis pause / a avancé à HH:MM:SS », fondu, réglage on/off
- [ ] Implémentation macOS (ui/macos) au moment du polissage Mac

## Journal du ticket

- 2026-08-06 : créé.
- 2026-08-06 : recherche faite (verdict infaisable en natif VLC), ADR-009 acté :
  overlay maison. Implémentation à planifier après VS-017/018.
