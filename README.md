# vibesync

Visionnage synchronisé de vidéos locales entre amis, à la Syncplay : chacun garde
son fichier sur son disque, ouvre VLC, et un serveur central maintient tout le
monde calé sur la même image (play/pause/seek).

## Fonctionnement

```
  Ami A                  Ami B                  Ami C
[vibesync.exe]       [VibeSync.app]        [vibesync.exe]
  pilote VLC A          pilote VLC B          pilote VLC C
      |                      |                      |
      |        wss://ton-serveur/ws (JSON)          |
      +----------------------+----------------------+
                             |
                    [ serveur vibesync ]
                    Docker, auto-hébergé
                    (Coolify + Traefik/TLS)
                    état autoritatif de la salle
```

Chaque client pilote **son propre** VLC (interface HTTP locale, 127.0.0.1) et se
connecte au serveur en WebSocket. Le serveur ne voit jamais les fichiers vidéo :
il ne fait que synchroniser position, pause et vitesse entre les clients d'une
même salle.

## Points forts

- **0 dépendance tierce**, partout (ADR-008) : serveur Go stdlib pur (WebSocket
  maison, pas de gorilla), client Windows en C + Win32 pur, client macOS en
  Swift + AppKit/SwiftUI sans SPM.
- Clients natifs mono-exe : **< 500 Ko** sur Windows, démarrage quasi instantané,
  CPU idle proche de 0 %. Pas de webview, pas de runtime managé.
- Serveur conteneurisé minimal : image Docker finale **~18 Mo** (Alpine, non-root),
  se déploie sur Coolify en quelques clics.
- Moteur de synchronisation (offset d'horloge, micro-seek, hold post-action)
  spécifié une fois dans `docs/protocol.md` et **gelé par des vecteurs de test
  partagés** (`test/vectors/*.json`) rejoués par les trois implémentations
  (Go, C, Swift).

## Arborescence

```
cmd/
  vibesync-server/   point d'entrée du serveur (lit les VIBESYNC_*, lance internal/server)
  vibesync/          client Go — implémentation de référence, PAS un livrable
internal/
  protocol/          types du protocole client<->serveur (source: docs/protocol.md)
  server/            salles, hub, état autoritatif, anti-abus
  ws/                WebSocket maison (handshake + framing RFC 6455), stdlib pur
  client/            moteur de sync (offset horloge, drift, micro-seek, hold)
  vlc/               pilotage VLC via son interface HTTP locale
  webui/             UI web de debug du client Go (pas un livrable)
ui/
  win32/             client Windows livrable : C pur + Win32 (GDI, WinHTTP, Winsock)
    src/               sources C ; build.bat compile release/tests/asan
  macos/             client macOS livrable : Swift + AppKit/SwiftUI (SwiftPM, 0 dépendance)
    Sources/, Tests/
test/
  vectors/           scénarios JSON figeant le moteur de sync (partagés Go/C/Swift)
  e2e/               tests d'intégration serveur+client Go (simulés)
  real/              test réel avec deux vrais VLC (Windows Sandbox, VIBESYNC_REAL=1)
scripts/
  build-macos.sh     build + bundle .app sur macOS
  check-size.ps1     garde-fou budget de taille (ADR-007)
  run-real-sandbox.ps1  lance test/real dans une Windows Sandbox jetable
docs/
  protocol.md        spec du protocole (source de vérité)
  decisions/          ADR (choix d'architecture actés)
  tickets/, journal/, research/, STATUS.md   bureaucratie de suivi de projet
  deploy-coolify.md, guide-amis-windows.md, guide-amis-macos.md   doc utilisateur/déploiement
```

## Quickstart développeur

Prérequis : Go 1.26+. Pour les clients natifs, voir `docs/build-macos.md` (macOS)
et `ui/win32/build.bat` (Windows, nécessite llvm-mingw).

```bash
go test ./...                       # tests (serveur, protocole, moteur de sync)
go vet ./... && staticcheck ./...   # analyse statique — doit passer avant tout commit
go build ./...                      # build complet (serveur + client Go de réf.)

docker build -t vibesync-server .   # image serveur (~18 Mo)
```

Client Windows (C pur, nécessite llvm-mingw dans le PATH ou installé sous
`tools/llvm-mingw`) :

```bat
ui\win32\build.bat        rem build\vibesync.exe
ui\win32\build.bat test   rem compile + rejoue les vecteurs de conformité
```

Test réel avec deux instances VLC, en Windows Sandbox jetable (réseau isolé,
VLC monté en lecture seule depuis l'hôte) :

```powershell
.\scripts\run-real-sandbox.ps1
```

## Documentation

- [`docs/protocol.md`](docs/protocol.md) — protocole client↔serveur (source de vérité)
- [`docs/decisions/`](docs/decisions/) — ADR (choix d'architecture)
- [`docs/build-macos.md`](docs/build-macos.md) — compiler le client macOS
- [`docs/deploy-coolify.md`](docs/deploy-coolify.md) — déployer le serveur
- [`docs/guide-amis-windows.md`](docs/guide-amis-windows.md) — guide pour un ami sous Windows
- [`docs/guide-amis-macos.md`](docs/guide-amis-macos.md) — guide pour un ami sous macOS
- [`docs/STATUS.md`](docs/STATUS.md) — état courant du projet
