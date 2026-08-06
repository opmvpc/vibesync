package vlc

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"net"
	"os/exec"
	"sync"
	"time"
)

// Process est un VLC lancé par nous, piloté par son interface HTTP.
type Process struct {
	*HTTPClient
	cmd  *exec.Cmd
	port int

	closeOnce sync.Once
	closeErr  error
	// KeepAlive à true : Close() ne tue pas VLC (l'utilisateur garde sa fenêtre).
	KeepAlive bool
}

var _ Controller = (*Process)(nil)

// LaunchOptions paramètre le démarrage de VLC.
type LaunchOptions struct {
	// Binary force l'exécutable ; vide = Locate().
	Binary string
	// FilePath est le média à ouvrir (obligatoire).
	FilePath string
	// ExtraArgs sont ajoutés avant le fichier.
	ExtraArgs []string
	// ReadyTimeout borne l'attente de l'interface HTTP (défaut 20 s).
	ReadyTimeout time.Duration
	// PrepareTimeout borne la mise en pause initiale du média (défaut 15 s).
	PrepareTimeout time.Duration
	// KeepAlive : ne pas tuer VLC à la fermeture du client.
	KeepAlive bool
}

// Launch démarre VLC avec son interface HTTP locale (port et mot de passe
// aléatoires, écoute sur 127.0.0.1 uniquement) et attend qu'elle réponde.
func Launch(ctx context.Context, opts LaunchOptions) (*Process, error) {
	if opts.FilePath == "" {
		return nil, fmt.Errorf("vlc: aucun fichier à ouvrir")
	}
	bin := opts.Binary
	if bin == "" {
		var err error
		if bin, err = Locate(); err != nil {
			return nil, err
		}
	}
	port, err := freePort()
	if err != nil {
		return nil, err
	}
	password, err := randomPassword()
	if err != nil {
		return nil, err
	}
	args := []string{
		"--extraintf=http",
		"--http-host=127.0.0.1",
		fmt.Sprintf("--http-port=%d", port),
		"--http-password=" + password,
		"--no-video-title-show",
		"--no-one-instance",
	}
	args = append(args, opts.ExtraArgs...)
	args = append(args, opts.FilePath)

	cmd := exec.Command(bin, args...)
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("vlc: démarrage de %q: %w", bin, err)
	}
	p := &Process{
		HTTPClient: NewHTTPClient(fmt.Sprintf("http://127.0.0.1:%d", port), password),
		cmd:        cmd,
		port:       port,
		KeepAlive:  opts.KeepAlive,
	}
	timeout := opts.ReadyTimeout
	if timeout <= 0 {
		timeout = 20 * time.Second
	}
	if err := p.WaitReady(ctx, timeout); err != nil {
		_ = p.Close()
		return nil, err
	}
	// VLC démarre la lecture tout seul à l'ouverture : on ne rend la main
	// qu'une fois le média effectivement arrêté au début (docs/protocol.md
	// §Chargement de fichier). Tant que ce n'est pas fait, le média n'est pas
	// considéré comme chargé et le moteur n'annonce pas le fichier.
	if err := Prepare(ctx, p, opts.PrepareTimeout); err != nil {
		_ = p.Close()
		return nil, err
	}
	return p, nil
}

// Port est le port de l'interface HTTP de ce VLC.
func (p *Process) Port() int { return p.port }

// Close arrête VLC (sauf si KeepAlive).
func (p *Process) Close() error {
	p.closeOnce.Do(func() {
		if p.cmd == nil || p.cmd.Process == nil {
			return
		}
		if p.KeepAlive {
			return
		}
		if err := p.cmd.Process.Kill(); err != nil {
			p.closeErr = fmt.Errorf("vlc: arrêt du process: %w", err)
		}
		_ = p.cmd.Wait()
	})
	return p.closeErr
}

// freePort réserve puis relâche un port libre sur la loopback.
func freePort() (int, error) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return 0, fmt.Errorf("vlc: pas de port libre: %w", err)
	}
	port := ln.Addr().(*net.TCPAddr).Port
	if err := ln.Close(); err != nil {
		return 0, fmt.Errorf("vlc: libération du port: %w", err)
	}
	return port, nil
}

func randomPassword() (string, error) {
	buf := make([]byte, 16)
	if _, err := rand.Read(buf); err != nil {
		return "", fmt.Errorf("vlc: génération du mot de passe: %w", err)
	}
	return hex.EncodeToString(buf), nil
}
