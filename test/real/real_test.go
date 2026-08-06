// Package real_test contient le test d'intégration avec de VRAIES instances de
// VLC : deux moteurs complets, chacun lançant son propre VLC (interface HTTP
// locale) sur son propre fichier, face à un vrai serveur vibesync.
//
// Il est volontairement opt-in : sans VIBESYNC_REAL=1 il se contente d'un skip,
// pour ne jamais démarrer VLC dans une exécution ordinaire de `go test ./...`.
//
//	VIBESYNC_REAL=1 go test ./test/real/ -v -timeout 10m
//
// Prérequis : VLC installé (chemin standard Windows/macOS, ou variable
// d'environnement VIBESYNC_VLC pointant sur l'exécutable). Deux fenêtres VLC
// s'ouvrent pendant le test et sont refermées à la fin — le test échoue s'il
// reste un process orphelin.
package real_test

import (
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"log/slog"
	"math"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/thibsix/vibesync/internal/client"
	"github.com/thibsix/vibesync/internal/server"
	"github.com/thibsix/vibesync/internal/vlc"
)

const (
	// envReal garde le test : il ne s'exécute que sur demande explicite.
	envReal = "VIBESYNC_REAL"

	// Budgets larges : VLC réel met plusieurs secondes à démarrer, et une
	// commande HTTP locale traverse une vraie pile réseau.
	startTimeout  = 60 * time.Second
	stepTimeout   = 30 * time.Second
	vlcReadyGrace = 45 * time.Second
	pollEvery     = 50 * time.Millisecond

	// driftMax : tolérance de synchronisation en conditions réelles. Les deux
	// positions sont lues dans deux instantanés rafraîchis toutes les 200 ms,
	// ce qui ajoute à lui seul jusqu'à ~0,2 s d'incertitude de mesure.
	driftMax = 0.7
	// holdDuration : durée pendant laquelle la synchronisation doit tenir.
	holdDuration = 3 * time.Second

	// Média de test : 10 minutes de silence.
	mediaSeconds = 600
	seekTarget   = 300.0
)

// --- Média : WAV silencieux écrit à la main ---

// writeSilentWAV écrit un WAV PCM 8 kHz / 8 bits / mono de durationSec
// secondes (~8 ko par seconde, soit ~4,8 Mo pour 10 minutes).
//
// En PCM 8 bits les échantillons sont **non signés** : le silence est la valeur
// médiane 128 (0 serait une composante continue en butée, inaudible elle aussi
// mais moins correcte). VLC lit ce fichier nativement et le seek y fonctionne,
// ce qui est tout ce qu'on lui demande.
func writeSilentWAV(path string, durationSec int) error {
	const (
		sampleRate    = 8000
		channels      = 1
		bitsPerSample = 8
	)
	blockAlign := channels * bitsPerSample / 8
	byteRate := sampleRate * blockAlign
	dataSize := byteRate * durationSec

	f, err := os.Create(path)
	if err != nil {
		return fmt.Errorf("création de %s: %w", path, err)
	}
	defer func() { _ = f.Close() }()

	header := make([]byte, 0, 44)
	u32 := func(v uint32) []byte {
		b := make([]byte, 4)
		binary.LittleEndian.PutUint32(b, v)
		return b
	}
	u16 := func(v uint16) []byte {
		b := make([]byte, 2)
		binary.LittleEndian.PutUint16(b, v)
		return b
	}
	header = append(header, "RIFF"...)
	header = append(header, u32(uint32(36+dataSize))...) // taille du chunk RIFF
	header = append(header, "WAVE"...)
	header = append(header, "fmt "...)
	header = append(header, u32(16)...)                 // taille du sous-chunk fmt
	header = append(header, u16(1)...)                  // format PCM
	header = append(header, u16(channels)...)           // canaux
	header = append(header, u32(sampleRate)...)         // fréquence d'échantillonnage
	header = append(header, u32(uint32(byteRate))...)   // octets par seconde
	header = append(header, u16(uint16(blockAlign))...) // alignement de bloc
	header = append(header, u16(bitsPerSample)...)      // bits par échantillon
	header = append(header, "data"...)
	header = append(header, u32(uint32(dataSize))...)
	if _, err := f.Write(header); err != nil {
		return fmt.Errorf("écriture de l'en-tête WAV: %w", err)
	}

	// Corps : silence, écrit par blocs pour ne pas allouer 4,8 Mo d'un coup.
	const chunk = 64 << 10
	buf := make([]byte, chunk)
	for i := range buf {
		buf[i] = 128
	}
	for written := 0; written < dataSize; {
		n := min(chunk, dataSize-written)
		if _, err := f.Write(buf[:n]); err != nil {
			return fmt.Errorf("écriture des données WAV: %w", err)
		}
		written += n
	}
	return f.Sync()
}

