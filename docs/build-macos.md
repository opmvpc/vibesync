# Construire et lancer le client macOS

Le client macOS (`ui/macos/`) est un paquet SwiftPM **sans aucune dépendance
externe** : uniquement Foundation, AppKit, SwiftUI et Security (ADR-008).
Le code a été écrit sur le PC ; ce document est le pas-à-pas exact pour le
compiler sur le Mac.

## 1. Prérequis

Un seul :

```sh
xcode-select --install     # Xcode Command Line Tools (fournit swift)
swift --version            # doit répondre (Swift 5.7 minimum)
```

Xcode complet n'est **pas** nécessaire. VLC doit être installé pour l'usage
réel (`/Applications/VLC.app`), pas pour compiler.

## 2. Tests — d'abord, toujours

```sh
cd ~/chemin/vers/vibesync/ui/macos
swift test
```

`swift test` rejoue les **12 vecteurs de conformité** (`test/vectors/*.json`),
qui gèlent le moteur de synchronisation partagé avec l'implémentation Go et le
port C. Les vecteurs sont cherchés dans cet ordre :

1. `$VIBESYNC_VECTORS` s'il est défini,
2. `../../test/vectors` (le cas normal quand on lance depuis `ui/macos`),
3. en remontant depuis les sources du test.

Pour les prendre ailleurs :

```sh
VIBESYNC_VECTORS=/chemin/vers/test/vectors swift test
```

Un échec de vecteur indique **le nom du vecteur, l'instant en millisecondes et
le champ divergent** — c'est ce qu'il faut me renvoyer tel quel.

## 3. Build et bundle

```sh
cd ~/chemin/vers/vibesync
./scripts/build-macos.sh
```

Le script :

1. `swift build -c release`
2. assemble `ui/macos/build/VibeSync.app` (Info.plist minimal,
   `org.vibesync.client`, macOS 13 minimum, Retina),
3. signe le bundle en ad hoc (`codesign --sign -`),
4. affiche la taille finale (objectif : très en dessous des 10 Mo du budget).

Le script est idempotent : on peut le relancer autant de fois que nécessaire.
Sur un Mac Intel, `./scripts/build-macos.sh --arm64` force la cible Apple
Silicon.

Lancer l'application :

```sh
open ui/macos/build/VibeSync.app
```

## 4. Distribution aux amis (Gatekeeper)

La signature est **ad hoc** : l'app n'est ni signée par un compte développeur
ni notarisée. Au premier lancement, macOS refuse un double-clic normal. La
manœuvre à indiquer aux amis :

> Clic droit sur **VibeSync.app** → **Ouvrir** → **Ouvrir** dans la boîte de
> dialogue. Une seule fois : les lancements suivants sont normaux.

Si l'app a été téléchargée (et non copiée depuis une clé USB ou AirDrop), on
peut aussi retirer la mise en quarantaine :

```sh
xattr -dr com.apple.quarantine /Applications/VibeSync.app
```

Sur macOS Sequoia et plus récent, le clic droit → Ouvrir ne suffit plus
toujours : le chemin est alors Réglages Système → Confidentialité et sécurité →
**Ouvrir quand même**, après un premier double-clic refusé. Les deux chemins
sont décrits pour les amis dans `docs/guide-amis-macos.md`.

### Ce que publie la CI (tags `v*`)

Le job `client-macos` de `.github/workflows/ci.yml` archive le bundle avec
`ditto -c -k --keepParent` (le seul outil qui préserve les xattrs, le bit
d'exécution et la signature d'une `.app`) sous le nom
**`VibeSync-macos-arm64.zip`**, et le job `release` l'attache à la même release
GitHub que `vibesync.exe`. Aucune notarisation : c'est un choix assumé (pas de
compte Apple Developer), d'où la manœuvre Gatekeeper ci-dessus.

## 5. Utilisation

1. Écran de connexion : serveur (`wss://…/ws`), pseudo, salle — mémorisés dans
   les préférences (`UserDefaults`), donc à ne saisir qu'une fois.
2. Écran de salle : **Ouvrir un fichier…** lance VLC sur votre copie du média
   (interface HTTP locale sur 127.0.0.1, port et mot de passe aléatoires).
   VLC démarre en pause à la position 0, le temps que la salle soit prête.
3. **Je suis prêt** quand tout le monde a chargé son fichier ; le serveur ne
   laisse démarrer la lecture que lorsque tous les membres sont prêts.
4. Lecture/pause, barre de position cliquable, chat et toasts sont
   synchronisés par le serveur ; l'indicateur de drift à droite montre l'écart
   courant avec la salle.

Pour forcer le chemin de VLC : `VIBESYNC_VLC=/chemin/vers/VLC open …` ou
exporter la variable avant de lancer l'app depuis un terminal.

## 6. Organisation du paquet (depuis VS-031)

`Package.swift` vit **à la racine du dépôt** (contrainte SwiftPM : une cible ne
référence pas de sources hors racine du paquet). Toutes les commandes `swift`
partent donc de la racine :

```sh
swift test               # 34 tests, dont le rejeu des 13 vecteurs via l'API C
swift build -c release
./scripts/build-macos.sh # bundle .app signé ad hoc + version injectée
./scripts/test-core-macos.sh  # suite C portable (asan+ubsan) hors SwiftPM
```

