package server

import (
	"strings"
	"testing"

	"github.com/thibsix/vibesync/internal/protocol"
)

// Reprise de session : docs/protocol.md §Comportements serveur, point 6.

// toastsOf extrait les textes des toasts reçus par un enregistreur.
func toastsOf(rec *recorder) []string {
	var out []string
	for _, m := range rec.all() {
		if m.Type != protocol.TypeToast {
			continue
		}
		if t, ok := m.Data.(protocol.Toast); ok {
			out = append(out, t.Text)
		}
	}
	return out
}

func containsSub(list []string, substr string) bool {
	for _, s := range list {
		if strings.Contains(strings.ToLower(s), strings.ToLower(substr)) {
			return true
		}
	}
	return false
}

func TestRepriseDeSessionMemeJeton(t *testing.T) {
	room, _ := newTestRoom()
	zombie, zombieRec, _ := joinSession(t, room, "alice", "jeton-alice")
	bob, bobRec := joinTest(t, room, "bob")

	// La salle est en lecture (ready-gate levé) : la reprise ne doit pas la
	// mettre en pause.
	room.handleSetReady(zombie, protocol.SetReady{Ready: true})
	room.handleSetReady(bob, protocol.SetReady{Ready: true})
	room.handleControl(zombie, protocol.Control{Action: protocol.ActionPlay, PositionSec: 42})
	if room.State().Paused {
		t.Fatal("préparation du test : la lecture n'a pas démarré")
	}
	bobRec.reset()

	repris, reprisRec, replaced := joinSession(t, room, "alice", "jeton-alice")
	if replaced == nil {
		t.Fatal("aucune connexion remplacée alors que le jeton correspond")
	}
	if replaced != sink(zombieRec) {
		t.Fatal("la connexion remplacée n'est pas celle du zombie")
	}
	replaced.evict()
	if zombieRec.evictions() != 1 {
		t.Fatalf("%d éviction(s) du zombie, attendu 1", zombieRec.evictions())
	}

	// Le remplaçant est opérationnel : welcome avec l'état courant.
	if n := reprisRec.countOf(protocol.TypeWelcome); n != 1 {
		t.Fatalf("%d welcome pour le remplaçant, attendu 1", n)
	}
	if got := room.size(); got != 2 {
		t.Fatalf("%d membres, attendu 2 (le zombie a été remplacé, pas ajouté)", got)
	}
	if repris.id == zombie.id {
		t.Fatal("le remplaçant devrait recevoir un nouvel identifiant")
	}

	// Pas de toast de départ, et la salle n'est pas mise en pause.
	if got := toastsOf(bobRec); containsSub(got, "a quitté") {
		t.Fatalf("toast de départ émis lors d'une reprise: %v", got)
	}
	if containsSub(toastsOf(bobRec), "pause auto") {
		t.Fatalf("pause automatique lors d'une reprise: %v", toastsOf(bobRec))
	}
	if room.State().Paused {
		t.Fatal("la salle a été mise en pause par la reprise de session")
	}
	if !containsSub(toastsOf(bobRec), "a repris sa session") {
		t.Fatalf("les autres membres ne sont pas informés de la reprise: %v", toastsOf(bobRec))
	}

	// Le remplaçant pilote la salle.
	room.handleControl(repris, protocol.Control{Action: protocol.ActionPause, PositionSec: 99})
	if st := room.State(); !st.Paused || st.PositionSec != 99 || st.SetBy != repris.id {
		t.Fatalf("le remplaçant ne pilote pas la salle: %+v", st)
	}
}

func TestRepriseRefuseeSansOuMauvaisJeton(t *testing.T) {
	cases := []struct {
		nom      string
		initial  string
		nouveau  string
		attendue error
	}{
		{"jeton différent", "jeton-alice", "jeton-imposteur", errNameTaken},
		{"jeton absent côté arrivant", "jeton-alice", "", errNameTaken},
		{"jeton absent des deux côtés", "", "", errNameTaken},
		{"jeton absent côté détenteur", "", "jeton-alice", errNameTaken},
	}
	for _, tc := range cases {
		t.Run(tc.nom, func(t *testing.T) {
			room, _ := newTestRoom()
			_, zombieRec, _ := joinSession(t, room, "alice", tc.initial)

			_, replaced, err := room.join("alice", tc.nouveau, 0, &recorder{})
			if err != tc.attendue {
				t.Fatalf("erreur = %v, attendu %v", err, tc.attendue)
			}
			if replaced != nil {
				t.Fatal("une connexion a été évincée malgré le refus")
			}
			if zombieRec.evictions() != 0 {
				t.Fatal("le détenteur légitime a été fermé")
			}
			if got := room.size(); got != 1 {
				t.Fatalf("%d membres, attendu 1", got)
			}
		})
	}
}

