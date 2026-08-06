package server

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/thibsix/vibesync/internal/protocol"
	"github.com/thibsix/vibesync/internal/ws"
)

// --- Harnais d'intégration ---

type testRig struct {
	http  *httptest.Server
	srv   *Server
	clock *fakeClock
	// destroyed reçoit le nom de chaque salle détruite : synchronisation
	// déterministe, sans attente active.
	destroyed chan string
}

func newRig(t *testing.T, cfg Config) *testRig {
	t.Helper()
	if cfg.RoomLinger == 0 {
		// Par défaut le harnais garde l'ancien comportement — salle détruite dès
		// qu'elle se vide — pour que `waitRoomDestroyed` reste une barrière de
		// synchronisation fiable. Les tests de reprise de séance demandent
		// explicitement leur fenêtre.
		cfg.RoomLinger = RoomLingerDisabled
	}
	clk := newFakeClock()
	srv := New(cfg, WithClock(clk), WithLogger(testLogger()))
	rig := &testRig{srv: srv, clock: clk, destroyed: make(chan string, 16)}
	// Branché avant tout démarrage de serveur : aucune goroutine ne peut encore
	// lire le champ.
	srv.hub.onRoomDestroyed = func(name string) {
		select {
		case rig.destroyed <- name:
		default:
		}
	}
	rig.http = httptest.NewServer(srv.Handler())
	t.Cleanup(rig.http.Close)
	return rig
}

// waitRoomDestroyed attend la destruction de la salle nommée.
func (r *testRig) waitRoomDestroyed(t *testing.T, name string) {
	t.Helper()
	deadline := time.After(5 * time.Second)
	for {
		select {
		case got := <-r.destroyed:
			if got == name {
				return
			}
		case <-deadline:
			t.Fatalf("salle %q non détruite à temps", name)
		}
	}
}

// waitRoomEmpty attend que la salle n'ait plus aucun membre (elle peut alors
// survivre en attente de reprise : `waitRoomDestroyed` ne conviendrait pas).
func (r *testRig) waitRoomEmpty(t *testing.T, name string) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		room := r.srv.hub.room(name)
		if room == nil || room.size() == 0 {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatalf("salle %q toujours occupée", name)
}

func (r *testRig) wsURL() string {
	return "ws" + strings.TrimPrefix(r.http.URL, "http") + "/ws"
}

// testConn adapte ws.Conn au harnais : Close() sans argument coupe la socket
// sans close handshake, comme le faisait le dialer historique.
type testConn struct{ *ws.Conn }

func (c *testConn) Close() error { return c.CloseNow() }

type wsTestClient struct {
	t    *testing.T
	conn *testConn
}

var pingToken atomic.Int64

func (r *testRig) dial(t *testing.T) *wsTestClient {
	t.Helper()
	c, _, err := r.tryDial(t)
	if err != nil {
		t.Fatalf("dial %s: %v", r.wsURL(), err)
	}
	return c
}

// tryDial ne fait pas échouer le test : sert à observer un refus HTTP. La
// réponse est reconstruite depuis *ws.HandshakeError, qui porte le statut
// renvoyé par le serveur (le dialer ne rend pas la réponse HTTP elle-même).
func (r *testRig) tryDial(t *testing.T) (*wsTestClient, *http.Response, error) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	conn, err := ws.Dial(ctx, r.wsURL())
	if err != nil {
		var he *ws.HandshakeError
		if errors.As(err, &he) {
			return nil, &http.Response{StatusCode: he.Status, Status: http.StatusText(he.Status)}, err
		}
		return nil, nil, err
	}
	t.Cleanup(func() { _ = conn.CloseNow() })
	return &wsTestClient{t: t, conn: &testConn{conn}}, nil, nil
}

func (c *wsTestClient) send(msgType string, data any) {
	c.t.Helper()
	raw, err := protocol.Encode(msgType, data)
	if err != nil {
		c.t.Fatalf("encode %s: %v", msgType, err)
	}
	_ = c.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	if err := c.conn.WriteMessage(ws.TextMessage, raw); err != nil {
		c.t.Fatalf("write %s: %v", msgType, err)
	}
}

// trySend n'échoue pas si la connexion a déjà été fermée par le serveur.
func (c *wsTestClient) trySend(msgType string, data any) error {
	raw, err := protocol.Encode(msgType, data)
	if err != nil {
		c.t.Fatalf("encode %s: %v", msgType, err)
	}
	_ = c.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	return c.conn.WriteMessage(ws.TextMessage, raw)
}

func (c *wsTestClient) sendRaw(raw string) {
	c.t.Helper()
	_ = c.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	if err := c.conn.WriteMessage(ws.TextMessage, []byte(raw)); err != nil {
		c.t.Fatalf("write brut: %v", err)
	}
}

func (c *wsTestClient) next() protocol.Envelope {
	c.t.Helper()
	_ = c.conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	_, raw, err := c.conn.ReadMessage()
	if err != nil {
		c.t.Fatalf("lecture: %v", err)
	}
	env, err := protocol.Decode(raw)
	if err != nil {
		c.t.Fatalf("décodage: %v (%s)", err, raw)
	}
	return env
}

