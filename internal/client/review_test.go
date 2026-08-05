package client

import (
	"errors"
	"math"
	"sync"
	"testing"
	"time"

	"github.com/thibsix/vibesync/internal/protocol"
)

// Tests issus de la review croisée (sol) : règles gelées dans
// docs/protocol.md §Comportements client.

// sessionEnd simule la chute de la session serveur.
func (h *harness) sessionEnd() {
	h.t.Helper()
	h.e.mu.Lock()
	gen := h.e.connGen
	h.e.mu.Unlock()
	h.e.onSessionEnd(gen)
}

// attach branche la conn factice sans welcome (session ouverte, pas encore d'état).
func (h *harness) attach() {
	h.e.mu.Lock()
	h.e.conn = h.conn
	h.e.req = ConnectRequest{URL: "ws://test/ws", Name: "thib", Room: "soirée"}
	h.e.mu.Unlock()
}

func (h *harness) welcome(state protocol.RoomState) {
	h.t.Helper()
	h.server(protocol.TypeWelcome, protocol.Welcome{
		SelfID: "u1", Room: "soirée", State: state,
		Users: []protocol.User{{ID: "u1", Name: "thib"}},
	})
}

// --- Conditions de correction ---

func TestAucuneCorrectionAvantLePremierPong(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 3600)
	h.attach()
	h.welcome(h.playing(300))
	h.fake.SeekTo(0)
	h.fake.Play()
	h.ticks(6)

	if h.fake.Seeks() != 0 {
		t.Fatalf("%d seek(s) sans mesure d'offset d'horloge", h.fake.Seeks())
	}
	if got := h.fake.Rate(); math.Abs(got-1) > 1e-9 {
		t.Fatalf("rate = %v sans mesure d'offset", got)
	}
	// Le premier pong débloque les corrections.
	h.pong(0)
	h.ticks(3)
	if h.fake.Seeks() == 0 {
		t.Fatal("aucune correction après la première mesure d'offset")
	}
}

func TestCorrectionsSuspenduesPendantLaReconnexion(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 3600)
	h.connect(h.playing(300))
	h.fake.SeekTo(300)
	h.fake.Play()
	h.ticks(5)
	seeks := h.fake.Seeks()

	h.sessionEnd()
	snap := h.e.Snapshot()
	if snap.Phase != PhaseConnecting || !snap.Retrying {
		t.Fatalf("phase après coupure = %q retrying=%v", snap.Phase, snap.Retrying)
	}
	h.e.mu.Lock()
	stillHasState := h.e.haveState || h.e.haveOffset
	h.e.mu.Unlock()
	if stillHasState {
		t.Fatal("l'état de référence survit à la coupure")
	}

	// La salle continue d'avancer côté serveur : sans état valide, on ne touche
	// surtout pas à VLC.
	h.clock.Advance(30 * time.Second)
	h.ticks(15)
	if h.fake.Seeks() != seeks {
		t.Fatalf("correction appliquée avec un état périmé (%d → %d seeks)", seeks, h.fake.Seeks())
	}
	if got := h.fake.Rate(); math.Abs(got-1) > 1e-9 {
		t.Fatalf("rate modifié pendant la reconnexion: %v", got)
	}
	if d := h.e.Snapshot().DriftSec; d != 0 {
		t.Fatalf("drift affiché = %v alors qu'aucune référence n'est valide", d)
	}

	// Rejoin : le welcome refait la référence, la resync doit repartir.
	h.connect(h.playing(1800))
	h.ticks(4)
	if h.fake.Seeks() == seeks {
		t.Fatal("aucune resynchronisation après le welcome de reconnexion")
	}
	if got := h.fake.Position(); math.Abs(got-1800) > 2 {
		t.Fatalf("position après rejoin = %v, attendu ≈1800", got)
	}
}

func TestGenerationDeConnexionIgnoreLesBouclesObsoletes(t *testing.T) {
	h := newHarness(t)
	h.connect(h.playing(10))
	h.e.mu.Lock()
	stale := h.e.connGen
	h.e.mu.Unlock()

	h.e.Disconnect() // la génération change
	h.e.setPhase(stale, PhaseConnected)
	h.e.setError(stale, "erreur d'une vieille boucle")
	h.e.onSessionEnd(stale)

	snap := h.e.Snapshot()
	if snap.Phase != PhaseIdle {
		t.Fatalf("phase = %q, une boucle obsolète a modifié l'état", snap.Phase)
	}
	if snap.LastError != "" {
		t.Fatalf("lastError = %q, une boucle obsolète a modifié l'état", snap.LastError)
	}
}

// --- Hold post-action ---

