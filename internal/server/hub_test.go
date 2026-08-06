package server

import (
	"errors"
	"math"
	"sync"
	"testing"
	"time"

	"github.com/opmvpc/vibesync/internal/protocol"
)

// Par défaut les hubs de test n'ont pas de fenêtre de reprise : la destruction
// est immédiate, comme avant VS-021. Les tests de linger passent leur propre
// horloge et leur propre fenêtre (newTestHubLinger).
func newTestHub() *Hub { return newTestHubWith(defaultMaxRooms, defaultMaxRoomSize) }

func newTestHubWith(maxRooms, maxRoomSize int) *Hub {
	return newHub(newFakeClock(), testLogger(), maxRooms, maxRoomSize, RoomLingerDisabled, testBuildInfo)
}

// newTestHubLinger construit un hub avec fenêtre de reprise et rend son horloge.
func newTestHubLinger(linger time.Duration, maxRooms int) (*Hub, *fakeClock) {
	clk := newFakeClock()
	return newHub(clk, testLogger(), maxRooms, defaultMaxRoomSize, linger, testBuildInfo), clk
}

func TestHubCreeEtDetruitLesSallesALaVolee(t *testing.T) {
	hub := newTestHub()
	if hub.roomCount() != 0 {
		t.Fatal("aucune salle attendue au démarrage")
	}

	room, alice, _, err := hub.join("salon", "Alice", "", 0, &recorder{})
	if err != nil {
		t.Fatalf("join: %v", err)
	}
	if hub.roomCount() != 1 || hub.room("salon") != room {
		t.Fatal("la salle doit être créée à la volée")
	}

	_, bob, _, err := hub.join("salon", "Bob", "", 0, &recorder{})
	if err != nil {
		t.Fatalf("join Bob: %v", err)
	}
	if hub.roomCount() != 1 {
		t.Fatal("Bob doit rejoindre la salle existante")
	}

	hub.leave(room, alice)
	if hub.roomCount() != 1 {
		t.Fatal("la salle ne doit pas être détruite tant qu'elle a un membre")
	}
	hub.leave(room, bob)
	if hub.roomCount() != 0 || hub.room("salon") != nil {
		t.Fatal("la salle doit être détruite une fois vide")
	}
}

func TestHubPseudoDejaPrisNeLaissePasDeSalleFantome(t *testing.T) {
	hub := newTestHub()
	room, alice, _, err := hub.join("salon", "Alice", "", 0, &recorder{})
	if err != nil {
		t.Fatalf("join: %v", err)
	}
	if _, _, _, err := hub.join("salon", "Alice", "", 0, &recorder{}); err != errNameTaken {
		t.Fatalf("errNameTaken attendu, obtenu %v", err)
	}
	if hub.roomCount() != 1 {
		t.Fatal("la salle existante doit être conservée")
	}

	// Une salle créée pour un join qui échoue ne doit pas subsister.
	other, zoe, _, err := hub.join("autre", "Zoe", "", 0, &recorder{})
	if err != nil {
		t.Fatalf("join: %v", err)
	}
	hub.leave(other, zoe)
	if hub.room("autre") != nil {
		t.Fatal("la salle vide doit être détruite")
	}
	hub.leave(room, alice)
	if hub.roomCount() != 0 {
		t.Fatalf("plus aucune salle attendue, %d restantes", hub.roomCount())
	}
}

func TestHubSallesIsolees(t *testing.T) {
	hub := newTestHub()
	recA := &recorder{}
	recB := &recorder{}
	room1, alice, _, _ := hub.join("salon", "Alice", "", 0, recA)
	_, _, _, _ = hub.join("cave", "Bob", "", 0, recB)
	room1.handleSetReady(alice, protocol.SetReady{Ready: true})
	recB.reset()

	room1.handleControl(alice, protocol.Control{Action: protocol.ActionPlay, PositionSec: 10})
	room1.handleChat(alice, protocol.Chat{Text: "coucou"})

	if n := len(recB.all()); n != 0 {
		t.Fatalf("la salle « cave » ne doit rien recevoir de « salon » (%v)", typesOf(recB.all()))
	}
	if st := hub.room("cave").State(); !st.Paused || st.PositionSec != 0 {
		t.Fatalf("l'état de « cave » doit être intact: %+v", st)
	}
}

