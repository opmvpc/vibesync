package server

import (
	"math"
	"strings"
	"testing"
	"time"

	"github.com/thibsix/vibesync/internal/protocol"
)

func TestJoinEnvoieWelcomeEtUsers(t *testing.T) {
	room, clk := newTestRoom()

	alice, recA := joinTest(t, room, "Alice")
	if alice.id != "u1" {
		t.Fatalf("id attendu u1, obtenu %q", alice.id)
	}
	welcome, ok := recA.lastOf(t, protocol.TypeWelcome).Data.(protocol.Welcome)
	if !ok {
		t.Fatal("payload welcome inattendu")
	}
	if welcome.SelfID != "u1" || welcome.Room != "salon" {
		t.Fatalf("welcome inattendu: %+v", welcome)
	}
	if !welcome.State.Paused || welcome.State.Rate != 1 || welcome.State.PositionSec != 0 {
		t.Fatalf("état initial inattendu: %+v", welcome.State)
	}
	if len(welcome.Users) != 1 {
		t.Fatalf("welcome.Users attendu 1, obtenu %d", len(welcome.Users))
	}

	recA.reset()
	_, recB := joinTest(t, room, "Bob")

	// Alice reçoit le toast d'arrivée et la liste à jour.
	if got := recA.lastToast(t); got.Level != protocol.LevelInfo || !strings.Contains(got.Text, "Bob") {
		t.Fatalf("toast d'arrivée inattendu: %+v", got)
	}
	if users := recA.lastUsers(t); len(users) != 2 {
		t.Fatalf("users attendu 2, obtenu %d", len(users))
	}
	// Bob ne reçoit pas de toast pour sa propre arrivée.
	if n := recB.countOf(protocol.TypeToast); n != 0 {
		t.Fatalf("le nouvel arrivant ne doit pas recevoir de toast d'arrivée (%d reçus)", n)
	}
	// L'état renvoyé au nouvel arrivant reflète bien l'horloge injectée.
	welcomeB := recB.lastOf(t, protocol.TypeWelcome).Data.(protocol.Welcome)
	if welcomeB.State.RefServerMs != msOf(clk.Now()) {
		t.Fatalf("refServerMs attendu %d, obtenu %d", msOf(clk.Now()), welcomeB.State.RefServerMs)
	}
}

func TestJoinPseudoDejaPris(t *testing.T) {
	room, _ := newTestRoom()
	joinTest(t, room, "Alice")

	if _, _, err := room.join("alice", "", 0, &recorder{}); err != errNameTaken {
		t.Fatalf("errNameTaken attendu (comparaison insensible à la casse), obtenu %v", err)
	}
	if _, _, err := room.join("Bob", "", 0, &recorder{}); err != nil {
		t.Fatalf("Bob doit pouvoir rejoindre: %v", err)
	}
}

func TestReadyGateBloqueLePremierPlay(t *testing.T) {
	room, _ := newTestRoom()
	alice, recA := joinTest(t, room, "Alice")
	_, recB := joinTest(t, room, "Bob")
	recA.reset()
	recB.reset()

	room.handleControl(alice, protocol.Control{Action: protocol.ActionPlay, PositionSec: 12})

	if !room.State().Paused {
		t.Fatal("la salle doit rester en pause tant que tout le monde n'est pas prêt")
	}
	toast := recA.lastToast(t)
	if toast.Level != protocol.LevelWarn {
		t.Fatalf("le refus doit être un toast warn, obtenu %+v", toast)
	}
	if !strings.Contains(toast.Text, "Alice") || !strings.Contains(toast.Text, "Bob") {
		t.Fatalf("le toast doit nommer les membres non prêts: %q", toast.Text)
	}
	// L'émetteur reçoit l'état courant pour annuler sa lecture locale ; les
	// autres ne reçoivent rien.
	if st := recA.lastState(t); !st.Paused {
		t.Fatalf("roomState renvoyé à l'émetteur inattendu: %+v", st)
	}
	if n := recB.countOf(protocol.TypeRoomState) + recB.countOf(protocol.TypeToast); n != 0 {
		t.Fatalf("les autres membres ne doivent rien recevoir sur un play refusé (%d messages)", n)
	}
}