func TestHoldLeveParLEcho(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.playing(100))
	h.fake.SeekTo(100)
	h.fake.Play()
	h.ticks(6)
	h.conn.take()

	h.fake.Pause() // action utilisateur → control + hold
	h.ticks(2)
	if got := controls(h.conn.take()); len(got) != 1 || got[0].Action != protocol.ActionPause {
		t.Fatalf("control attendu, obtenu %+v", got)
	}
	if h.fake.State() != "paused" {
		t.Fatalf("VLC relancé pendant le hold (état %q)", h.fake.State())
	}

	// Écho du serveur : setBy = notre id → le hold tombe et l'état s'applique.
	echo := h.paused(101)
	echo.SetBy = "u1"
	h.server(protocol.TypeRoomState, echo)
	h.e.mu.Lock()
	held := !h.e.userHoldUntil.IsZero()
	pending := h.e.pendingRS != nil
	h.e.mu.Unlock()
	if held || pending {
		t.Fatalf("hold non levé par l'écho (held=%v pending=%v)", held, pending)
	}
	if !h.e.Snapshot().Paused {
		t.Fatal("l'écho n'a pas été appliqué")
	}
}

func TestRoomStateDAutruiMemoriseePendantLeHold(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 3600)
	h.connect(h.playing(100))
	h.fake.SeekTo(100)
	h.fake.Play()
	h.ticks(6)
	h.conn.take()
	seeks := h.fake.Seeks()

	h.fake.Pause() // action utilisateur → hold de 2 s
	h.ticks(2)
	h.conn.take()

	// Un roomState d'autrui arrive pendant le hold : il précède forcément le
	// traitement de notre control côté serveur → mémorisé, pas appliqué.
	h.server(protocol.TypeRoomState, h.playing(2000))
	h.e.mu.Lock()
	pending := h.e.pendingRS != nil
	applied := h.e.roomState.PositionSec
	h.e.mu.Unlock()
	if !pending {
		t.Fatal("roomState d'autrui non mémorisé pendant le hold")
	}
	if applied == 2000 {
		t.Fatal("roomState d'autrui appliqué pendant le hold")
	}
	h.ticks(3)
	if h.fake.Seeks() != seeks {
		t.Fatal("correction appliquée pendant le hold")
	}

	// Expiration du hold sans écho : le dernier roomState mémorisé s'applique.
	h.clock.Advance(UserHold)
	h.ticks(3)
	h.e.mu.Lock()
	applied = h.e.roomState.PositionSec
	pending = h.e.pendingRS != nil
	h.e.mu.Unlock()
	if applied != 2000 || pending {
		t.Fatalf("roomState mémorisé non appliqué à l'expiration (pos=%v pending=%v)", applied, pending)
	}
	if h.fake.Seeks() == seeks {
		t.Fatal("aucune resynchronisation après l'expiration du hold")
	}
}

func TestDernierRoomStateGagnePendantLeHold(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 3600)
	h.connect(h.playing(100))
	h.fake.SeekTo(100)
	h.fake.Play()
	h.ticks(4)
	h.e.Pause() // action UI → hold

	h.server(protocol.TypeRoomState, h.playing(500))
	h.server(protocol.TypeRoomState, h.playing(900))
	h.clock.Advance(UserHold)
	h.ticks(2)
	h.e.mu.Lock()
	got := h.e.roomState.PositionSec
	h.e.mu.Unlock()
	if got != 900 {
		t.Fatalf("position de référence = %v, attendu le dernier roomState (900)", got)
	}
}

func TestAucuneCorrectionPendantLeHold(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 3600)
	h.connect(h.playing(100))
	h.fake.SeekTo(100)
	h.fake.Play()
	h.ticks(6)
	seeks := h.fake.Seeks()

	h.fake.SeekTo(900) // l'utilisateur saute : control seek + hold
	h.ticks(2)
	h.ticks(5) // 1 s de plus, toujours dans le hold
	if h.fake.Seeks() != seeks {
		t.Fatalf("le moteur a corrigé l'utilisateur pendant le hold (%d → %d)", seeks, h.fake.Seeks())
	}
	if got := h.fake.Position(); got < 900 {
		t.Fatalf("position ramenée en arrière pendant le hold: %v", got)
	}
	// Sans écho, les corrections reprennent à l'expiration.
	h.clock.Advance(UserHold)
	h.ticks(3)
	if h.fake.Seeks() == seeks {
		t.Fatal("les corrections n'ont pas repris après le hold")
	}
}

// --- Assainissement ---