// waitFor consomme les messages jusqu'au premier du type demandé.
func (c *wsTestClient) waitFor(msgType string) protocol.Envelope {
	c.t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		env := c.next()
		if env.Type == msgType {
			return env
		}
	}
	c.t.Fatalf("aucun message de type %q reçu à temps", msgType)
	return protocol.Envelope{}
}

// sync sert de barrière : après retour, tout ce qui avait été mis en file pour
// ce client a été consommé (le pong est renvoyé après traitement séquentiel).
func (c *wsTestClient) sync() {
	c.t.Helper()
	token := pingToken.Add(1)
	c.send(protocol.TypePing, protocol.Ping{T: token})
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		env := c.next()
		if env.Type != protocol.TypePong {
			continue
		}
		if mustData[protocol.Pong](c.t, env).T == token {
			return
		}
	}
	c.t.Fatal("pong de synchronisation non reçu")
}

func (c *wsTestClient) hello(name, room string) protocol.Welcome {
	c.t.Helper()
	return c.helloSession(name, room, "")
}

// helloSession envoie un hello portant un jeton de reprise de session.
func (c *wsTestClient) helloSession(name, room, session string) protocol.Welcome {
	c.t.Helper()
	c.send(protocol.TypeHello, protocol.Hello{
		Version: protocol.Version, Name: name, Room: room, Session: session,
	})
	return mustData[protocol.Welcome](c.t, c.waitFor(protocol.TypeWelcome))
}

func (c *wsTestClient) expectClosed() {
	c.t.Helper()
	_ = c.conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	for {
		if _, _, err := c.conn.ReadMessage(); err != nil {
			return
		}
	}
}

func mustData[T any](t *testing.T, env protocol.Envelope) T {
	t.Helper()
	v, err := protocol.DecodeData[T](env)
	if err != nil {
		t.Fatalf("data invalide pour %q: %v", env.Type, err)
	}
	return v
}

// --- HTTP ---