func TestReadyGateLevePuisPlayLibre(t *testing.T) {
	room, clk := newTestRoom()
	alice, recA := joinTest(t, room, "Alice")
	bob, recB := joinTest(t, room, "Bob")

	room.handleSetReady(alice, protocol.SetReady{Ready: true})
	room.handleSetReady(bob, protocol.SetReady{Ready: true})
	recA.reset()
	recB.reset()

	room.handleControl(alice, protocol.Control{Action: protocol.ActionPlay, PositionSec: 12})

	st := room.State()
	if st.Paused || st.PositionSec != 12 || st.SetBy != alice.id || st.Rate != 1 {
		t.Fatalf("état après play inattendu: %+v", st)
	}
	if st.RefServerMs != msOf(clk.Now()) {
		t.Fatalf("refServerMs attendu %d, obtenu %d", msOf(clk.Now()), st.RefServerMs)
	}
	// Broadcast à tous, y compris l'émetteur.
	if got := recA.lastState(t); got != st {
		t.Fatalf("l'émetteur doit recevoir le roomState: %+v", got)
	}
	if got := recB.lastState(t); got != st {
		t.Fatalf("les autres doivent recevoir le roomState: %+v", got)
	}

	// Après le premier démarrage, le ready-gate ne s'applique plus.
	charlie, _ := joinTest(t, room, "Charlie") // pas prêt
	room.handleControl(alice, protocol.Control{Action: protocol.ActionPause, PositionSec: 20})
	recA.reset()
	room.handleControl(charlie, protocol.Control{Action: protocol.ActionPlay, PositionSec: 20})
	if room.State().Paused {
		t.Fatal("après le premier démarrage, play doit être libre même sans ready")
	}
}

func TestControlCompenseLaLatenceDeLEmetteur(t *testing.T) {
	room, clk := newTestRoom()
	alice, _ := joinTest(t, room, "Alice")
	room.handleSetReady(alice, protocol.SetReady{Ready: true})
	room.observeRTT(alice, 200*time.Millisecond)

	if alice.latencyMs != 200 {
		t.Fatalf("latence attendue 200 ms, obtenue %d", alice.latencyMs)
	}

	room.handleControl(alice, protocol.Control{Action: protocol.ActionPlay, PositionSec: 10})
	st := room.State()
	if want := msOf(clk.Now()) - 100; st.RefServerMs != want {
		t.Fatalf("refServerMs compensé attendu %d, obtenu %d", want, st.RefServerMs)
	}

	// Une pause fige la position telle que rapportée, sans compensation.
	room.handleControl(alice, protocol.Control{Action: protocol.ActionPause, PositionSec: 42.5})
	st = room.State()
	if !st.Paused || st.PositionSec != 42.5 || st.RefServerMs != msOf(clk.Now()) {
		t.Fatalf("état après pause inattendu: %+v", st)
	}

	// Un seek en pause ne compense pas non plus.
	room.handleControl(alice, protocol.Control{Action: protocol.ActionSeek, PositionSec: 60})
	st = room.State()
	if !st.Paused || st.PositionSec != 60 || st.RefServerMs != msOf(clk.Now()) {
		t.Fatalf("état après seek en pause inattendu: %+v", st)
	}

	// Un seek en lecture compense.
	room.handleControl(alice, protocol.Control{Action: protocol.ActionPlay, PositionSec: 60})
	room.handleControl(alice, protocol.Control{Action: protocol.ActionSeek, PositionSec: 90})
	st = room.State()
	if st.Paused || st.RefServerMs != msOf(clk.Now())-100 {
		t.Fatalf("état après seek en lecture inattendu: %+v", st)
	}
}

