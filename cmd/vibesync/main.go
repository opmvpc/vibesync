// Commande vibesync : le client desktop. Il câble la GUI locale (internal/webui),
// le moteur de synchronisation (internal/client) et le driver VLC (internal/vlc).
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/thibsix/vibesync/internal/client"
	"github.com/thibsix/vibesync/internal/webui"
)

func main() {
	var (
		addr     = flag.String("ui-addr", "127.0.0.1:0", "adresse d'écoute de l'UI locale")
		headless = flag.Bool("headless", false, "ne pas ouvrir le navigateur (une UI native se connectera à /ui)")
		keepVLC  = flag.Bool("keep-vlc", false, "laisser VLC ouvert à la fermeture du client")
		debug    = flag.Bool("debug", false, "journal détaillé")
	)
	flag.Parse()

	level := slog.LevelInfo
	if *debug {
		level = slog.LevelDebug
	}
	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level}))

	engine := client.New(client.Config{Logger: logger, KeepVLCOpen: *keepVLC})
	server, err := webui.New(engine, webui.Options{})
	if err != nil {
		logger.Error("démarrage de l'UI impossible", "err", err)
		os.Exit(1)
	}
	if err := server.Start(webui.Options{Addr: *addr}); err != nil {
		logger.Error("écoute impossible", "err", err)
		os.Exit(1)
	}

	// Ligne machine-lisible : une UI native s'y raccorde sans navigateur.
	handshake, _ := json.Marshal(map[string]any{
		"uiPort":  server.Port(),
		"uiToken": server.Token(),
		"uiURL":   server.URL(),
	})
	fmt.Println(string(handshake))

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	go engine.Run(ctx)

	if *headless {
		logger.Info("mode headless", "url", server.URL())
	} else if err := webui.OpenBrowser(server.URL()); err != nil {
		logger.Warn("navigateur non ouvert, allez-y à la main", "url", server.URL(), "err", err)
	}

	<-ctx.Done()
	logger.Info("arrêt en cours")

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if err := server.Shutdown(shutdownCtx); err != nil {
		logger.Warn("arrêt de l'UI", "err", err)
	}
	if err := engine.Close(); err != nil {
		logger.Warn("arrêt du moteur", "err", err)
	}
}
