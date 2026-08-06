// Commande vibesync : le client desktop. Il câble la GUI locale (internal/webui),
// le moteur de synchronisation (internal/client) et le driver VLC (internal/vlc).
//
// La version applicative vient du fichier VERSION du repo, injectée au build :
//
//	go build -ldflags "-X main.appVersion=$(cat VERSION)" ./cmd/vibesync
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

// appVersion est renseignée au build (`-X main.appVersion=…`) depuis le fichier
// VERSION du repo ; « dev » pour un binaire construit à la main — et « dev »
// n'est pas une version comparable, donc aucune invitation à mettre à jour.
var appVersion = client.DevVersion

func main() {
	var (
		addr     = flag.String("ui-addr", "127.0.0.1:0", "adresse d'écoute de l'UI locale")
		headless = flag.Bool("headless", false, "ne pas ouvrir le navigateur (une UI native se connectera à /ui)")
		keepVLC  = flag.Bool("keep-vlc", false, "laisser VLC ouvert à la fermeture du client")
		debug    = flag.Bool("debug", false, "journal détaillé")
		version  = flag.Bool("version", false, "afficher la version et quitter")
		stateDir = flag.String("state-dir", "",
			"dossier de l'état persistant (défaut : dossier de config utilisateur)")
	)
	flag.Parse()
	if *version {
		fmt.Println(appVersion)
		return
	}

	level := slog.LevelInfo
	if *debug {
		level = slog.LevelDebug
	}
	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level}))
	logger.Info("vibesync", "version", appVersion)

	// Le jeton de reprise de session y est conservé d'un lancement à l'autre :
	// c'est lui qui permet de récupérer son pseudo tout de suite après une
	// fermeture, sans attendre l'expiration de la connexion précédente.
	dir := *stateDir
	if dir == "" {
		var err error
		if dir, err = client.DefaultStateDir(); err != nil {
			logger.Warn("état persistant indisponible, jeton de session non conservé", "err", err)
		}
	}

	engine := client.New(client.Config{
		Logger:      logger,
		KeepVLCOpen: *keepVLC,
		Version:     appVersion,
		StateDir:    dir,
	})
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
