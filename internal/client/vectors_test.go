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

	"github.com/opmvpc/vibesync/internal/protocol"
	"github.com/opmvpc/vibesync/internal/vlc"
	"github.com/opmvpc/vibesync/internal/vlc/vlctest"
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
	// KeepOutput dit au rejeu quoi faire des messages émis en réaction immédiate
	// à cet événement : false (défaut, champ absent) = ils ne font pas partie de
	// la trace et sont jetés ; true = ils sont conservés et apparaissent dans le
	// `toServer` du premier pas qui suit. Sans ce drapeau, deux vecteurs aux
	// événements identiques attendraient des traces différentes et aucune règle
	// de rejeu unique ne pourrait satisfaire les deux.
	KeepOutput bool `json:"keepOutput,omitempty"`
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
	Doc         string `json:"_doc"`
	Name        string `json:"name"`
	Description string `json:"description"`
	// Scenario détaille, quand c'est nécessaire, les préconditions que
	// `initialVLC` ne porte pas : état du transport, de la connexion, du
	// fichier. Absent quand le scénario se lit entièrement dans les événements.
	Scenario       string        `json:"scenario,omitempty"`
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
	scenario    string
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
			Scenario:       s.scenario,
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
	b.vec.Events = append(b.vec.Events, vectorEvent{
		AtMs: b.atMs(), Type: msgType, Data: raw, KeepOutput: true,
	})
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