func TestHubPlafondDeSalles(t *testing.T) {
	hub := newTestHubWith(2, defaultMaxRoomSize)
	if _, _, _, err := hub.join("salon", "Alice", "", 0, &recorder{}); err != nil {
		t.Fatalf("join: %v", err)
	}
	if _, _, _, err := hub.join("cave", "Bob", "", 0, &recorder{}); err != nil {
		t.Fatalf("join: %v", err)
	}
	if _, _, _, err := hub.join("grenier", "Carol", "", 0, &recorder{}); !errors.Is(err, errTooManyRooms) {
		t.Fatalf("errTooManyRooms attendu, obtenu %v", err)
	}
	if hub.roomCount() != 2 {
		t.Fatalf("2 salles attendues, %d obtenues", hub.roomCount())
	}
	// Rejoindre une salle existante reste possible.
	if _, _, _, err := hub.join("salon", "Carol", "", 0, &recorder{}); err != nil {
		t.Fatalf("join dans une salle existante: %v", err)
	}
}

func TestHubPlafondDeMembresParSalle(t *testing.T) {
	hub := newTestHubWith(defaultMaxRooms, 2)
	room, alice, _, _ := hub.join("salon", "Alice", "", 0, &recorder{})
	if _, _, _, err := hub.join("salon", "Bob", "", 0, &recorder{}); err != nil {
		t.Fatalf("join Bob: %v", err)
	}
	if _, _, _, err := hub.join("salon", "Carol", "", 0, &recorder{}); !errors.Is(err, errRoomFull) {
		t.Fatalf("errRoomFull attendu, obtenu %v", err)
	}
	// Une place se libère → le refus disparaît.
	hub.leave(room, alice)
	if _, _, _, err := hub.join("salon", "Carol", "", 0, &recorder{}); err != nil {
		t.Fatalf("join après libération d'une place: %v", err)
	}
}

// Le join tient le verrou du hub : une salle détruite au même instant doit être
// soit conservée (le join a précédé), soit recréée proprement.
func TestHubJoinConcurrentAvecDestructionDeLaDerniereSession(t *testing.T) {
	for i := 0; i < 200; i++ {
		hub := newTestHub()
		room, alice, _, err := hub.join("salon", "Alice", "", 0, &recorder{})
		if err != nil {
			t.Fatalf("préparation: %v", err)
		}

		var wg sync.WaitGroup
		wg.Add(2)
		go func() { defer wg.Done(); hub.leave(room, alice) }()
		go func() {
			defer wg.Done()
			if _, _, _, err := hub.join("salon", "Bob", "", 0, &recorder{}); err != nil {
				t.Errorf("join concurrent: %v", err)
			}
		}()
		wg.Wait()

		live := hub.room("salon")
		if live == nil {
			t.Fatalf("itération %d : la salle doit exister (ressuscitée ou recréée)", i)
		}
		users := live.Users()
		if len(users) != 1 || users[0].Name != "Bob" {
			t.Fatalf("itération %d : la salle vivante doit contenir le seul Bob, obtenu %+v", i, users)
		}
		if hub.roomCount() != 1 {
			t.Fatalf("itération %d : une seule salle attendue, %d obtenues", i, hub.roomCount())
		}
	}
}

// --- Reprise de séance : salle vide conservée (VS-021, §Modèle) ---

// playAlone monte une salle d'un seul membre en lecture depuis pos.
func playAlone(t *testing.T, hub *Hub, room, name string, pos float64) (*Room, *member) {
	t.Helper()
	r, m, _, err := hub.join(room, name, "", 0, &recorder{})
	if err != nil {
		t.Fatalf("join %s: %v", name, err)
	}
	r.handleSetReady(m, protocol.SetReady{Ready: true})
	r.handleControl(m, protocol.Control{Action: protocol.ActionPlay, PositionSec: pos})
	if r.State().Paused {
		t.Fatal("préparation : la lecture n'a pas démarré")
	}
	return r, m
}

