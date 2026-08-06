package client

import (
	"bytes"
	"context"
	"encoding/json"
	"flag"
	"math"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/thibsix/vibesync/internal/protocol"
	"github.com/thibsix/vibesync/internal/vlc"
	"github.com/thibsix/vibesync/internal/vlc/vlctest"
)

// TestVectors rejoue des scénarios de référence et compare leur trace aux
// fichiers golden de test/vectors/. Ces fichiers sont le contrat de conformité
// des futures réimplémentations du moteur (C/Win32, Swift) : ils sont committés
// et ne se régénèrent QUE sur demande explicite :
//
//	go test ./internal/client -run TestVectors -update
//
// Sans le drapeau, toute divergence fait échouer le test.

var updateVectors = flag.Bool("update", false, "régénère les fichiers golden de test/vectors/")

const vectorsDoc = "Vecteur de conformité du moteur de sync vibesync (golden file, " +
	"régénéré par `go test ./internal/client -run TestVectors -update`). " +
	"Horloge simulée : tous les temps sont en millisecondes depuis le début du " +
	"scénario. `initialVLC` = état complet de VLC au départ ; `events` = entrées " +
	"injectées (messages serveur, actions de l'utilisateur dans VLC, coupure de " +
	"session) ; `trace` = une entrée par poll de 200 ms avec l'état observé de " +
	"VLC, la position attendue calculée, le drift, les commandes envoyées à VLC " +
	"et les messages envoyés au serveur. Une réimplémentation est conforme si, " +
	"pour le même `initialVLC` et les mêmes `events`, elle produit les mêmes " +
	"`vlcCommands` et `toServer` aux mêmes instants."

type vectorEvent struct {
	AtMs int64           `json:"atMs"`
	Type string          `json:"type"`
	Data json.RawMessage `json:"data,omitempty"`
}

type vectorCommand struct {
	Cmd   string   `json:"cmd"`
	Value *float64 `json:"value,omitempty"`
}

type vectorMessage struct {
	Type string          `json:"type"`
	Data json.RawMessage `json:"data"`
}

type vectorStep struct {
	AtMs        int64           `json:"atMs"`
	VLCState    string          `json:"vlcState"`
	VLCPosition float64         `json:"vlcPositionSec"`
	Expected    float64         `json:"expectedPositionSec"`
	Drift       float64         `json:"driftSec"`
	Commands    []vectorCommand `json:"vlcCommands,omitempty"`
	ToServer    []vectorMessage `json:"toServer,omitempty"`
}

// vectorVLC est l'état initial complet de VLC : aucune hypothèse implicite.
type vectorVLC struct {
	FileName    string  `json:"fileName"`
	DurationSec float64 `json:"durationSec"`
	State       string  `json:"state"`
	PositionSec float64 `json:"positionSec"`
	Rate        float64 `json:"rate"`
}

type vector struct {
	Doc            string        `json:"_doc"`
	Name           string        `json:"name"`
	Description    string        `json:"description"`
	PollIntervalMs int64         `json:"pollIntervalMs"`
	InitialVLC     vectorVLC     `json:"initialVLC"`
	Events         []vectorEvent `json:"events"`
	Trace          []vectorStep  `json:"trace"`
}

// recorder enregistre les commandes envoyées à VLC.
type recorder struct {
	vlc.Controller
	cmds []vectorCommand
}

func (r *recorder) Pause(ctx context.Context) error {
	r.cmds = append(r.cmds, vectorCommand{Cmd: "pause"})
	return r.Controller.Pause(ctx)
}

func (r *recorder) Resume(ctx context.Context) error {
	r.cmds = append(r.cmds, vectorCommand{Cmd: "resume"})
	return r.Controller.Resume(ctx)
}

func (r *recorder) Seek(ctx context.Context, pos float64) error {
	v := math.Round(pos)
	r.cmds = append(r.cmds, vectorCommand{Cmd: "seek", Value: &v})
	return r.Controller.Seek(ctx, pos)
}