func TestPositionCouranteAvanceAvecLHorloge(t *testing.T) {
	room, clk := newTestRoom()
	alice, _ := joinTest(t, room, "Alice")
	room.handleSetReady(alice, protocol.SetReady{Ready: true})
	room.handleControl(alice, protocol.Control{Action: protocol.ActionPlay, PositionSec: 10})

	clk.Advance(5 * time.Second)
	room.mu.Lock()
	pos := room.positionAtLocked(msOf(clk.Now()))
	room.mu.Unlock()
	if math.Abs(pos-15) > 0.001 {
		t.Fatalf("position attendue 15 s, obtenue %v", pos)
	}

	// En pause la position ne bouge plus.
	room.handleControl(alice, protocol.Control{Action: protocol.ActionPause, PositionSec: 15})
	clk.Advance(10 * time.Second)
	room.mu.Lock()
	pos = room.positionAtLocked(msOf(clk.Now()))
	room.mu.Unlock()
	if pos != 15 {
		t.Fatalf("position en pause attendue 15 s, obtenue %v", pos)
	}
}

func TestControlActionInconnueIgnoree(t *testing.T) {
	room, _ := newTestRoom()
	alice, recA := joinTest(t, room, "Alice")
	room.handleSetReady(alice, protocol.SetReady{Ready: true})
	recA.reset()

	room.handleControl(alice, protocol.Control{Action: "teleport", PositionSec: 10})
	if !room.State().Paused {
		t.Fatal("une action inconnue ne doit rien changer")
	}
	if n := recA.countOf(protocol.TypeRoomState); n != 0 {
		t.Fatalf("aucun broadcast attendu, %d reçus", n)
	}
}

func TestControlPositionAberranteAssainie(t *testing.T) {
	room, _ := newTestRoom()
	alice, _ := joinTest(t, room, "Alice")
	room.handleSetReady(alice, protocol.SetReady{Ready: true})

	room.handleControl(alice, protocol.Control{Action: protocol.ActionSeek, PositionSec: -42})
	if got := room.State().PositionSec; got != 0 {
		t.Fatalf("position négative attendue ramenée à 0, obtenue %v", got)
	}
	room.handleControl(alice, protocol.Control{Action: protocol.ActionSeek, PositionSec: math.Inf(1)})
	if got := room.State().PositionSec; got != 0 {
		t.Fatalf("position infinie attendue neutralisée, obtenue %v", got)
	}
	room.handleControl(alice, protocol.Control{Action: protocol.ActionSeek, PositionSec: 1e12})
	if got := room.State().PositionSec; got != maxPositionSec {
		t.Fatalf("position démesurée attendue bornée à %v, obtenue %v", maxPositionSec, got)
	}
	room.handleControl(alice, protocol.Control{Action: protocol.ActionSeek, PositionSec: math.NaN()})
	if got := room.State().PositionSec; got != 0 {
		t.Fatalf("NaN attendu ramené à 0, obtenu %v", got)
	}
}

func TestReportBufferingDeclenchePauseImmediate(t *testing.T) {
	room, clk := newTestRoom()
	alice, recA := joinTest(t, room, "Alice")
	bob, recB := joinTest(t, room, "Bob")
	room.handleSetReady(alice, protocol.SetReady{Ready: true})
	room.handleSetReady(bob, protocol.SetReady{Ready: true})
	room.handleControl(alice, protocol.Control{Action: protocol.ActionPlay, PositionSec: 0})
	clk.Advance(3 * time.Second)
	recA.reset()
	recB.reset()

	room.handleReport(bob, protocol.Report{PositionSec: 3, Buffering: true})

	st := room.State()
	if !st.Paused || st.SetBy != setByServer {
		t.Fatalf("pause auto attendue: %+v", st)
	}
	if math.Abs(st.PositionSec-3) > 0.05 {
		t.Fatalf("la salle doit être figée à la position de référence (~3 s), obtenue %v", st.PositionSec)
	}
	for name, rec := range map[string]*recorder{"Alice": recA, "Bob": recB} {
		toast := rec.lastToast(t)
		if toast.Level != protocol.LevelWarn || !strings.Contains(toast.Text, "bufferise") {
			t.Fatalf("%s : toast de pause auto attendu, obtenu %+v", name, toast)
		}
		if !rec.lastState(t).Paused {
			t.Fatalf("%s : roomState en pause attendu", name)
		}
	}
}

