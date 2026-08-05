---
id: VS-007
titre: Docker + Coolify + CI (lint, tests, releases Windows/macOS)
statut: ouvert
priorité: normale
dépend-de: [VS-006]
créé: 2026-08-05
mis-à-jour: 2026-08-05
---

## Contexte

Déploiement serveur sur Coolify (ADR-002) et distribution du client aux amis
(windows/amd64, darwin/arm64).

## Critères d'acceptation

- [ ] Dockerfile multi-stage (build Go → image finale minimale non-root), build local OK
- [ ] docker-compose.yml + doc Coolify (domaine, wss, healthcheck `/healthz`, env)
- [ ] Cross-compilation locale des 2 binaires client vérifiée (`dist/`)
- [ ] Workflow GitHub Actions : vet + staticcheck + tests + build 3 cibles sur tag

## Journal du ticket

- 2026-08-05 : créé.
