package webui

import (
	"context"
	"encoding/json"
	"net/http"
	"time"

	"github.com/thibsix/vibesync/internal/client"
	"github.com/thibsix/vibesync/internal/protocol"
	"github.com/thibsix/vibesync/internal/ws"
)

// Types de messages du canal /ui (UI → moteur).
const (
	CmdConnect    = "connect"
	CmdDisconnect = "disconnect"
	CmdOpenFile   = "openFile"
	CmdSetReady   = "setReady"
	CmdPlay       = "play"
	CmdPause      = "pause"
	CmdSeek       = "seek"
	CmdChat       = "chat"
	CmdBrowse     = "browse"
)

// Types de messages du canal /ui (moteur → UI).
const (
	MsgHello  = "hello"
	MsgState  = client.EventState
	MsgToast  = client.EventToast
	MsgChat   = client.EventChat
	MsgBrowse = "browse"
	MsgError  = "error"
)

// UIVersion est la version du contrat /ui.
const UIVersion = 1

// --- Charges utiles UI → moteur ---

// ConnectCmd demande la connexion à un serveur vibesync.
type ConnectCmd struct {
	Server   string `json:"server"`
	Name     string `json:"name"`
	Room     string `json:"room"`
	Password string `json:"password"`
}

// OpenFileCmd ouvre un fichier local dans VLC.
type OpenFileCmd struct {
	Path string `json:"path"`
}

// SetReadyCmd fixe l'état « prêt » (idempotent, pas un toggle).
type SetReadyCmd struct {
	Ready bool `json:"ready"`
}

// SeekCmd demande un saut de position volontaire.
type SeekCmd struct {
	PositionSec float64 `json:"positionSec"`
}

// ChatCmd envoie un message de salle.
type ChatCmd struct {
	Text string `json:"text"`
}

// BrowseCmd liste un dossier ("" = dossier personnel).
type BrowseCmd struct {
	Path string `json:"path"`
}

// --- Charges utiles moteur → UI ---

// HelloMsg est le premier message envoyé à l'UI.
type HelloMsg struct {
	UIVersion       int    `json:"uiVersion"`
	ProtocolVersion int    `json:"protocolVersion"`
	OS              string `json:"os"`
}

// ErrorMsg signale une erreur locale (pas une erreur serveur).
type ErrorMsg struct {
	Code string `json:"code"`
	Text string `json:"text"`
}

type uiConn struct {
	ws  *ws.Conn
	out chan []byte
}

func (u *uiConn) send(msgType string, data any) {
	raw, err := protocol.Encode(msgType, data)
	if err != nil {
		return
	}
	select {
	case u.out <- raw:
	default: // UI trop lente : on saute
	}
}

func (s *Server) handleUI(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		http.Error(w, "token invalide", http.StatusUnauthorized)
		return
	}
	// Contrôle d'origine : internal/ws le laisse à l'appelant (une UI native
	// n'envoie pas d'Origin ; un navigateur doit venir de la boucle locale).
	if !checkLocalOrigin(r) {
		http.Error(w, "origine non autorisée", http.StatusForbidden)
		return
	}
	conn, err := ws.Upgrade(w, r)
	if err != nil {
		return
	}
	// Le pong de réponse à un ping de l'UI est émis depuis la boucle de
	// lecture : il lui faut sa propre échéance d'écriture, celle posée par la
	// pompe d'écriture pouvant être expirée.
	conn.AutoWriteTimeout = 10 * time.Second
	u := &uiConn{ws: conn, out: make(chan []byte, 64)}
	events, cancel := s.eng.Subscribe()
	defer cancel()

	ctx, stop := context.WithCancel(r.Context())
	defer stop()

	// Pompe d'écriture : un seul écrivain sur la connexion.
	go func() {
		defer func() { _ = conn.CloseNow() }()
		for {
			select {
			case <-ctx.Done():
				return
			case raw, ok := <-u.out:
				if !ok {
					return
				}
				_ = conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
				if err := conn.WriteMessage(ws.TextMessage, raw); err != nil {
					return
				}
			}
		}
	}()

	// Relais des événements du moteur.
	go func() {
		for {
			select {
			case <-ctx.Done():
				return
			case ev, ok := <-events:
				if !ok {
					return
				}
				u.send(ev.Kind, ev.Payload())
			}
		}
	}()

	u.send(MsgHello, HelloMsg{UIVersion: UIVersion, ProtocolVersion: protocol.Version, OS: goos()})
	snap := s.eng.Snapshot()
	u.send(MsgState, &snap)

	for {
		_, raw, err := conn.ReadMessage()
		if err != nil {
			return
		}
		s.handleCommand(ctx, u, raw)
	}
}

// handleCommand exécute une commande venue de l'UI.
func (s *Server) handleCommand(ctx context.Context, u *uiConn, raw []byte) {
	env, err := protocol.Decode(raw)
	if err != nil {
		u.send(MsgError, ErrorMsg{Code: "protocol", Text: err.Error()})
		return
	}
	switch env.Type {
	case CmdConnect:
		cmd, err := protocol.DecodeData[ConnectCmd](env)
		if err != nil {
			u.send(MsgError, ErrorMsg{Code: "protocol", Text: err.Error()})
			return
		}
		url, err := NormalizeServerURL(cmd.Server)
		if err != nil {
			u.send(MsgError, ErrorMsg{Code: "badServer", Text: err.Error()})
			return
		}
		if cmd.Name == "" || cmd.Room == "" {
			u.send(MsgError, ErrorMsg{Code: "badRequest", Text: "pseudo et salle sont obligatoires"})
			return
		}
		s.eng.Connect(client.ConnectRequest{
			URL:      url,
			Name:     cmd.Name,
			Room:     cmd.Room,
			Password: cmd.Password,
		})
	case CmdDisconnect:
		s.eng.Disconnect()
	case CmdOpenFile:
		cmd, err := protocol.DecodeData[OpenFileCmd](env)
		if err != nil {
			u.send(MsgError, ErrorMsg{Code: "protocol", Text: err.Error()})
			return
		}
		if err := s.eng.OpenFile(ctx, cmd.Path); err != nil {
			u.send(MsgError, ErrorMsg{Code: "vlc", Text: err.Error()})
		}
	case CmdSetReady:
		if cmd, err := protocol.DecodeData[SetReadyCmd](env); err == nil {
			s.eng.SetReady(cmd.Ready)
		}
	case CmdPlay:
		s.eng.Play()
	case CmdPause:
		s.eng.Pause()
	case CmdSeek:
		if cmd, err := protocol.DecodeData[SeekCmd](env); err == nil {
			s.eng.Seek(cmd.PositionSec)
		}
	case CmdChat:
		if cmd, err := protocol.DecodeData[ChatCmd](env); err == nil {
			s.eng.Chat(cmd.Text)
		}
	case CmdBrowse:
		cmd, _ := protocol.DecodeData[BrowseCmd](env)
		listing, err := Browse(cmd.Path)
		if err != nil {
			u.send(MsgError, ErrorMsg{Code: "fs", Text: err.Error()})
			return
		}
		u.send(MsgBrowse, listing)
	default:
		u.send(MsgError, ErrorMsg{Code: "unknown", Text: "commande inconnue: " + env.Type})
	}
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}