// --- Harnais ---

type peer struct {
	name string
	eng  *client.Engine
	path string

	mu     sync.Mutex
	procs  []*vlc.Process
	toasts []string
}

func (p *peer) snap() client.Snapshot { return p.eng.Snapshot() }

// pos est la dernière position observée de VLC (secondes).
func (p *peer) pos() float64 { return p.snap().VLC.PositionSec }

// state est l'état réel de VLC vu par le moteur.
func (p *peer) state() string { return p.snap().VLC.State }

func (p *peer) playing() bool { return p.state() == string(vlc.StatePlaying) }
func (p *peer) paused() bool  { return p.state() == string(vlc.StatePaused) }

func (p *peer) addProc(proc *vlc.Process) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.procs = append(p.procs, proc)
}

func (p *peer) processes() []*vlc.Process {
	p.mu.Lock()
	defer p.mu.Unlock()
	out := make([]*vlc.Process, len(p.procs))
	copy(out, p.procs)
	return out
}

func (p *peer) collect(events <-chan client.Event) {
	for ev := range events {
		if ev.Kind != client.EventToast || ev.Toast == nil {
			continue
		}
		p.mu.Lock()
		p.toasts = append(p.toasts, fmt.Sprintf("[%s] %s", ev.Toast.Level, ev.Toast.Text))
		p.mu.Unlock()
	}
}

func (p *peer) allToasts() []string {
	p.mu.Lock()
	defer p.mu.Unlock()
	out := make([]string, len(p.toasts))
	copy(out, p.toasts)
	return out
}

type rig struct {
	t     *testing.T
	url   string
	room  string
	peers []*peer
}

func newRig(t *testing.T) *rig {
	t.Helper()
	srv := server.New(server.Config{},
		server.WithLogger(slog.New(slog.NewTextHandler(io.Discard, nil))))
	ts := httptest.NewServer(srv.Handler())
	t.Cleanup(ts.Close)
	return &rig{
		t:    t,
		url:  "ws" + strings.TrimPrefix(ts.URL, "http") + "/ws",
		room: "salon-reel",
	}
}

