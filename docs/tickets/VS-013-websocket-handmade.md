---
id: VS-013
titre: WebSocket handmade en Go stdlib — retirer gorilla (0 dépendance)
statut: terminé
priorité: haute
dépend-de: [VS-003, VS-004]
créé: 2026-08-05
mis-à-jour: 2026-08-05
---

## Contexte

ADR-008 : zéro dépendance. `internal/ws` maison (RFC 6455 complet) puis migration
de tout le repo (serveur, client de référence, webui, harnais de test).

## Critères d'acceptation

- [x] `internal/ws` : Upgrade serveur + Dial client, framing complet, close handshake,
      ping/pong, read limit (défaut 1 Mio), validation UTF-8, erreurs typées
- [x] Serveur et client Go migrés, `go.mod` sans aucun require
- [x] Tests : handshake (vecteur RFC), framing hostile (MaxInt64, longueurs non
      minimales, contrôle fragmenté), interop réelle serveur↔dialer, benchmarks 0 alloc
- [x] Review sécurité codex sol high : 11 findings corrigés
- [x] `go vet` + `staticcheck` propres
- (l'interop avec un client WinHTTP sera validée par VS-014, qui l'implémente)

## Journal du ticket

- 2026-08-05 : créé (pivot handmade ADR-008).
- 2026-08-05 : package livré par agent Opus C, review sol high (1 bloquant : overflow
  int64 contournant la read limit), 11 fixes, puis migration complète du repo.
  API étendue de OnPing et AutoWriteTimeout pour préserver les comportements.
  Zéro require dans go.mod. Vérifié et intégré par l'orchestrateur.