func TestRepriseInsensibleALaCasseDuPseudo(t *testing.T) {
	room, _ := newTestRoom()
	_, zombieRec, _ := joinSession(t, room, "Alice", "jeton-alice")
	_, _, replaced := joinSession(t, room, "alice", "jeton-alice")
	if replaced == nil {
		t.Fatal("la reprise doit fonctionner quelle que soit la casse du pseudo")
	}
	replaced.evict()
	if zombieRec.evictions() != 1 {
		t.Fatal("zombie non évincé")
	}
	if got := room.size(); got != 1 {
		t.Fatalf("%d membres, attendu 1", got)
	}
}

func TestDeuxReprisesSuccessives(t *testing.T) {
	room, _ := newTestRoom()
	_, rec1, _ := joinSession(t, room, "alice", "jeton-alice")
	m2, rec2, replaced1 := joinSession(t, room, "alice", "jeton-alice")
	if replaced1 == nil {
		t.Fatal("première reprise refusée")
	}
	replaced1.evict()

	m3, rec3, replaced2 := joinSession(t, room, "alice", "jeton-alice")
	if replaced2 == nil {
		t.Fatal("seconde reprise refusée")
	}
	replaced2.evict()

	if rec1.evictions() != 1 || rec2.evictions() != 1 {
		t.Fatalf("évictions: rec1=%d rec2=%d, attendu 1 et 1", rec1.evictions(), rec2.evictions())
	}
	if rec3.evictions() != 0 {
		t.Fatal("le dernier arrivant ne doit pas être évincé")
	}
	if got := room.size(); got != 1 {
		t.Fatalf("%d membres après deux reprises, attendu 1", got)
	}
	if m2.id == m3.id {
		t.Fatal("chaque reprise doit recevoir un identifiant distinct")
	}
	// Seul le dernier pilote la salle.
	room.handleControl(m2, protocol.Control{Action: protocol.ActionSeek, PositionSec: 10})
	if room.State().PositionSec == 10 {
		t.Fatal("un membre évincé pilote encore la salle")
	}
	room.handleControl(m3, protocol.Control{Action: protocol.ActionSeek, PositionSec: 20})
	if got := room.State().PositionSec; got != 20 {
		t.Fatalf("position = %v, le dernier remplaçant devrait piloter", got)
	}
}

// TestZombieMuetApresReprise : le zombie peut encore avoir des messages en vol
// au moment où sa place est reprise ; aucun ne doit plus toucher la salle.
func TestZombieMuetApresReprise(t *testing.T) {
	room, clk := newTestRoom()
	zombie, zombieRec, _ := joinSession(t, room, "alice", "jeton-alice")
	_, bobRec := joinTest(t, room, "bob")
	repris, _, replaced := joinSession(t, room, "alice", "jeton-alice")
	if replaced == nil {
		t.Fatal("reprise refusée")
	}
	replaced.evict()

	before := room.State()
	bobRec.reset()
	zombieRec.reset()

	// Salve de messages du zombie, comme s'ils étaient déjà en vol.
	room.handleControl(zombie, protocol.Control{Action: protocol.ActionPlay, PositionSec: 500})
	room.handleControl(zombie, protocol.Control{Action: protocol.ActionSeek, PositionSec: 900})
	room.handleSetReady(zombie, protocol.SetReady{Ready: true})
	room.handleSetFile(zombie, protocol.SetFile{Name: "autre.mkv", DurationSec: 10})
	room.handleReport(zombie, protocol.Report{PositionSec: 0, Buffering: true})
	room.handleChat(zombie, protocol.Chat{Text: "coucou"})
	room.handlePing(zombie, protocol.Ping{T: 1})
	room.observeRTT(zombie, 0)
	clk.Advance(0)

	if got := room.State(); got != before {
		t.Fatalf("le zombie a modifié l'état de la salle: %+v → %+v", before, got)
	}
	if n := len(zombieRec.all()); n != 0 {
		t.Fatalf("le zombie a reçu %d message(s) après son éviction: %+v", n, zombieRec.all())
	}
	if n := bobRec.countOf(protocol.TypeChatEvent); n != 0 {
		t.Fatalf("le chat du zombie a été relayé (%d)", n)
	}
	if containsSub(toastsOf(bobRec), "bufferise") {
		t.Fatal("le report du zombie a déclenché une pause auto")
	}
	for _, u := range room.Users() {
		if u.ID == zombie.id {
			t.Fatal("le zombie apparaît encore dans la liste des membres")
		}
		if u.Ready {
			t.Fatalf("le setReady du zombie a été appliqué au membre %s", u.ID)
		}
	}
	// Le remplaçant, lui, fonctionne toujours.
	room.handleSetReady(repris, protocol.SetReady{Ready: true})
	var reprisReady bool
	for _, u := range room.Users() {
		if u.ID == repris.id {
			reprisReady = u.Ready
		}
	}
	if !reprisReady {
		t.Fatal("le remplaçant n'est plus opérationnel")
	}
}