Cibles : `VSCore` (couche C commune de `core/`, ADR-010 — `core/src` compilé
aussi par `ui/win32/build.bat`, `core/posix` réservé à macOS), `VibeSync`
(l'app, `ui/macos/Sources/VibeSync`) et `VibeSyncTests`. Le moteur Swift natif
et le moteur C sont tous deux rejoués contre `test/vectors/` tant que la
bascule de phase 3 (VS-032) n'est pas faite.

Le code a été compilé, testé et validé en réel sur Mac le 2026-08-06 (voir §7) :
une erreur de build signale une régression, pas un défaut d'écriture à
l'aveugle.

## 7. Test réel automatisé (`scripts/run-real-macos.sh`)

Équivalent macOS de `scripts/run-real-sandbox.ps1` : une séance réelle à **deux
clients et deux vrais VLC sur une seule machine**, avec verdict PASS/FAIL.

```sh
VIBESYNC_PASSWORD=… ./scripts/run-real-macos.sh [fichier-video] [url-serveur]
```

- **fichier-video** (facultatif) : sans lui, un WAV silencieux de 10 minutes est
  généré (perl, fourni par macOS — ni ffmpeg ni rien d'autre).
- **url-serveur** (facultatif) : défaut `ws://127.0.0.1:8080/ws`, et dans ce cas
  un serveur doit **déjà tourner** (`go run ./cmd/vibesync-server`). Avec une
  URL `wss://…`, le test vise le serveur déployé.
- `VIBESYNC_PASSWORD` : mot de passe du serveur. Jamais en argument — `argv` est
  lisible par tous les processus de la machine.
- `VIBESYNC_ROOM` : salle imposée (défaut `vibesync-test-$RANDOM`, pour ne pas
  tomber dans une vraie séance en cours).
- `VIBESYNC_KEEP=1` : conserver le dossier de travail (journaux + états).

Points de contrôle : (a) les deux clients connectés et qui se voient, (b) les
deux VLC attachés et le fichier déclaré, (c) `play` depuis le client 1 → la
position avance des deux côtés et tient 5 s, (d) `pause` depuis le client 2 →
les deux en pause, (e) `seek` depuis le client 1 → positions alignées, (f) écart
final entre les deux VLC < 0,5 s, (g) fermeture propre (close 1000, aucun VLC
orphelin). Code retour non nul dès qu'un point échoue.

### Comment le script pilote l'application

Le client macOS n'a pas de ligne de commande. Il lit donc, **et seulement si
`VIBESYNC_AUTO_URL` est présente**, un mode « pilote »
(`Sources/VibeSync/UI/AutoPilot.swift`) :

| Variable | Rôle |
| --- | --- |
| `VIBESYNC_AUTO_URL` | serveur — sa présence active le mode auto |
| `VIBESYNC_AUTO_NAME` / `_ROOM` / `_PASSWORD` | identité et salle |
| `VIBESYNC_AUTO_FILE` | média ouvert dans VLC au démarrage |
| `VIBESYNC_AUTO_STATUS` | fichier où l'app réécrit son état (une ligne JSON, chaque seconde) |
| `VIBESYNC_AUTO_CMDS` | fichier de commandes lu au fil de l'eau |
| `VIBESYNC_AUTO_SCENARIO` | étiquette libre, recopiée dans l'état |
| `VIBESYNC_SUITE` | suite `UserDefaults` alternative (isolation des instances) |

Commandes acceptées, une par ligne : `play`, `pause`, `seek <secondes>`,
`ready [0|1]`, `unready`, `chat <texte>`, `open <chemin>`, `quit`. Le script en
ajoute une puis attend d'avoir vérifié son point de contrôle : rien n'est minuté
en dur, un VLC lent ne fait pas échouer le test.

Sans ces variables, **aucun changement de comportement** : l'application
démarre normalement sur l'écran de connexion.

Deux pièges rencontrés, corrigés dans le script — les reproduire si l'on écrit
un autre harnais :

1. **Lancer l'app par `open -n -a … --env`**, pas en exécutant le binaire du
   bundle. Une app lancée directement depuis un shell hérite du contexte de ce
   shell ; sous un shell restreint (agent, CI, session sans Aqua) `URLSession`
   reste muette — la connexion ne part jamais et aucune erreur ne remonte.
   `open` ne rend pas le pid : c'est l'app qui l'écrit dans son état.
2. **`VIBESYNC_SUITE` est indispensable.** Un `HOME` distinct ne suffit pas :
   les préférences passent par `cfprefsd`, qui résout le dossier de
   l'utilisateur lui-même. Sans suite distincte, les deux instances présentent
   le **même jeton de session** au serveur (VS-028) et se chassent l'une
   l'autre.

### Ce qui reste à faire à la main

Le harnais ne couvre pas tout ; à vérifier à deux machines (le Mac + le PC) :
chat, coupure réseau (couper le Wi-Fi 15 s) et reprise automatique sans perdre
le pseudo, mot de passe mémorisé au trousseau, ouverture automatique du fichier
d'un participant (dossiers médias), bannière de mise à jour.