func TestHealthz(t *testing.T) {
	rig := newRig(t, Config{})
	resp, err := http.Get(rig.http.URL + "/healthz")
	if err != nil {
		t.Fatalf("GET /healthz: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("statut attendu 200, obtenu %d", resp.StatusCode)
	}
	buf := make([]byte, 8)
	n, _ := resp.Body.Read(buf)
	if got := string(buf[:n]); got != "ok" {
		t.Fatalf("corps attendu \"ok\", obtenu %q", got)
	}
}

func TestHealthzMethodeRefusee(t *testing.T) {
	rig := newRig(t, Config{})
	resp, err := http.Post(rig.http.URL+"/healthz", "text/plain", strings.NewReader(""))
	if err != nil {
		t.Fatalf("POST /healthz: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusMethodNotAllowed {
		t.Fatalf("statut attendu 405, obtenu %d", resp.StatusCode)
	}
}

func TestWSSansUpgradeRepond400(t *testing.T) {
	rig := newRig(t, Config{})
	resp, err := http.Get(rig.http.URL + "/ws")
	if err != nil {
		t.Fatalf("GET /ws: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusOK {
		t.Fatal("une requête sans upgrade ne doit pas aboutir")
	}
}

// --- Poignée de main ---

func TestIntegrationHelloWelcome(t *testing.T) {
	rig := newRig(t, Config{})
	c := rig.dial(t)

	welcome := c.hello("Alice", "salon")
	if welcome.SelfID != "u1" || welcome.Room != "salon" {
		t.Fatalf("welcome inattendu: %+v", welcome)
	}
	if !welcome.State.Paused || welcome.State.Rate != 1 {
		t.Fatalf("état initial inattendu: %+v", welcome.State)
	}
	if len(welcome.Users) != 1 || welcome.Users[0].Name != "Alice" {
		t.Fatalf("users inattendus: %+v", welcome.Users)
	}
	// Le ping applicatif renvoie l'écho et l'horloge serveur.
	c.send(protocol.TypePing, protocol.Ping{T: 42})
	pong := mustData[protocol.Pong](t, c.waitFor(protocol.TypePong))
	if pong.T != 42 || pong.ServerMs != msOf(rig.clock.Now()) {
		t.Fatalf("pong inattendu: %+v", pong)
	}
}

// VS-023 : le welcome annonce la version du serveur et où trouver un client à
// jour. Champs purement informatifs — la compatibilité dure reste le numéro de
// protocole, refusé au hello.
func TestIntegrationWelcomePorteLaVersionEtLeLienDeTelechargement(t *testing.T) {
	rig := newRig(t, Config{Version: "1.4.2", DownloadURL: "https://exemple.test/dl"})
	c := rig.dial(t)
	welcome := c.hello("Alice", "salon")
	if welcome.ServerVersion != "1.4.2" {
		t.Fatalf("version du serveur attendue 1.4.2, obtenue %q", welcome.ServerVersion)
	}
	if welcome.DownloadURL != "https://exemple.test/dl" {
		t.Fatalf("lien de téléchargement attendu, obtenu %q", welcome.DownloadURL)
	}
	if rig.srv.Version() != "1.4.2" {
		t.Fatalf("Version() = %q", rig.srv.Version())
	}
}

// Sans injection au build : « dev » et le lien par défaut, pour un binaire
// compilé à la main comme pour les tests.
func TestIntegrationWelcomeVersionParDefaut(t *testing.T) {
	rig := newRig(t, Config{})
	c := rig.dial(t)
	welcome := c.hello("Alice", "salon")
	if welcome.ServerVersion != defaultVersion {
		t.Fatalf("version par défaut attendue %q, obtenue %q", defaultVersion, welcome.ServerVersion)
	}
	if welcome.DownloadURL != defaultDownloadURL {
		t.Fatalf("lien par défaut attendu %q, obtenu %q", defaultDownloadURL, welcome.DownloadURL)
	}
}

func TestIntegrationVersionMismatch(t *testing.T) {
	rig := newRig(t, Config{})
	c := rig.dial(t)
	c.send(protocol.TypeHello, protocol.Hello{Version: protocol.Version + 1, Name: "Alice", Room: "salon"})

	errMsg := mustData[protocol.ErrorMsg](t, c.waitFor(protocol.TypeError))
	if errMsg.Code != protocol.ErrVersionMismatch {
		t.Fatalf("code attendu %q, obtenu %q", protocol.ErrVersionMismatch, errMsg.Code)
	}
	c.expectClosed()
	if rig.srv.hub.roomCount() != 0 {
		t.Fatal("aucune salle ne doit avoir été créée")
	}
}

func TestIntegrationBadPassword(t *testing.T) {
	rig := newRig(t, Config{Password: "s3cret"})

	c := rig.dial(t)
	c.send(protocol.TypeHello, protocol.Hello{Version: protocol.Version, Name: "Alice", Room: "salon", Password: "nope"})
	errMsg := mustData[protocol.ErrorMsg](t, c.waitFor(protocol.TypeError))
	if errMsg.Code != protocol.ErrBadPassword {
		t.Fatalf("code attendu %q, obtenu %q", protocol.ErrBadPassword, errMsg.Code)
	}
	c.expectClosed()

	// Le bon mot de passe passe.
	ok := rig.dial(t)
	ok.send(protocol.TypeHello, protocol.Hello{Version: protocol.Version, Name: "Alice", Room: "salon", Password: "s3cret"})
	if w := mustData[protocol.Welcome](t, ok.waitFor(protocol.TypeWelcome)); w.SelfID == "" {
		t.Fatal("welcome attendu avec le bon mot de passe")
	}
}

func TestIntegrationNameTaken(t *testing.T) {
	rig := newRig(t, Config{})
	a := rig.dial(t)
	a.hello("Alice", "salon")

	b := rig.dial(t)
	b.send(protocol.TypeHello, protocol.Hello{Version: protocol.Version, Name: "alice", Room: "salon"})
	errMsg := mustData[protocol.ErrorMsg](t, b.waitFor(protocol.TypeError))
	if errMsg.Code != protocol.ErrNameTaken {
		t.Fatalf("code attendu %q, obtenu %q", protocol.ErrNameTaken, errMsg.Code)
	}
	b.expectClosed()

	// La salle d'origine n'est pas perturbée.
	a.sync()
	if users := rig.srv.hub.room("salon").Users(); len(users) != 1 {
		t.Fatalf("1 membre attendu, obtenu %d", len(users))
	}
}

func TestIntegrationHelloInvalide(t *testing.T) {
	rig := newRig(t, Config{})

	cases := []struct {
		nom   string
		hello protocol.Hello
	}{
		{"pseudo vide", protocol.Hello{Version: protocol.Version, Name: "  ", Room: "salon"}},
		{"salle vide", protocol.Hello{Version: protocol.Version, Name: "Alice", Room: ""}},
		{"pseudo trop long", protocol.Hello{Version: protocol.Version, Name: strings.Repeat("x", maxNameLen+1), Room: "salon"}},
		{"pseudo avec caractère de contrôle", protocol.Hello{Version: protocol.Version, Name: "Ali\x00ce", Room: "salon"}},
		// Caractères de FORMAT Unicode : zero-width space, joiner, marque bidi.
		{"pseudo avec zero-width space", protocol.Hello{Version: protocol.Version, Name: "Alice\u200b", Room: "salon"}},
		{"pseudo avec zero-width joiner", protocol.Hello{Version: protocol.Version, Name: "Al\u200dice", Room: "salon"}},
		{"pseudo avec marque bidi", protocol.Hello{Version: protocol.Version, Name: "\u202eAlice", Room: "salon"}},
		{"salle avec zero-width space", protocol.Hello{Version: protocol.Version, Name: "Alice", Room: "sal\u200bon"}},
	}
	for _, tc := range cases {
		t.Run(tc.nom, func(t *testing.T) {
			c := rig.dial(t)
			c.send(protocol.TypeHello, tc.hello)
			errMsg := mustData[protocol.ErrorMsg](t, c.waitFor(protocol.TypeError))
			if errMsg.Code != protocol.ErrProtocol {
				t.Fatalf("code attendu %q, obtenu %q", protocol.ErrProtocol, errMsg.Code)
			}
			c.expectClosed()
		})
	}
	if rig.srv.hub.roomCount() != 0 {
		t.Fatalf("aucune salle attendue, %d créées", rig.srv.hub.roomCount())
	}
}

func TestIntegrationMessageAvantHelloRefuse(t *testing.T) {
	rig := newRig(t, Config{})
	c := rig.dial(t)
	c.send(protocol.TypePing, protocol.Ping{T: 1})
	errMsg := mustData[protocol.ErrorMsg](t, c.waitFor(protocol.TypeError))
	if errMsg.Code != protocol.ErrProtocol {
		t.Fatalf("code attendu %q, obtenu %q", protocol.ErrProtocol, errMsg.Code)
	}
	c.expectClosed()
}

func TestIntegrationEntreesMalveillantesNeTuentPasLaConnexion(t *testing.T) {
	rig := newRig(t, Config{})
	c := rig.dial(t)
	c.hello("Alice", "salon")

	// Types inconnus, data mal typée, JSON cassé : tout est ignoré.
	c.sendRaw(`{"type":"quantum","data":{"x":1}}`)
	c.sendRaw(`{"type":"control","data":{"action":42,"positionSec":"nope"}}`)
	c.sendRaw(`{"type":"report","data":"pas un objet"}`)
	c.sendRaw(`{"type":"setFile","data":{"durationSec":"beaucoup"}}`)
	c.sendRaw(`{"type":"chat","data":null}`)
	c.sendRaw(`pas du json du tout`)
	c.send(protocol.TypeControl, protocol.Control{Action: "teleport", PositionSec: 1})

	// La connexion vit toujours et l'état de la salle est intact.
	c.sync()
	if st := rig.srv.hub.room("salon").State(); !st.Paused || st.PositionSec != 0 {
		t.Fatalf("état de salle altéré: %+v", st)
	}
}

// --- Comportements de salle ---

func TestIntegrationReadyGate(t *testing.T) {
	rig := newRig(t, Config{})
	c := rig.dial(t)
	c.hello("Alice", "salon")

	// Premier play sans ready : refusé par un toast warn (pas une erreur).
	c.send(protocol.TypeControl, protocol.Control{Action: protocol.ActionPlay, PositionSec: 5})
	toast := mustData[protocol.Toast](t, c.waitFor(protocol.TypeToast))
	if toast.Level != protocol.LevelWarn || !strings.Contains(toast.Text, "Alice") {
		t.Fatalf("toast de refus attendu, obtenu %+v", toast)
	}
	st := mustData[protocol.RoomState](t, c.waitFor(protocol.TypeRoomState))
	if !st.Paused {
		t.Fatalf("l'état renvoyé doit rester en pause: %+v", st)
	}

	// Ready puis play : accepté.
	c.send(protocol.TypeSetReady, protocol.SetReady{Ready: true})
	c.send(protocol.TypeControl, protocol.Control{Action: protocol.ActionPlay, PositionSec: 5})
	st = mustData[protocol.RoomState](t, c.waitFor(protocol.TypeRoomState))
	if st.Paused || st.PositionSec != 5 || st.SetBy != "u1" {
		t.Fatalf("play attendu accepté, obtenu %+v", st)
	}
}

func TestIntegrationControlDiffuseATous(t *testing.T) {
	rig := newRig(t, Config{})
	a := rig.dial(t)
	welcomeA := a.hello("Alice", "salon")
	b := rig.dial(t)
	b.hello("Bob", "salon")

	a.send(protocol.TypeSetReady, protocol.SetReady{Ready: true})
	b.send(protocol.TypeSetReady, protocol.SetReady{Ready: true})
	a.sync()
	b.sync()

	a.send(protocol.TypeControl, protocol.Control{Action: protocol.ActionPlay, PositionSec: 30})
	for name, c := range map[string]*wsTestClient{"Alice": a, "Bob": b} {
		st := mustData[protocol.RoomState](t, c.waitFor(protocol.TypeRoomState))
		if st.Paused || st.PositionSec != 30 || st.SetBy != welcomeA.SelfID {
			t.Fatalf("%s : roomState inattendu %+v", name, st)
		}
	}

	// Un seek de Bob est également propagé aux deux.
	b.send(protocol.TypeControl, protocol.Control{Action: protocol.ActionSeek, PositionSec: 120})
	for name, c := range map[string]*wsTestClient{"Alice": a, "Bob": b} {
		st := mustData[protocol.RoomState](t, c.waitFor(protocol.TypeRoomState))
		if st.PositionSec != 120 || st.SetBy != "u2" {
			t.Fatalf("%s : seek non propagé %+v", name, st)
		}
	}
}

func TestIntegrationReportEnRetardDeclenchePauseAuto(t *testing.T) {
	rig := newRig(t, Config{})
	a := rig.dial(t)
	a.hello("Alice", "salon")
	b := rig.dial(t)
	b.hello("Bob", "salon")

	a.send(protocol.TypeSetReady, protocol.SetReady{Ready: true})
	b.send(protocol.TypeSetReady, protocol.SetReady{Ready: true})
	a.sync()
	b.sync()
	a.send(protocol.TypeControl, protocol.Control{Action: protocol.ActionPlay, PositionSec: 0})
	a.sync()
	b.sync()

	// Bob décroche : premier constat (pas encore de pause), puis > 2 s plus tard.
	rig.clock.Advance(10 * time.Second)
	b.send(protocol.TypeReport, protocol.Report{PositionSec: 1})
	b.sync()
	a.sync()
	if rig.srv.hub.room("salon").State().Paused {
		t.Fatal("le premier constat de retard ne doit pas mettre en pause")
	}

	rig.clock.Advance(3 * time.Second)
	b.send(protocol.TypeReport, protocol.Report{PositionSec: 2})

	for name, c := range map[string]*wsTestClient{"Alice": a, "Bob": b} {
		st := mustData[protocol.RoomState](t, c.waitFor(protocol.TypeRoomState))
		if !st.Paused || st.SetBy != setByServer {
			t.Fatalf("%s : pause auto attendue, obtenu %+v", name, st)
		}
		toast := mustData[protocol.Toast](t, c.waitFor(protocol.TypeToast))
		if toast.Level != protocol.LevelWarn || !strings.Contains(toast.Text, "Bob") {
			t.Fatalf("%s : toast de pause auto attendu, obtenu %+v", name, toast)
		}
	}
}

func TestIntegrationReportBufferingDeclenchePauseAuto(t *testing.T) {
	rig := newRig(t, Config{})
	a := rig.dial(t)
	a.hello("Alice", "salon")
	// Deux membres : une salle à un seul occupant ne se met jamais en pause
	// automatiquement (§Comportements serveur 2, garde-fous).
	b := rig.dial(t)
	b.hello("Bob", "salon")
	a.send(protocol.TypeSetReady, protocol.SetReady{Ready: true})
	b.send(protocol.TypeSetReady, protocol.SetReady{Ready: true})
	a.sync()
	b.sync()
	a.send(protocol.TypeControl, protocol.Control{Action: protocol.ActionPlay, PositionSec: 0})
	a.sync()
	b.sync()

	b.send(protocol.TypeReport, protocol.Report{PositionSec: 0, Buffering: true})
	st := mustData[protocol.RoomState](t, a.waitFor(protocol.TypeRoomState))
	if !st.Paused || st.SetBy != setByServer {
		t.Fatalf("pause auto attendue, obtenu %+v", st)
	}
	toast := mustData[protocol.Toast](t, a.waitFor(protocol.TypeToast))
	if !strings.Contains(toast.Text, "bufferise") {
		t.Fatalf("toast de buffering attendu, obtenu %+v", toast)
	}
}

// Le bug terrain VS-017 : seul dans sa salle, Thibault manipulait VLC et
// récoltait des « Pause auto : opmvpc bufferise » qui écrasaient ses commandes.
func TestIntegrationSeulEnSalleAucunePauseAuto(t *testing.T) {
	rig := newRig(t, Config{})
	a := rig.dial(t)
	a.hello("Alice", "salon")
	a.send(protocol.TypeSetReady, protocol.SetReady{Ready: true})
	a.send(protocol.TypeControl, protocol.Control{Action: protocol.ActionPlay, PositionSec: 0})
	a.sync()

	// Buffering franc, puis retard massif et soutenu : rien ne doit bouger.
	a.send(protocol.TypeReport, protocol.Report{PositionSec: 0, Buffering: true})
	rig.clock.Advance(20 * time.Second)
	a.send(protocol.TypeReport, protocol.Report{PositionSec: 0})
	rig.clock.Advance(20 * time.Second)
	a.send(protocol.TypeReport, protocol.Report{PositionSec: 0})
	a.sync()

	if st := rig.srv.hub.room("salon").State(); st.Paused {
		t.Fatalf("aucune pause auto attendue dans une salle à un seul membre: %+v", st)
	}
}

func TestIntegrationDeconnexionPauseAuto(t *testing.T) {
	rig := newRig(t, Config{})
	a := rig.dial(t)
	a.hello("Alice", "salon")
	b := rig.dial(t)
	b.hello("Bob", "salon")

	a.send(protocol.TypeSetReady, protocol.SetReady{Ready: true})
	b.send(protocol.TypeSetReady, protocol.SetReady{Ready: true})
	a.sync()
	b.sync()
	a.send(protocol.TypeControl, protocol.Control{Action: protocol.ActionPlay, PositionSec: 0})
	a.sync()

	rig.clock.Advance(4 * time.Second)
	_ = b.conn.Close()

	st := mustData[protocol.RoomState](t, a.waitFor(protocol.TypeRoomState))
	if !st.Paused || st.SetBy != setByServer {
		t.Fatalf("pause auto attendue après déconnexion, obtenu %+v", st)
	}
	if st.PositionSec < 3.9 || st.PositionSec > 4.1 {
		t.Fatalf("position figée ~4 s attendue, obtenue %v", st.PositionSec)
	}
	users := mustData[protocol.UsersMsg](t, a.waitFor(protocol.TypeUsers))
	if len(users.Users) != 1 || users.Users[0].Name != "Alice" {
		t.Fatalf("liste users à jour attendue, obtenue %+v", users.Users)
	}
}

func TestIntegrationSallesIsolees(t *testing.T) {
	rig := newRig(t, Config{})
	a := rig.dial(t)
	a.hello("Alice", "salon")
	b := rig.dial(t)
	b.hello("Bob", "cave")
	b.sync() // vide la file d'accueil de Bob

	if rig.srv.hub.roomCount() != 2 {
		t.Fatalf("2 salles attendues, %d obtenues", rig.srv.hub.roomCount())
	}

	a.send(protocol.TypeSetReady, protocol.SetReady{Ready: true})
	a.send(protocol.TypeControl, protocol.Control{Action: protocol.ActionPlay, PositionSec: 99})
	a.send(protocol.TypeChat, protocol.Chat{Text: "coucou salon"})
	a.sync()

	// Rien de « salon » ne doit fuiter vers « cave » : le prochain message de
	// Bob doit être son propre pong.
	token := pingToken.Add(1)
	b.send(protocol.TypePing, protocol.Ping{T: token})
	env := b.next()
	if env.Type != protocol.TypePong {
		t.Fatalf("message inattendu dans la salle « cave » : %s", env.Type)
	}
	if mustData[protocol.Pong](t, env).T != token {
		t.Fatal("pong inattendu")
	}
	if st := rig.srv.hub.room("cave").State(); !st.Paused || st.PositionSec != 0 {
		t.Fatalf("état de « cave » altéré: %+v", st)
	}

	// Chaque salle attribue ses propres identifiants.
	if rig.srv.hub.room("cave").Users()[0].ID != "u1" {
		t.Fatal("les identifiants sont locaux à la salle")
	}
}

func TestIntegrationChatEtToastsDeSalle(t *testing.T) {
	rig := newRig(t, Config{})
	a := rig.dial(t)
	a.hello("Alice", "salon")

	b := rig.dial(t)
	b.hello("Bob", "salon")

	// Alice est prévenue de l'arrivée de Bob.
	toast := mustData[protocol.Toast](t, a.waitFor(protocol.TypeToast))
	if toast.Level != protocol.LevelInfo || !strings.Contains(toast.Text, "Bob a rejoint") {
		t.Fatalf("toast d'arrivée attendu, obtenu %+v", toast)
	}

	b.send(protocol.TypeChat, protocol.Chat{Text: "salut tout le monde"})
	for name, c := range map[string]*wsTestClient{"Alice": a, "Bob": b} {
		ev := mustData[protocol.ChatEvent](t, c.waitFor(protocol.TypeChatEvent))
		if ev.From != "Bob" || ev.Text != "salut tout le monde" || ev.ServerMs != msOf(rig.clock.Now()) {
			t.Fatalf("%s : chatEvent inattendu %+v", name, ev)
		}
	}

	// Fichiers de durées différentes : avertissement à toute la salle.
	a.send(protocol.TypeSetFile, protocol.SetFile{Name: "film.mkv", DurationSec: 5400, SizeBytes: 1})
	a.sync()
	b.send(protocol.TypeSetFile, protocol.SetFile{Name: "film-vo.mkv", DurationSec: 5000, SizeBytes: 2})
	for name, c := range map[string]*wsTestClient{"Alice": a, "Bob": b} {
		toast := mustData[protocol.Toast](t, c.waitFor(protocol.TypeToast))
		if toast.Level != protocol.LevelWarn || !strings.Contains(toast.Text, "durées différentes") {
			t.Fatalf("%s : avertissement de durée attendu, obtenu %+v", name, toast)
		}
	}
}

func TestIntegrationNouvelArrivantRecoitLEtatCourant(t *testing.T) {
	rig := newRig(t, Config{})
	a := rig.dial(t)
	a.hello("Alice", "salon")
	a.send(protocol.TypeSetReady, protocol.SetReady{Ready: true})
	a.send(protocol.TypeControl, protocol.Control{Action: protocol.ActionPlay, PositionSec: 60})
	a.sync()

	playRef := msOf(rig.clock.Now())
	rig.clock.Advance(20 * time.Second)

	b := rig.dial(t)
	welcome := b.hello("Bob", "salon")
	if welcome.SelfID != "u2" {
		t.Fatalf("identifiant attendu u2, obtenu %q", welcome.SelfID)
	}
	if welcome.State.Paused || welcome.State.PositionSec != 60 || welcome.State.RefServerMs != playRef {
		t.Fatalf("le nouvel arrivant doit recevoir l'état en cours: %+v", welcome.State)
	}
	// Position attendue côté client : 60 + 20 s écoulées.
	pos := welcome.State.PositionSec +
		float64(msOf(rig.clock.Now())-welcome.State.RefServerMs)/1000*welcome.State.Rate
	if pos < 79.9 || pos > 80.1 {
		t.Fatalf("position courante attendue ~80 s, obtenue %v", pos)
	}
	if len(welcome.Users) != 2 {
		t.Fatalf("2 membres attendus dans le welcome, obtenus %d", len(welcome.Users))
	}
}

// Fenêtre de reprise (VS-021) : bout en bout, un revenant récupère la séance
// interrompue et le toast qui lui dit où elle en était.
func TestIntegrationSalleEnLingerReprendLaSeance(t *testing.T) {
	rig := newRig(t, Config{RoomLinger: 30 * time.Minute})
	a := rig.dial(t)
	a.hello("Alice", "salon")
	a.send(protocol.TypeSetReady, protocol.SetReady{Ready: true})
	a.send(protocol.TypeControl, protocol.Control{Action: protocol.ActionPlay, PositionSec: 500})
	a.sync()

	// Tout plante pendant la séance, en pleine lecture.
	rig.clock.Advance(10 * time.Second)
	_ = a.conn.Close()
	rig.waitRoomEmpty(t, "salon")
	if rig.srv.hub.room("salon") == nil {
		t.Fatal("la salle doit survivre au départ du dernier membre")
	}

	rig.clock.Advance(2 * time.Minute)
	b := rig.dial(t)
	welcome := b.helloSession("Alice", "salon", "jeton-alice")
	if !welcome.State.Paused {
		t.Fatalf("la séance reprise doit être en pause: %+v", welcome.State)
	}
	if welcome.State.PositionSec < 509.9 || welcome.State.PositionSec > 510.1 {
		t.Fatalf("position reprise attendue ≈510 s, obtenue %v", welcome.State.PositionSec)
	}
	toast := mustData[protocol.Toast](t, b.waitFor(protocol.TypeToast))
	if toast.Level != protocol.LevelInfo || !strings.Contains(toast.Text, "00:08:30") {
		t.Fatalf("toast « Séance reprise à 00:08:30 » attendu, obtenu %+v", toast)
	}
}

func TestIntegrationSalleEnLingerDetruiteApresExpiration(t *testing.T) {
	rig := newRig(t, Config{RoomLinger: time.Minute})
	a := rig.dial(t)
	a.hello("Alice", "salon")
	_ = a.conn.Close()
	rig.waitRoomEmpty(t, "salon")

	rig.clock.Advance(2 * time.Minute)
	rig.srv.hub.gc()
	rig.waitRoomDestroyed(t, "salon")
	if rig.srv.hub.roomCount() != 0 {
		t.Fatalf("plus aucune salle attendue, %d restantes", rig.srv.hub.roomCount())
	}
}

// Fenêtre de reprise désactivée par le harnais : la salle disparaît sur-le-champ.
func TestIntegrationSalleDetruiteQuandVide(t *testing.T) {
	rig := newRig(t, Config{})
	a := rig.dial(t)
	a.hello("Alice", "salon")
	if rig.srv.hub.roomCount() != 1 {
		t.Fatal("salle attendue créée")
	}
	_ = a.conn.Close()

	rig.waitRoomDestroyed(t, "salon")
	if rig.srv.hub.roomCount() != 0 || rig.srv.hub.room("salon") != nil {
		t.Fatal("la salle doit être détruite après le départ du dernier membre")
	}

	// Et elle est recréée proprement au hello suivant (identifiants remis à u1).
	b := rig.dial(t)
	welcome := b.hello("Bob", "salon")
	if welcome.SelfID != "u1" || len(welcome.Users) != 1 || !welcome.State.Paused {
		t.Fatalf("salle recréée à neuf attendue, obtenu %+v", welcome)
	}
}

// --- Anti-abus (spec §Comportements serveur, point 6) ---

func TestIntegrationFloodFermeLaConnexion(t *testing.T) {
	rig := newRig(t, Config{})
	c := rig.dial(t)
	c.hello("Alice", "salon")

	// Horloge figée : aucun jeton ne se recharge, la rafale est vite épuisée.
	for i := 0; i < msgRateBurst+10; i++ {
		if err := c.trySend(protocol.TypePing, protocol.Ping{T: int64(i)}); err != nil {
			break // le serveur a déjà coupé
		}
	}
	errMsg := mustData[protocol.ErrorMsg](t, c.waitFor(protocol.TypeError))
	if errMsg.Code != protocol.ErrProtocol {
		t.Fatalf("code attendu %q, obtenu %q", protocol.ErrProtocol, errMsg.Code)
	}
	c.expectClosed()
	rig.waitRoomDestroyed(t, "salon")
}

func TestIntegrationDebitNormalNonAffecte(t *testing.T) {
	rig := newRig(t, Config{})
	c := rig.dial(t)
	c.hello("Alice", "salon")

	// 10 messages par seconde simulée pendant 12 s : sous la limite de 20/s.
	//
	// La barrière après chaque envoi n'est pas cosmétique : sans elle, rien ne
	// garantit que le serveur traite un report avant que le test n'avance
	// l'horloge du suivant. Les écritures partent en rafale dans le tampon TCP,
	// le serveur peut les consommer toutes au même instant simulé — et le seau
	// à jetons, plafonné à msgRateBurst, refuse alors la fin de la rafale
	// (« flood détecté »). Le débit *observé par le serveur* doit être piloté,
	// pas seulement le débit d'écriture.
	for i := 0; i < 60; i++ {
		rig.clock.Advance(200 * time.Millisecond)
		c.send(protocol.TypeReport, protocol.Report{PositionSec: float64(i)})
		c.sync() // le report ci-dessus est traité avant le prochain tic d'horloge
	}
	if users := rig.srv.hub.room("salon").Users(); users[0].PositionSec != 59 {
		t.Fatalf("tous les reports doivent avoir été traités, dernier %v", users[0].PositionSec)
	}
}

func TestIntegrationChatThrottleSansFermeture(t *testing.T) {
	rig := newRig(t, Config{})
	c := rig.dial(t)
	c.hello("Alice", "salon")

	for i := 0; i < chatRateBurst+3; i++ {
		c.send(protocol.TypeChat, protocol.Chat{Text: "spam"})
	}

	// Le budget chat est dépassé : toast d'avertissement, connexion préservée.
	toast := mustData[protocol.Toast](t, c.waitFor(protocol.TypeToast))
	if toast.Level != protocol.LevelWarn || !strings.Contains(toast.Text, "chat") {
		t.Fatalf("toast de throttle attendu, obtenu %+v", toast)
	}
	c.send(protocol.TypeSetReady, protocol.SetReady{Ready: true})
	c.sync()
	if !rig.srv.hub.room("salon").Users()[0].Ready {
		t.Fatal("la connexion doit rester utilisable après un throttle de chat")
	}
}

func TestIntegrationPlafondDeMembresParSalle(t *testing.T) {
	rig := newRig(t, Config{MaxRoomSize: 2})
	a := rig.dial(t)
	a.hello("Alice", "salon")
	b := rig.dial(t)
	b.hello("Bob", "salon")

	c := rig.dial(t)
	c.send(protocol.TypeHello, protocol.Hello{Version: protocol.Version, Name: "Carol", Room: "salon"})
	errMsg := mustData[protocol.ErrorMsg](t, c.waitFor(protocol.TypeError))
	if errMsg.Code != protocol.ErrProtocol || !strings.Contains(errMsg.Text, "pleine") {
		t.Fatalf("refus « salle pleine » attendu, obtenu %+v", errMsg)
	}
	c.expectClosed()

	// La salle existante n'est pas perturbée.
	a.sync()
	if got := len(rig.srv.hub.room("salon").Users()); got != 2 {
		t.Fatalf("2 membres attendus, obtenu %d", got)
	}
}

func TestIntegrationPlafondDeSalles(t *testing.T) {
	rig := newRig(t, Config{MaxRooms: 1})
	a := rig.dial(t)
	a.hello("Alice", "salon")

	b := rig.dial(t)
	b.send(protocol.TypeHello, protocol.Hello{Version: protocol.Version, Name: "Bob", Room: "cave"})
	errMsg := mustData[protocol.ErrorMsg](t, b.waitFor(protocol.TypeError))
	if errMsg.Code != protocol.ErrProtocol || !strings.Contains(errMsg.Text, "salles") {
		t.Fatalf("refus « trop de salles » attendu, obtenu %+v", errMsg)
	}
	b.expectClosed()

	// Rejoindre la salle déjà ouverte reste possible.
	c := rig.dial(t)
	if w := c.hello("Bob", "salon"); w.SelfID != "u2" {
		t.Fatalf("Bob doit pouvoir rejoindre la salle existante, obtenu %+v", w)
	}
}

func TestIntegrationPlafondDeConnexions(t *testing.T) {
	rig := newRig(t, Config{MaxClients: 1})
	a := rig.dial(t)
	a.hello("Alice", "salon")

	_, resp, err := rig.tryDial(t)
	if err == nil {
		t.Fatal("la deuxième connexion doit être refusée")
	}
	if resp == nil || resp.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("statut 503 attendu, obtenu %v", resp)
	}

	// La place se libère à la déconnexion.
	_ = a.conn.Close()
	rig.waitRoomDestroyed(t, "salon")
	b := rig.dial(t)
	b.hello("Bob", "salon")
}
