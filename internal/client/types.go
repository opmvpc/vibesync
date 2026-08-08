package client

import (
	"time"

	"github.com/opmvpc/vibesync/internal/protocol"
)

// Constantes de synchronisation (docs/protocol.md §Comportements client).
const (
	// PollInterval est la période d'interrogation de VLC.
	PollInterval = 200 * time.Millisecond
	// DeadZoneSec : en deçà, on ne corrige rien. Élargie à 1,5 s par VS-038 —
	// au-dessus du bruit de la position rendue par VLC (±0,15 s) et du
	// perceptible. La vitesse n'est JAMAIS utilisée pour corriger la dérive :
	// au-delà de la zone morte, c'est un micro-seek (et lui seul).
	DeadZoneSec = 1.5
	// HardSeekSec : au-delà, seek immédiat sans attendre la persistance
	// (réveil de veille, lecteur qui décroche).
	HardSeekSec = 5.0
	// StartAlignSec : au départ d'une lecture (pause → play), on cale d'abord
	// la position par un seek si l'écart atteint ce seuil — aucune correction
	// ultérieure ne le résorberait, il est sous la zone morte.
	StartAlignSec = 0.3
	// UserSeekSec : saut de position inexpliqué au-delà duquel on considère
	// que l'utilisateur a manipulé VLC.
	UserSeekSec = 3.0
	// GraceWindow : fenêtre anti-boucle après application d'un roomState (ou
	// d'une commande que l'on a nous-même envoyée à VLC).
	GraceWindow = 500 * time.Millisecond
	// UserHold : après une action volontaire (détectée dans VLC ou demandée
	// depuis l'UI), on suspend les corrections le temps que le serveur réponde
	// par un roomState — sinon le moteur « corrigerait » l'utilisateur.
	// Le hold est levé dès l'arrivée du roomState.
	UserHold = 2 * time.Second
	// PausedSeekSec : en pause, seek uniquement à partir de ce drift (le seek
	// HTTP est arrondi à la seconde : en deçà il n'améliorerait rien).
	PausedSeekSec = 0.6
	// BufferingSuspend : durée pendant laquelle la détection de buffering est
	// neutralisée après tout seek (commandé ou utilisateur) et après chaque
	// transition play/pause. Un seek fige mécaniquement la position le temps que
	// VLC cherche : sans cette fenêtre, le moteur crie « buffering » à chaque
	// manipulation et le serveur met la salle en pause (VS-017,
	// docs/protocol.md §Comportements client, Buffering).
	BufferingSuspend = 2 * time.Second
	// MinRate / MaxRate bornent le rate accepté dans un roomState (§Assainissement).
	MinRate = 0.25
	MaxRate = 4.0
	// ChatQueueMax borne la file des messages de chat composés hors ligne : au
	// delà, les plus anciens sont abandonnés (docs/protocol.md §Erreurs et
	// robustesse, File d'attente hors ligne).
	ChatQueueMax = 20
	// VirginResumeSec : sur une salle vierge (jamais pilotée), un lecteur local
	// au-delà de ce seuil déclenche UNE reprise `control seek` à sa position.
	// En deçà, on considère que la séance n'avait pas commencé.
	VirginResumeSec = 5.0

	pingEvery   = 2 * time.Second
	reportEvery = time.Second

	offsetSamples = 5
	// driftSamples : taille de l'historique de dérive (5 polls ≈ 1 s). Le
	// micro-seek exige un historique PLEIN dont la médiane dépasse la zone
	// morte — c'est ce qui empêche un pic de bruit de déclencher un recalage
	// (docs/protocol.md §Persistance de la dérive).
	driftSamples = 5
)

// maxSessionTokenLen borne le jeton relu du fichier d'état : le serveur refuse
// au-delà (maxSessionLen côté server), autant ne pas le lui envoyer.
const maxSessionTokenLen = 128

// DevVersion est la version d'un binaire construit sans injection de VERSION :
// illisible en semver, donc jamais comparée (aucune bannière de mise à jour).
const DevVersion = "dev"

// ConnectRequest est la demande de connexion venue de l'UI.
type ConnectRequest struct {
	URL      string // URL ws:// ou wss:// complète (déjà normalisée)
	Name     string
	Room     string
	Password string
}

// Phase décrit l'état de la connexion au serveur.
type Phase string

const (
	PhaseIdle       Phase = "idle"
	PhaseConnecting Phase = "connecting"
	PhaseConnected  Phase = "connected"
)

// VLCSnapshot est la vue de VLC exposée à l'UI.
type VLCSnapshot struct {
	Running     bool    `json:"running"`
	Available   bool    `json:"available"`
	BinaryPath  string  `json:"binaryPath"`
	State       string  `json:"state"`
	PositionSec float64 `json:"positionSec"`
	DurationSec float64 `json:"durationSec"`
	Rate        float64 `json:"rate"`
	FilePath    string  `json:"filePath"`
	FileName    string  `json:"fileName"`
	Buffering   bool    `json:"buffering"`
	Error       string  `json:"error"`
}

// Snapshot est l'état complet poussé à l'UI.
type Snapshot struct {
	Phase Phase `json:"phase"`
	// Version est celle de ce client, ServerVersion celle annoncée par le
	// serveur dans le welcome, DownloadURL l'adresse d'une version à jour.
	Version       string          `json:"version"`
	ServerVersion string          `json:"serverVersion"`
	DownloadURL   string          `json:"downloadUrl"`
	ServerURL     string          `json:"serverUrl"`
	Room          string          `json:"room"`
	Name          string          `json:"name"`
	SelfID        string          `json:"selfId"`
	Users         []protocol.User `json:"users"`
	Ready         bool            `json:"ready"`
	Paused        bool            `json:"paused"`
	RoomPosition  float64         `json:"roomPositionSec"`
	RoomRate      float64         `json:"roomRate"`
	DriftSec      float64         `json:"driftSec"`
	Correcting    string          `json:"correcting"` // "", "seek"
	LatencyMs     int64           `json:"latencyMs"`
	ClockOffsetMs int64           `json:"clockOffsetMs"`
	Retrying      bool            `json:"retrying"`
	LastError     string          `json:"lastError"`
	// PendingChats est la file des messages composés hors ligne, dans l'ordre
	// d'envoi : l'UI les affiche « en attente » jusqu'à la reconnexion.
	PendingChats []string    `json:"pendingChats"`
	VLC          VLCSnapshot `json:"vlc"`
}

// Types de messages moteur → UI (canal local /ui, cf.
// docs/research/2026-08-05-ui-protocol-draft.md). Même enveloppe
// {type, data} que le protocole serveur.
const (
	EventState = "state"
	EventToast = "toast"
	EventChat  = "chat"
)

// Event est ce que le moteur pousse à l'UI. Le transport (internal/webui)
// l'encode en enveloppe {type, data}.
type Event struct {
	Kind  string
	State *Snapshot
	Toast *protocol.Toast
	Chat  *protocol.ChatEvent
}

// Payload est la donnée à placer dans le champ `data` de l'enveloppe.
func (e Event) Payload() any {
	switch e.Kind {
	case EventState:
		return e.State
	case EventToast:
		return e.Toast
	case EventChat:
		return e.Chat
	default:
		return struct{}{}
	}
}
