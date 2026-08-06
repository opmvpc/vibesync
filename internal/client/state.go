package client

import (
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
)

// État persistant du client de référence.
//
// Le seul élément aujourd'hui est le jeton de reprise de session, et il doit
// survivre à un redémarrage : sans cela, relancer l'application donne un jeton
// neuf, le serveur ne reconnaît plus le détenteur du pseudo et le refuse
// (`name_taken`) tant que la connexion zombie n'a pas expiré — 60 s de timeout
// de lecture (VS-028 ; docs/protocol.md §Messages client → serveur).
//
// Le fichier est un objet JSON dont on ne réécrit que les clés connues : une
// clé ajoutée par une version ultérieure survit à un enregistrement, comme le
// fait déjà le client C avec son ini.

const (
	// stateFileName est le fichier d'état, dans StateDir.
	stateFileName = "state.json"
	// stateSessionKey est la clé du jeton de reprise de session.
	stateSessionKey = "session"
	// sessionTokenBytes est la taille du jeton tiré (docs/protocol.md exige au
	// moins 16 octets hexadécimaux).
	sessionTokenBytes = 16
	// stateDirPerm / stateFilePerm : l'état n'est lisible que par son
	// propriétaire, le jeton tenant lieu de preuve d'identité au sens bonne foi.
	stateDirPerm  = 0o700
	stateFilePerm = 0o600
)

// DefaultStateDir donne l'emplacement standard de l'état du client de
// référence : <UserConfigDir>/vibesync — %AppData%\vibesync sous Windows,
// ~/.config/vibesync sous Linux, ~/Library/Application Support/vibesync sous
// macOS.
func DefaultStateDir() (string, error) {
	dir, err := os.UserConfigDir()
	if err != nil {
		return "", fmt.Errorf("client: dossier de configuration introuvable: %w", err)
	}
	return filepath.Join(dir, "vibesync"), nil
}

// loadSessionToken rend le jeton de reprise de session conservé dans dir, en le
// générant et en l'écrivant s'il est absent, illisible ou invalide.
//
// Un dossier vide désactive la persistance : le jeton ne vit alors que le temps
// du processus (c'est ce que font les tests et le harnais e2e, qui ne doivent
// écrire nulle part). Aucune erreur de fichier n'est fatale — au pire on repart
// avec un jeton neuf, ce qui était le comportement d'avant VS-028.
func loadSessionToken(dir string, log *slog.Logger) (string, error) {
	if dir == "" {
		return newSessionToken()
	}
	path := filepath.Join(dir, stateFileName)

	state, err := readState(path)
	if err != nil {
		if !errors.Is(err, os.ErrNotExist) {
			// État corrompu : il sera réécrit. Ne pas démarrer serait une bien
			// pire réponse qu'un jeton neuf.
			log.Warn("état du client illisible, il sera réécrit", "fichier", path, "err", err)
		}
		state = map[string]json.RawMessage{}
	}
	if token, ok := sessionFromState(state); ok {
		return token, nil
	}

	token, err := newSessionToken()
	if err != nil {
		return "", err
	}
	raw, err := json.Marshal(token)
	if err != nil {
		return "", fmt.Errorf("client: encodage du jeton de session: %w", err)
	}
	state[stateSessionKey] = raw
	if err := writeState(path, state); err != nil {
		// Sans persistance, le pseudo pourra être refusé après un redémarrage
		// tant que la connexion zombie vit : gênant, pas bloquant.
		log.Warn("jeton de session non persisté", "fichier", path, "err", err)
	}
	return token, nil
}

// readState lit le fichier d'état. Les valeurs restent brutes : ce qu'on ne
// comprend pas doit pouvoir être réécrit tel quel.
func readState(path string) (map[string]json.RawMessage, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	state := map[string]json.RawMessage{}
	if err := json.Unmarshal(raw, &state); err != nil {
		return nil, fmt.Errorf("client: état %s illisible: %w", path, err)
	}
	return state, nil
}

// writeState réécrit le fichier d'état. L'écriture passe par un fichier
// temporaire puis un renommage : une coupure au mauvais moment laisse l'ancien
// état intact plutôt qu'un fichier tronqué.
func writeState(path string, state map[string]json.RawMessage) error {
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, stateDirPerm); err != nil {
		return fmt.Errorf("client: création de %s: %w", dir, err)
	}
	raw, err := json.MarshalIndent(state, "", "  ")
	if err != nil {
		return fmt.Errorf("client: encodage de l'état: %w", err)
	}
	raw = append(raw, '\n')

	tmp, err := os.CreateTemp(dir, stateFileName+".*")
	if err != nil {
		return fmt.Errorf("client: fichier temporaire dans %s: %w", dir, err)
	}
	tmpName := tmp.Name()
	defer func() { _ = os.Remove(tmpName) }() // no-op si le renommage a réussi

	if err := tmp.Chmod(stateFilePerm); err != nil && !errors.Is(err, os.ErrInvalid) {
		// Windows ne gère pas les permissions POSIX : on n'en fait pas un échec.
		_ = err
	}
	if _, err := tmp.Write(raw); err != nil {
		_ = tmp.Close()
		return fmt.Errorf("client: écriture de l'état: %w", err)
	}
	if err := tmp.Close(); err != nil {
		return fmt.Errorf("client: fermeture de l'état: %w", err)
	}
	if err := os.Rename(tmpName, path); err != nil {
		return fmt.Errorf("client: installation de %s: %w", path, err)
	}
	return nil
}

// sessionFromState extrait un jeton exploitable de l'état lu.
func sessionFromState(state map[string]json.RawMessage) (string, bool) {
	raw, ok := state[stateSessionKey]
	if !ok {
		return "", false
	}
	var token string
	if err := json.Unmarshal(raw, &token); err != nil {
		return "", false
	}
	if !validSessionToken(token) {
		return "", false
	}
	return token, true
}

// validSessionToken vérifie la forme attendue par la spec : hexadécimal, au
// moins 16 octets. Un jeton bricolé à la main ou tronqué est rejeté et
// remplacé — mieux vaut un jeton neuf qu'un jeton que le serveur refusera.
func validSessionToken(token string) bool {
	if len(token) > maxSessionTokenLen {
		return false
	}
	buf, err := hex.DecodeString(token)
	return err == nil && len(buf) >= sessionTokenBytes
}