func TestReportRetardDoitDurerPourDeclencherLaPause(t *testing.T) {
	room, clk := newTestRoom()
	alice, _ := joinTest(t, room, "Alice")
	bob, recB := joinTest(t, room, "Bob")
	room.handleSetReady(alice, protocol.SetReady{Ready: true})
	room.handleSetReady(bob, protocol.SetReady{Ready: true})
	room.handleControl(alice, protocol.Control{Action: protocol.ActionPlay, PositionSec: 0})

	// Retard important mais premier constat : on note l'instant, sans pause.
	clk.Advance(10 * time.Second)
	room.handleReport(bob, protocol.Report{PositionSec: 1})
	if room.State().Paused {
		t.Fatal("le premier constat de retard ne doit pas mettre en pause")
	}
	if bob.lateSince.IsZero() {
		t.Fatal("lateSince doit être armé")
	}

	// Toujours en retard mais depuis moins de 2 s : toujours pas de pause.
	clk.Advance(1 * time.Second)
	room.handleReport(bob, protocol.Report{PositionSec: 2})
	if room.State().Paused {
		t.Fatal("retard non soutenu (< 2 s) : pas de pause")
	}

	// Au-delà de 2 s de retard soutenu : pause auto.
	clk.Advance(1500 * time.Millisecond)
	recB.reset()
	room.handleReport(bob, protocol.Report{PositionSec: 3})
	if !room.State().Paused {
		t.Fatal("retard soutenu > 2 s : pause auto attendue")
	}
	if toast := recB.lastToast(t); !strings.Contains(toast.Text, "retard") {
		t.Fatalf("toast de retard attendu, obtenu %+v", toast)
	}
}

func TestReportRattrapageAnnuleLeRetard(t *testing.T) {
	room, clk := newTestRoom()
	alice, _ := joinTest(t, room, "Alice")
	bob, _ := joinTest(t, room, "Bob")
	room.handleSetReady(alice, protocol.SetReady{Ready: true})
	room.handleSetReady(bob, protocol.SetReady{Ready: true})
	room.handleControl(alice, protocol.Control{Action: protocol.ActionPlay, PositionSec: 0})

	clk.Advance(10 * time.Second)
	room.handleReport(bob, protocol.Report{PositionSec: 1}) // 9 s de retard
	clk.Advance(1 * time.Second)
	room.handleReport(bob, protocol.Report{PositionSec: 10.5}) // rattrapé
	if !bob.lateSince.IsZero() {
		t.Fatal("le retard doit être oublié une fois rattrapé")
	}
	clk.Advance(5 * time.Second)
	room.handleReport(bob, protocol.Report{PositionSec: 15.5})
	if room.State().Paused {
		t.Fatal("aucune pause ne doit survenir après rattrapage")
	}
}

func TestReportEnPauseNeDeclencheRien(t *testing.T) {
	room, clk := newTestRoom()
	alice, _ := joinTest(t, room, "Alice")
	joinTest(t, room, "Bob")

	clk.Advance(5 * time.Second)
	room.handleReport(alice, protocol.Report{PositionSec: 0, Buffering: true})
	if got := room.State().SetBy; got == setByServer {
		t.Fatal("aucune pause auto ne doit être déclenchée alors que la salle est déjà en pause")
	}
}

func TestReportMetAJourLaListeDesUtilisateursAvecThrottle(t *testing.T) {
	room, clk := newTestRoom()
	alice, recA := joinTest(t, room, "Alice")
	recA.reset()

	// Même instant que le dernier broadcast users → throttlé.
	room.handleReport(alice, protocol.Report{PositionSec: 3})
	if n := recA.countOf(protocol.TypeUsers); n != 0 {
		t.Fatalf("broadcast users attendu throttlé, %d reçus", n)
	}
	clk.Advance(1500 * time.Millisecond)
	room.handleReport(alice, protocol.Report{PositionSec: 4})
	users := recA.lastUsers(t)
	if len(users) != 1 || users[0].PositionSec != 4 {
		t.Fatalf("liste users à jour attendue, obtenue %+v", users)
	}
}

