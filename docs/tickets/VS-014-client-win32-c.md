---
id: VS-014
titre: Client Windows handmade — C pur + Win32, un seul exe
statut: ouvert
priorité: haute
dépend-de: [VS-013]
créé: 2026-08-05
mis-à-jour: 2026-08-05
---

## Contexte

ADR-008. Remplace VS-011 (WPF, abandonné). `ui/win32/` : C, zéro lib tierce, APIs OS
uniquement. Toolchain : WinLibs GCC UCRT (installée sur la machine). Spec fonctionnelle :
`docs/protocol.md` (§Comportements client) + mêmes écrans que l'ex-VS-011.

## Critères d'acceptation

- [ ] Un exe unique < 500 Ko, démarrage < 100 ms, DPI-aware, thème sombre soigné
- [ ] UI immediate-mode GDI double-buffer : connexion (serveur/pseudo/salle mémorisés
      dans %APPDATA%\vibesync.ini), salle (participants/ready/latence, bouton Prêt,
      barre de position cliquable, chat, toasts, indicateur de drift)
- [ ] WebSocket via WinHTTP (ws:// et wss://), reconnexion backoff, ping applicatif
- [ ] Moteur de sync conforme spec, validé par les vecteurs `test/vectors/*.json`
- [ ] VLC : lancement CreateProcess (+ IFileOpenDialog pour choisir le fichier),
      pilotage HTTP 127.0.0.1 via Winsock, position fine `position × length`
- [ ] Build reproductible : `ui/win32/build.bat` (gcc, -O2, flags warnings stricts),
      exécuté aussi par la CI plus tard
- [ ] Tests : moteur de sync + JSON + parsing status VLC compilés en exe de test
      console (`build.bat test`), verts ; ASan si dispo sinon revue mémoire stricte
- [ ] Testé en réel sur la machine Win10 contre le serveur local + vrai VLC

## Journal du ticket

- 2026-08-05 : créé (pivot handmade ADR-008).
