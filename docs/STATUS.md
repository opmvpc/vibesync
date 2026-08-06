# STATUS — vibesync

*Dernière mise à jour : 2026-08-06 — **v0.2.0 taguée***

## Où on en est

**v0.2.0 sortie** : tag poussé, release GitHub avec exe Windows via CI. Serveur en
prod sur Coolify (https://vibesync.choboai.com, mdp `onlyvibes`, auto-deploy sur
push main). CI **entièrement verte** (qa-go + client-windows, 13/13 vecteurs des
deux côtés, flake réglé pour de vrai). Test réel sandbox : **PASS 7/7, drift
final 0,10 s**. Tous les retours terrain de Thibault sont traités côté
Windows ; les clients v0.1 verront la bannière de mise à jour à leur prochaine
connexion.

Client Windows : 242 Ko (48 % du budget ADR-008), 1 282 vérifications test+asan.
Aucun agent en vol.

## Chantiers

| Ticket | Titre | Statut |
|---|---|---|
| VS-001..008, 013, 014, 016..019, 021..026 | Socle → sprint retours terrain | terminés |
| VS-009..012 | Pistes Wails/WPF/SwiftUI-façade | abandonnés (ADR-006/008) |
| VS-015 | Client macOS Swift | code livré — **build/test sur le Mac (dispo aujourd'hui)** + port des règles récentes (resserrages terra, vecteur 13/keepOutput, Keychain, dossiers médias, injection version) |
| VS-020 | OSD dans VLC | ADR-009 (overlay maison) — implémentation à faire |

## Prochaine action

1. **Sur le Mac de Thibault** (VS-015) : `swift test` puis `scripts/build-macos.sh`
   (docs/build-macos.md). Le moteur Swift doit rejouer les 13 vecteurs — dont le
   13 régénéré avec `keepOutput`. À porter : resserrages terra, reprise vierge
   sur mémoire de séance, MinSuspendGap, file de chat liée à la salle, Keychain
   (VS-025), dossiers médias (VS-026), injection version.
2. VS-020 : overlay OSD Windows (layered window sur VLC, ADR-009).
3. Reliquats : captures mac dans le guide amis ; renommage module Go
   thibsix→opmvpc (aucun agent n'écrit actuellement — bon moment).

## Repères

- Spec : `docs/protocol.md` (source de vérité) ; vecteurs : `test/vectors/` (13,
  avec champs `keepOutput` et `scenario` depuis c811207)
- Sonde prod : `go run ./tools/probe wss://vibesync.choboai.com/ws [mdp]`
- Validation réelle : `scripts/run-real-sandbox.ps1` (Windows Sandbox)
- Rapports d'agents : `docs/research/`