func TestSetReadyBroadcastEtIdempotence(t *testing.T) {
	room, _ := newTestRoom()
	alice, recA := joinTest(t, room, "Alice")
	recA.reset()

	room.handleSetReady(alice, protocol.SetReady{Ready: true})
	if users := recA.lastUsers(t); !users[0].Ready {
		t.Fatal("ready doit être reflété dans la liste des utilisateurs")
	}
	n := recA.countOf(protocol.TypeUsers)
	room.handleSetReady(alice, protocol.SetReady{Ready: true})
	if recA.countOf(protocol.TypeUsers) != n {
		t.Fatal("un setReady sans changement ne doit pas rebroadcaster")
	}
}

func TestSetFileAvertitSurDureesDifferentes(t *testing.T) {
	room, _ := newTestRoom()
	alice, recA := joinTest(t, room, "Alice")
	bob, recB := joinTest(t, room, "Bob")

	room.handleSetFile(alice, protocol.SetFile{Name: "film.mkv", DurationSec: 5400, SizeBytes: 42})
	recA.reset()
	recB.reset()

	// Écart < 2 s : aucun avertissement.
	room.handleSetFile(bob, protocol.SetFile{Name: "film.mkv", DurationSec: 5401, SizeBytes: 43})
	if n := recB.countOf(protocol.TypeToast); n != 0 {
		t.Fatalf("aucun toast attendu pour un écart de 1 s, %d reçus", n)
	}
	if users := recB.lastUsers(t); users[1].File == nil || users[1].File.Name != "film.mkv" {
		t.Fatalf("fichier attendu dans la liste users, obtenu %+v", users[1])
	}

	// Écart > 2 s : toast warn à toute la salle.
	room.handleSetFile(bob, protocol.SetFile{Name: "film-vf.mkv", DurationSec: 5000})
	for name, rec := range map[string]*recorder{"Alice": recA, "Bob": recB} {
		toast := rec.lastToast(t)
		if toast.Level != protocol.LevelWarn || !strings.Contains(toast.Text, "durées différentes") {
			t.Fatalf("%s : toast d'avertissement attendu, obtenu %+v", name, toast)
		}
	}
	if room.State().Paused != true {
		t.Fatal("l'avertissement ne doit pas bloquer la salle (état inchangé)")
	}
}

// Spec §Comportements serveur, point 5 : une durée ≤ 0 signifie « inconnue » et
// est exclue de la comparaison — elle ne doit donc jamais déclencher le toast.
func TestSetFileDureeInconnueNAvertitPas(t *testing.T) {
	for _, duree := range []float64{0, -1, -3600} {
		room, _ := newTestRoom()
		alice, recA := joinTest(t, room, "Alice")
		bob, recB := joinTest(t, room, "Bob")
		room.handleSetFile(alice, protocol.SetFile{Name: "film.mkv", DurationSec: 5400})
		recA.reset()
		recB.reset()

		// Durée inconnue déclarée par Bob face à une durée connue d'Alice.
		room.handleSetFile(bob, protocol.SetFile{Name: "film.mkv", DurationSec: duree})
		if n := recA.countOf(protocol.TypeToast) + recB.countOf(protocol.TypeToast); n != 0 {
			t.Fatalf("durée %v : aucun avertissement attendu, %d toasts reçus", duree, n)
		}

		// Et l'inverse : durée inconnue déjà en place, puis durée connue.
		room.handleSetFile(alice, protocol.SetFile{Name: "film.mkv", DurationSec: duree})
		recA.reset()
		recB.reset()
		room.handleSetFile(bob, protocol.SetFile{Name: "film.mkv", DurationSec: 9000})
		if n := recA.countOf(protocol.TypeToast) + recB.countOf(protocol.TypeToast); n != 0 {
			t.Fatalf("durée %v : aucun avertissement attendu dans l'autre sens, %d toasts reçus", duree, n)
		}
		// La durée reste stockée telle que déclarée (assainie).
		if got := room.Users()[1].File.DurationSec; got != 9000 {
			t.Fatalf("durée attendue 9000, obtenue %v", got)
		}
	}
}

