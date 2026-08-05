package client

import (
	"time"

	"github.com/thibsix/vibesync/internal/protocol"
)

// Constantes de synchronisation (docs/protocol.md §Comportements client).
const (
	// PollInterval est la période d'interrogation de VLC.
	PollInterval = 200 * time.Millisecond
	// DeadZoneSec : en deçà, on ne corrige rien.
	DeadZoneSec = 0.1
	// HardSeekSec : au-delà, seek dur (puis affinage par nudge).
	HardSeekSec = 2.0
	// NudgeFast / NudgeSlow sont les facteurs de rate-nudge.
	NudgeFast = 1.05
	NudgeSlow = 0.95
	// NudgeExitSec : hystérésis du nudge — engagé au-delà de DeadZoneSec, il
	// ne se désengage qu'en dessous de ce seuil.
	NudgeExitSec = 0.03
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
	// MinRate / MaxRate bornent le rate accepté dans un roomState (§Assainissement).
	MinRate = 0.25
	MaxRate = 4.0

	pingEvery   = 2 * time.Second
	reportEvery = time.Second

	offsetSamples = 5
)

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
	Phase         Phase           `json:"phase"`
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
	Correcting    string          `json:"correcting"` // "", "nudge", "seek"
	LatencyMs     int64           `json:"latencyMs"`
	ClockOffsetMs int64           `json:"clockOffsetMs"`
	Retrying      bool            `json:"retrying"`
	LastError     string          `json:"lastError"`
	VLC           VLCSnapshot     `json:"vlc"`
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
