# Rapport de recherche : contrôle VLC, algo de sync, alternatives (agent Sonnet, 2026-08-05)

## 1. Réimplémentations du protocole Syncplay

Aucun serveur alternatif mûr. Seule trouvaille serveur : RXJpaw/SyncPlay-Server
(TypeScript, URL en 404 au moment du test). Côté client : syncplay-web/syncplay-java,
syncweb-js, SyncPlay.NET, Synkplay mobile. Un serveur Go/Rust serait un chantier
quasi vierge.

## 2. Contrôle de VLC : Lua vs RC/telnet vs HTTP

Syncplay a choisi l'interface **Lua** native (`syncplay.lua` dans `lua/intf/`) :
accès in-process aux objets VLC (`vlc.var.get(input,"time")`…), latence minimale,
précision microseconde — mais fragile aux versions de VLC et installation manuelle.

- **RC/telnet** : socket texte, commandes limitées, pas fait pour du polling haute
  fréquence, sécurisation pénible.
- **HTTP** (`/requests/status.json`) : JSON, interrogeable depuis n'importe quel
  langage, pas de socket long-lived. **Le compromis le plus réaliste pour un client
  maison** hors Lua ; latence d'un aller-retour HTTP local, largement suffisante pour
  play/pause/seek. Attention : `time` à la seconde près → utiliser `position × length`.

## 3. Alternatives fichiers locaux + VLC

- **Jellyfin SyncPlay** : exige une bibliothèque Jellyfin centrale (le serveur diffuse
  le contenu) — ne couvre pas « chacun son fichier local dans VLC ».
- **OpenTogetherTube** : vidéos servies en HTTP/YouTube — hors sujet.
- Syncplay reste la seule solution mature du use case exact.

## 4. Points durs d'une sync de lecture (et gestion Syncplay)

- **Drift progressif** : correction douce par ralentissement du playback rate en cas
  de désync mineure, seek brutal en cas de désync majeure (seuils configurables).
- **Latence réseau** : pings réguliers client↔serveur pour estimer le RTT et ajuster
  la position cible.
- **Buffering d'un participant** : traité comme désync — ralentir les autres ou jump.
- **Ready state** : la lecture globale ne (re)démarre que quand tout le monde est prêt.
- **Rejoin** : le serveur garde la position de référence de la salle et resynchronise
  l'arrivant ; « pause on disconnect » optionnelle.

→ Cœur algorithmique à reproduire : ready-state explicite, double stratégie
nudge/seek selon l'ampleur du drift, pause-on-disconnect, resync au join.

## Sources

- https://raw.githubusercontent.com/Syncplay/syncplay/master/syncplay/resources/lua/intf/syncplay.lua
- https://wiki.videolan.org/VLC_HTTP_requests/ · https://wiki.videolan.org/documentation:modules/rc/
- https://syncplay.pl/guide/client/ · https://syncplay.pl/about/syncplay/ · https://syncplay.pl/guide/trouble/
- https://github.com/topics/syncplay · https://github.com/dyc3/opentogethertube
