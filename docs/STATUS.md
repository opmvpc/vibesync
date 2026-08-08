# STATUS — vibesync

*Dernière mise à jour : 2026-08-08 (soir) — **VS-038 : 1× constant + micro-seek**
(le nudge ±5 % est supprimé des 3 implémentations)*

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
| VS-029 | Attache VLC chez l'utilisateur | **terminé** — bug terrain trouvé/corrigé dans le moteur commun, séance 2 clients C PASS 13/13 dans la VM, vecteur 14 |
| VS-020 | Overlay OSD Windows (ADR-009) | en attente du PC Windows (demande de Thibault) |
| VS-030..034, 036 | Couche C commune (ADR-010, 5 phases) + fix semver | **terminés, CI verte 3 jobs** |
| VS-035 | UB str_to_i64 (préexistant) | ouvert, priorité basse |
| VS-038 | Resync 1× constant + micro-seek (retour terrain nudge) | **terminé** — zone morte 1,5 s, médiane de persistance, seek immédiat 5 s ; 0 commande rate dans les 2 séances réelles |

## Prochaine action

**v0.2.1 est publiée** (tag sur 769aca1, CI verte, release avec vibesync.exe
258 Ko + VibeSync-macos-arm64.zip 311 Ko — la première .app). Serveur prod
redéployé (Coolify healthy, sonde OK depuis la VM).

**Sprint reliquats du 08 (journal du jour), publié en v0.2.2** : VS-035 clos,
ini_flush durci PUIS rendu atomique (VS-037 terminé le jour même à la demande
de Thibault), les TROIS lanceurs VLC alignés et gelés par tests (launch.go
15/12 par OS, Swift 12 — il lui manquait --lua-intf=http, le filet
anti-Syncplay), captures mac du guide faites sur une vraie séance prod, icône
.icns de VibeSync.app livrée puis regénérée par genicon -iconset (357 Ko,
bundle 1,4 Mio).

**VS-038 livré le soir du 08** (retour terrain de Thibault sur le nudge) : la
vitesse ne corrige plus jamais la dérive — zone morte 1,5 s, micro-seek
conditionné à la médiane des 5 derniers polls, seek immédiat à 5 s. Les trois
implémentations sont alignées, 14/14 vecteurs des deux côtés, et les deux
séances réelles (mac PASS 11/11, VM 2 clients C PASS 14/14) affichent
**0 commande rate**. À faire : tag d'une version portant ce changement.

Il ne reste QUE ce qui exige un vrai PC Windows x86_64 : VS-020 (overlay OSD),
revérification one-instance, asan du C Win32. Idée notée : un gel commun des
3 listes de drapeaux (les gels actuels sont indépendants).

## Repères

- Spec : `docs/protocol.md` (source de vérité) ; vecteurs : `test/vectors/` (14)
- Test réel mac : `VIBESYNC_PASSWORD=... ./scripts/run-real-macos.sh "" wss://vibesync.choboai.com/ws`
- Sonde prod : `go run ./tools/probe wss://vibesync.choboai.com/ws [mdp]` (Go absent du Mac — passer par la CI ou la VM)
- Rapports d'agents : `docs/research/` (analyse couche C commune : 2026-08-06)