// newPeer lance un moteur en configuration de production : vrai driver VLC,
// vrai lancement du process, vraie interface HTTP locale. Seuls le logger, les
// délais et l'enregistrement du handle de process sont adaptés au test.
func (r *rig) newPeer(name, mediaPath string) *peer {
	t := r.t
	t.Helper()

	p := &peer{name: name, path: mediaPath}
	eng := client.New(client.Config{
		Logger: slog.New(slog.NewTextHandler(io.Discard, nil)),
		// Même chose que le lanceur par défaut, mais on garde le handle du
		// process pour garantir qu'aucun VLC ne survit au test.
		Launcher: func(ctx context.Context, path string) (vlc.Controller, error) {
			proc, err := vlc.Launch(ctx, vlc.LaunchOptions{
				FilePath:     path,
				ReadyTimeout: vlcReadyGrace,
			})
			if err != nil {
				return nil, err
			}
			p.addProc(proc)
			t.Logf("[%s] VLC lancé (interface HTTP sur le port %d)", name, proc.Port())
			return proc, nil
		},
	})
	p.eng = eng

	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(func() {
		cancel()
		// Close ferme la session serveur ET tue le VLC lancé par le moteur.
		if err := eng.Close(); err != nil {
			t.Errorf("[%s] arrêt du moteur : %v", name, err)
		}
		t.Logf("[%s] moteur arrêté", name)
	})
	go eng.Run(ctx)

	events, unsub := eng.Subscribe()
	t.Cleanup(unsub)
	go p.collect(events)

	r.peers = append(r.peers, p)

	eng.Connect(client.ConnectRequest{URL: r.url, Name: name, Room: r.room})
	r.waitFor(startTimeout, name+" connecté au serveur", func() bool {
		return p.snap().Phase == client.PhaseConnected
	})
	t.Logf("[%s] connecté à la salle %q", name, r.room)

	if err := eng.OpenFile(ctx, mediaPath); err != nil {
		t.Fatalf("[%s] ouverture de %s dans VLC: %v", name, mediaPath, err)
	}
	r.waitFor(startTimeout, name+" a déclaré son fichier (durée connue)", func() bool {
		return p.snap().VLC.DurationSec > 0
	})
	// VLC démarre la lecture tout seul à l'ouverture : le driver doit l'avoir
	// ramené en pause au début avant de déclarer le fichier (docs/protocol.md
	// §Chargement de fichier). C'est ce qui permet un départ synchronisé.
	s := p.snap()
	t.Logf("[%s] fichier ouvert : %s — durée %.0f s, état à l'ouverture : %s à %.2f s",
		name, filepath.Base(mediaPath), s.VLC.DurationSec, s.VLC.State, s.VLC.PositionSec)
	if s.VLC.State != string(vlc.StatePaused) || s.VLC.PositionSec >= vlc.StartTolerance {
		t.Errorf("[%s] le média aurait dû être arrêté au début après l'ouverture (état %s à %.2f s)",
			name, s.VLC.State, s.VLC.PositionSec)
	}
	return p
}

func (r *rig) waitFor(timeout time.Duration, desc string, cond func() bool) {
	r.t.Helper()
	deadline := time.Now().Add(timeout)
	for {
		if cond() {
			return
		}
		if time.Now().After(deadline) {
			r.t.Fatalf("délai dépassé (%s) en attendant : %s\n%s", timeout, desc, r.dump())
		}
		time.Sleep(pollEvery)
	}
}

// holds vérifie qu'une propriété reste vraie pendant toute une durée.
func (r *rig) holds(d time.Duration, desc string, cond func() bool) {
	r.t.Helper()
	deadline := time.Now().Add(d)
	for time.Now().Before(deadline) {
		if !cond() {
			r.t.Fatalf("propriété non tenue : %s\n%s", desc, r.dump())
		}
		time.Sleep(pollEvery)
	}
}

func (r *rig) dump() string {
	var b strings.Builder
	b.WriteString("--- état des pairs ---\n")
	for _, p := range r.peers {
		s := p.snap()
		fmt.Fprintf(&b, "  %-6s (%s) phase=%-10s vlc=%-7s pos=%8.2f durée=%6.0f drift=%+6.2f corr=%-6q "+
			"buffering=%v ready=%v | salle: pos=%8.2f paused=%v | latence=%d ms err=%q %q\n",
			p.name, filepath.Base(p.path), s.Phase, s.VLC.State, s.VLC.PositionSec, s.VLC.DurationSec,
			s.DriftSec, s.Correcting, s.VLC.Buffering, s.Ready, s.RoomPosition, s.Paused,
			s.LatencyMs, s.LastError, s.VLC.Error)
	}
	b.WriteString("--- toasts ---\n")
	for _, p := range r.peers {
		for _, msg := range p.allToasts() {
			fmt.Fprintf(&b, "  %s: %s\n", p.name, msg)
		}
	}
	return b.String()
}