// TestRepriseIgnoreLePlafondDeSalle : une salle pleine doit quand même laisser
// un de ses membres reprendre sa place (la taille ne change pas).
func TestRepriseIgnoreLePlafondDeSalle(t *testing.T) {
	hub := newHub(newFakeClock(), testLogger(), 4, 2)
	if _, _, _, err := hub.join("salon", "alice", "jeton-alice", 0, &recorder{}); err != nil {
		t.Fatalf("join alice: %v", err)
	}
	if _, _, _, err := hub.join("salon", "bob", "jeton-bob", 0, &recorder{}); err != nil {
		t.Fatalf("join bob: %v", err)
	}
	// Salle pleine pour un nouveau venu.
	if _, _, _, err := hub.join("salon", "carol", "jeton-carol", 0, &recorder{}); err != errRoomFull {
		t.Fatalf("erreur = %v, attendu errRoomFull", err)
	}
	// Mais pas pour une reprise de session.
	_, _, replaced, err := hub.join("salon", "alice", "jeton-alice", 0, &recorder{})
	if err != nil {
		t.Fatalf("reprise refusée dans une salle pleine: %v", err)
	}
	if replaced == nil {
		t.Fatal("aucune connexion remplacée")
	}
	if got := hub.room("salon").size(); got != 2 {
		t.Fatalf("%d membres après reprise, attendu 2", got)
	}
}

// TestRepriseWebSocket vérifie le bout en bout : le zombie est réellement fermé
// par le serveur et le remplaçant reçoit un welcome utilisable.
func TestRepriseWebSocket(t *testing.T) {
	rig := newRig(t, Config{})
	witness := rig.dial(t)
	witness.hello("bob", "salon")

	zombie := rig.dial(t)
	zombie.helloSession("alice", "salon", "jeton-alice")
	witness.waitFor(protocol.TypeUsers)

	repris := rig.dial(t)
	welcome := repris.helloSession("alice", "salon", "jeton-alice")
	if welcome.SelfID == "" {
		t.Fatal("welcome sans identifiant pour le remplaçant")
	}

	// Le serveur ferme la connexion zombie de lui-même.
	zombie.expectClosed()

	// Le remplaçant est pleinement opérationnel.
	repris.send(protocol.TypeControl, protocol.Control{Action: protocol.ActionPause, PositionSec: 33})
	env := repris.waitFor(protocol.TypeRoomState)
	if st := mustData[protocol.RoomState](t, env); st.PositionSec != 33 {
		t.Fatalf("roomState = %+v", st)
	}
	// Et le témoin n'a jamais vu partir alice.
	witness.sync()
}

// TestRepriseRefuseeWebSocket : sans le bon jeton, c'est toujours name_taken.
func TestRepriseRefuseeWebSocket(t *testing.T) {
	rig := newRig(t, Config{})
	holder := rig.dial(t)
	holder.helloSession("alice", "salon", "jeton-alice")

	intrus := rig.dial(t)
	intrus.send(protocol.TypeHello, protocol.Hello{
		Version: protocol.Version, Name: "alice", Room: "salon", Session: "pas-le-bon",
	})
	env := intrus.waitFor(protocol.TypeError)
	if msg := mustData[protocol.ErrorMsg](t, env); msg.Code != protocol.ErrNameTaken {
		t.Fatalf("code = %q, attendu %q", msg.Code, protocol.ErrNameTaken)
	}
	intrus.expectClosed()
	// Le détenteur légitime n'a pas bougé.
	holder.sync()
}

// TestJetonDeSessionTropLong : garde-fou d'assainissement.
func TestJetonDeSessionTropLong(t *testing.T) {
	rig := newRig(t, Config{})
	c := rig.dial(t)
	c.send(protocol.TypeHello, protocol.Hello{
		Version: protocol.Version, Name: "alice", Room: "salon",
		Session: strings.Repeat("a", maxSessionLen+1),
	})
	env := c.waitFor(protocol.TypeError)
	if msg := mustData[protocol.ErrorMsg](t, env); msg.Code != protocol.ErrProtocol {
		t.Fatalf("code = %q, attendu %q", msg.Code, protocol.ErrProtocol)
	}
	c.expectClosed()
}
