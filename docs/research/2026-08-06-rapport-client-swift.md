# Client macOS Swift — rapport de rédaction (VS-015)

Code écrit sur le PC, **jamais compilé** (aucune toolchain Swift ici). Relu deux
fois fichier par fichier ; c'est ma seule « compilation ».

## Structure

`ui/macos/` — paquet SwiftPM, tools-version 5.7, macOS 13, zéro dépendance.
Deux cibles comme demandé : `VibeSync` (exécutable) et `VibeSyncTests`.

- `Engine/` — `Time.swift` (nanosecondes Int64 + constantes de sync),
  `Types.swift` (VLCStatus, RoomState, VLCCommand, ClientMessage, Decision),
  `Engine.swift` : portage fidèle de `engine.c`, struct pur, événements →
  `[Decision]`. Aucun import réseau/UI.
- `Net/` — `JSON.swift` (écrivain maison + accesseurs sur JSONSerialization),
  `Protocol.swift` (hello/messages, décodage tolérant, jeton `SecRandomCopyBytes`),
  `WebSocketClient.swift` (URLSessionWebSocketTask, événements sur la file
  principale).
- `VLC/` — parseur de `status.json` (position fine, assainissement identique à
  Go), client HTTP Basic auth, lanceur `Process` + port libre POSIX.
- `UI/` — AppKit explicite (pas de `@main`), fenêtre unique hébergeant SwiftUI :
  écran connexion (mémorisé dans UserDefaults) et écran salle (participants,
  Prêt, barre cliquable, chat, toasts, drift, latence).

## Choix

- **JSON maison en sortie** plutôt que Codable : enveloppes `{type, data}`
  hétérogènes et surtout contrôle exact du format des flottants
  (`positionSec` doit relire la même valeur qu'en Go).
- **Optionals plutôt que sentinelle `INT64_MIN`** pour les fenêtres temporelles :
  en Swift la soustraction sur la sentinelle piégerait à l'exécution.
- **Tout sur la file principale**, moteur en struct : aucun verrou, aucune
  course, et `swift test` reste déterministe.
- Nouvelles règles de spec intégrées : `Engine.fileDeclared` (setFile seulement
  après la pause de chargement observée, pause+seek 0 forcés par le driver) et
  `Sync.startSeekSec = 0,3` (calage avant lancement de la lecture).

## Risques de compilation identifiés

1. `swift test` sur une cible exécutable : supporté depuis Swift 5.5, mais si la
   toolchain coince, la parade (extraction d'une cible `VibeSyncCore`) est
   décrite dans `docs/build-macos.md §6`, sans toucher au code.
2. `freePort()` (socket/bind/getsockname en Darwin brut) — le seul endroit avec
   des `withMemoryRebound` ; repli automatique sur un port tiré au hasard.
3. API SwiftUI : tout est macOS 11/12 volontairement ; attendre des
   avertissements de dépréciation, pas des erreurs.

## À vérifier sur le Mac

- `swift test` : les 12 vecteurs (**régénérés entre-temps** — une divergence sur
  `setFile`/seek de départ est attendue et localisée, le message d'échec donne
  vecteur + instant + champ) ;
- lancement réel de VLC (chemin, drapeaux, pause forcée à l'ouverture) ;
- taille du bundle et Gatekeeper (clic droit → Ouvrir).