func TestHubSalleVideConserveeAvecSaSeanceGelee(t *testing.T) {
	hub, clk := newTestHubLinger(30*time.Minute, defaultMaxRooms)
	room, alice := playAlone(t, hub, "salon", "alice", 100)

	clk.Advance(30 * time.Second)
	hub.leave(room, alice)

	if hub.room("salon") != room {
		t.Fatal("la salle vide doit être conservée pendant la fenêtre de reprise")
	}
	st := room.State()
	if !st.Paused {
		t.Fatalf("la séance doit être gelée en pause: %+v", st)
	}
	if math.Abs(st.PositionSec-130) > 0.01 {
		t.Fatalf("position gelée attendue ≈130 s, obtenue %v", st.PositionSec)
	}
	// Le gel doit tenir : sans lui, la position de référence continuerait à
	// courir avec l'horloge.
	clk.Advance(10 * time.Minute)
	hub.gc()
	if hub.room("salon") == nil {
		t.Fatal("la salle ne doit pas être détruite avant la fin de la fenêtre")
	}
	if st := room.State(); math.Abs(st.PositionSec-130) > 0.01 {
		t.Fatalf("la position gelée a bougé: %v", st.PositionSec)
	}
}

// Le gel doit dater du départ du dernier membre, pas du moment où le hub s'en
// aperçoit : entre les deux, l'horloge peut avoir avancé (le hub reprend le
// verrou, la machine souffle) et la séance retomberait n'importe où.
func TestHubGelDateDuDepartPasDeLaPriseEnCompte(t *testing.T) {
	hub, clk := newTestHubLinger(30*time.Minute, defaultMaxRooms)
	room, alice := playAlone(t, hub, "salon", "alice", 100)

	clk.Advance(30 * time.Second)
	if !room.leave(alice) {
		t.Fatal("la salle devrait être vide")
	}
	clk.Advance(2 * time.Minute) // le hub met du temps à s'en occuper
	hub.retireIfEmpty(room)

	if st := room.State(); math.Abs(st.PositionSec-130) > 0.01 {
		t.Fatalf("séance gelée à %v, attendu 130 s (instant du départ)", st.PositionSec)
	}
}

func TestHubSalleEnLingerDetruiteApresExpiration(t *testing.T) {
	hub, clk := newTestHubLinger(30*time.Minute, defaultMaxRooms)
	var destroyed []string
	hub.onRoomDestroyed = func(name string) { destroyed = append(destroyed, name) }
	room, alice := playAlone(t, hub, "salon", "alice", 10)
	hub.leave(room, alice)

	clk.Advance(29 * time.Minute)
	hub.gc()
	if hub.roomCount() != 1 || len(destroyed) != 0 {
		t.Fatalf("salle détruite trop tôt (détruites: %v)", destroyed)
	}

	clk.Advance(2 * time.Minute)
	hub.gc()
	if hub.roomCount() != 0 || hub.room("salon") != nil {
		t.Fatal("la salle doit être détruite une fois la fenêtre écoulée")
	}
	if len(destroyed) != 1 || destroyed[0] != "salon" {
		t.Fatalf("hook de destruction attendu une fois pour « salon », obtenu %v", destroyed)
	}

	// Salle recréée à neuf : identifiants et état repartent de zéro.
	r2, _, _, err := hub.join("salon", "bob", "", 0, &recorder{})
	if err != nil {
		t.Fatalf("join après expiration: %v", err)
	}
	if r2 == room {
		t.Fatal("une salle neuve était attendue")
	}
	if st := r2.State(); st.PositionSec != 0 || !st.Paused {
		t.Fatalf("état neuf attendu, obtenu %+v", st)
	}
}

