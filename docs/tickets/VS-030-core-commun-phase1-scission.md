---
id: VS-030
titre: Core commun phase 1 — scission portable/Win32 de vlc.c, media.c, ini.c, base.c
statut: terminé
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

- [x] `vlc.c` scindé : parsing status + construction requêtes/commandes/ligne de
      commande (portable) vs Winsock/CreateProcessW/locate (Win32)
- [x] `media.c` scindé : algorithme borné (portable, primitive `vs_dir_iter`
      abstraite) vs FindFirstFileW/défauts de chemins (Win32)
- [x] `ini.c` scindé : parse/get/set/write (portable) vs chemin+fichier (Win32)
- [x] `base.c` scindé : Str8/Builder/utf8/nombres (portable) vs arènes
      VirtualAlloc/aléa/horloge/log derrière macros ou fichier plateforme
- [x] Aucun `#include <windows.h>` ni wchar_t dans les fichiers portables ;
      aucun changement de comportement (diff de logique nul)
- [x] `build.bat` : variable `CORE_SHARED` listant les fichiers portables
- [x] `test_main.c` scindé en `test_core.c` (portable, 13 vecteurs inclus) +
      `test_win32.c` ; build.bat test + asan verts, 13/13, budget < 500 Ko

## Journal du ticket

- 2026-08-06 : créé (ADR-010).
- 2026-08-06 : livré (ffa4ef9). Partie portable exécutée sur macOS : 792
  vérifications, 13/13 vecteurs, asan+ubsan verts — première fois que le C du
  client tourne hors Windows. CI Windows verte (client-windows, parité des
  sites CHECK ±0). Frontières : platform.h/VsDirOps, name_eq_ci reste
  plateforme (CompareStringOrdinal), UTF-16 confiné aux *_win32.c. Trouvaille
  UBSan → VS-035.
