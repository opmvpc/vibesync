---
id: VS-007
titre: Docker + Coolify + CI (lint, tests, client Windows, budget taille)
statut: terminé
priorité: normale
dépend-de: [VS-006]
créé: 2026-08-05
mis-à-jour: 2026-08-06
---

## Contexte

Déploiement serveur sur Coolify (ADR-002) et CI. Périmètre ajusté par ADR-008 :
les livrables client sont le C Windows (buildé en CI) et le Swift macOS (buildé
sur le Mac, hors CI pour l'instant).

## Critères d'acceptation

- [x] Dockerfile multi-stage (Alpine non-root, healthcheck) — build local OK,
      conteneur smoke-testé (healthz 200), image ~18 Mo
- [x] docker-compose.yml + `docs/deploy-coolify.md` (domaine, wss, env, redeploy)
- [x] Workflow GitHub Actions : QA Go (vet+staticcheck+tests) + build/test client C
      Windows (llvm-mingw, 973 vérifications, budget 500 Ko) + artifact — **runs verts**
- [x] Budget taille vérifié en CI (échec si vibesync.exe > 500 Ko)

## Journal du ticket

- 2026-08-05 : créé ; Dockerfile/compose/CI draft posés en attente des agents.
- 2026-08-06 : CI réécrite pour ADR-008, runs verts sur GitHub, doc Coolify rédigée
  (agent Sonnet VS-008). Reste hors ticket : job macOS quand le build sera
  reproductible sur le Mac. Terminé.