// drift est l'écart entre les positions réellement lues dans les deux VLC.
func drift(a, b *peer) float64 { return math.Abs(a.pos() - b.pos()) }

func (r *rig) logPositions(step string) {
	r.t.Helper()
	var parts []string
	for _, p := range r.peers {
		parts = append(parts, fmt.Sprintf("%s=%.2fs (%s)", p.name, p.pos(), p.state()))
	}
	r.t.Logf("→ %s : %s", step, strings.Join(parts, " | "))
}

// assertNoOrphanVLC vérifie qu'aucun VLC lancé par le test ne survit à l'arrêt
// des moteurs : on interroge son interface HTTP, qui ne doit plus répondre.
// Doit être enregistré AVANT la création des pairs pour s'exécuter APRÈS leurs
// cleanups (t.Cleanup est LIFO) : sinon on constaterait simplement que VLC
// tourne encore, ce qui est normal avant l'arrêt des moteurs.
func assertNoOrphanVLC(t *testing.T, peers ...*peer) {
	t.Helper()
	for _, p := range peers {
		for _, proc := range p.processes() {
			ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
			_, err := proc.Status(ctx)
			cancel()
			if err != nil {
				t.Logf("[%s] VLC (port %d) bien arrêté", p.name, proc.Port())
				continue
			}
			// Orphelin : on le signale ET on tente de le tuer, pour ne pas
			// laisser traîner un vlc.exe sur la machine de l'orchestrateur.
			t.Errorf("[%s] VLC répond encore sur le port %d après l'arrêt du moteur : "+
				"process orphelin", p.name, proc.Port())
			if err := proc.Close(); err != nil {
				t.Errorf("[%s] impossible de tuer le VLC orphelin (port %d) : %v — "+
					"tuez-le à la main (Gestionnaire des tâches → vlc.exe)", p.name, proc.Port(), err)
			}
		}
	}
}

// --- Test ---

