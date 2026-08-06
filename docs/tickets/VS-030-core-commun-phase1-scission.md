---
id: VS-030
titre: Core commun phase 1 — scission portable/Win32 de vlc.c, media.c, ini.c, base.c
statut: ouvert
priorité: haute
dépend-de: []
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

ADR-010, analyse `docs/research/2026-08-06-analyse-couche-c-commune.md` §1/§6.
Préparer l'extraction en cloisonnant, côté `ui/win32` uniquement, le C portable
du C Win32 — la frontière UTF-8/UTF-16 est LE risque n°1 : la partie portable ne
doit voir que de l'UTF-8 (`Str8`).

## Critères d'acceptation

- [ ] `vlc.c` scindé : parsing status + construction requêtes/commandes/ligne de
      commande (portable) vs Winsock/CreateProcessW/locate (Win32)
- [ ] `media.c` scindé : algorithme borné (portable, primitive `vs_dir_iter`
      abstraite) vs FindFirstFileW/défauts de chemins (Win32)
- [ ] `ini.c` scindé : parse/get/set/write (portable) vs chemin+fichier (Win32)
- [ ] `base.c` scindé : Str8/Builder/utf8/nombres (portable) vs arènes
      VirtualAlloc/aléa/horloge/log derrière macros ou fichier plateforme
- [ ] Aucun `#include <windows.h>` ni wchar_t dans les fichiers portables ;
      aucun changement de comportement (diff de logique nul)
- [ ] `build.bat` : variable `CORE_SHARED` listant les fichiers portables
- [ ] `test_main.c` scindé en `test_core.c` (portable, 13 vecteurs inclus) +
      `test_win32.c` ; build.bat test + asan verts, 13/13, budget < 500 Ko

## Journal du ticket

- 2026-08-06 : créé (ADR-010).
