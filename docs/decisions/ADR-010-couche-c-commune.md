---
id: ADR-010
titre: Couche applicative C commune aux clients Windows et macOS (lib statique)
statut: accepté           # proposé par Thibault le 2026-08-06, accepté sur analyse
date: 2026-08-06
---

## Contexte

Depuis ADR-008, la logique client existe en trois exemplaires : Go
(`internal/client`, référence qui génère `test/vectors/`), C (`ui/win32`,
produit Windows) et Swift (`ui/macos`, produit macOS). Chaque règle nouvelle se
paie trois fois, et le port Swift avait déjà dérivé de la référence en deux
jours (constaté lors de VS-015 : keepOutput, reprise salle vierge, suspension
buffering manquants). L'analyse chiffrée
(`docs/research/2026-08-06-analyse-couche-c-commune.md`) montre que
`engine.c`, `protocol.c`, `json.c` et `conn.c` sont déjà du C portable pur
(2 730 lignes, zéro include OS) avec une API « décisions entrantes/sortantes,
polling, buffers bornés » directement consommable par Swift, et que ~1 720
lignes de Swift dupliquent cette logique ligne à ligne.

## Décision

Les deux clients natifs partagent une couche applicative C commune, liée
statiquement :

- Le client Win32 la consomme directement (liste `CORE_SHARED` dans build.bat).
- Le client macOS la consomme via une cible C SwiftPM (`VSCore` +
  `module.modulemap`) — interop C native, zéro dépendance, ADR-008 respecté.
- Contenu cible : moteur de sync, protocole (encode/décode, jeton, semver),
  JSON, parsing du status VLC, construction de la ligne de commande VLC,
  normalisation d'URL et politique de reconnexion (`conn.c`, jamais porté en
  Swift — écart UX comblé), logique bornée des dossiers médias derrière une
  primitive de parcours par OS.
- Reste natif de plein droit : UI (GDI / AppKit-SwiftUI), transports WebSocket
  (WinHTTP / URLSessionWebSocketTask), lancement de process, persistance
  (ini / UserDefaults), secrets (DPAPI / Keychain).
- Le client Go reste la référence de comportement et le générateur des
  vecteurs ; le moteur Swift natif n'est retiré qu'après bascule verte.
- La frontière UTF-8/UTF-16 est isolée côté Win32 AVANT toute extraction
  (risque n°1 de l'analyse) : la couche commune parle UTF-8 exclusivement.
- Migration en 5 phases (tickets VS-030..034), chaque phase laissant les 13
  vecteurs verts des deux côtés et `build.bat test` + `swift test` verts.

## Alternatives écartées

- **Statu quo (3 implémentations)** : coût par règle ×3, dérive prouvée.
- **Core en processus séparé + protocole UI** (ex-VS-010, ADR-006) : déjà
  abandonné — IPC, cycle de vie double, à rebours du « un seul exe ».
- **Core en Go partagé (cgo/gomobile)** : runtime Go embarqué dans chaque
  client, à rebours d'ADR-008 (poids, GC, toolchain croisée).
- **Réécrire les clients dans un seul langage/UI multiplateforme** : abandonné
  par ADR-006/008 (natif assumé).

## Conséquences

- Une règle nouvelle s'écrit deux fois (Go référence + C commun) au lieu de
  trois, et les vecteurs la verrouillent des deux côtés.
- Le cœur C devient compilable et testable sur macOS : fin du dev 100 % à
  l'aveugle pour la logique partagée, et un job CI `macos-latest` rejoue la
  partie portable de `test_main.c` (déjà anticipé dans ci.yml).
- Transition : double couverture temporaire (moteur Swift natif + VSCore) ;
  coût estimé 10-14 jours-agent.
- Le budget taille (ADR-007) reste trivial : la lib est du C sans dépendance.
