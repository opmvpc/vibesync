---
id: ADR-002
titre: Mono-repo Go, protocole WebSocket JSON, TLS délégué à Coolify/Traefik
statut: accepté
date: 2026-08-05
---

## Contexte

Cible de déploiement : Coolify (PaaS Docker) avec proxy Traefik. Syncplay utilise du
TCP brut port 8999 + TLS opportuniste, ce qui oblige à publier un port et gérer les
certificats soi-même. Le client doit être un binaire simple pour Windows et macOS arm64.

## Décision

- **Go** pour serveur et client, mono-repo `github.com/opmvpc/vibesync`, package
  partagé `internal/protocol` généré depuis la spec `docs/protocol.md`.
- **Transport WebSocket** (`/ws`), messages JSON `{type, data}`, versionnés.
- **TLS terminé par Traefik/Coolify** : le serveur écoute en HTTP simple dans le
  conteneur, les clients se connectent en `wss://` sur le domaine. Aucun port TCP brut
  à publier, aucun certificat à gérer dans l'app.
- Pure Go sans cgo → cross-compilation `windows/amd64` et `darwin/arm64` depuis
  n'importe quelle machine.

## Alternatives écartées

- TCP brut à la Syncplay : pas d'avantage, complexité TLS et exposition de port.
- TypeScript/Node : packaging desktop moins propre qu'un binaire statique Go.
- Rust : compétence/vélocité moindre des agents sur ce projet, bénéfice marginal.

## Conséquences

- Image Docker multi-stage → binaire statique sur `scratch`/`alpine` (~10-15 Mo).
- Un healthcheck HTTP `/healthz` gratuit pour Coolify.
- Websocket = passage propre à travers les proxys ; prévoir ping applicatif pour
  mesurer la latence (les pings WS ne suffisent pas pour l'offset d'horloge).
