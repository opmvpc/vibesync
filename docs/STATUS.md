# STATUS — vibesync

*Dernière mise à jour : 2026-08-07 — **ADR-010 intégralement livré***

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

**Couche applicative C commune (ADR-010) : 4 phases sur 5 livrées le jour
même.** `core/` est la vérité unique des deux clients : moteur, protocole,
JSON, parsing status VLC, médias, politique de connexion. L'app macOS tourne
sur le moteur C (CoreEngine.swift = frontière sans décision, −883 lignes
dupliquées ; puis −~420 lignes de règles en phase 4), chaque phase validée par
les 13 vecteurs des deux côtés + séance réelle PASS 10/10 contre la prod. La
suite C tourne sur macOS (878 checks asan+ubsan via scripts/test-core-macos.sh).
Trouvailles en chemin : fichiers accentués NFD introuvables sur APFS (corrigé,
normalisation NFC dans name_eq_ci posix), bannière de versions fausse sur les
2 clients (VS-036 corrigé : proto_newer_version, portage exact du Go).
**Phase 5 livrée le 07 (b9dfa78)** : job CI client-macos (suite C asan+ubsan,
swift test, budget .app) — les 3 jobs verts ensemble. ADR-010 : FAIT.

**VM Win11 opérationnelle par SSH** (`ssh -i ~/.ssh/vibesync_vm_ed25519
OPMVPC@192.168.64.2` — jamais par l'écran) et **provisionnée par script**
(`scripts/provision-vm.ps1`, rejouable) : MinGit/Go/VLC en ARM64 natif,
llvm-mingw ucrt aarch64, repo cloné. **`build.bat test` : 1437 vérifications,
0 échec, 13/13 vecteurs** — y compris DPAPI/Winsock/WinHTTP réels (émulation
x86_64) ; `go build/vet/test` verts. Bonus : la VM a débusqué un vrai bug de
`build.bat` (« if errorlevel 1 » avale les codes négatifs → un crash Windows
passait pour un succès ; corrigé) et établi qu'ASan est impossible sur Windows
ARM64 (émulation + pas de runtime aarch64) — la couverture sanitizer du C
commun reste le job client-macos, seul le C Win32 est hors sanitizer.

**Release .app tranchée par Thibault : signature ad hoc** (pas de compte Apple
Developer). CI adaptée : vérif dure de la signature dans client-macos, zip
`ditto` sur tag v*, release à 3 needs qui attache exe Windows + .app macOS en
un seul `gh release create` (rejouable). Guides amis/build mis à jour (2
chemins Gatekeeper dont Sequoia). Publiée au prochain tag (v0.2.1).

## Chantiers

| Ticket | Titre | Statut |
|---|---|---|
| VS-001..008, 013..019, 021..028 | Socle → sprint retours terrain → clients natifs | terminés |
| VS-009..012 | Pistes Wails/WPF/SwiftUI-façade | abandonnés (ADR-006/008) |
| VS-029 | Attache VLC chez l'utilisateur | blindage livré ; **validation réelle Windows en attente (VM ou PC)** |
| VS-020 | Overlay OSD Windows (ADR-009) | en attente du PC Windows (demande de Thibault) |
| VS-030..034, 036 | Couche C commune (ADR-010, 5 phases) + fix semver | **terminés, CI verte 3 jobs** |
| VS-035 | UB str_to_i64 (préexistant) | ouvert, priorité basse |

## Prochaine action

1. Dérouler les critères Windows-only de VS-029 dans la VM par SSH (repro
   vlcrc Syncplay, détection d'action réelle, séance 2 clients C) puis tag
   v0.2.1 (premier tag qui publie aussi la .app).
2. Reliquats : aligner internal/vlc/launch.go sur les 9 nouveaux drapeaux VLC
   du C ; captures mac du guide amis ; retours de ini_flush ignorés
   (durcissement) ; VS-035.

## Repères

- Spec : `docs/protocol.md` (source de vérité) ; vecteurs : `test/vectors/` (13)
- Test réel mac : `VIBESYNC_PASSWORD=... ./scripts/run-real-macos.sh "" wss://vibesync.choboai.com/ws`
- Sonde prod : `go run ./tools/probe wss://vibesync.choboai.com/ws [mdp]` (Go absent du Mac — passer par la CI ou la VM)
- Rapports d'agents : `docs/research/` (analyse couche C commune : 2026-08-06)
