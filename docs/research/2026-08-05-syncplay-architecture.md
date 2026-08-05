# Rapport de recherche : architecture Syncplay (agent Sonnet, 2026-08-05)

## 1. Architecture générale

Syncplay repose sur un modèle **client léger + serveur central**, entièrement écrit en
**Python** (réseau **Twisted**, GUI PySide2/Qt). Points d'entrée : `syncplayClient.py`
et `syncplayServer.py`. Le serveur ne fait que relayer des événements (pas de flux
vidéo — chacun lit son fichier local). Licence Apache 2.0.

## 2. Protocole client↔serveur

- TCP, protocole texte ligne par ligne (`LineReceiver`) — un objet **JSON** par ligne (`\r\n`).
- Port par défaut : **8999**.
- TLS « opportuniste » depuis la 1.6.3 (démarre en clair, upgrade si possible) ;
  nécessite des certs signés CA (Let's Encrypt), fichiers `cert.pem`/`privkey.pem`/`chain.pem`.
- Pas de doc de protocole exhaustive ; la doc côté lecteur est en tête de `syncplay.lua`.

## 3. Intégration VLC

VLC est contrôlé via un **script d'interface Lua** (`syncplay.lua`) installé dans le
dossier `lua/intf/` de VLC. Le script interroge VLC (fichier, position, pause) et
applique les commandes. Couplage fragile : script obsolète après mise à jour VLC
(issues #193, #301), installation manuelle. mpv (socket JSON IPC natif) est
l'intégration recommandée par le projet.

## 4. Serveur officiel

Très léger (Python pur, pas de BDD obligatoire). Options : `--port`, `--password`
(+ env `SYNCPLAY_PASSWORD`), `--salt`, `--isolate-room`, `--disable-chat`,
`--disable-ready`, `--motd-file`, `--stats-db-file`, IPv6. Salles dynamiques, non
persistées. Ready state par utilisateur. Gestion des désync/seeks native au protocole.

## 5. Docker

**Pas d'image officielle.** Images communautaires :
- `dnomd343/syncplay` (aussi `ghcr.io/dnomd343/syncplay`) — multi-arch, MIT, env
  `PORT`, `MOTD`, `DISABLE_CHAT`, `CERT_DIR`…
- `ninetaillabs/syncplay-server` — 100K+ pulls mais vieille d'un an+, non-root, TLS.
- Divers forks quasi identiques encapsulant `syncplayServer.py`.

## 6. État du projet

Actif : **1.7.6 (4 août 2026)**, 1.7.5 (fév. 2026), 1.7.4 (mars 2025). Écosystème
vivant (client mobile Kotlin `yuroyami/syncplay-mobile`).

## Sources

- https://github.com/Syncplay/syncplay (+ `syncplay/protocols.py`)
- https://github.com/Syncplay/syncplay/wiki/TLS-support
- https://syncplay.pl/guide/server/ · https://syncplay.pl/changelog/
- https://github.com/Syncplay/syncplay/issues/301 · /issues/193
- https://github.com/dnomd343/syncplay-docker
- https://hub.docker.com/r/ninetaillabs/syncplay-server
