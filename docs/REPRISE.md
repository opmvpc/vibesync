# REPRISE — état exact au 2026-08-06 (coupure de quota)

Fichier de reprise d'urgence. Lire aussi `docs/STATUS.md`, les tickets, et le
journal `docs/journal/2026-08-06.md`. Contexte long : la v0.2.0 est sortie
(release GitHub + prod Coolify OK, CI verte, sandbox PASS 7/7 drift 0,10 s),
puis Thibault a fait la **première vraie séance C↔C** et remonté 3 bugs terrain.

## Agents EN VOL au moment de la coupure (rien de leur travail n'est committé)

Leurs modifs sont dans l'arbre de travail, NON committées — voir `git status`.
NE PAS faire `git add -A` (règle absolue). Les relancer depuis leurs transcripts
si tués (SendMessage sur leur id ; sinon relire leur fichier tasks/*.output).

0. ~~Agent Opus « icônes »~~ : RENDU et committé (a63f207, VS-027 terminé) juste
   avant la coupure. ui/win32 est donc LIBRE pour l'agent C.
1. **Agent Go (id ab3ae55ba397b6158)** — part Go/serveur de VS-028 : jeton de
   session persisté dans les réglages du client de réf (internal/client/state.go
   créé), close 1000 au départ volontaire retirée immédiatement côté serveur,
   3 tests (persistance, close→pseudo libéré avec jeton différent, kill brutal→
   reprise avec même jeton). Fichiers : cmd/vibesync/main.go, internal/client/.
   QA : build/vet/staticcheck + go test -count=1 -shuffle=on ./...

## À faire au retour (dans l'ordre)

1. Réceptionner les 2 rendus : contre-vérifier (QA ci-dessus), committer par
   chemins explicites, pousser (auto-deploy Coolify sur push main).
2. **Dispatcher VS-029 (CRITIQUE) à l'agent C (id a85587ec9aa30f8a1)** dès que
   l'agent icônes a rendu (collision ui/win32 sinon) — lire le ticket
   docs/tickets/VS-029-attache-vlc-et-controles.md. En bref : l'attache HTTP à
   VLC échoue chez Thibault (VLC lancé joue en autoplay, app dit « aucun fichier
   ouvert », aucun contrôle, pause VLC non détectée). Suspect n°1 : vlcrc perso
   configuré par Syncplay (lua intf). Exigences : blindage du lancement contre
   la config utilisateur, VLC orphelin fermé en cas d'échec, erreur actionnable
   + trace dans %APPDATA%\vibesync.log, et PROUVER la détection d'action
   utilisateur (pause/play/seek dans VLC) avec le vrai client C en sandbox —
   le harnais actuel ne teste que le client Go (trou à combler).
   + part client C de VS-028 : jeton persisté dans l'ini + close 1000 à la
   fermeture de fenêtre et au « Quitter la salle ».
3. Thibault doit fournir s'il peut : dernières lignes de %APPDATA%\vibesync.log
   (sa machine ou celle de l'ami) pour le diagnostic VS-029.
4. Après fixes : CI verte → sandbox → tag v0.2.1 → clôture tickets/STATUS/journal.

## File d'attente après v0.2.1

- VS-015 : build Swift sur le Mac de Thibault (dispo aujourd'hui) + port des
  règles récentes (resserrages terra, vecteur 13/keepOutput, Keychain, dossiers
  médias, injection version, jeton persisté).
- VS-020 : overlay OSD Windows (ADR-009).
- Renommage module Go thibsix→opmvpc (quand aucun agent n'écrit).

## Rappels de contexte qui ne sont écrits nulle part ailleurs

- Le tag v0.2.0 a des notes de release réécrites pour les amis (gh release edit).
- La prod porte VERSION 0.2.0 → les clients v0.1 voient la bannière de mise à jour.
- Spec amendée d45c1e5 : jeton persisté + close 1000 volontaire (§hello,
  §Erreurs et robustesse).
- Le flake TestIntegrationDebitNormalNonAffecte est MORT (barrière sync(),
  vérifié -count=50) — ne pas le rouvrir.
- Vecteurs : champs `keepOutput` et `scenario` depuis c811207 ; 13/13 rejoués
  par Go ET C. Toute évolution du moteur = régénérer côté Go (`-update`) puis
  rejouer côté C.
- Machine : Go = "C:\Program Files\Go\bin\go.exe" (pas dans PATH), staticcheck =
  %USERPROFILE%\go\bin, clang = C:\Users\thibs\tools\llvm-mingw\bin, cmd /c pour
  build.bat (attention [Environment]::CurrentDirectory ≠ Set-Location en PS 5.1).