func TestRoomStateInvalideIgnore(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.playing(100))
	h.e.mu.Lock()
	ref := h.e.roomState
	h.e.mu.Unlock()

	now := h.clock.Now().UnixMilli()
	invalides := []protocol.RoomState{
		{PositionSec: 500, Rate: 0, RefServerMs: now},      // rate hors [0,25 ; 4]
		{PositionSec: 500, Rate: 10, RefServerMs: now},     // rate trop grand
		{PositionSec: 500, Rate: 0.1, RefServerMs: now},    // rate trop petit
		{PositionSec: -5, Rate: 1, RefServerMs: now},       // position négative
		{PositionSec: 500, Rate: 1, RefServerMs: 0},        // référence temporelle absente
		{PositionSec: 500, Rate: math.NaN(), Paused: true}, // rate non fini
	}
	for i, rs := range invalides {
		if math.IsNaN(rs.Rate) {
			// NaN ne passe pas json.Marshal : on injecte le JSON brut.
			h.e.handleRaw([]byte(`{"type":"roomState","data":{"paused":true,"positionSec":500,"rate":null,"refServerMs":1}}`))
		} else {
			h.server(protocol.TypeRoomState, rs)
		}
		h.e.mu.Lock()
		got := h.e.roomState
		h.e.mu.Unlock()
		if got != ref {
			t.Fatalf("cas %d: roomState invalide %+v adopté (%+v)", i, rs, got)
		}
	}
	// Un nombre hors domaine flottant est rejeté au décodage, sans panique.
	h.e.handleRaw([]byte(`{"type":"roomState","data":{"positionSec":1e999,"rate":1,"refServerMs":1}}`))
	h.e.mu.Lock()
	got := h.e.roomState
	h.e.mu.Unlock()
	if got != ref {
		t.Fatalf("position non finie adoptée: %+v", got)
	}
	// Un rate valide non standard (0,5) reste accepté.
	ok := h.playing(200)
	ok.Rate = 0.5
	h.server(protocol.TypeRoomState, ok)
	h.e.mu.Lock()
	rate := h.e.roomState.Rate
	h.e.mu.Unlock()
	if rate != 0.5 {
		t.Fatalf("rate 0,5 rejeté à tort (rate = %v)", rate)
	}
}

func TestSeekUICadre(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.paused(0))
	h.ticks(2) // le moteur apprend la durée
	h.conn.take()

	h.e.Seek(99999)
	h.clock.Advance(UserHold)
	h.e.Seek(-40)
	h.clock.Advance(UserHold)
	h.e.Seek(math.NaN())
	h.e.Seek(math.Inf(1))

	got := controls(h.conn.take())
	if len(got) != 2 {
		t.Fatalf("%d controls, attendu 2 (les positions non finies sont ignorées): %+v", len(got), got)
	}
	if got[0].PositionSec != 1200 {
		t.Fatalf("seek au-delà de la durée = %v, attendu 1200", got[0].PositionSec)
	}
	if got[1].PositionSec != 0 {
		t.Fatalf("seek négatif = %v, attendu 0", got[1].PositionSec)
	}
}

// failingConn échoue à l'écriture pour simuler une connexion morte.
type failingConn struct {
	mu       sync.Mutex
	closed   bool
	writeErr error
}

func (c *failingConn) ReadMessage() ([]byte, error) { select {} }
func (c *failingConn) WriteMessage([]byte) error    { return c.writeErr }
func (c *failingConn) Close() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.closed = true
	return nil
}
func (c *failingConn) isClosed() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.closed
}

func TestErreurDEcritureFermeLaConnexion(t *testing.T) {
	h := newHarness(t)
	bad := &failingConn{writeErr: errors.New("tuyau cassé")}
	h.e.mu.Lock()
	h.e.conn = bad
	h.e.phase = PhaseConnected
	h.e.mu.Unlock()

	h.e.Chat("coucou")
	if !bad.isClosed() {
		t.Fatal("une erreur d'écriture doit fermer la connexion (→ reconnexion)")
	}
}

// --- Détection / seuils ---

func TestActionUtilisateurPendantLaGraceNestPasAbsorbee(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 3600)
	h.connect(h.playing(300))
	h.fake.SeekTo(100)
	h.fake.Play()
	h.ticks(2) // seek dur : arme la fenêtre de grâce
	h.conn.take()
	if h.fake.Seeks() == 0 {
		t.Fatal("le seek dur attendu n'a pas eu lieu")
	}

	h.fake.Pause() // l'utilisateur agit pendant la fenêtre de grâce
	h.ticks(2)     // encore dans la grâce : rien ne doit être décidé
	h.ticks(3)     // après la grâce : l'écart doit être vu

	got := controls(h.conn.take())
	if len(got) == 0 {
		t.Fatal("action utilisateur absorbée par la fenêtre de grâce")
	}
	if got[0].Action != protocol.ActionPause {
		t.Fatalf("action remontée = %q, attendu pause", got[0].Action)
	}
}

func TestSeuilSeekEnPause(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.paused(11.49))
	h.fake.SeekTo(10.51) // drift 0,98 s : mêmes secondes arrondies… mais corrigeable
	h.ticks(4)
	if h.fake.Seeks() == 0 {
		t.Fatalf("drift de 0,98 s non corrigé en pause (position %v)", h.fake.Position())
	}
	if got := math.Abs(h.fake.Position() - 11.49); got >= 0.98 {
		t.Fatalf("le seek n'a pas amélioré le drift (écart %v)", got)
	}

	// Sous le seuil : aucune correction (le seek entier n'apporterait rien).
	h2 := newHarness(t)
	h2.openFile("ep1.mkv", 1200)
	h2.connect(h2.paused(10.6))
	h2.fake.SeekTo(10.2) // drift 0,4 s
	h2.ticks(5)
	if h2.fake.Seeks() != 0 {
		t.Fatalf("%d seek(s) pour un drift de 0,4 s en pause", h2.fake.Seeks())
	}
}
