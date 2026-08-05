package ws

import (
	"bufio"
	"errors"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

// TestAcceptKeyRFCVector rejoue le vecteur officiel de la RFC 6455 §1.3.
func TestAcceptKeyRFCVector(t *testing.T) {
	const key = "dGhlIHNhbXBsZSBub25jZQ=="
	const want = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
	if got := acceptKey(key); got != want {
		t.Fatalf("acceptKey = %q, attendu %q", got, want)
	}
}

func TestValidClientKey(t *testing.T) {
	cases := map[string]bool{
		"dGhlIHNhbXBsZSBub25jZQ==": true,
		"":                         false,
		"pas du base64!":           false,
		"c2hvcnQ=":                 false, // 5 octets
	}
	for key, want := range cases {
		if got := validClientKey(key); got != want {
			t.Fatalf("validClientKey(%q) = %v", key, got)
		}
	}
}

func TestHeaderContainsToken(t *testing.T) {
	h := http.Header{}
	h.Set("Connection", "keep-alive, Upgrade")
	h.Add("Upgrade", "WebSocket")
	h.Add("Autre", "a,b")
	if !headerContainsToken(h, "Connection", "upgrade") {
		t.Fatal("token upgrade non trouvé dans une liste")
	}
	if !headerContainsToken(h, "Upgrade", "websocket") {
		t.Fatal("comparaison insensible à la casse cassée")
	}
	if !headerContainsToken(h, "Autre", "b") {
		t.Fatal("dernier token de la liste non trouvé")
	}
	if headerContainsToken(h, "Connection", "close") {
		t.Fatal("token absent trouvé")
	}
	if headerContainsToken(h, "Absente", "x") {
		t.Fatal("en-tête absente trouvée")
	}
}

// upgradeTestServer démarre un serveur dont le handler tente l'upgrade et
// renvoie l'erreur éventuelle sur un canal.
func upgradeTestServer(t *testing.T) (*httptest.Server, <-chan error) {
	t.Helper()
	errCh := make(chan error, 8)
	var mu sync.Mutex
	var conns []*Conn
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		c, err := Upgrade(w, r)
		if err != nil {
			errCh <- err
			return
		}
		// La connexion est hijackée : le handler peut rendre la main.
		mu.Lock()
		conns = append(conns, c)
		mu.Unlock()
		errCh <- nil
	}))
	t.Cleanup(func() {
		srv.Close()
		mu.Lock()
		defer mu.Unlock()
		for _, c := range conns {
			_ = c.CloseNow()
		}
	})
	return srv, errCh
}

func rawRequest(t *testing.T, addr, req string) *http.Response {
	t.Helper()
	nc, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	t.Cleanup(func() { _ = nc.Close() })
	_ = nc.SetDeadline(time.Now().Add(5 * time.Second))
	if _, err := nc.Write([]byte(req)); err != nil {
		t.Fatalf("write: %v", err)
	}
	resp, err := http.ReadResponse(bufio.NewReader(nc), &http.Request{Method: http.MethodGet})
	if err != nil {
		t.Fatalf("read response: %v", err)
	}
	return resp
}

func TestUpgradeRejectsBadRequests(t *testing.T) {
	srv, errCh := upgradeTestServer(t)
	addr := srv.Listener.Addr().String()
	const goodKey = "dGhlIHNhbXBsZSBub25jZQ=="

	tests := []struct {
		name   string
		req    string
		status int
	}{
		{"méthode POST", "POST /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + goodKey + "\r\nContent-Length: 0\r\n\r\n", http.StatusMethodNotAllowed},
		{"Connection absente", "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + goodKey + "\r\n\r\n", http.StatusBadRequest},
		{"Upgrade absente", "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + goodKey + "\r\n\r\n", http.StatusBadRequest},
		{"mauvaise version", "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 8\r\nSec-WebSocket-Key: " + goodKey + "\r\n\r\n", http.StatusUpgradeRequired},
		{"clé absente", "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\r\n", http.StatusBadRequest},
		{"clé malformée", "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: abcd\r\n\r\n", http.StatusBadRequest},
		{"clé dupliquée", "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + goodKey + "\r\nSec-WebSocket-Key: " + goodKey + "\r\n\r\n", http.StatusBadRequest},
		{"version dupliquée", "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + goodKey + "\r\n\r\n", http.StatusBadRequest},
		{"version absente", "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + goodKey + "\r\n\r\n", http.StatusBadRequest},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			resp := rawRequest(t, addr, tc.req)
			if resp.StatusCode != tc.status {
				t.Fatalf("statut = %d, attendu %d", resp.StatusCode, tc.status)
			}
			if tc.status == http.StatusUpgradeRequired && resp.Header.Get("Sec-WebSocket-Version") != "13" {
				t.Fatal("426 sans en-tête Sec-WebSocket-Version: 13")
			}
			select {
			case err := <-errCh:
				var he *HandshakeError
				if !errors.As(err, &he) {
					t.Fatalf("erreur du handler = %v, attendu *HandshakeError", err)
				}
				if he.Status != tc.status {
					t.Fatalf("HandshakeError.Status = %d", he.Status)
				}
			case <-time.After(5 * time.Second):
				t.Fatal("le handler n'a pas rendu la main")
			}
		})
	}
}

func TestUpgradeAcceptsValidRequest(t *testing.T) {
	srv, errCh := upgradeTestServer(t)
	const key = "dGhlIHNhbXBsZSBub25jZQ=="
	req := "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\n" +
		"Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key + "\r\n\r\n"
	resp := rawRequest(t, srv.Listener.Addr().String(), req)
	if resp.StatusCode != http.StatusSwitchingProtocols {
		t.Fatalf("statut = %d", resp.StatusCode)
	}
	if got := resp.Header.Get("Sec-WebSocket-Accept"); got != "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" {
		t.Fatalf("Sec-WebSocket-Accept = %q", got)
	}
	if !strings.EqualFold(resp.Header.Get("Upgrade"), "websocket") {
		t.Fatalf("Upgrade = %q", resp.Header.Get("Upgrade"))
	}
	if err := <-errCh; err != nil {
		t.Fatalf("Upgrade: %v", err)
	}
}
