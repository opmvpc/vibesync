package client

import (
	"encoding/json"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"testing"
)

// Persistance du jeton de reprise de session (VS-028) : un redémarrage du
// client doit réutiliser le même jeton, sans quoi le serveur ne reconnaît plus
// le détenteur du pseudo et le refuse jusqu'à l'expiration du zombie.

func quietTestLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

// newStateEngine construit un moteur qui persiste son état dans dir, sans
// toucher au vrai VLC.
func newStateEngine(t *testing.T, dir string) *Engine {
	t.Helper()
	e := New(Config{
		StateDir: dir,
		Logger:   quietTestLogger(),
		Locator:  func() (string, error) { return "/faux/vlc", nil },
	})
	t.Cleanup(func() { _ = e.Close() })
	return e
}

func readStateFile(t *testing.T, dir string) map[string]json.RawMessage {
	t.Helper()
	state, err := readState(filepath.Join(dir, stateFileName))
	if err != nil {
		t.Fatalf("état illisible: %v", err)
	}
	return state
}

func TestJetonDeSessionPersisteEntreDeuxLancements(t *testing.T) {
	dir := t.TempDir()

	premier := newStateEngine(t, dir).Session()
	if !validSessionToken(premier) {
		t.Fatalf("jeton invalide au premier lancement: %q", premier)
	}
	if _, err := os.Stat(filepath.Join(dir, stateFileName)); err != nil {
		t.Fatalf("état non écrit au premier lancement: %v", err)
	}

	// « Relancement » de l'application : même dossier, même jeton.
	if second := newStateEngine(t, dir).Session(); second != premier {
		t.Fatalf("jeton régénéré au relancement: %q puis %q", premier, second)
	}
}

func TestJetonDeSessionSansPersistance(t *testing.T) {
	// StateDir vide : rien n'est écrit et chaque moteur a son propre jeton.
	a, b := newStateEngine(t, "").Session(), newStateEngine(t, "").Session()
	if !validSessionToken(a) || !validSessionToken(b) {
		t.Fatalf("jetons invalides: %q / %q", a, b)
	}
	if a == b {
		t.Fatal("deux moteurs sans persistance devraient tirer des jetons distincts")
	}
}

func TestJetonDeSessionEtatIllisibleOuInvalide(t *testing.T) {
	cases := map[string]string{
		"json corrompu":       "{ceci n'est pas du json",
		"objet incomplet":     `{"session":`,
		"jeton non textuel":   `{"session": 42}`,
		"jeton non hexa":      `{"session": "pas-du-tout-hexadecimal!!"}`,
		"jeton trop court":    `{"session": "abcdef"}`,
		"jeton vide":          `{"session": ""}`,
		"clé session absente": `{"autre": "chose"}`,
		"fichier vide":        ``,
	}
	for nom, contenu := range cases {
		t.Run(nom, func(t *testing.T) {
			dir := t.TempDir()
			path := filepath.Join(dir, stateFileName)
			if err := os.WriteFile(path, []byte(contenu), 0o600); err != nil {
				t.Fatal(err)
			}

			token := newStateEngine(t, dir).Session()
			if !validSessionToken(token) {
				t.Fatalf("jeton non régénéré: %q", token)
			}
			// L'état est réparé sur place : le lancement suivant est stable.
			if relance := newStateEngine(t, dir).Session(); relance != token {
				t.Fatalf("état non réparé, jeton encore changé: %q puis %q", token, relance)
			}
			if got, ok := sessionFromState(readStateFile(t, dir)); !ok || got != token {
				t.Fatalf("jeton absent du fichier réécrit (%q, ok=%v)", got, ok)
			}
		})
	}
}

// Les clés qu'on ne comprend pas doivent survivre à un enregistrement : le
// fichier d'état est partagé avec les réglages à venir.
func TestEtatConserveLesClesInconnues(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, stateFileName)
	if err := os.WriteFile(path, []byte(`{"reglageFutur": {"volume": 3}}`), 0o600); err != nil {
		t.Fatal(err)
	}

	token := newStateEngine(t, dir).Session()
	state := readStateFile(t, dir)
	if got, ok := sessionFromState(state); !ok || got != token {
		t.Fatalf("jeton non écrit: %q ok=%v", got, ok)
	}
	raw, ok := state["reglageFutur"]
	if !ok {
		t.Fatalf("clé inconnue perdue à l'enregistrement: %v", state)
	}
	// La valeur est réécrite telle quelle (l'indentation, elle, peut changer).
	var garde struct {
		Volume int `json:"volume"`
	}
	if err := json.Unmarshal(raw, &garde); err != nil || garde.Volume != 3 {
		t.Fatalf("clé inconnue altérée: %s (%v)", raw, err)
	}
}

// Un état impossible à écrire ne doit pas empêcher de démarrer : on perd la
// persistance, pas l'application.
func TestEtatNonEcrivableNEmpechePasDeDemarrer(t *testing.T) {
	// Un fichier là où on attend un dossier : MkdirAll échouera.
	barrage := filepath.Join(t.TempDir(), "pas-un-dossier")
	if err := os.WriteFile(barrage, []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}

	token := newStateEngine(t, barrage).Session()
	if !validSessionToken(token) {
		t.Fatalf("aucun jeton utilisable sans persistance: %q", token)
	}
}

func TestDefaultStateDir(t *testing.T) {
	dir, err := DefaultStateDir()
	if err != nil {
		t.Skipf("dossier de configuration indisponible sur cette machine: %v", err)
	}
	if filepath.Base(dir) != "vibesync" {
		t.Fatalf("dossier d'état inattendu: %q", dir)
	}
	if !filepath.IsAbs(dir) {
		t.Fatalf("dossier d'état non absolu: %q", dir)
	}
}

func TestValidSessionToken(t *testing.T) {
	cases := map[string]bool{
		"":                                   false,
		"abcdef":                             false,
		"zz3d5e5f5a5b5c5d5e5f60616263646566": false,
		"0123456789abcdef0123456789abcdef":   true,
		"0123456789ABCDEF0123456789ABCDEF":   true,
		"0123456789abcdef0123456789abcde":    false, // longueur impaire
	}
	for token, want := range cases {
		if got := validSessionToken(token); got != want {
			t.Fatalf("validSessionToken(%q) = %v, attendu %v", token, got, want)
		}
	}
	// Un jeton plus long que ce que le serveur accepte est rejeté d'emblée.
	trop := make([]byte, 2*maxSessionTokenLen)
	for i := range trop {
		trop[i] = 'a'
	}
	if validSessionToken(string(trop)) {
		t.Fatal("un jeton au-delà de la limite serveur devrait être rejeté")
	}
}
