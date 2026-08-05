# vibesync

Visionnage synchronisé de vidéos locales entre amis : chacun ouvre son fichier dans VLC,
un serveur central (Docker sur Coolify) synchronise play/pause/seek. Clone maison du
use case Syncplay — serveur ET client custom, tout en Go.

## Reprise de session

1. Lire `docs/STATUS.md` (état courant + prochaine action)
2. Tickets ouverts dans `docs/tickets/`
3. Journal du jour dans `docs/journal/`
4. Décisions structurantes dans `docs/decisions/`

## Architecture (résumé — détails dans les ADR)

- **Mono-repo Go**, module `github.com/thibsix/vibesync`, deux binaires :
  - `cmd/vibesync-server` — serveur de salles, WebSocket+JSON, état autoritatif
  - `cmd/vibesync` — client desktop (Windows + macOS arm64) : lance VLC avec son
    interface HTTP locale, moteur de sync, GUI = web UI locale embarquée (embed.FS)
- Protocole maison versionné : `docs/protocol.md` (source de vérité), types Go dans
  `internal/protocol/`
- Déploiement : image Docker multi-stage → Coolify, TLS/wss via Traefik

## Règles de travail

- **Délégation** : recherche → agents Sonnet ; codegen → agents Opus ; review croisée
  (codex terra ou juge Opus). Voir skill global `delegation`. Les sous-agents ne
  committent JAMAIS ; l'orchestrateur review, teste, committe.
- **QA non négociable** : tout code livré avec tests. `go test ./...`, `go vet ./...`
  et `staticcheck ./...` doivent passer avant tout commit. Tests d'intégration avec
  faux VLC (httptest) dans `internal/vlc/` et `test/`.
- Les sous-agents écrivent leurs rapports dans `docs/research/`, jamais dans
  tickets/STATUS.
- Commits : `feat:`, `fix:`, `test:`, `docs:`, `chore:`. Bureaucratie mise à jour à
  chaque étape franchie (STATUS + journal + tickets).

## Commandes usuelles

```bash
go build ./...                  # build complet
go test ./...                   # tests
go vet ./... && staticcheck ./...   # analyse statique
docker build -t vibesync-server .   # image serveur
```

VLC local (tests réels) : `C:\Program Files\VideoLAN\VLC\vlc.exe`
