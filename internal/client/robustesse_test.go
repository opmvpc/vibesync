package client

import (
	"fmt"
	"math"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/opmvpc/vibesync/internal/protocol"
)

// Robustesse des déconnexions (VS-024) : docs/protocol.md §Erreurs et
// robustesse — file d'attente hors ligne, reconnexion sans écrasement, salle
// vierge.

// chatTexts extrait, dans l'ordre, les textes des chats envoyés au serveur.
func chatTexts(envs []protocol.Envelope) []string {
	var out []string
	for _, env := range envs {
		if env.Type != protocol.TypeChat {
			continue
		}
		if c, err := protocol.DecodeData[protocol.Chat](env); err == nil {
			out = append(out, c.Text)
		}
	}
	return out
}

// --- File d'attente hors ligne ---

func TestChatHorsLigneMisEnFileEtLivreDansLOrdre(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.paused(0))
	h.conn.take()

	h.sessionEnd()
	for _, msg := range []string{"un", "deux", "trois"} {
		h.e.Chat(msg)
	}
	if n := countType(h.conn.take(), protocol.TypeChat); n != 0 {
		t.Fatalf("%d chat(s) envoyé(s) alors que la session est tombée", n)
	}
	pending := h.e.Snapshot().PendingChats
	if !slices.Equal(pending, []string{"un", "deux", "trois"}) {
		t.Fatalf("file exposée à l'UI = %v", pending)
	}

	// Reconnexion : livraison dans l'ordre de composition.
	h.attach()
	h.welcome(h.paused(0))
	if got := chatTexts(h.conn.take()); !slices.Equal(got, []string{"un", "deux", "trois"}) {
		t.Fatalf("chats livrés = %v, attendu [un deux trois]", got)
	}
	if pending := h.e.Snapshot().PendingChats; len(pending) != 0 {
		t.Fatalf("file non vidée après livraison: %v", pending)
	}

	// Et la file ne se rejoue pas au welcome suivant.
	h.welcome(h.paused(0))
	if got := chatTexts(h.conn.take()); len(got) != 0 {
		t.Fatalf("chats rejoués une seconde fois: %v", got)
	}
}

// La file appartient à la salle pour laquelle elle a été composée : elle ne
// survit qu'aux reconnexions automatiques vers cette même salle.
func TestFileDeChatLieeALaSalle(t *testing.T) {
	t.Run("changement de salle : rien n'est envoyé ailleurs", func(t *testing.T) {
		h := newHarness(t)
		h.connect(h.paused(0)) // salle « soirée »
		h.sessionEnd()
		h.e.Chat("message pour la soirée")
		if len(h.e.Snapshot().PendingChats) != 1 {
			t.Fatal("message non mis en file")
		}

		h.e.Connect(ConnectRequest{URL: "ws://test/ws", Name: "thib", Room: "cave"})
		if got := h.e.Snapshot().PendingChats; len(got) != 0 {
			t.Fatalf("la file a suivi le changement de salle: %v", got)
		}
		h.attach()
		h.conn.take()
		h.server(protocol.TypeWelcome, protocol.Welcome{
			SelfID: "u1", Room: "cave", State: h.paused(0),
			Users: []protocol.User{{ID: "u1", Name: "thib"}},
		})
		if got := chatTexts(h.conn.take()); len(got) != 0 {
			t.Fatalf("message de « soirée » livré dans « cave »: %v", got)
		}
	})

	t.Run("déconnexion volontaire : la file est jetée", func(t *testing.T) {
		h := newHarness(t)
		h.connect(h.paused(0))
		h.sessionEnd()
		h.e.Chat("tapé pendant la coupure")
		h.e.Disconnect() // l'utilisateur quitte de lui-même
		if got := h.e.Snapshot().PendingChats; len(got) != 0 {
			t.Fatalf("la file survit à un départ volontaire: %v", got)
		}
		h.attach()
		h.conn.take()
		h.welcome(h.paused(0))
		if got := chatTexts(h.conn.take()); len(got) != 0 {
			t.Fatalf("message livré après un départ volontaire: %v", got)
		}
	})
}

func TestFileDeChatBornee(t *testing.T) {
	h := newHarness(t)
	h.connect(h.paused(0))
	h.sessionEnd()

	for i := range ChatQueueMax + 5 {
		h.e.Chat(fmt.Sprintf("m%02d", i))
	}
	pending := h.e.Snapshot().PendingChats
	if len(pending) != ChatQueueMax {
		t.Fatalf("%d messages en file, attendu %d", len(pending), ChatQueueMax)
	}
	// Les plus anciens sautent, les plus récents restent, dans l'ordre.
	if pending[0] != "m05" || pending[len(pending)-1] != fmt.Sprintf("m%02d", ChatQueueMax+4) {
		t.Fatalf("mauvais bout de file conservé: %v", pending)
	}
	for i, msg := range pending {
		if want := fmt.Sprintf("m%02d", i+5); msg != want {
			t.Fatalf("file désordonnée en %d: %q, attendu %q", i, msg, want)
		}
	}
}

