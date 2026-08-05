// Commande vibesync-server : serveur de salles de visionnage synchronisé.
//
// Configuration par l'environnement :
//   - VIBESYNC_ADDR           adresse d'écoute HTTP (défaut ":8080")
//   - VIBESYNC_PASSWORD       mot de passe global optionnel exigé dans le hello
//   - VIBESYNC_LOG            niveau de log : debug|info|warn|error (défaut info)
//   - VIBESYNC_MAX_CLIENTS    connexions simultanées (défaut 200)
//   - VIBESYNC_MAX_ROOMS      salles vivantes (défaut 50)
//   - VIBESYNC_MAX_ROOM_SIZE  membres par salle (défaut 20)
package main

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/thibsix/vibesync/internal/server"
)

func main() {
	level := server.LogLevelFromEnv()
	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level}))
	slog.SetDefault(logger)

	cfg := server.ConfigFromEnv()
	srv := server.New(cfg, server.WithLogger(logger))

	httpSrv := &http.Server{
		Addr:              cfg.Addr,
		Handler:           srv.Handler(),
		ReadHeaderTimeout: 10 * time.Second,
		// Pas de ReadTimeout/WriteTimeout : ils casseraient les connexions
		// WebSocket longue durée (deadlines gérées par le serveur lui-même).
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	errCh := make(chan error, 1)
	go func() {
		logger.Info("démarrage du serveur vibesync", "addr", cfg.Addr,
			"password", cfg.Password != "", "log", level.String(),
			"maxClients", cfg.MaxClients, "maxRooms", cfg.MaxRooms, "maxRoomSize", cfg.MaxRoomSize)
		if err := httpSrv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			errCh <- err
			return
		}
		errCh <- nil
	}()

	select {
	case err := <-errCh:
		if err != nil {
			logger.Error("serveur HTTP arrêté sur erreur", "err", err)
			os.Exit(1)
		}
	case <-ctx.Done():
		logger.Info("signal d'arrêt reçu, fermeture en cours")
		shutdownCtx, cancel := context.WithTimeout(context.Background(), server.ShutdownGrace)
		defer cancel()
		if err := httpSrv.Shutdown(shutdownCtx); err != nil {
			logger.Warn("arrêt non gracieux", "err", err)
		}
	}
	logger.Info("serveur arrêté")
}