func TestHubRetourDansUneSalleEnLingerReprendLaSeance(t *testing.T) {
	hub, clk := newTestHubLinger(30*time.Minute, defaultMaxRooms)
	room, alice := playAlone(t, hub, "salon", "alice", 3600+120+9) // 01:02:09
	hub.leave(room, alice)
	clk.Advance(2 * time.Minute)

	rec := &recorder{}
	back, m, _, err := hub.join("salon", "alice", "", 0, rec)
	if err != nil {
		t.Fatalf("retour dans la salle en linger: %v", err)
	}
	if back != room {
		t.Fatal("le revenant doit retomber sur la salle conservée")
	}
	welcome, ok := rec.lastOf(t, protocol.TypeWelcome).Data.(protocol.Welcome)
	if !ok {
		t.Fatal("payload welcome inattendu")
	}
	if !welcome.State.Paused || math.Abs(welcome.State.PositionSec-3729) > 0.01 {
		t.Fatalf("welcome doit porter la séance interrompue: %+v", welcome.State)
	}
	if !containsSub(toastsOf(rec), "séance reprise à 01:02:09") {
		t.Fatalf("toast de reprise attendu, obtenu %v", toastsOf(rec))
	}

	// La salle est de nouveau vivante : un second départ rouvre une fenêtre
	// neuve, et l'arrivant suivant n'est plus un « revenant ».
	rec2 := &recorder{}
	if _, _, _, err := hub.join("salon", "bob", "", 0, rec2); err != nil {
		t.Fatalf("join bob: %v", err)
	}
	if containsSub(toastsOf(rec2), "séance reprise") {
		t.Fatalf("aucune reprise attendue dans une salle occupée: %v", toastsOf(rec2))
	}
	hub.leave(room, m)
	clk.Advance(time.Hour)
	hub.gc()
	if hub.room("salon") == nil {
		t.Fatal("une salle encore occupée par Bob ne doit jamais expirer")
	}
}

// Le plafond de salles compte les salles en attente de reprise (choix assumé,
// cf. commentaire de Hub) ; une salle expirée, elle, ne bloque personne.
func TestHubSalleEnLingerCompteDansLePlafond(t *testing.T) {
	hub, clk := newTestHubLinger(30*time.Minute, 1)
	room, alice := playAlone(t, hub, "salon", "alice", 10)
	hub.leave(room, alice)

	if _, _, _, err := hub.join("cave", "bob", "", 0, &recorder{}); !errors.Is(err, errTooManyRooms) {
		t.Fatalf("errTooManyRooms attendu tant que « salon » attend une reprise, obtenu %v", err)
	}
	// Une fois la fenêtre écoulée, le join fait lui-même le ménage.
	clk.Advance(31 * time.Minute)
	if _, _, _, err := hub.join("cave", "bob", "", 0, &recorder{}); err != nil {
		t.Fatalf("join après expiration de « salon »: %v", err)
	}
	if hub.room("salon") != nil {
		t.Fatal("« salon » devait être recyclée par le join")
	}
}

func TestHubGcPeriodeBornee(t *testing.T) {
	cases := map[time.Duration]time.Duration{
		10 * time.Second: gcMinPeriod,
		time.Minute:      15 * time.Second,
		30 * time.Minute: gcMaxPeriod,
	}
	for linger, want := range cases {
		hub, _ := newTestHubLinger(linger, defaultMaxRooms)
		if got := hub.gcPeriod(); got != want {
			t.Fatalf("linger %s : période %s, attendue %s", linger, got, want)
		}
	}
}

func TestHubJoinConcurrent(t *testing.T) {
	hub := newTestHub()
	var wg sync.WaitGroup
	names := []string{"a", "b", "c", "d", "e", "f", "g", "h"}
	for _, name := range names {
		wg.Add(1)
		go func(name string) {
			defer wg.Done()
			room, m, _, err := hub.join("salon", name, "", 0, &recorder{})
			if err != nil {
				t.Errorf("join %s: %v", name, err)
				return
			}
			hub.leave(room, m)
		}(name)
	}
	wg.Wait()
	if hub.roomCount() != 0 {
		t.Fatalf("toutes les salles doivent être détruites, %d restantes", hub.roomCount())
	}
}