func TestSetFileNomVideIgnore(t *testing.T) {
	room, _ := newTestRoom()
	alice, recA := joinTest(t, room, "Alice")
	recA.reset()
	room.handleSetFile(alice, protocol.SetFile{Name: "   ", DurationSec: 10})
	if alice.file != nil {
		t.Fatal("un nom de fichier vide doit être ignoré")
	}
	if n := recA.countOf(protocol.TypeUsers); n != 0 {
		t.Fatalf("aucun broadcast attendu, %d reçus", n)
	}
}

func TestChatDiffuseUnChatEvent(t *testing.T) {
	room, clk := newTestRoom()
	alice, recA := joinTest(t, room, "Alice")
	_, recB := joinTest(t, room, "Bob")
	recA.reset()
	recB.reset()

	room.handleChat(alice, protocol.Chat{Text: "  salut  "})
	for name, rec := range map[string]*recorder{"Alice": recA, "Bob": recB} {
		ev, ok := rec.lastOf(t, protocol.TypeChatEvent).Data.(protocol.ChatEvent)
		if !ok {
			t.Fatalf("%s : payload chatEvent inattendu", name)
		}
		if ev.From != "Alice" || ev.Text != "salut" || ev.ServerMs != msOf(clk.Now()) {
			t.Fatalf("%s : chatEvent inattendu %+v", name, ev)
		}
	}

	recA.reset()
	room.handleChat(alice, protocol.Chat{Text: "   "})
	if n := recA.countOf(protocol.TypeChatEvent); n != 0 {
		t.Fatalf("un chat vide doit être ignoré, %d reçus", n)
	}

	room.handleChat(alice, protocol.Chat{Text: strings.Repeat("a", maxChatLen+50)})
	ev := recA.lastOf(t, protocol.TypeChatEvent).Data.(protocol.ChatEvent)
	if len([]rune(ev.Text)) != maxChatLen {
		t.Fatalf("le texte doit être tronqué à %d runes, obtenu %d", maxChatLen, len([]rune(ev.Text)))
	}
}

func TestLeaveEnLecturePauseAutomatiquement(t *testing.T) {
	room, clk := newTestRoom()
	alice, recA := joinTest(t, room, "Alice")
	bob, _ := joinTest(t, room, "Bob")
	room.handleSetReady(alice, protocol.SetReady{Ready: true})
	room.handleSetReady(bob, protocol.SetReady{Ready: true})
	room.handleControl(alice, protocol.Control{Action: protocol.ActionPlay, PositionSec: 0})
	clk.Advance(7 * time.Second)
	recA.reset()

	if empty := room.leave(bob); empty {
		t.Fatal("la salle n'est pas vide")
	}
	st := room.State()
	if !st.Paused || st.SetBy != setByServer {
		t.Fatalf("pause auto attendue après déconnexion: %+v", st)
	}
	if math.Abs(st.PositionSec-7) > 0.05 {
		t.Fatalf("position figée ~7 s attendue, obtenue %v", st.PositionSec)
	}
	var sawLeave, sawAutoPause bool
	for _, msg := range recA.all() {
		if msg.Type != protocol.TypeToast {
			continue
		}
		text := msg.Data.(protocol.Toast).Text
		if strings.Contains(text, "a quitté") {
			sawLeave = true
		}
		if strings.Contains(text, "déconnecté") {
			sawAutoPause = true
		}
	}
	if !sawLeave || !sawAutoPause {
		t.Fatalf("toasts de départ et de pause auto attendus, reçus %v", recA.all())
	}
	if users := recA.lastUsers(t); len(users) != 1 {
		t.Fatalf("liste users à 1 membre attendue, obtenue %d", len(users))
	}
}

