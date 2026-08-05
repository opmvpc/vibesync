# Rapport — `internal/ws` : WebSocket handmade (VS-013)

Package Go stdlib pur (0 import tiers), 1103 lignes de code + 1163 de tests.
Migration serveur/client **non faite** (hors périmètre).

## API finale

```go
func Upgrade(w http.ResponseWriter, r *http.Request) (*Conn, error)
func Dial(ctx context.Context, rawURL string) (*Conn, error)
func DialWithConfig(ctx, rawURL string, cfg *DialConfig) (*Conn, error)
type DialConfig struct { TLSConfig *tls.Config; Header http.Header; HandshakeTimeout time.Duration }

func (c *Conn) ReadMessage() (msgType int, data []byte, err error)
func (c *Conn) WriteMessage(msgType int, data []byte) error
func (c *Conn) WriteFragments(msgType int, frags ...[]byte) error
func (c *Conn) WritePing/WritePong(payload []byte) error
func (c *Conn) WriteClose(code uint16, reason string) error   // non bloquant
func (c *Conn) Close(code uint16, reason string) error        // handshake complet
func (c *Conn) CloseNow() error
func (c *Conn) SetReadDeadline/SetWriteDeadline(t time.Time) error
func (c *Conn) SetReadLimit(n int64)
c.OnPong func([]byte)   // + NetConn/LocalAddr/RemoteAddr
```

Erreurs typées : `CloseError` (fermeture du pair), `ProtocolError` (code de close
déjà envoyé au pair), `HandshakeError`, `ErrClosed`, helper `IsCloseCode`.

## Choix

- **Aucune goroutine ni channel interne.** Écritures sérialisées par mutex (sûres
  en concurrence) ; une seule goroutine lectrice ; `Close` côté lecteur,
  `CloseNow` pour débloquer depuis ailleurs. Un unique watchdog éphémère dans
  `Dial` pour l'annulation de contexte.
- **Buffers réutilisés** : en-tête, clé et tampon de masquage (client seulement)
  dans la `Conn`. Écriture = **0 alloc/msg** (18–95 ns) ; lecture = **1 alloc**
  (le payload rendu à l'appelant), masquage 8 octets à la fois (15 Go/s).
- Lecture de payload **incrémentale** : une longueur annoncée mensongère
  n'alloue que ce qui arrive réellement. `SetReadLimit` sur le message réassemblé.
- Pings reçus → pong automatique pendant `ReadMessage` ; close reçue → écho
  automatique puis `CloseError`.

## Limites assumées

Pas de permessage-deflate, ni sous-protocole, ni proxy, ni redirection.
UTF-8 texte validé après réassemblage des fragments (pas de rejet au premier
octet fautif). Contrôle d'`Origin` laissé à l'appelant. `-race` non exécutable
(pas de gcc sur la machine) — validé par `-count=5 -shuffle=on`.

## Post-review sol (11 points, tous corrigés)

- **Bloquant** — dépassement de `int64(len(data))+h.length` : une continuation
  annonçant MaxInt64 rendait la somme négative et contournait la limite. La
  comparaison se fait désormais par soustraction (`h.length > limit-got`), avec
  test dédié.
- **Majeurs** — (2) longueurs étendues non minimales rejetées (close 1002) ;
  (3) `SetReadLimit` par défaut à **1 Mio** au lieu d'illimité ; (4) `Upgrade`
  exige **une seule** occurrence de `Sec-WebSocket-Key` / `-Version` ;
  (5) le transport TCP est fermé dès le close handshake complet (`closeOnce`,
  pas de « use of closed network connection » remonté).
- **Mineurs** — (6) `readPayload` n'agrandit le tampon qu'à hauteur des octets
  **déjà arrivés** (`bufio.Peek`/`Buffered`) : allocation bornée à ~2× le reçu,
  jamais à l'annonce ; testé par mesure d'allocations (1 Tio annoncé, 3 octets
  reçus → < 32 Kio). (7) close code 1010 refusé venant d'un serveur ;
  (8) en-têtes réservées filtrées après canonicalisation ; (9) réponse d'upgrade :
  HTTP/1.1 et `Sec-WebSocket-Accept` unique exigés ; (10) course du chien de
  garde de `Dial` réglée par CAS atomique (transport soit fermé, soit remis,
  jamais les deux) ; (11) test placebo remplacé par de vraies assertions.

Coût : lecture de messages > taille du tampon bufio = 2 allocations au lieu
d'une (compromis assumé contre l'amplification mémoire). Écriture toujours
0 alloc. Couverture 87,0 %.

## Migration (VS-013 partie 2) — gorilla retiré du repo

`go.mod` ne contient plus aucun `require`. `go.sum` est vide mais conservé : le
Dockerfile fait `COPY go.mod go.sum ./` (hors périmètre).

Correspondances : `Upgrader.Upgrade`→`ws.Upgrade`, `Dialer.DialContext`→
`ws.Dial`/`DialWithConfig`, `WriteMessage(PingMessage)`→`WritePing`,
`WriteMessage(CloseMessage, FormatCloseMessage(...))`→`WriteClose`,
`SetPongHandler`→`OnPong`, `Conn.Close()`→`CloseNow()` (fermeture brutale ;
`ws.Conn.Close` fait, lui, le close handshake complet). Deadlines, `SetReadLimit`
(64 Kio serveur), périodes de ping (30 s) et fenêtre de silence (60 s serveur,
70 s client) inchangées ; moteur de reconnexion non touché.

**API `ws` étendue (2 ajouts, testés)** :
- `Conn.OnPing` — le client se sert du ping serveur pour repousser son échéance
  de lecture ; sans hook, une salle silencieuse aurait coupé au bout de 70 s.
- `Conn.AutoWriteTimeout` — les trames émises *depuis la boucle de lecture*
  (pong automatique, écho de close) héritaient de la dernière échéance posée
  par la pompe d'écriture, expirée entre deux pings : un simple ping du pair
  aurait tué la connexion. gorilla posait une échéance propre, c'est restauré.

**Écarts de comportement assumés** (aucune assertion de test modifiée) :
- `ws.Dial` ne rend pas la `*http.Response` du refus ; le harnais serveur la
  reconstruit depuis `HandshakeError.Status` (le test 503 est intact).
- `internal/webui` : le contrôle d'origine était porté par `Upgrader.CheckOrigin` ;
  `ws` le laisse à l'appelant, `handleUI` appelle donc `checkLocalOrigin` et
  répond 403 avant l'upgrade — même effet.
- Limite de lecture par défaut de 1 Mio là où gorilla était illimité (webui et
  client) ; le serveur garde ses 64 Kio explicites.
- À réception d'un close du pair, `ws` ferme le transport après l'écho : les
  messages encore en file d'écriture ne partent plus (gorilla les écrivait
  dans le vide, le pair étant déjà en fermeture).

Mentions restantes de « gorilla » : uniquement dans `docs/` (ADR-005, ADR-008,
ticket VS-013, STATUS, rapports) — traces historiques volontairement
conservées, et hors périmètre d'un sous-agent.
