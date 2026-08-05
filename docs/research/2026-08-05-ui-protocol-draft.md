# Brouillon : canal local UI ↔ cœur (`/ui`) — v1

Rédigé par l'agent client (VS-004/VS-005). **Brouillon**, à promouvoir en spec
officielle par l'orchestrateur. Ce canal est le contrat que les UIs natives
(SwiftUI, WinUI 3) réimplémenteront ; la web UI embarquée en est l'implémentation
de référence (mode debug).

## Transport

- Le client écoute sur `127.0.0.1:<port aléatoire>` (`--ui-addr` pour forcer).
- Au démarrage, le binaire écrit **une ligne JSON sur stdout** :
  `{"uiPort":54321,"uiToken":"…","uiURL":"http://127.0.0.1:54321/?token=…"}`.
- WebSocket sur `GET /ui?token=<uiToken>` — messages **texte JSON**.
  Token invalide → `401`. Origin non locale → upgrade refusé.
- `--headless` : n'ouvre pas le navigateur, sert quand même `/ui`.
- Enveloppe identique au protocole serveur : `{"type": "...", "data": {...}}`.
- Tout message inconnu est ignoré (forward-compat). Les commandes sont
  **idempotentes** (`setReady{ready}` et non un toggle, `seek` absolu).

## UI → cœur

| type | data | notes |
|---|---|---|
| `connect` | `{server, name, room, password}` | `server` accepte `hote`, `hote:port`, `ws(s)://…`, `http(s)://…` ; normalisé côté cœur en `wss://hote/ws` (`ws` si hôte local) |
| `disconnect` | `{}` | ferme la session serveur, laisse VLC ouvert |
| `openFile` | `{path}` | chemin absolu local ; lance VLC |
| `setReady` | `{ready}` | |
| `play` / `pause` | `{}` | action volontaire → `control` vers le serveur |
| `seek` | `{positionSec}` | idem, position absolue en secondes |
| `chat` | `{text}` | |
| `browse` | `{path}` | `""` = dossier personnel ; réponse `browse` |

```json
{"type":"connect","data":{"server":"vibesync.exemple.fr","name":"thib","room":"soirée","password":""}}
{"type":"seek","data":{"positionSec":1830.5}}
{"type":"browse","data":{"path":"D:\\Films"}}
```

## Cœur → UI

| type | data | notes |
|---|---|---|
| `hello` | `{uiVersion, protocolVersion, os}` | premier message |
| `state` | état complet (voir ci-dessous) | poussé à chaque changement (≈5/s pendant la lecture) |
| `toast` | `{level, text}` | relais des toasts serveur |
| `chat` | `{from, text, serverMs}` | relais du chat serveur |
| `browse` | `{path, parent, roots[], entries[]}` | réponse à `browse` |
| `error` | `{code, text}` | erreur **locale** : `badServer`, `badRequest`, `vlc`, `fs`, `protocol`, `unknown` |

L'état est toujours envoyé **complet** (pas de diff) : une UI native peut se
contenter de remplacer son modèle à chaque `state`.

```json
{"type":"state","data":{
  "phase":"connected","serverUrl":"wss://exemple.fr/ws","room":"soirée","name":"thib","selfId":"u1",
  "users":[{"id":"u1","name":"thib","ready":true,"file":{"name":"ep1.mkv","durationSec":1420,"sizeBytes":734003200},"positionSec":312.4,"latencyMs":28}],
  "ready":true,"paused":false,"roomPositionSec":312.6,"roomRate":1,
  "driftSec":-0.04,"correcting":"","latencyMs":28,"clockOffsetMs":-134,
  "retrying":false,"lastError":"",
  "vlc":{"running":true,"available":true,"binaryPath":"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe",
         "state":"playing","positionSec":312.56,"durationSec":1420,"rate":1,
         "filePath":"D:\\Films\\ep1.mkv","fileName":"ep1.mkv","buffering":false,"error":""}}}
```

- `phase` : `idle` | `connecting` | `connected`.
- `correcting` : `""` | `nudge` | `seek` (correction en cours, pour l'indicateur de sync).
- `vlc.available=false` + `vlc.error` : VLC introuvable → l'UI affiche l'aide
  (`VIBESYNC_VLC`).

```json
{"type":"browse","data":{
  "path":"D:\\Films","parent":"D:\\",
  "roots":[{"name":"Dossier personnel","path":"C:\\Users\\thib","isDir":true},{"name":"D:","path":"D:\\","isDir":true}],
  "entries":[{"name":"Saison 1","path":"D:\\Films\\Saison 1","isDir":true,"sizeBytes":0},
             {"name":"ep1.mkv","path":"D:\\Films\\ep1.mkv","isDir":false,"sizeBytes":734003200}]}}
```

Tri : dossiers d'abord, puis fichiers vidéo (extensions courantes), casse ignorée ;
fichiers cachés exclus. L'explorateur est aussi joignable en HTTP :
`GET /api/fs?token=…&path=…` (même charge utile).
