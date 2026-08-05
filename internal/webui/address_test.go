package webui

import "testing"

func TestNormalizeServerURL(t *testing.T) {
	cases := []struct {
		in, want string
	}{
		{"vibesync.exemple.fr", "wss://vibesync.exemple.fr/ws"},
		{"  vibesync.exemple.fr  ", "wss://vibesync.exemple.fr/ws"},
		{"vibesync.exemple.fr:8443", "wss://vibesync.exemple.fr:8443/ws"},
		{"localhost:8080", "ws://localhost:8080/ws"},
		{"127.0.0.1:8080", "ws://127.0.0.1:8080/ws"},
		{"ws://127.0.0.1:8080", "ws://127.0.0.1:8080/ws"},
		{"ws://127.0.0.1:8080/", "ws://127.0.0.1:8080/ws"},
		{"ws://127.0.0.1:8080/ws", "ws://127.0.0.1:8080/ws"},
		{"wss://vibesync.exemple.fr/ws", "wss://vibesync.exemple.fr/ws"},
		{"http://vibesync.exemple.fr", "ws://vibesync.exemple.fr/ws"},
		{"https://vibesync.exemple.fr", "wss://vibesync.exemple.fr/ws"},
		{"HTTPS://vibesync.exemple.fr", "wss://vibesync.exemple.fr/ws"},
		{"https://vibesync.exemple.fr/salon", "wss://vibesync.exemple.fr/salon"},
		{"wss://exemple.fr/ws#frag", "wss://exemple.fr/ws"},
	}
	for _, tc := range cases {
		got, err := NormalizeServerURL(tc.in)
		if err != nil {
			t.Errorf("%q: erreur inattendue: %v", tc.in, err)
			continue
		}
		if got != tc.want {
			t.Errorf("%q → %q, attendu %q", tc.in, got, tc.want)
		}
	}
}

func TestNormalizeServerURLErreurs(t *testing.T) {
	for _, in := range []string{"", "   ", "ftp://exemple.fr", "wss://"} {
		if got, err := NormalizeServerURL(in); err == nil {
			t.Errorf("%q accepté → %q", in, got)
		}
	}
}
