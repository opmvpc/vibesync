package client

import (
	"math"
	"testing"

	"github.com/thibsix/vibesync/internal/protocol"
	"github.com/thibsix/vibesync/internal/vlc"
)

// docs/protocol.md §Départ et reprise de lecture : au passage pause → lecture,
// le moteur cale d'abord la position (seek si l'écart ≥ 0,3 s) puis lance VLC.
// Sans ça, un écart de départ de 0,5 s coûterait ~10 s de nudge à 5 %/s.

func TestDepartDeLectureCaleLaPositionAvantDeLancer(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.paused(100))
	// Le média de ce client est 0,55 s plus loin : sous le seuil de correction
	// en pause (0,6 s), donc personne ne l'a recalé.
	h.fake.SeekTo(100.55)
	h.ticks(4)
	if h.fake.Seeks() != 0 {
		t.Fatalf("%d seek(s) en pause pour 0,5 s d'écart (seuil 0,6 s)", h.fake.Seeks())
	}
	if h.fake.State() != "paused" {
		t.Fatalf("état = %q avant le départ", h.fake.State())
	}

	// La salle démarre la lecture.
	h.server(protocol.TypeRoomState, h.playing(100))
	h.ticks(1)

	if h.fake.Seeks() == 0 {
		t.Fatal("aucun seek de calage : le moteur compte sur le nudge au départ")
	}
	if h.fake.State() != "playing" {
		t.Fatalf("état = %q, la lecture aurait dû être lancée", h.fake.State())
	}
	// L'ordre compte : on cale la position pendant que VLC est encore en pause,
	// puis on lance la lecture (cf. golden test/vectors/08-hold-sans-echo.json).
	if got := h.fake.Position(); math.Abs(got-100) > 0.05 {
		t.Fatalf("position après calage = %v, attendu la seconde entière visée", got)
	}
	// Le drift exposé est celui calculé avant l'action ; au poll suivant il
	// doit refléter le calage.
	h.ticks(1)
	if d := math.Abs(h.e.Snapshot().DriftSec); d > StartAlignSec {
		t.Fatalf("drift après calage = %v, attendu < %v", d, StartAlignSec)
	}
}

func TestDepartSansEcartNeSeekPas(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.paused(100))
	h.fake.SeekTo(100)
	h.ticks(4)
	h.fake.SeekTo(100.1) // écart sous le seuil de calage
	seeks := h.fake.Seeks()

	h.server(protocol.TypeRoomState, h.playing(100.1))
	h.ticks(1)
	if h.fake.Seeks() != seeks {
		t.Fatalf("seek inutile au départ (écart < %v s)", StartAlignSec)
	}
	if h.fake.State() != "playing" {
		t.Fatalf("état = %q, la lecture aurait dû être lancée", h.fake.State())
	}
}

func TestDepartConvergeSansAttendreLeNudge(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.paused(300))
	h.fake.SeekTo(300.55)
	h.ticks(3)

	h.server(protocol.TypeRoomState, h.playing(300))
	// Convergence dans la zone morte en quelques ticks, pas en dizaines de
	// secondes : le calage a fait l'essentiel du travail.
	converged := false
	for range 15 { // 3 s simulées
		h.tick(PollInterval)
		if math.Abs(h.e.Snapshot().DriftSec) <= DeadZoneSec {
			converged = true
			break
		}
	}
	if !converged {
		t.Fatalf("pas de convergence rapide au départ, drift = %v", h.e.Snapshot().DriftSec)
	}
}

// Le fichier n'est déclaré au serveur qu'une fois le média arrêté au début :
// c'est le driver qui garantit l'état, le moteur ne doit pas l'annoncer avant.
func TestFichierDeclareApresPreparation(t *testing.T) {
	h := newHarness(t)
	h.connect(h.paused(0))
	h.conn.take()
	h.openFile("ep1.mkv", 1200)

	// Le lanceur du harnais applique vlc.Prepare : à ce stade VLC est en pause
	// au début, comme après un vrai lancement.
	if got := h.fake.State(); got != "paused" {
		t.Fatalf("état après ouverture = %q, attendu paused", got)
	}
	if got := h.fake.Position(); got >= vlc.StartTolerance {
		t.Fatalf("position après ouverture = %v", got)
	}
	h.ticks(2)
	var declared *protocol.SetFile
	for _, env := range h.conn.take() {
		if env.Type != protocol.TypeSetFile {
			continue
		}
		sf, err := protocol.DecodeData[protocol.SetFile](env)
		if err != nil {
			t.Fatalf("setFile illisible: %v", err)
		}
		declared = &sf
	}
	if declared == nil || declared.DurationSec <= 0 {
		t.Fatalf("setFile non envoyé avec la durée: %+v", declared)
	}
}