func TestLeaveEnPauseNeChangePasLEtat(t *testing.T) {
	room, _ := newTestRoom()
	alice, _ := joinTest(t, room, "Alice")
	bob, _ := joinTest(t, room, "Bob")
	room.handleSetReady(alice, protocol.SetReady{Ready: true})
	room.handleSetReady(bob, protocol.SetReady{Ready: true})
	room.handleControl(alice, protocol.Control{Action: protocol.ActionPause, PositionSec: 30})
	before := room.State()

	room.leave(bob)
	if room.State() != before {
		t.Fatalf("l'état ne doit pas changer: avant %+v, après %+v", before, room.State())
	}
}

func TestLeaveDernierMembreRendLaSalleVide(t *testing.T) {
	room, _ := newTestRoom()
	alice, _ := joinTest(t, room, "Alice")
	if !room.leave(alice) {
		t.Fatal("la salle doit être signalée vide")
	}
	// Un leave répété ne panique pas.
	if !room.leave(alice) {
		t.Fatal("la salle doit rester vide")
	}
}

func TestObserveRTTMoyenneGlissante(t *testing.T) {
	room, clk := newTestRoom()
	alice, _ := joinTest(t, room, "Alice")

	for _, rtt := range []time.Duration{100, 200, 300} {
		clk.Advance(time.Second)
		room.observeRTT(alice, rtt*time.Millisecond)
	}
	if alice.latencyMs != 200 {
		t.Fatalf("moyenne attendue 200 ms, obtenue %d", alice.latencyMs)
	}
	// Fenêtre glissante : seules les rttWindow dernières mesures comptent.
	for i := 0; i < rttWindow; i++ {
		clk.Advance(time.Second)
		room.observeRTT(alice, 50*time.Millisecond)
	}
	if alice.latencyMs != 50 {
		t.Fatalf("moyenne glissante attendue 50 ms, obtenue %d", alice.latencyMs)
	}
	if len(alice.rtts) != rttWindow {
		t.Fatalf("fenêtre attendue %d mesures, obtenue %d", rttWindow, len(alice.rtts))
	}
	if users := room.Users(); users[0].LatencyMs != 50 {
		t.Fatalf("latence attendue dans la liste users, obtenue %+v", users[0])
	}
}

func TestPingRepondPong(t *testing.T) {
	room, clk := newTestRoom()
	alice, recA := joinTest(t, room, "Alice")
	recA.reset()

	room.handlePing(alice, protocol.Ping{T: 1234})
	pong, ok := recA.lastOf(t, protocol.TypePong).Data.(protocol.Pong)
	if !ok {
		t.Fatal("payload pong inattendu")
	}
	if pong.T != 1234 || pong.ServerMs != msOf(clk.Now()) {
		t.Fatalf("pong inattendu: %+v", pong)
	}
}

func TestAccesConcurrentSansCourse(t *testing.T) {
	room, clk := newTestRoom()
	alice, _ := joinTest(t, room, "Alice")
	bob, _ := joinTest(t, room, "Bob")
	room.handleSetReady(alice, protocol.SetReady{Ready: true})
	room.handleSetReady(bob, protocol.SetReady{Ready: true})

	done := make(chan struct{})
	for _, m := range []*member{alice, bob} {
		go func(m *member) {
			defer func() { done <- struct{}{} }()
			for i := 0; i < 200; i++ {
				room.handleControl(m, protocol.Control{Action: protocol.ActionPlay, PositionSec: float64(i)})
				room.handleReport(m, protocol.Report{PositionSec: float64(i)})
				room.handleChat(m, protocol.Chat{Text: "coucou"})
				room.observeRTT(m, 10*time.Millisecond)
				clk.Advance(time.Millisecond)
			}
		}(m)
	}
	<-done
	<-done
	if len(room.Users()) != 2 {
		t.Fatal("les deux membres doivent toujours être présents")
	}
}
