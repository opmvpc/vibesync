// Package protocol est le miroir Go de docs/protocol.md (source de vérité).
// Toute évolution passe par la spec d'abord.
package protocol

import (
	"encoding/json"
	"fmt"
)

// Version du protocole ; un hello avec une autre version est refusé.
const Version = 1

// Types de messages client → serveur.
const (
	TypeHello    = "hello"
	TypePing     = "ping"
	TypeSetReady = "setReady"
	TypeSetFile  = "setFile"
	TypeControl  = "control"
	TypeReport   = "report"
	TypeChat     = "chat"
)

// Types de messages serveur → client.
const (
	TypeWelcome   = "welcome"
	TypePong      = "pong"
	TypeRoomState = "roomState"
	TypeUsers     = "users"
	TypeChatEvent = "chatEvent"
	TypeToast     = "toast"
	TypeError     = "error"
)

// Actions de Control.
const (
	ActionPlay  = "play"
	ActionPause = "pause"
	ActionSeek  = "seek"
)

// Niveaux de Toast.
const (
	LevelInfo  = "info"
	LevelWarn  = "warn"
	LevelError = "error"
)

// Codes d'Error.
const (
	ErrVersionMismatch = "version_mismatch"
	ErrBadPassword     = "bad_password"
	ErrNameTaken       = "name_taken"
	ErrProtocol        = "protocol"
)

// Envelope enveloppe tout message échangé sur le WebSocket.
type Envelope struct {
	Type string          `json:"type"`
	Data json.RawMessage `json:"data"`
}

// --- Client → serveur ---

type Hello struct {
	Version  int    `json:"version"`
	Name     string `json:"name"`
	Room     string `json:"room"`
	Password string `json:"password,omitempty"`
}

type Ping struct {
	T int64 `json:"t"` // clientMs
}

type SetReady struct {
	Ready bool `json:"ready"`
}

type SetFile struct {
	Name        string  `json:"name"`
	DurationSec float64 `json:"durationSec"`
	SizeBytes   int64   `json:"sizeBytes"`
}

type Control struct {
	Action      string  `json:"action"`
	PositionSec float64 `json:"positionSec"`
}

type Report struct {
	PositionSec float64 `json:"positionSec"`
	Paused      bool    `json:"paused"`
	Buffering   bool    `json:"buffering"`
}

type Chat struct {
	Text string `json:"text"`
}

// --- Serveur → client ---

type FileInfo struct {
	Name        string  `json:"name"`
	DurationSec float64 `json:"durationSec"`
	SizeBytes   int64   `json:"sizeBytes"`
}

type User struct {
	ID          string    `json:"id"`
	Name        string    `json:"name"`
	Ready       bool      `json:"ready"`
	File        *FileInfo `json:"file,omitempty"`
	PositionSec float64   `json:"positionSec"`
	LatencyMs   int64     `json:"latencyMs"`
}

// RoomState est l'état autoritatif d'une salle. Position courante en lecture :
// PositionSec + (nowServerMs-RefServerMs)/1000 × Rate.
type RoomState struct {
	Paused      bool    `json:"paused"`
	PositionSec float64 `json:"positionSec"`
	Rate        float64 `json:"rate"`
	RefServerMs int64   `json:"refServerMs"`
	SetBy       string  `json:"setBy"`
}

type Welcome struct {
	SelfID string    `json:"selfId"`
	Room   string    `json:"room"`
	State  RoomState `json:"state"`
	Users  []User    `json:"users"`
}

type Pong struct {
	T        int64 `json:"t"` // echo du clientMs du Ping
	ServerMs int64 `json:"serverMs"`
}

type UsersMsg struct {
	Users []User `json:"users"`
}

type ChatEvent struct {
	From     string `json:"from"`
	Text     string `json:"text"`
	ServerMs int64  `json:"serverMs"`
}

type Toast struct {
	Level string `json:"level"`
	Text  string `json:"text"`
}

type ErrorMsg struct {
	Code string `json:"code"`
	Text string `json:"text"`
}

// Encode fabrique le JSON d'une enveloppe {type, data}.
func Encode(msgType string, data any) ([]byte, error) {
	raw, err := json.Marshal(data)
	if err != nil {
		return nil, fmt.Errorf("protocol: encode data de %q: %w", msgType, err)
	}
	return json.Marshal(Envelope{Type: msgType, Data: raw})
}

// Decode parse une enveloppe ; Data reste brut, à décoder selon Type.
func Decode(raw []byte) (Envelope, error) {
	var env Envelope
	if err := json.Unmarshal(raw, &env); err != nil {
		return Envelope{}, fmt.Errorf("protocol: enveloppe invalide: %w", err)
	}
	if env.Type == "" {
		return Envelope{}, fmt.Errorf("protocol: enveloppe sans type")
	}
	return env, nil
}

// DecodeData décode le champ Data d'une enveloppe vers le type attendu.
func DecodeData[T any](env Envelope) (T, error) {
	var v T
	if err := json.Unmarshal(env.Data, &v); err != nil {
		return v, fmt.Errorf("protocol: data invalide pour %q: %w", env.Type, err)
	}
	return v, nil
}
