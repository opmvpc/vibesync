package webui

import (
	"errors"
	"fmt"
	"net"
	"net/url"
	"strings"
)

// NormalizeServerURL accepte ce qu'un humain tape (« hote », « hote:port »,
// « ws(s)://… », « http(s)://… ») et renvoie une URL WebSocket complète.
//
// Règles : schéma absent → wss (ws pour une adresse locale, sinon impossible
// de tester sans TLS) ; http→ws, https→wss ; chemin absent → /ws.
func NormalizeServerURL(raw string) (string, error) {
	s := strings.TrimSpace(raw)
	if s == "" {
		return "", errors.New("adresse du serveur vide")
	}
	if !strings.Contains(s, "://") {
		s = defaultScheme(s) + "://" + strings.TrimPrefix(s, "//")
	}
	u, err := url.Parse(s)
	if err != nil {
		return "", fmt.Errorf("adresse invalide: %w", err)
	}
	switch strings.ToLower(u.Scheme) {
	case "ws", "wss":
		u.Scheme = strings.ToLower(u.Scheme)
	case "http":
		u.Scheme = "ws"
	case "https":
		u.Scheme = "wss"
	default:
		return "", fmt.Errorf("schéma %q non supporté (attendu ws, wss, http ou https)", u.Scheme)
	}
	if u.Host == "" {
		return "", errors.New("hôte manquant dans l'adresse du serveur")
	}
	if u.Path == "" || u.Path == "/" {
		u.Path = "/ws"
	}
	u.Fragment = ""
	u.User = nil
	return u.String(), nil
}

func defaultScheme(hostish string) string {
	host := strings.TrimPrefix(hostish, "//")
	if i := strings.IndexAny(host, "/?#"); i >= 0 {
		host = host[:i]
	}
	if h, _, err := net.SplitHostPort(host); err == nil {
		host = h
	}
	host = strings.Trim(host, "[]")
	switch strings.ToLower(host) {
	case "localhost", "127.0.0.1", "::1":
		return "ws"
	}
	return "wss"
}
