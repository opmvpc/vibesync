package server

import (
	"errors"
	"sync"
	"testing"

	"github.com/thibsix/vibesync/internal/protocol"
)

func newTestHub() *Hub { return newTestHubWith(defaultMaxRooms, defaultMaxRoomSize) }

func newTestHubWith(maxRooms, maxRoomSize int) *Hub {
	return newHub(newFakeClock(), testLogger(), maxRooms, maxRoomSize)
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
