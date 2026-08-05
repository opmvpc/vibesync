---
id: VS-013
titre: WebSocket handmade en Go stdlib — retirer gorilla (0 dépendance)
statut: ouvert
priorité: haute
dépend-de: [VS-003, VS-004]
créé: 2026-08-05
mis-à-jour: 2026-08-05
---

## Contexte

ADR-008 : zéro dépendance. Le serveur (et le client Go de référence) utilisent
gorilla/websocket ; on le remplace par `internal/ws` maison : handshake RFC 6455
(Sec-WebSocket-Accept = SHA1+base64), framing (masquage client, fragmentation,
ping/pong/close, limites de taille), côté serveur ET côté dialer (pour le client Go
de référence et les tests). Pas de permessage-deflate (inutile ici).

## Critères d'acceptation

- [ ] `internal/ws` : Upgrade serveur + Dial client, API minimale (ReadMessage/
      WriteMessage/Close/SetDeadline), texte + control frames
- [ ] Serveur et client Go migrés, `gorilla/websocket` absent de go.mod (0 require)
- [ ] Tests : handshake (clé/accept, mauvaises requêtes), framing (fragmenté, masqué,
      payloads 125/126/64K+, close codes), interop réelle serveur↔dialer
- [ ] Interop vérifiée avec un client WinHTTP (sera re-testée en VS-014)
- [ ] `go vet` + `staticcheck` propres

## Journal du ticket

- 2026-08-05 : créé (pivot handmade ADR-008).
