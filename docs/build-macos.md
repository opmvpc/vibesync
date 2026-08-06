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

## 6. Si erreurs de build

C'est l'hypothèse par défaut : le code n'a **jamais été compilé** (écrit sur
Windows, sans toolchain Swift). Les erreurs attendues sont mécaniques
(signature d'API, inférence de type), pas structurelles.

**Marche à suivre : me renvoyer la sortie brute de `swift build` ou
`swift test`, telle quelle, avec les numéros de ligne.** Ne pas corriger à
l'aveugle : chaque fichier a un rôle isolé, la correction est locale.

Points connus à surveiller, dans l'ordre de probabilité :

1. **`swift test` refuse de lier la cible exécutable.** SwiftPM sait tester une
   cible `executableTarget` depuis Swift 5.5, mais certaines toolchains
   coincent. Parade sans toucher au code :

   ```sh
   cd ui/macos
   mkdir -p Sources/VibeSyncCore
   git mv Sources/VibeSync/Engine Sources/VibeSync/Net Sources/VibeSync/VLC Sources/VibeSyncCore/
   ```

   puis dans `Package.swift`, remplacer les cibles par :

   ```swift
   .target(name: "VibeSyncCore", path: "Sources/VibeSyncCore"),
   .executableTarget(name: "VibeSync", dependencies: ["VibeSyncCore"], path: "Sources/VibeSync"),
   .testTarget(name: "VibeSyncTests", dependencies: ["VibeSyncCore"], path: "Tests/VibeSyncTests"),
   ```

   et ajouter `import VibeSyncCore` en tête des fichiers de `Sources/VibeSync/UI/`
   et de `main.swift` (les tests remplacent `@testable import VibeSync` par
   `@testable import VibeSyncCore`).

2. **Un vecteur diverge.** Les vecteurs `test/vectors/*.json` ont été
   régénérés le 06-08 pour intégrer deux règles neuves de
   `docs/protocol.md §Comportements client` — *Chargement de fichier* (pause
   forcée + position 0, `setFile` seulement une fois la pause observée) et
   *Départ et reprise de lecture* (seek de calage au-delà de 0,3 s avant de
   lancer la lecture). Le moteur Swift les implémente
   (`Engine.fileDeclared`, `Sync.startSeekSec`) mais n'a pas pu être confronté
   aux nouveaux fichiers. Une divergence ici est **attendue et facile** :
   le message d'échec donne le vecteur, l'instant et la valeur obtenue.

3. **Concurrence Swift 6.** Le manifeste est en `swift-tools-version:5.7`,
   donc en mode langage Swift 5 : les diagnostics `Sendable` ne devraient pas
   apparaître. S'ils apparaissent quand même, ce sont des avertissements, pas
   des erreurs.

4. **API SwiftUI.** Tout est volontairement macOS 11/12 (`onChange(of:perform:)`,
   `Color(nsColor:)`, `Label`, `overlay(alignment:)`). Des avertissements de
   dépréciation sur macOS 14+ sont normaux et sans effet.

## 7. Test réel à faire sur le Mac

- serveur local (`go run ./cmd/vibesync-server`) ou Coolify, deux clients
  (le Mac + le PC Windows), le même film ;
- vérifier : ready des deux côtés, démarrage synchrone, pause distante,
  seek distant, chat, coupure réseau (couper le Wi-Fi 15 s) et reprise
  automatique sans perdre le pseudo (jeton de session).