// Seul le chat est rejoué : un control composé hors ligne écraserait la salle
// avec une action périmée, ready et fichier sont re-déclarés (état courant).
func TestSeulLeChatEstRejoue(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.paused(0))
	h.ticks(2)
	h.conn.take()

	h.sessionEnd()
	h.e.Seek(600)
	h.e.Play()
	h.e.SetReady(true)
	h.e.Chat("je reviens")
	h.conn.take()

	h.attach()
	h.welcome(h.paused(0))
	envs := h.conn.take()
	if got := controls(envs); len(got) != 0 {
		t.Fatalf("control périmé rejoué après la reconnexion: %+v", got)
	}
	if got := chatTexts(envs); !slices.Equal(got, []string{"je reviens"}) {
		t.Fatalf("chats livrés = %v", got)
	}
	if countType(envs, protocol.TypeSetFile) == 0 || countType(envs, protocol.TypeSetReady) == 0 {
		t.Fatalf("fichier et ready doivent être re-déclarés: %+v", envs)
	}
}

// Le ready de l'utilisateur survit à la reconnexion : le serveur nous recrée
// « pas prêt », c'est notre état local qui est re-déclaré.
func TestReadyRedeclareApresChaqueWelcome(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.paused(0))
	h.e.SetReady(true)
	h.conn.take()

	for i := range 2 {
		// Le welcome décrit un membre neuf, donc « pas prêt » côté serveur.
		h.server(protocol.TypeWelcome, protocol.Welcome{
			SelfID: "u1", Room: "soirée", State: h.paused(0),
			Users: []protocol.User{{ID: "u1", Name: "thib", Ready: false}},
		})
		var ready *protocol.SetReady
		for _, env := range h.conn.take() {
			if env.Type != protocol.TypeSetReady {
				continue
			}
			if sr, err := protocol.DecodeData[protocol.SetReady](env); err == nil {
				ready = &sr
			}
		}
		if ready == nil || !ready.Ready {
			t.Fatalf("welcome %d : setReady{true} attendu, obtenu %+v", i+1, ready)
		}
		if !h.e.Snapshot().Ready {
			t.Fatalf("welcome %d : le ready local a été effacé par le welcome", i+1)
		}
	}
}

// --- Reconnexion sans écrasement ---

// L'alignement qui suit un welcome peut demander un gros seek local ; il ne doit
// JAMAIS repartir vers le serveur comme une action de l'utilisateur.
func TestAlignementPostWelcomeNEmetAucunControl(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 3600)
	h.connect(h.playing(300))
	h.fake.SeekTo(300)
	h.fake.Play()
	h.ticks(5)

	// Coupure : VLC continue tout seul pendant que la salle avance de son côté.
	h.sessionEnd()
	h.clock.Advance(45 * time.Second)
	h.ticks(10)
	h.conn.take()

	// Retour : la salle est 1200 s plus loin que notre lecteur.
	h.attach()
	h.welcome(h.playing(1500))
	h.pong(0)
	seeks := h.fake.Seeks()
	h.ticks(10)

	if h.fake.Seeks() == seeks {
		t.Fatal("aucun seek d'alignement après le welcome")
	}
	if got := h.fake.Position(); math.Abs(got-1500) > 3 {
		t.Fatalf("VLC non aligné sur la salle: %v", got)
	}
	if got := controls(h.conn.take()); len(got) != 0 {
		t.Fatalf("control émis pendant l'alignement (la salle aurait été écrasée): %+v", got)
	}
}

// --- Salle vierge ---

// virginState est un welcome de salle jamais pilotée : setBy vide, position 0.
func virginState() protocol.RoomState {
	return protocol.RoomState{Paused: true, PositionSec: 0, Rate: 1, RefServerMs: 1}
}

// seanceEnCours amène le moteur à connaître la séance d'une salle : c'est la
// condition sans laquelle aucune reprise n'est possible.
func seanceEnCours(t *testing.T, h *harness, positionSec float64) {
	t.Helper()
	h.openFile("ep1.mkv", 7200)
	h.connect(h.paused(positionSec))
	h.fake.SeekTo(positionSec)
	h.ticks(4)
	h.conn.take()
}