func (r *recorder) SetRate(ctx context.Context, rate float64) error {
	v := round3(rate)
	r.cmds = append(r.cmds, vectorCommand{Cmd: "rate", Value: &v})
	return r.Controller.SetRate(ctx, rate)
}

func (r *recorder) drain() []vectorCommand {
	out := r.cmds
	r.cmds = nil
	return out
}

func round3(v float64) float64 { return math.Round(v*1000) / 1000 }

// vecBuilder pilote un scénario et en construit la trace.
type vecBuilder struct {
	t     *testing.T
	h     *harness
	rec   *recorder
	start time.Time
	vec   *vector
}

// vecSetup décrit l'état initial de VLC pour un scénario.
type vecSetup struct {
	name        string
	description string
	file        string
	durationSec float64
	positionSec float64
	playing     bool
}

func newVecBuilder(t *testing.T, s vecSetup) *vecBuilder {
	t.Helper()
	clock := vlctest.NewClock(time.Time{})
	fake := vlctest.New(clock.Now)
	t.Cleanup(fake.Close)
	fake.LoadFile(s.file, s.durationSec)
	rec := &recorder{Controller: vlc.NewHTTPClient(fake.URL(), fake.Password())}
	conn := newFakeConn()
	e := New(Config{
		Clock:   clock,
		Dialer:  DialerFunc(func(context.Context, string) (Conn, error) { return conn, nil }),
		Locator: func() (string, error) { return "/faux/vlc", nil },
		Launcher: func(ctx context.Context, _ string) (vlc.Controller, error) {
			if err := vlc.Prepare(ctx, rec, 5*time.Second); err != nil {
				return nil, err
			}
			return rec, nil
		},
	})
	t.Cleanup(func() { _ = e.Close() })
	h := &harness{t: t, e: e, clock: clock, fake: fake, conn: conn}
	e.mu.Lock()
	e.conn = conn // session déjà établie : on veut voir les messages sortants
	e.mu.Unlock()

	path := filepath.Join(t.TempDir(), s.file)
	if err := os.WriteFile(path, []byte("données vidéo"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := e.OpenFile(context.Background(), path); err != nil {
		t.Fatalf("OpenFile: %v", err)
	}
	// Le média est ouvert et arrêté au début : on installe alors l'état VLC de
	// départ du scénario.
	fake.SeekTo(s.positionSec)
	if s.playing {
		fake.Play()
	}
	rec.drain()
	conn.take()

	state := "paused"
	if s.playing {
		state = "playing"
	}
	return &vecBuilder{
		t: t, h: h, rec: rec, start: clock.Now(),
		vec: &vector{
			Doc: vectorsDoc, Name: s.name, Description: s.description,
			PollIntervalMs: PollInterval.Milliseconds(),
			InitialVLC: vectorVLC{
				FileName:    s.file,
				DurationSec: s.durationSec,
				State:       state,
				PositionSec: round3(s.positionSec),
				Rate:        1,
			},
			Events: []vectorEvent{}, Trace: []vectorStep{},
		},
	}
}

func (b *vecBuilder) atMs() int64 { return b.h.clock.Now().Sub(b.start).Milliseconds() }

// event injecte un message serveur et le consigne.
func (b *vecBuilder) event(msgType string, data any) {
	raw, err := json.Marshal(data)
	if err != nil {
		b.t.Fatal(err)
	}
	b.vec.Events = append(b.vec.Events, vectorEvent{AtMs: b.atMs(), Type: msgType, Data: raw})
	b.h.server(msgType, data)
	// Les réponses immédiates au message (setFile/setReady/ping du welcome) ne
	// font pas partie de la trace : seules les décisions par poll y figurent.
	b.h.conn.take()
	b.rec.drain()
}

// eventKeep consigne un message serveur SANS vider la file d'envoi : ce que le
// moteur émet en réaction immédiate apparaît alors dans le premier pas de trace
// qui suit. Réservé aux scénarios dont la réaction au message EST la règle
// testée (reprise « salle vierge »).
func (b *vecBuilder) eventKeep(msgType string, data any) {
	raw, err := json.Marshal(data)
	if err != nil {
		b.t.Fatal(err)
	}
	b.vec.Events = append(b.vec.Events, vectorEvent{AtMs: b.atMs(), Type: msgType, Data: raw})
	b.h.server(msgType, data)
}

// welcomeKeep injecte un welcome (et son pong) en gardant les messages émis en
// réaction, cf. eventKeep.
func (b *vecBuilder) welcomeKeep(state protocol.RoomState) {
	b.eventKeep(protocol.TypeWelcome, protocol.Welcome{
		SelfID: "u1", Room: "salon", State: state,
		Users: []protocol.User{{ID: "u1", Name: "thib"}},
	})
	now := b.h.clock.Now().UnixMilli()
	b.eventKeep(protocol.TypePong, protocol.Pong{T: now, ServerMs: now})
}

// welcome injecte un welcome puis le pong qui débloque les corrections.
func (b *vecBuilder) welcome(state protocol.RoomState) {
	b.event(protocol.TypeWelcome, protocol.Welcome{
		SelfID: "u1", Room: "salon", State: state,
		Users: []protocol.User{{ID: "u1", Name: "thib"}},
	})
	now := b.h.clock.Now().UnixMilli()
	b.event(protocol.TypePong, protocol.Pong{T: now, ServerMs: now})
}

// userEvent consigne une action de l'utilisateur dans VLC.
func (b *vecBuilder) userEvent(kind string, arg float64) {
	var raw json.RawMessage
	if kind == "userSeek" {
		raw, _ = json.Marshal(map[string]float64{"positionSec": arg})
	}
	b.vec.Events = append(b.vec.Events, vectorEvent{AtMs: b.atMs(), Type: kind, Data: raw})
	switch kind {
	case "userPause":
		b.h.fake.Pause()
	case "userPlay":
		b.h.fake.Play()
	case "userSeek":
		b.h.fake.SeekTo(arg)
	}
}

// sessionLost consigne une coupure de la session serveur.
func (b *vecBuilder) sessionLost() {
	b.vec.Events = append(b.vec.Events, vectorEvent{AtMs: b.atMs(), Type: "connectionLost"})
	b.h.sessionEnd()
	b.h.conn.take()
	b.rec.drain()
}

// wait avance l'horloge sans poll (le scénario saute un intervalle).
func (b *vecBuilder) wait(d time.Duration) {
	b.vec.Events = append(b.vec.Events, vectorEvent{
		AtMs: b.atMs(), Type: "wait",
		Data: json.RawMessage(`{"durationMs":` + itoa(d.Milliseconds()) + `}`),
	})
	b.h.clock.Advance(d)
}

func itoa(v int64) string {
	raw, _ := json.Marshal(v)
	return string(raw)
}

// run exécute n polls en consignant la trace.
func (b *vecBuilder) run(n int) {
	for range n {
		b.h.tick(PollInterval)
		e := b.h.e
		e.mu.Lock()
		step := vectorStep{
			AtMs:        b.atMs(),
			VLCState:    string(e.status.State),
			VLCPosition: round3(e.status.PositionSec),
			Expected:    round3(e.expectedPositionLocked(b.h.clock.Now())),
			Drift:       round3(e.drift),
		}
		e.mu.Unlock()
		step.Commands = b.rec.drain()
		for _, env := range b.h.conn.take() {
			step.ToServer = append(step.ToServer, vectorMessage{Type: env.Type, Data: env.Data})
		}
		b.vec.Trace = append(b.vec.Trace, step)
	}
}

func vectorsDir() string { return filepath.Join("..", "..", "test", "vectors") }

// check compare la trace au golden file, ou le régénère avec -update.
func (b *vecBuilder) check() {
	b.t.Helper()
	raw, err := json.MarshalIndent(b.vec, "", "  ")
	if err != nil {
		b.t.Fatal(err)
	}
	raw = append(raw, '\n')
	path := filepath.Join(vectorsDir(), b.vec.Name+".json")

	if *updateVectors {
		if err := os.MkdirAll(vectorsDir(), 0o755); err != nil {
			b.t.Fatalf("création de %s: %v", vectorsDir(), err)
		}
		if err := os.WriteFile(path, raw, 0o600); err != nil {
			b.t.Fatalf("écriture de %s: %v", path, err)
		}
		return
	}

	want, err := os.ReadFile(path)
	if err != nil {
		b.t.Fatalf("golden manquant (%v) — régénérez avec: go test ./internal/client -run TestVectors -update", err)
	}
	if bytes.Equal(bytes.ReplaceAll(want, []byte("\r\n"), []byte("\n")), raw) {
		return
	}
	b.t.Errorf("le comportement du moteur diverge du vecteur %s:\n%s\n"+
		"Si la divergence est voulue (et conforme à docs/protocol.md), régénérez :\n"+
		"  go test ./internal/client -run TestVectors -update", b.vec.Name, firstDiff(want, raw))
}

// firstDiff situe la première ligne divergente pour un message d'échec lisible.
func firstDiff(want, got []byte) string {
	wl := bytes.Split(bytes.ReplaceAll(want, []byte("\r\n"), []byte("\n")), []byte("\n"))
	gl := bytes.Split(got, []byte("\n"))
	for i := range max(len(wl), len(gl)) {
		var w, g []byte
		if i < len(wl) {
			w = wl[i]
		}
		if i < len(gl) {
			g = gl[i]
		}
		if !bytes.Equal(w, g) {
			return "  ligne " + itoa(int64(i+1)) + "\n  golden : " + string(w) + "\n  obtenu : " + string(g)
		}
	}
	return "  (différence hors contenu textuel)"
}

func TestVectors(t *testing.T) {
	// 1. Zone morte : rien ne doit bouger.
	b := newVecBuilder(t, vecSetup{
		name: "01-zone-morte", file: "ep1.mkv", durationSec: 1200, positionSec: 100, playing: true,
		description: "VLC est aligné sur la salle : aucune correction.",
	})
	b.welcome(b.h.playing(100))
	b.run(10)
	b.check()

	// 2. Nudge : 0,6 s d'avance → 0,95× puis retour à 1 après convergence
	// (hystérésis : le nudge ne se relâche qu'en dessous de 0,03 s).
	b = newVecBuilder(t, vecSetup{
		name: "02-nudge-avance", file: "ep1.mkv", durationSec: 1200, positionSec: 100.6, playing: true,
		description: "Drift de +0,6 s : rate-nudge à 0,95× jusqu'à l'hystérésis basse, puis retour à 1×.",
	})
	b.welcome(b.h.playing(100))
	b.run(4)
	b.wait(11 * time.Second) // convergence simulée
	b.run(6)
	b.check()

	// 3. Seek dur : 180 s de retard.
	b = newVecBuilder(t, vecSetup{
		name: "03-seek-dur", file: "ep1.mkv", durationSec: 3600, positionSec: 120, playing: true,
		description: "Drift de -180 s : seek dur vers la position attendue puis affinage au rate.",
	})
	b.welcome(b.h.playing(300))
	b.run(8)
	b.check()

	// 4. Pause manuelle de l'utilisateur → control pause, VLC n'est pas relancé.
	b = newVecBuilder(t, vecSetup{
		name: "04-pause-manuelle", file: "ep1.mkv", durationSec: 1200, positionSec: 100, playing: true,
		description: "L'utilisateur met VLC en pause : control pause au serveur, puis hold (VLC reste en pause).",
	})
	b.welcome(b.h.playing(100))
	b.run(6)
	b.userEvent("userPause", 0)
	b.run(4)
	b.check()

	// 5. Pause distante : anti-boucle, aucun control ne doit repartir.
	b = newVecBuilder(t, vecSetup{
		name: "05-pause-distante", file: "ep1.mkv", durationSec: 1200, positionSec: 100, playing: true,
		description: "Le serveur diffuse une pause : le moteur pause VLC sans la renvoyer comme action utilisateur (fenêtre de grâce).",
	})
	b.welcome(b.h.playing(100))
	b.run(4)
	b.event(protocol.TypeRoomState, b.h.paused(120))
	b.run(8)
	b.check()

	// 6. Rejoin : welcome sur une salle déjà lancée.
	b = newVecBuilder(t, vecSetup{
		name: "06-rejoin", file: "ep1.mkv", durationSec: 3600, positionSec: 0, playing: true,
		description: "Arrivée dans une salle en cours de lecture à 1200 s : resynchronisation immédiate.",
	})
	b.welcome(b.h.playing(1200))
	b.run(6)
	b.check()

	// 7. Hold levé par l'écho du serveur (setBy = soi).
	b = newVecBuilder(t, vecSetup{
		name: "07-hold-avec-echo", file: "ep1.mkv", durationSec: 1200, positionSec: 100, playing: true,
		description: "Action utilisateur puis écho du serveur (setBy = soi) : le hold tombe et l'état est adopté immédiatement.",
	})
	b.welcome(b.h.playing(100))
	b.run(5)
	b.userEvent("userPause", 0)
	b.run(2)
	echo := b.h.paused(101)
	echo.SetBy = "u1"
	b.event(protocol.TypeRoomState, echo)
	b.run(5)
	b.check()

	// 8. Hold sans écho : roomState d'autrui mémorisé, appliqué à l'expiration.
	b = newVecBuilder(t, vecSetup{
		name: "08-hold-sans-echo", file: "ep1.mkv", durationSec: 3600, positionSec: 100, playing: true,
		description: "Action utilisateur, puis roomState d'autrui pendant le hold : mémorisé, appliqué seulement à l'expiration des 2 s.",
	})
	b.welcome(b.h.playing(100))
	b.run(5)
	b.userEvent("userPause", 0)
	b.run(2)
	b.event(protocol.TypeRoomState, b.h.playing(2000))
	b.run(4)
	b.wait(UserHold)
	b.run(4)
	b.check()

	// 9. Seuil de seek en pause : 0,98 s corrigé, 0,4 s ignoré.
	b = newVecBuilder(t, vecSetup{
		name: "09-seuil-pause", file: "ep1.mkv", durationSec: 1200, positionSec: 10.51, playing: false,
		description: "En pause : drift de 0,98 s corrigé par seek (seuil 0,6 s), puis drift résiduel < 0,6 s laissé tel quel.",
	})
	b.welcome(b.h.paused(11.49))
	b.run(8)
	b.check()

	// 10. Offset d'horloge : la médiane doit ignorer un RTT aberrant.
	b = newVecBuilder(t, vecSetup{
		name: "10-offset-median", file: "ep1.mkv", durationSec: 1200, positionSec: 100, playing: true,
		description: "Cinq pongs dont un aberrant : l'offset retenu est la médiane, la position attendue n'est pas perturbée.",
	})
	b.welcome(b.h.playing(100))
	b.run(2)
	for _, delta := range []int64{40, 60, 50, 9000, 45} {
		now := b.h.clock.Now().UnixMilli()
		b.event(protocol.TypePong, protocol.Pong{T: now - 100, ServerMs: now + delta - 50})
		b.run(2)
	}
	b.run(4)
	b.check()

	// 11. Hystérésis : un drift de 0,05 s n'engage pas le nudge.
	b = newVecBuilder(t, vecSetup{
		name: "11-hysteresis", file: "ep1.mkv", durationSec: 1200, positionSec: 100.05, playing: true,
		description: "Drift de 0,05 s : sous le seuil d'engagement (0,1 s), aucun nudge n'est engagé.",
	})
	b.welcome(b.h.playing(100))
	b.run(8)
	b.check()

	// 12. Coupure : aucune correction avec un état périmé, resync au rejoin.
	b = newVecBuilder(t, vecSetup{
		name: "12-coupure-rejoin", file: "ep1.mkv", durationSec: 3600, positionSec: 300, playing: true,
		description: "Session perdue : l'état de référence est invalidé et VLC n'est plus piloté ; le welcome de reconnexion resynchronise.",
	})
	b.welcome(b.h.playing(300))
	b.run(4)
	b.sessionLost()
	b.wait(20 * time.Second)
	b.run(5)
	b.welcome(b.h.playing(1500))
	b.run(5)
	b.check()

	// 13. Salle vierge : le serveur est revenu tout neuf (redémarrage) et ne
	// connaît plus la séance. Le client propose sa propre position par UNE
	// reprise `control seek` ; le hold post-action empêche l'alignement sur la
	// position 0 de la salle vierge, puis l'écho du serveur adopte la reprise.
	b = newVecBuilder(t, vecSetup{
		name: "13-reprise-salle-vierge", file: "ep1.mkv", durationSec: 7200,
		positionSec: 1800, playing: true,
		description: "Le serveur redémarre : welcome d'une salle vierge (setBy vide, " +
			"position 0) alors que VLC est loin dans le film. Le client émet UNE " +
			"reprise control seek à sa position au lieu de se laisser ramener à 0, " +
			"puis adopte l'écho du serveur.",
	})
	b.welcome(b.h.playing(1800))
	b.run(4)
	b.sessionLost()
	b.wait(5 * time.Second)
	b.run(3)
	// Le serveur revient : salle jamais pilotée. La trace garde ici les messages
	// émis en réaction au welcome — la reprise en fait partie.
	b.welcomeKeep(protocol.RoomState{Paused: true, PositionSec: 0, Rate: 1, RefServerMs: 1})
	b.run(6)
	// Le serveur applique la reprise et la renvoie en écho (setBy = nous).
	reprise := b.h.paused(b.vlcPosition())
	reprise.SetBy = "u1"
	b.event(protocol.TypeRoomState, reprise)
	b.run(4)
	b.check()
}

// vlcPosition est la dernière position de VLC observée par le moteur : sert à
// fabriquer un écho serveur cohérent avec la reprise que le client vient
// d'émettre.
func (b *vecBuilder) vlcPosition() float64 {
	b.h.e.mu.Lock()
	defer b.h.e.mu.Unlock()
	return round3(b.h.e.status.PositionSec)
}

// TestVectorsGoldenComplets vérifie la forme des fichiers committés.
func TestVectorsGoldenComplets(t *testing.T) {
	files, err := filepath.Glob(filepath.Join(vectorsDir(), "*.json"))
	if err != nil {
		t.Fatal(err)
	}
	if len(files) < 13 {
		t.Fatalf("%d vecteurs golden, attendu au moins 13", len(files))
	}
	for _, f := range files {
		raw, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		var v vector
		if err := json.Unmarshal(raw, &v); err != nil {
			t.Fatalf("%s: %v", f, err)
		}
		switch {
		case v.Doc == "":
			t.Errorf("%s: documentation d'en-tête absente", f)
		case v.Name == "" || v.Description == "":
			t.Errorf("%s: nom ou description absents", f)
		case v.InitialVLC.State == "" || v.InitialVLC.FileName == "" || v.InitialVLC.DurationSec <= 0:
			t.Errorf("%s: état VLC initial incomplet: %+v", f, v.InitialVLC)
		case len(v.Events) == 0 || len(v.Trace) == 0:
			t.Errorf("%s: scénario vide", f)
		case v.PollIntervalMs != PollInterval.Milliseconds():
			t.Errorf("%s: intervalle de poll incohérent (%d)", f, v.PollIntervalMs)
		}
	}
}
