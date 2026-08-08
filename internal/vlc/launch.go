package vlc

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"net"
	"os/exec"
	"runtime"
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
	args := launchArgs(runtime.GOOS, port, password)
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

// oneInstanceArgs : les trois drapeaux de la famille « instance unique »
// (libvlc-module.c les déclare dans un bloc conditionnel : Windows, ou Linux
// avec D-Bus). Le VLC macOS ne les connaît PAS et refuse de démarrer —
// « unknown option or missing mandatory argument » — vérifié sur VLC 3.0.23 :
// les douze autres drapeaux passent, ces trois-là sont rejetés un par un. Le
// client Swift fait la même exclusion. On les réserve donc à Windows, seule
// plateforme où le blindage VS-029 les a validés.
var oneInstanceArgs = []string{
	"--no-one-instance",
	"--no-one-instance-when-started-from-file",
	"--no-playlist-enqueue",
}

// launchArgs — TOUT ce dont on dépend est forcé explicitement (VS-029).
//
// Miroir du `vlc_build_command` du client C (core/src/vlc_core.c), qui porte
// le raisonnement complet. En résumé : le vlcrc de l'utilisateur gagne sur les
// défauts de VLC, jamais sur la ligne de commande, et un VLC configuré par
// Syncplay faisait échouer l'attache HTTP en laissant un VLC orphelin en
// lecture. Chaque drapeau neutralise un réglage qui peut venir du vlcrc :
//
//	--extraintf=http     l'interface de pilotage ; sur la ligne de commande
//	                     elle REMPLACE l'`extraintf` du vlcrc.
//	--lua-intf=http      filet si le vlcrc a fait de luaintf l'interface
//	                     PRINCIPALE : au moins c'est notre script http qui
//	                     s'exécute, pas syncplay.lua.
//	--no-one-instance / --no-one-instance-when-started-from-file
//	                     sinon le média part à l'instance VLC déjà ouverte —
//	                     qui joue — et notre port n'est écouté par personne.
//	                     Le second vaut vrai par défaut : cause racine la plus
//	                     probable. Windows seulement (voir oneInstanceArgs).
//	--no-playlist-enqueue    sinon le média est enfilé au lieu d'être ouvert.
//	                         Windows seulement (même bloc VLC).
//	--playlist-autostart     sinon rien ne démarre, le statut reste
//	                         « stopped » et Prepare tourne dans le vide.
//	--start-paused       l'autoplay est dompté AVANT l'attache : même si
//	                     l'attache échoue, rien ne part en lecture sauvage.
//	                     Prepare reste nécessaire (il constate l'état) mais
//	                     converge immédiatement. Sur macOS le drapeau est
//	                     accepté mais INOPÉRANT (VLC 3.0.23 démarre quand
//	                     même la lecture) : seul Prepare tranche.
//	--no-random --no-loop --no-repeat  le moteur de sync raisonne sur un média
//	                     unique joué une fois.
//	--no-play-and-exit   VLC ne doit pas disparaître en fin de média.
//	--no-video-title-show  confort, déjà là avant VS-029.
//
// Volontairement ABSENT : `--intf=<module>`. Forcer l'interface principale
// obligerait à parier sur son nom (qt/qt4/macosx selon version et OS) et un
// nom inconnu empêche VLC de démarrer.
func launchArgs(goos string, port int, password string) []string {
	args := []string{
		"--extraintf=http",
		"--lua-intf=http",
		"--http-host=127.0.0.1",
		fmt.Sprintf("--http-port=%d", port),
		"--http-password=" + password,
	}
	if goos == "windows" {
		args = append(args, oneInstanceArgs...)
	}
	return append(args,
		"--playlist-autostart",
		"--start-paused",
		"--no-random",
		"--no-loop",
		"--no-repeat",
		"--no-play-and-exit",
		"--no-video-title-show",
	)
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
