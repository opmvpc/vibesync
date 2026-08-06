# STATUS — vibesync

*Dernière mise à jour : 2026-08-06 (soir) — reprise post-coupure de quota, sur le Mac*

## Où on en est

**Le client macOS est né et validé en réel** : VS-015 terminé (73499fc). Moteur
Swift réaligné 13/13 vecteurs, ports VS-023/025/026/028 (version, Keychain,
dossiers médias, jeton persisté + close 1000), review croisée terra appliquée,
**test réel PASS 10/10** contre la prod avec 2 vrais VLC (harnais
`scripts/run-real-macos.sh`, écart final 0,004 s). Binaire 1,1 Mo (11 % du
budget). Le harnais a débusqué 3 bugs terrain (reconnexion distante impossible
— isOpen tuait le handshake toutes les 200 ms —, `--no-one-instance` refusé par
le VLC macOS, App Nap).

**Côté Windows (codé à l'aveugle depuis le Mac, CI juge — verte)** : VS-028
part C (jeton persisté ini + close 1000, un seul propriétaire des handles
WinHTTP) et blindage VS-029 (lancement VLC durci contre un vlcrc Syncplay,
cause racine probable `--one-instance-when-started-from-file`, VLC orphelin
tué, vs_log %APPDATA%) livrés en 8b2982d.

Module Go renommé thibsix→opmvpc (f676295). Serveur prod inchangé
(vibesync.choboai.com, mdp `onlyvibes`, auto-deploy sur push). CI verte
(attention : 3 flakes infra GitHub « Failed to resolve action download info »
aujourd'hui — toujours re-lancer avant de chercher un bug).

**Nouvelle direction validée par Thibault : couche applicative C commune aux
deux clients** (lib statique, PAS le modèle core+façade abandonné d'ADR-006).
Analyse chiffrée : `docs/research/2026-08-06-analyse-couche-c-commune.md` —
faisable en 5 phases, ~2 730 lignes de C déjà portables telles quelles,
risque principal l'UTF-16 dispersé. ADR-010 + tickets à rédiger.

**Environnement Windows local en préparation** : UTM installé (+ utmctl), ISO
Windows 11 ARM64 25H2 française officielle en téléchargement vers ~/Downloads
→ VM à monter pour retrouver build.bat/asan/tests réels Windows (M1 Pro : pas de
virtualisation imbriquée, donc pas de Windows Sandbox dans la VM ; snapshots
UTM en guise de jetable). VMware Fusion impossible sans compte Broadcom.

## Chantiers

| Ticket | Titre | Statut |
|---|---|---|
| VS-001..008, 013..019, 021..028 | Socle → sprint retours terrain → clients natifs | terminés |
| VS-009..012 | Pistes Wails/WPF/SwiftUI-façade | abandonnés (ADR-006/008) |
| VS-029 | Attache VLC chez l'utilisateur | blindage livré ; **validation réelle Windows en attente (VM ou PC)** |
| VS-020 | Overlay OSD Windows (ADR-009) | en attente du PC Windows (demande de Thibault) |
| (à créer) | Couche C commune — ADR-010 + phases 1..5 | analyse faite, ADR à rédiger |

## Prochaine action

1. Rédiger ADR-010 (couche C commune) + tickets de phases, faire valider, puis
   lancer la phase 1 (scission portable/Win32 de vlc.c/media.c/ini.c/base.c).
2. Monter la VM Win11 (UTM + ISO dans ~/Downloads) → dérouler les critères
   Windows-only de VS-029 (repro vlcrc Syncplay, détection d'action réelle,
   séance 2 clients C) puis tag v0.2.1.
3. Reliquats : aligner internal/vlc/launch.go sur les 9 nouveaux drapeaux VLC
   du C ; toast « Reprise à HH:MM:SS » dans l'UI mac ; captures mac du guide
   amis ; retours de ini_flush ignorés (durcissement, non bloquant).

## Repères

- Spec : `docs/protocol.md` (source de vérité) ; vecteurs : `test/vectors/` (13)
- Test réel mac : `VIBESYNC_PASSWORD=... ./scripts/run-real-macos.sh "" wss://vibesync.choboai.com/ws`
- Sonde prod : `go run ./tools/probe wss://vibesync.choboai.com/ws [mdp]` (Go absent du Mac — passer par la CI ou la VM)
- Rapports d'agents : `docs/research/` (analyse couche C commune : 2026-08-06)