// noise joue `polls` polls en plaçant VLC à « position attendue ± 0,15 s » par
// un userSeek sous le seuil de détection (3 s). C'est le bruit que rend le vrai
// VLC (VS-029, mesuré dans la VM Win11). Avant VS-038 il faisait churner le
// nudge ; il est désormais dix fois sous la zone morte et ne doit produire
// AUCUNE commande — mais c'est toujours le régime dans lequel la détection
// d'action utilisateur doit fonctionner. Le seul vocabulaire employé est celui
// que tous les harnais de rejeu comprennent (userSeek + poll) : aucun réglage
// caché du faux VLC.
func (b *vecBuilder) noise(polls int) {
	for i := 1; i <= polls; i++ {
		jitter := 0.15
		if i%2 == 0 {
			jitter = -0.15
		}
		next := b.h.clock.Now().Add(PollInterval)
		b.h.e.mu.Lock()
		want := b.h.e.expectedPositionLocked(next) + jitter
		b.h.e.mu.Unlock()
		// Le seek place la position MAINTENANT : on retranche ce que VLC va
		// lire d'ici au poll pour qu'il s'y présente à `want`.
		b.userEvent("userSeek", round3(want-PollInterval.Seconds()*b.h.fake.Rate()))
		b.run(1)
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

	// 2. Micro-seek : 2,5 s d'avance, au-dessus de la zone morte (1,5 s) et sous
	// le seuil du seek immédiat (5 s). La vitesse n'est jamais touchée : le
	// recalage se fait par UN seek, et seulement une fois la dérive avérée sur
	// les 5 derniers polls (docs/protocol.md §Persistance de la dérive).
	b = newVecBuilder(t, vecSetup{
		name: "02-micro-seek-avance", file: "ep1.mkv", durationSec: 1200, positionSec: 102.5, playing: true,
		description: "Drift de +2,5 s : rien pendant les 4 premiers polls (historique de " +
			"dérive incomplet), puis UN micro-seek de recalage au 5e. Aucune commande rate.",
	})
	b.welcome(b.h.playing(100))
	b.run(10)
	b.check()

	// 3. Seek immédiat : 180 s de retard, très au-delà des 5 s — la médiane
	// n'est pas consultée.
	b = newVecBuilder(t, vecSetup{
		name: "03-seek-dur", file: "ep1.mkv", durationSec: 3600, positionSec: 120, playing: true,
		description: "Drift de -180 s : seek immédiat vers la position attendue (≥ 5 s, " +
			"sans attendre la persistance), puis plus rien à corriger.",
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

	// 11. Zone morte large puis micro-seek : 1,2 s de dérive ne déclenchent
	// rien, même prolongés ; quand la référence bouge et porte la dérive à
	// 2,2 s, le recalage ne part qu'après 5 polls de persistance.
	b = newVecBuilder(t, vecSetup{
		name: "11-zone-morte-micro-seek", file: "ep1.mkv", durationSec: 1200,
		positionSec: 101.2, playing: true,
		description: "Zone morte de 1,5 s : un drift de 1,2 s ne déclenche rien, même " +
			"prolongé. La référence bouge alors d'une seconde (roomState) : la dérive " +
			"passe à ≈2,2 s, et UN micro-seek part au 3e poll qui suit — l'instant où " +
			"la MÉDIANE des 5 derniers polls dépasse enfin le seuil. Aucune commande rate.",
	})
	b.welcome(b.h.playing(100))
	b.run(6)
	// La salle recule d'une seconde : la dérive sort de la zone morte.
	b.event(protocol.TypeRoomState, b.h.playing(round3(b.h.fake.Position())-2.2))
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
		description: "Le serveur redémarre et revient sans mémoire : welcome d'une " +
			"salle vierge (setBy vide, position 0). Le client, qui connaissait déjà " +
			"la séance de CETTE salle, émet UNE reprise control seek à la dernière " +
			"position de salle connue au lieu de se laisser ramener à 0, puis adopte " +
			"l'écho du serveur.",
		scenario: "Préconditions du moteur au premier événement : session serveur " +
			"déjà établie (le hello est passé, la connexion est ouverte), aucun état " +
			"de salle connu, aucune mesure d'offset d'horloge. Fichier ep1.mkv déjà " +
			"ouvert dans le lecteur, durée 7200 s, taille 15 octets (c'est elle que " +
			"portent les setFile de la trace), position 1800 s, lecture en cours, " +
			"rate 1. Déroulé : (1) welcome + pong d'une salle en lecture à 1800 s — " +
			"c'est ce passage qui rend la reprise possible plus tard, un premier join " +
			"ne proposerait jamais rien ; (2) la connexion tombe et 5 s s'écoulent " +
			"sans serveur, le lecteur continue seul ; (3) le serveur revient avec une " +
			"salle vierge — ce welcome et son pong portent \"keepOutput\": true, la " +
			"reprise qu'ils déclenchent fait partie de la trace ; (4) le serveur " +
			"renvoie la reprise en écho (setBy = u1) et le moteur s'aligne dessus.",
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
	// Le serveur applique la reprise telle quelle et la renvoie en écho
	// (setBy = nous) : la position de l'écho est EXACTEMENT celle du control
	// émis, pas une valeur voisine reconstruite après coup.
	reprise := b.h.paused(b.lastControlPosition())
	reprise.SetBy = "u1"
	b.event(protocol.TypeRoomState, reprise)
	b.run(4)
	b.check()

	// 14. Action utilisateur sous le bruit de position (VS-029, redessiné par
	// VS-038). La position que rend VLC oscille de ±0,15 s autour de la
	// référence. Ce bruit ne doit produire AUCUNE commande (il est dix fois
	// sous la zone morte) et la fenêtre de grâce ne doit pas rester ouverte :
	// sinon la détection d'action utilisateur ne tourne plus jamais en lecture
	// et une pause faite dans VLC est annulée au lieu de partir au serveur.
	b = newVecBuilder(t, vecSetup{
		name: "14-action-utilisateur-sous-bruit", file: "ep1.mkv", durationSec: 7200,
		positionSec: 1000, playing: true,
		description: "Lecture bruitée (position VLC oscillant de ±0,15 s) : aucune " +
			"commande n'est envoyée à VLC, et pause puis reprise puis seek faits " +
			"DANS VLC sont bien détectés et remontés en control.",
		scenario: "Préconditions du moteur au premier événement : session serveur " +
			"déjà établie, aucun état de salle connu, aucune mesure d'offset " +
			"d'horloge. Fichier ep1.mkv ouvert, durée 7200 s, taille 15 octets, " +
			"position 1000 s, lecture en cours, rate 1. Les événements `userSeek` " +
			"de ±0,15 s ne sont PAS des actions utilisateur (bien sous le seuil de " +
			"3 s) : ils reproduisent le bruit de la position rendue par VLC — dix " +
			"fois sous la zone morte de 1,5 s, donc aucune correction. Déroulé : " +
			"(1) welcome + pong d'une salle en lecture à 1000 s, puis 10 polls " +
			"bruités ; (2) l'utilisateur met VLC en pause lui-même — le control " +
			"pause doit partir ; (3) le serveur renvoie la pause en écho " +
			"(setBy = u1) et le hold tombe ; (4) l'utilisateur relance la lecture " +
			"dans VLC — control play, écho du serveur ; (5) 8 polls bruités, puis " +
			"l'utilisateur saute de 300 s dans la barre de VLC — control seek.",
	})
	b.welcome(b.h.playing(1000))
	b.noise(10)
	// L'utilisateur appuie sur Espace dans VLC, en pleine lecture bruitée.
	b.userEvent("userPause", 0)
	b.run(3)
	echoPause := b.h.paused(b.lastControlPosition())
	echoPause.SetBy = "u1"
	b.event(protocol.TypeRoomState, echoPause)
	b.run(3)
	// … puis il relance la lecture, toujours dans VLC.
	b.userEvent("userPlay", 0)
	b.run(3)
	echoPlay := b.h.playing(b.lastControlPosition())
	echoPlay.SetBy = "u1"
	b.event(protocol.TypeRoomState, echoPlay)
	b.noise(8)
	// … et finit par un saut à la souris dans la barre de VLC.
	b.userEvent("userSeek", round3(b.h.fake.Position())+300)
	b.run(3)
	b.check()
}

// lastControlPosition rend la position du dernier `control` que le moteur a
// envoyé dans ce scénario : de quoi fabriquer un écho serveur fidèle.
func (b *vecBuilder) lastControlPosition() float64 {
	for i := len(b.vec.Trace) - 1; i >= 0; i-- {
		for j := len(b.vec.Trace[i].ToServer) - 1; j >= 0; j-- {
			msg := b.vec.Trace[i].ToServer[j]
			if msg.Type != protocol.TypeControl {
				continue
			}
			var c protocol.Control
			if err := json.Unmarshal(msg.Data, &c); err != nil {
				b.t.Fatalf("control illisible dans la trace: %v", err)
			}
			return c.PositionSec
		}
	}
	b.t.Fatal("aucun control dans la trace : le scénario n'a pas produit de reprise")
	return 0
}

// TestVectorsGoldenComplets vérifie la forme des fichiers committés.
func TestVectorsGoldenComplets(t *testing.T) {
	files, err := filepath.Glob(filepath.Join(vectorsDir(), "*.json"))
	if err != nil {
		t.Fatal(err)
	}
	if len(files) < 14 {
		t.Fatalf("%d vecteurs golden, attendu au moins 14", len(files))
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