func TestReelDoubleVLC(t *testing.T) {
	if os.Getenv(envReal) != "1" {
		t.Skipf("test réel désactivé ; pour le lancer : %s=1 go test ./test/real/ -v -timeout 10m", envReal)
	}

	binary, err := vlc.Locate()
	if err != nil {
		t.Fatalf("VLC introuvable : %v\nInstallez VLC ou renseignez VIBESYNC_VLC.", err)
	}
	t.Logf("VLC utilisé : %s", binary)

	// 1. Média : deux copies d'un WAV silencieux de 10 minutes.
	dir := t.TempDir()
	pathA := filepath.Join(dir, "a.wav")
	pathB := filepath.Join(dir, "b.wav")
	for _, p := range []string{pathA, pathB} {
		if err := writeSilentWAV(p, mediaSeconds); err != nil {
			t.Fatalf("génération du média : %v", err)
		}
	}
	if fi, err := os.Stat(pathA); err == nil {
		t.Logf("média généré : %s et %s (%.1f Mo chacun, %d s)",
			filepath.Base(pathA), filepath.Base(pathB), float64(fi.Size())/(1<<20), mediaSeconds)
	}

	// 2. Serveur + deux moteurs, chacun avec son VLC.
	r := newRig(t)
	// Enregistré avant les pairs : t.Cleanup étant LIFO, cette vérification
	// s'exécutera en dernier, une fois les deux moteurs arrêtés.
	t.Cleanup(func() { assertNoOrphanVLC(t, r.peers...) })

	t.Log("=== étape 1/7 : démarrage des deux clients (VLC réel) ===")
	a := r.newPeer("alice", pathA)
	b := r.newPeer("bob", pathB)

	r.waitFor(stepTimeout, "les deux membres se voient", func() bool {
		return len(a.snap().Users) == 2 && len(b.snap().Users) == 2
	})
	r.logPositions("les deux clients sont dans la salle")

	// 3. Prêts.
	t.Log("=== étape 2/7 : les deux participants se déclarent prêts ===")
	a.eng.SetReady(true)
	b.eng.SetReady(true)
	r.waitFor(stepTimeout, "tout le monde est prêt côté serveur", func() bool {
		for _, p := range []*peer{a, b} {
			for _, u := range p.snap().Users {
				if !u.Ready {
					return false
				}
			}
			if len(p.snap().Users) != 2 {
				return false
			}
		}
		return true
	})
	t.Log("→ ready-gate levé")

	// 4. Alice lance la lecture.
	t.Log("=== étape 3/7 : alice lance la lecture ===")
	a.eng.Play()
	r.waitFor(stepTimeout, "les deux VLC jouent", func() bool { return a.playing() && b.playing() })
	r.waitFor(stepTimeout, "la lecture progresse des deux côtés", func() bool {
		return a.pos() > 1 && b.pos() > 1
	})
	r.waitFor(stepTimeout, fmt.Sprintf("drift < %.1f s", driftMax), func() bool {
		return drift(a, b) < driftMax
	})
	r.logPositions("lecture synchronisée")

	t.Logf("=== étape 4/7 : la synchronisation doit tenir %s ===", holdDuration)
	r.holds(holdDuration, "synchronisation stable en lecture", func() bool {
		return a.playing() && b.playing() && drift(a, b) < driftMax
	})
	r.logPositions(fmt.Sprintf("stable (drift final %.2f s)", drift(a, b)))

	// 5. Pause depuis alice.
	t.Log("=== étape 5/7 : alice met en pause, bob doit suivre ===")
	a.eng.Pause()
	r.waitFor(stepTimeout, "les deux VLC sont en pause", func() bool { return a.paused() && b.paused() })
	r.holds(time.Second, "la pause tient des deux côtés", func() bool { return a.paused() && b.paused() })
	r.waitFor(stepTimeout, fmt.Sprintf("positions alignées en pause (< %.1f s)", driftMax), func() bool {
		return drift(a, b) < driftMax
	})
	r.logPositions("en pause")

	// 6. Seek depuis alice.
	t.Logf("=== étape 6/7 : alice saute à %.0f s, bob doit converger ===", seekTarget)
	a.eng.Seek(seekTarget)
	r.waitFor(stepTimeout, "les deux VLC ont sauté près de la cible", func() bool {
		return math.Abs(a.pos()-seekTarget) < 3 && math.Abs(b.pos()-seekTarget) < 3
	})
	r.waitFor(stepTimeout, fmt.Sprintf("convergence après seek < %.1f s", driftMax), func() bool {
		return drift(a, b) < driftMax
	})
	r.logPositions("après seek")

	// 7. Bob relance : le sens inverse doit marcher aussi.
	t.Log("=== étape 7/7 : bob relance la lecture, alice doit suivre ===")
	b.eng.Play()
	r.waitFor(stepTimeout, "les deux VLC rejouent", func() bool { return a.playing() && b.playing() })
	r.waitFor(stepTimeout, "la lecture repart de la nouvelle position", func() bool {
		return a.pos() > seekTarget && b.pos() > seekTarget
	})
	r.holds(holdDuration, "synchronisation stable après reprise", func() bool {
		return a.playing() && b.playing() && drift(a, b) < driftMax
	})
	r.logPositions("lecture synchronisée après seek")

	// Bilan des notifications reçues (utile à la lecture manuelle du log).
	for _, p := range []*peer{a, b} {
		if toasts := p.allToasts(); len(toasts) > 0 {
			t.Logf("[%s] toasts reçus : %s", p.name, strings.Join(toasts, " · "))
		}
	}
	// Les t.Cleanup enchaînent : arrêt des moteurs (qui tuent VLC), puis
	// vérification qu'aucun process n'a survécu.
	t.Log("=== scénario terminé : arrêt des moteurs et des VLC ===")
}
