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

- **Philosophie handmade (ADR-008) : 0 dépendance tierce, partout.**
  - `cmd/vibesync-server` — serveur de salles Go **stdlib pur** (WebSocket maison
    `internal/ws`), WebSocket+JSON, état autoritatif. Docker → Coolify.
  - `ui/win32/` — client Windows : **un exe C pur + Win32** (GDI immediate-mode,
    WinHTTP pour wss, Winsock pour VLC, JSON maison, arènes). < 500 Ko.
  - `ui/macos/` — client macOS : **un binaire Swift** (AppKit/SwiftUI,
    URLSessionWebSocketTask), aucune dépendance SPM. Build sur le Mac de Thibault.
  - `cmd/vibesync` + `internal/client|vlc|webui` — client Go = **implémentation de
    référence et harnais de test** (pas un livrable) ; génère les vecteurs
    `test/vectors/*.json` que les moteurs C et Swift doivent rejouer.
- Le moteur de sync (drift, offset horloge, ready) est spécifié dans
  `docs/protocol.md` §Comportements client et gelé par les vecteurs de test.
- Budget : client < 10 Mo (`scripts/check-size.ps1`) — trivial depuis ADR-008.
- Protocole client↔serveur versionné : `docs/protocol.md`, types Go `internal/protocol/`
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
