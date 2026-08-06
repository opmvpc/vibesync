---
id: VS-014
titre: Client Windows handmade — C pur + Win32, un seul exe
statut: terminé
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

- [x] Un exe unique < 500 Ko (172 Ko), démarrage < 100 ms (47 ms mesurés), DPI v2,
      thème sombre soigné, CPU idle 0 %
- [x] UI immediate-mode GDI double-buffer complète (connexion + salle + chat +
      toasts + drift), tout dessiné main, ini %APPDATA%
- [x] WebSocket via WinHTTP (ws/wss), reconnexion backoff, jeton de session
- [x] Moteur conforme spec, 12/12 vecteurs golden rejoués
- [x] VLC : CreateProcessW + IFileOpenDialog, HTTP Winsock, position fine
- [x] Build reproductible `build.bat` (clang llvm-mingw), exécuté en CI (runs verts)
- [x] 1 010 vérifications + ASan verts (json/protocol/vlc/engine/net/ini hostiles)
- [x] Testé en réel : 2 clients + serveur Go + vrai VLC (pause au chargement OK,
      zéro orphelin), captures dans docs/research/captures/

## Journal du ticket

- 2026-08-05 : créé (pivot handmade ADR-008).
- 2026-08-06 : cœur livré (541 checks) → review sol high (2 bloquants concurrence)
  → 11 fixes + règles autoplay/calage (973 checks) → UI GDI (1 010 checks, 172 Ko,
  47 ms). Verdict esthétique orchestrateur : validé. Terminé.
