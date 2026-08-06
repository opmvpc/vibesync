# Rapport — cœur du client Windows handmade (VS-014, passe 1)

Périmètre : `ui/win32/**`. Aucune fenêtre : le cœur seul, testable.

## Fichiers

`ui/win32/build.bat` (release / test / asan / clean) et `ui/win32/src/` :
`base.c/h` (arènes VirtualAlloc réserve+commit, Str8/StrBuf, Builder, UTF-8↔UTF-16
main, nombres, temps ns), `json.c/h` (parseur + écrivain, tout en arène),
`protocol.c/h` (docs/protocol.md v1), `engine.c/h` (moteur de sync **pur**),
`vlc.c/h` (localisation, CreateProcessW, HTTP/1.1 Winsock, status.json),
`net.c/h` (WebSocket WinHTTP, thread dédié + file SPSC SRWLOCK), `main.c`
(boucle 200 ms sans UI), `test_main.c`. ~2 400 lignes de cœur, 1 100 de tests.

## Décisions

- **Moteur pur** : `engine_on_pong/on_roomstate/on_vlc_status/on_tick` rendent des
  décisions (`VsOutput` : commandes VLC + messages serveur) ; zéro dépendance à
  `net.c`/`vlc.c`, zéro E/S, zéro verrou — c'est ce qui le rend rejouable.
- **Réseau** : WinHTTP synchrone sur un thread dédié (réassemblage des
  `*_FRAGMENT_BUFFER_TYPE`), événements poussés dans une file à créneaux fixes
  (16 × 16 Kio) — pas d'allocation ni de question de propriété. Émission depuis le
  thread appelant sous SRWLOCK (WinHTTP autorise send/receive concurrents).
- **hello** porte le champ `session` (16 octets BCryptGenRandom, hex), conservé
  pour la vie du process ; vérifié en réel (`reprise=false` côté serveur).
- **UCRT** (`snprintf`/`strtod`) uniquement pour l'aller-retour exact des doubles ;
  `ucrtbase.dll` est un composant de Windows 10, pas une lib tierce.

## Écarts avec la référence Go (assumés)

1. Pas d'`outbox` interne : l'appelant émet immédiatement, donc rien à purger à
   l'invalidation de référence.
2. Détecteur de buffering placé dans `engine.c` (pur) plutôt que dans `vlc.c`.
3. `vlc_locate` : `%VIBESYNC_VLC%` + chemins standards, sans recherche dans le PATH.
4. JSON : plus strict que Go (UTF-8 mal formé refusé, profondeur ≤ 32, entrée
   ≤ 8 Mio) ; substituts orphelins → U+FFFD comme Go.
5. Bornes dures : message WS ≤ 16 Kio, 64 participants, StrBuf 512 o (troncature).

## QA

`build.bat test` : **541 vérifications, 0 échec**, dont **les 12 vecteurs rejoués et
conformes**. Le rejeu réimplémente `vlctest.Fake` et passe par `vlc_parse_status`,
donc json + vlc sont exercés au passage ; tolérance 1e-3 sur la trace (golden
arrondi au millième), 1e-6 sur les charges utiles. Harnais mutation-testé
(`PAUSED_SEEK 0.6→0.2` et `USER_HOLD 2→1 s` font bien échouer 09 et 08).
`build.bat asan` : vert ; l'arène s'empoisonne elle-même sous ASan
(`__asan_poison_memory_region`), vérifié par un dépassement volontaire détecté.
Smoke test réel contre le serveur Go local : upgrade wss/ws, `welcome`, ping/pong,
`name_taken` fatal, fermeture propre.

**Taille** : `vibesync.exe` release = **64 512 octets** (63 Ko) ; tests 381 Ko.