func TestRepriseSalleVierge(t *testing.T) {
	h := newHarness(t)
	// Séance connue à 01:02:05 dans « soirée », puis le serveur disparaît.
	seanceEnCours(t, h, 3725)
	h.sessionEnd()
	h.clock.Advance(3 * time.Second)
	h.ticks(5)
	h.conn.take()

	events, unsub := h.e.Subscribe()
	defer unsub()
	drainToasts(events)

	// Le serveur revient tout neuf.
	h.attach()
	h.welcome(virginState())

	got := controls(h.conn.take())
	if len(got) != 1 || got[0].Action != protocol.ActionSeek {
		t.Fatalf("UNE reprise control seek attendue, obtenu %+v", got)
	}
	if math.Abs(got[0].PositionSec-3725) > 1 {
		t.Fatalf("reprise à %v, attendu ≈3725", got[0].PositionSec)
	}
	var repris bool
	for _, toast := range drainToasts(events) {
		if strings.HasPrefix(toast.Text, "Reprise à ") {
			if toast.Text != "Reprise à 01:02:05" {
				t.Fatalf("toast de reprise = %q", toast.Text)
			}
			repris = true
		}
	}
	if !repris {
		t.Fatal("aucun toast de reprise")
	}

	// La reprise est un control : le hold protège la position pendant que le
	// serveur répond. Sans lui, l'alignement ramènerait VLC à 0.
	h.pong(0)
	h.ticks(5)
	if pos := h.fake.Position(); pos < 3700 {
		t.Fatalf("VLC ramené à %v : l'état vierge a écrasé la séance", pos)
	}
}

// La reprise vise la dernière position de SALLE connue, pas la position brute
// de VLC : l'utilisateur a pu avancer à la main, ou ouvrir autre chose.
func TestRepriseViseLaPositionDeSalleConnue(t *testing.T) {
	h := newHarness(t)
	seanceEnCours(t, h, 1200)
	h.sessionEnd()

	// Hors ligne, l'utilisateur part se balader dans son fichier.
	h.fake.SeekTo(4000)
	h.fake.Play()
	h.ticks(5)
	h.conn.take()

	h.attach()
	h.welcome(virginState())
	got := controls(h.conn.take())
	if len(got) != 1 {
		t.Fatalf("UNE reprise attendue, obtenu %+v", got)
	}
	if math.Abs(got[0].PositionSec-1200) > 1 {
		t.Fatalf("reprise à %v : c'est la position de VLC, pas celle de la séance",
			got[0].PositionSec)
	}
}

// Une seule reprise par connexion : le roomState suivant (même vierge en
// apparence) ne la relance pas.
func TestRepriseSalleViergeUneSeuleFoisParConnexion(t *testing.T) {
	h := newHarness(t)
	seanceEnCours(t, h, 900)
	h.sessionEnd()
	h.attach()

	h.welcome(virginState())
	if n := len(controls(h.conn.take())); n != 1 {
		t.Fatalf("%d control(s) à la reprise, attendu 1", n)
	}
	h.server(protocol.TypeRoomState, virginState())
	h.ticks(3)
	if got := controls(h.conn.take()); len(got) != 0 {
		t.Fatalf("reprise rejouée hors welcome: %+v", got)
	}
}

func TestPasDeRepriseSalleVierge(t *testing.T) {
	// Chaque cas part d'un moteur qui a connu une séance à 900 s dans « soirée »,
	// sauf mention contraire.
	cases := []struct {
		nom     string
		prepare func(t *testing.T, h *harness)
		state   protocol.RoomState
	}{
		{
			// Le cas qui compte : rien ne justifie de piloter une salle neuve
			// juste parce que VLC est loin dans un fichier.
			nom: "premier join, VLC à 40 minutes",
			prepare: func(t *testing.T, h *harness) {
				h.openFile("ep1.mkv", 7200)
				h.fake.SeekTo(2400)
				h.fake.Play()
				h.ticks(4)
				h.attach()
			},
			state: virginState(),
		},
		{
			nom:     "séance connue trop courte",
			prepare: func(t *testing.T, h *harness) { seanceEnCours(t, h, 3); h.sessionEnd(); h.attach() },
			state:   virginState(),
		},
		{
			nom:     "salle déjà pilotée par un autre",
			prepare: func(t *testing.T, h *harness) { seanceEnCours(t, h, 900); h.sessionEnd(); h.attach() },
			state: protocol.RoomState{
				Paused: true, PositionSec: 0, Rate: 1, RefServerMs: 1, SetBy: "u2"},
		},
		{
			nom:     "salle avec une position",
			prepare: func(t *testing.T, h *harness) { seanceEnCours(t, h, 900); h.sessionEnd(); h.attach() },
			state: protocol.RoomState{
				Paused: true, PositionSec: 800, Rate: 1, RefServerMs: 1},
		},
		{
			// Séance suivie ailleurs : elle n'a rien à faire dans cette salle-ci.
			nom: "séance connue dans une autre salle",
			prepare: func(t *testing.T, h *harness) {
				seanceEnCours(t, h, 900)
				h.sessionEnd()
				h.e.Connect(ConnectRequest{URL: "ws://test/ws", Name: "thib", Room: "cave"})
				h.attach()
			},
			state: virginState(),
		},
	}
	for _, tc := range cases {
		t.Run(tc.nom, func(t *testing.T) {
			h := newHarness(t)
			tc.prepare(t, h)
			h.conn.take()

			h.welcome(tc.state)
			if got := controls(h.conn.take()); len(got) != 0 {
				t.Fatalf("reprise émise à tort: %+v", got)
			}
		})
	}
}
