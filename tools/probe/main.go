// probe vérifie un serveur vibesync depuis l'extérieur : TLS/wss via le proxy,
// version du protocole, mot de passe, latence. Usage :
//
//	go run ./tools/probe wss://hote/ws [motdepasse]
package main

import (
	"context"
	"fmt"
	"os"
	"time"

	"github.com/thibsix/vibesync/internal/protocol"
	"github.com/thibsix/vibesync/internal/ws"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: probe wss://hote/ws [motdepasse]")
		os.Exit(2)
	}
	url := os.Args[1]
	password := ""
	if len(os.Args) > 2 {
		password = os.Args[2]
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	start := time.Now()
	conn, err := ws.Dial(ctx, url)
	if err != nil {
		fmt.Printf("dial: ÉCHEC (%v)\n", err)
		os.Exit(1)
	}
	defer func() { _ = conn.Close(1000, "probe terminé") }()
	fmt.Printf("dial: OK en %v\n", time.Since(start).Round(time.Millisecond))

	hello, _ := protocol.Encode(protocol.TypeHello, protocol.Hello{
		Version: protocol.Version, Name: "probe", Room: "probe-diagnostic", Password: password,
	})
	_ = conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	if err := conn.WriteMessage(ws.TextMessage, hello); err != nil {
		fmt.Printf("hello: ÉCHEC écriture (%v)\n", err)
		os.Exit(1)
	}
	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	_, raw, err := conn.ReadMessage()
	if err != nil {
		fmt.Printf("réponse: ÉCHEC lecture (%v)\n", err)
		os.Exit(1)
	}
	env, err := protocol.Decode(raw)
	if err != nil {
		fmt.Printf("réponse: indéchiffrable (%v)\n", err)
		os.Exit(1)
	}
	switch env.Type {
	case protocol.TypeWelcome:
		w, _ := protocol.DecodeData[protocol.Welcome](env)
		fmt.Printf("welcome: OK (id %s, salle %q) — accès %s\n", w.SelfID, w.Room,
			map[bool]string{true: "AVEC mot de passe", false: "SANS mot de passe"}[password != ""])
	case protocol.TypeError:
		e, _ := protocol.DecodeData[protocol.ErrorMsg](env)
		fmt.Printf("erreur serveur: code=%s (%s)\n", e.Code, e.Text)
	default:
		fmt.Printf("réponse inattendue: %s\n", env.Type)
	}
}
