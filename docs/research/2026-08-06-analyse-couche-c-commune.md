---
titre: Faisabilité — couche C commune win32/macOS via target C SPM
date: 2026-08-06
statut: exploratoire (lecture seule, arbre non committé inclus VS-028/029)
auteur: agent Sonnet (Explore), consigné par l'orchestrateur
---

## 1. Inventaire ui/win32/src/ (lignes, portabilité)

| Fichier | .c/.h | Rôle | Portabilité |
|---|---|---|---|
| base.h | 200 | types u8..f64, Arena, Str8, StrBuf, Builder | portable pur (stdint/stddef only) |
| base.c | 626 | impl. base.h | mixte : Str8/Builder/nombres/utf8 (~250 l.) portable pur ; arènes (~90 l., `VirtualAlloc`) portable moy. abstraction (→ mmap/VirtualAlloc macro) ; `vs_now_ns` (`FILETIME`, ~10 l.) et `vs_random_bytes` (`BCryptGenRandom`, ~6 l.) idem ; `vs_log`/`log_path` (~90 l., `%APPDATA%`, `GetEnvironmentVariableW`) intrinsèque Win32 |
| json.h/.c | 105/622 | parseur+writer JSON sur arène | portable pur (aucun include OS) |
| protocol.h/.c | 124/307 | encodage/décodage docs/protocol.md, jeton session, semver | portable pur |
| engine.h/.c | 309/787 | **moteur de sync**, machine à états pure | portable pur |
| conn.h/.c | 67/209 | normalisation adresse + politique de reconnexion | portable pur (aucun include OS) |
| vlc.h/.c | 103/683 | HTTP VLC (Winsock) + parsing status + lancement process | mixte : base64/`http_parse_response`/`vlc_build_request`/`vlc_parse_status`/`vlc_build_command` (~300-350 l.) portable pur ; `winsock_init`/`http_get`/`free_port` (Winsock), `vlc_launch` (`CreateProcessW`), `vlc_locate`/`file_exists`/`env_var` (`GetFileAttributesW`, UTF-16, chemins `Program Files`) intrinsèque Win32 |
| net.h/.c | 143/479 | client WebSocket **WinHTTP**, thread dédié | `net_parse_url` (~70 l.) portable pur ; reste (~400 l., handles WinHTTP, `net_thread`) intrinsèque Win32 |
| ini.h/.c | 54/161 | fichier `%APPDATA%\vibesync.ini` | parse/get/set/write (~100 l.) portable pur ; `ini_path`+`ini_load_file`/`save_file` (UTF-16, API fichier Win32, ~60 l.) intrinsèque Win32 |
| health.h/.c | 41/130 | sonde `/healthz` en WinHTTP | intrinsèque Win32 (codes d'erreur WinHTTP) |
| secret.h/.c | 33/88 | mot de passe chiffré **DPAPI** (`CryptProtectData`) | intrinsèque Win32 (macOS : Keychain natif) |
| media.h/.c | 47/199 | dossiers médias, recherche bornée | `media_dirs_join`/`split` (~50 l.) portable pur ; `scan`/`consider`/`name_eq_ci`/`wlen` (`FindFirstFileW`, UTF-16, ~80 l.) et `media_default_dir` intrinsèque Win32 (portable moy. réécriture `readdir` POSIX) |
| ui.h/.c | 325/2068 | UI immediate-mode dessinée à la main en **GDI** | intrinsèque Win32, aucun équivalent souhaitable côté macOS (AppKit/SwiftUI natif) |
| main.c | — /2073 | fenêtre, `wnd_proc`, threads réseau/VLC/health | intrinsèque Win32 (orchestrateur, pas de logique métier propre) |
| test_main.c | — /3308 | harnais (1282+ vérifications, rejeu des 13 vecteurs) | inclut `windows.h`/`winsock2.h` pour `WSAStartup` (tests réseau) et exerce des chemins Win32 (secret, ui, media) — à cloisonner par cible si repris sur macOS |

## 2. Duplication en regard (Swift ↔ C)

| Swift (ui/macos/Sources/VibeSync/) | Lignes | Équivalent C | Doublon de logique | Reste natif |
|---|---|---|---|---|
| Engine/Engine.swift | 791 | engine.c (787) | quasi 1:1, commentaire du fichier l'assume (« Port Swift de ui/win32/src/engine.c ») | — |
| Engine/Types.swift | 156 | engine.h (types) | 1:1 | — |
| Engine/Time.swift | 92 | portions temps d'engine.h/base.h | ~1:1 | — |
| Net/Protocol.swift | 257 | protocol.c/.h (307/124) | encodage/décodage, jeton session, semver | — |
| Net/JSON.swift | 149 | json.c/.h (622/105) | Swift plus court (`JSONSerialization` en lecture) : logique métier dupliquée, pas le parseur bas niveau | — |
| VLC/VLCStatusParser.swift | 77 | vlc.c: `vlc_parse_status`+annexes (~120 l.) | 1:1, documenté | — |
| VLC/MediaLibrary.swift | 103 | media.c (199, dont ~130 l. logique bornée) | 1:1, « Port de ui/win32/src/media.c » | — |
| Net/Version.swift | 98 | `proto_semver_cmp` (protocol.c, ~25 l.) + bannière (main.c ~15 l.) | logique élargie côté Swift | — |
| (absent) « conn normalize » | — | conn.c: `conn_normalize_url`/`conn_should_attempt` (209 l.) | **non dupliqué** : `WebSocketClient.parseURL` (Foundation) n'auto-préfixe pas un hôte nu comme `conn_normalize_url` — écart UX potentiel | — |
| VLC/VLCClient.swift (256) / VLCLauncher.swift (176) | 432 | vlc.c non-portable (~380 l.) | comportement identique (retry/prepare/backoff) réimplémenté sur APIs différentes | `URLSession`, `Process` |
| Net/WebSocketClient.swift | 200 | net.c non-portable (~400 l.) | protocole WS délégué à l'OS | `URLSessionWebSocketTask` |
| Net/Keychain.swift | 71 | secret.c (88, DPAPI) | même finalité, API différente | `Security.framework` |
| UI/AppModel.swift (707) + vues (~600) | ~1300 | main.c (2073) + ui.c (2068) | orchestration seulement | `AppKit`/`SwiftUI` |
| UI/Preferences.swift | 144 | ini.c portable (~100 l.) + secret | mêmes règles réimplémentées sur `UserDefaults` | `UserDefaults` |

## 3. Contenu candidat de la couche commune

| Bloc | Vit aujourd'hui (C) | Vit aujourd'hui (Swift) | Extractible sans toucher Win32 ? |
|---|---|---|---|
| Moteur de sync | engine.c/h (1096 l., déjà isolé, 0 dép. net/vlc/ui) | Engine+Types+Time (1039 l.) | **Oui, immédiat** — déjà « pur » |
| Protocole | protocol.c/h (431 l.) | Protocol.swift (257 l.) | **Oui** — 0 dépendance OS |
| JSON | json.c/h (727 l.) | JSON.swift (149 l.) | **Oui** — 0 dépendance OS |
| Parsing status VLC | ~120 l. mêlées à vlc.c | VLCStatusParser.swift (77 l.) | **Oui, après découpage fichier** (`vlc_status.c`) |
| Règles de version | `proto_semver_cmp` (25 l.) | Version.swift (98 l.) | Oui pour le cœur semver ; « bannière » reste UI |
| Dossiers médias | ~130 l. portables mêlées à `FindFirstFileW` | MediaLibrary.swift (103 l.) | Partiel — algorithme extractible avec primitive `vs_dir_iter` par OS |
| Jeton de session | `proto_session_token`/`_valid` (~25 l.) | Protocol.swift + Preferences | Oui (validation/format) ; génération via abstraction aléa |
| Normalisation adresse / retry | conn.c (276 l., 0 dép. OS) | **non porté** | Oui, et souhaitable (comble l'écart UX §2) |

Total « déjà pur » réutilisable tel quel : **2 730 lignes** (engine+protocol+json+conn+base.h). Avec les sous-ensembles portables de base.c/vlc.c/media.c/ini.c après découpage : **≈ 3 800-4 000 lignes de C** candidates, contre **≈ 1 720 lignes de Swift** dupliquant la même logique.

## 4. Frontière d'API du moteur

`engine.c/h` est déjà « décisions entrantes/sortantes sans E/S », même contrat qu'Engine.swift : `engine_on_welcome/_pong/_roomstate/_vlc_status/_tick/_user_control` en entrée, `VsOutput{cmds[], msgs[], dropped, resume_toast}` en sortie — API par **polling** (pas de callback), état dans `VsEngine` alloué par l'appelant, pas de threading interne (l'appelant sérialise). Les champs d'état sont des `StrBuf` bornés (512 o) : aucun pointeur ne survit à l'appelant. Pour Swift via interop C : (1) tout borner en `StrBuf`/tableaux fixes à la frontière (déjà le cas pour `VsMsg`/`VsOutput`) ; (2) `Arena`/`Str8` non bornés restent internes au module C ; (3) le modèle polling convient tel quel à `AppModel.swift` ; aucun callback C→Swift requis au départ.

## 5. Build et CI

- **Windows** : `build.bat` compile déjà une liste `CORE` en un appel clang ; extraire une variable `CORE_SHARED` ne casse rien.
- **macOS** : `Package.swift` n'a aucune cible C aujourd'hui ; ajouter `.target(name: "VSCore", path: ...)` + `module.modulemap` est la voie SPM standard — zéro dépendance, conforme ADR-008.
- **CI** : job `client-windows` seul aujourd'hui ; le commentaire de `ci.yml` anticipe déjà un job `macos-latest`. Rejouer `test_main.c` sur macOS = non-régression transversale (mêmes vecteurs, deux toolchains clang), après scission en `test_core.c` (portable) + `test_win32.c`.

## 6. Risques et coût

- **Points durs** : UTF-16↔UTF-8 (la majorité du code « intrinsèquement Win32 » de base.c/media.c/vlc.c vient de là) ; arènes `VirtualAlloc` → abstraction mmap (~90 l.) ; double maintenance temporaire (Go référence + C partagé + Swift pendant la bascule) ; cibles C SwiftPM parfois capricieuses selon toolchain.
- **Coût grossier** : découpages (~1-2 j-agent), cible C SPM + premier lien (~1-2), rejeu vecteurs via API C côté Swift en double couverture (~2-3), bascule et retrait des fichiers Swift dupliqués (~3-4), job CI macOS + test_main scindé (~1-2).
- **Phases sûres** (13 vecteurs verts des deux côtés à chaque phase, Swift natif en secours jusqu'à bascule complète) :
  1. Scinder vlc.c/media.c/ini.c/base.c en portable/Win32, côté win32 uniquement (aucun changement de comportement).
  2. Cible C SPM `VSCore` (fichiers portables + engine/protocol/json/conn), liée par les tests Swift seulement ; rejeu des 13 vecteurs via l'API C EN PLUS de VectorsTests.swift.
  3. Bascule de l'exécutable sur VSCore pour le moteur ; retrait Engine/Types/Time.swift une fois vert.
  4. Bascule protocole/JSON/VLC-status/media/version un à un (ajout parallèle → bascule → retrait).
  5. Job CI `macos-latest` rejouant la partie portable de test_main + `swift test`.

**Faisable en 5 phases, coût estimé 10-14 jours-agent, risque principal : la conversion UTF-16↔UTF-8 dispersée dans vlc.c/media.c/base.c, à isoler derrière une frontière stable avant toute extraction, faute de quoi la cible C portera silencieusement des dépendances Win32 résiduelles.**
