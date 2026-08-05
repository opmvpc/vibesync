package vlc

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

// HTTPClient parle à l'interface HTTP de VLC (`/requests/status.json`).
// L'authentification est un basic auth avec un utilisateur vide.
type HTTPClient struct {
	base     string
	password string
	hc       *http.Client
}

var _ Controller = (*HTTPClient)(nil)

// NewHTTPClient construit un client sur une base du type "http://127.0.0.1:8080".
func NewHTTPClient(base, password string) *HTTPClient {
	base = strings.TrimSuffix(base, "/")
	return &HTTPClient{
		base:     base,
		password: password,
		hc: &http.Client{
			Timeout: 3 * time.Second,
			Transport: &http.Transport{
				DialContext:         (&net.Dialer{Timeout: time.Second}).DialContext,
				MaxIdleConnsPerHost: 4,
				DisableCompression:  true,
			},
		},
	}
}

// BaseURL renvoie l'URL de l'interface HTTP pilotée.
func (c *HTTPClient) BaseURL() string { return c.base }

// statusJSON est le sous-ensemble de status.json qui nous intéresse.
type statusJSON struct {
	State       string  `json:"state"`
	Position    float64 `json:"position"`
	Length      float64 `json:"length"`
	Time        float64 `json:"time"`
	Rate        float64 `json:"rate"`
	Information struct {
		Category struct {
			Meta map[string]any `json:"meta"`
		} `json:"category"`
	} `json:"information"`
}

func (c *HTTPClient) call(ctx context.Context, params url.Values) (Status, error) {
	u := c.base + "/requests/status.json"
	if len(params) > 0 {
		u += "?" + params.Encode()
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u, nil)
	if err != nil {
		return Status{}, fmt.Errorf("vlc: requête: %w", err)
	}
	req.SetBasicAuth("", c.password)
	resp, err := c.hc.Do(req)
	if err != nil {
		return Status{}, fmt.Errorf("vlc: interface HTTP injoignable: %w", err)
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode == http.StatusUnauthorized {
		return Status{}, fmt.Errorf("vlc: authentification refusée par l'interface HTTP")
	}
	if resp.StatusCode != http.StatusOK {
		return Status{}, fmt.Errorf("vlc: status HTTP %d", resp.StatusCode)
	}
	var raw statusJSON
	if err := json.NewDecoder(resp.Body).Decode(&raw); err != nil {
		return Status{}, fmt.Errorf("vlc: status.json illisible: %w", err)
	}
	return statusFrom(raw), nil
}

// statusFrom convertit et assainit la réponse de VLC : toute valeur non finie
// ou hors bornes est ramenée à une valeur sûre (docs/protocol.md §Assainissement).
func statusFrom(raw statusJSON) Status {
	st := Status{State: StateStopped}
	if finite(raw.Length) && raw.Length > 0 {
		st.LengthSec = raw.Length
	}
	if finite(raw.Rate) && raw.Rate > 0 {
		st.Rate = raw.Rate
	} else {
		st.Rate = 1
	}
	switch strings.ToLower(raw.State) {
	case "playing":
		st.State = StatePlaying
	case "paused":
		st.State = StatePaused
	default:
		st.State = StateStopped
	}
	switch {
	case st.LengthSec > 0:
		// Position fine : `time` n'a qu'une résolution d'une seconde. La
		// fraction rendue par VLC est bornée à [0,1] avant multiplication.
		st.PositionSec = clamp01(raw.Position) * st.LengthSec
	case finite(raw.Time) && raw.Time > 0:
		st.PositionSec = raw.Time
	}
	st.FileName = metaFileName(raw.Information.Category.Meta)
	return st
}

func finite(v float64) bool { return !math.IsNaN(v) && !math.IsInf(v, 0) }

func clamp01(v float64) float64 {
	if !finite(v) || v < 0 {
		return 0
	}
	if v > 1 {
		return 1
	}
	return v
}

func metaFileName(meta map[string]any) string {
	for _, key := range []string{"filename", "title", "now_playing"} {
		if v, ok := meta[key]; ok {
			if s, ok := v.(string); ok && strings.TrimSpace(s) != "" {
				return s
			}
		}
	}
	return ""
}

// Status interroge VLC sans lui envoyer de commande.
func (c *HTTPClient) Status(ctx context.Context) (Status, error) {
	return c.call(ctx, nil)
}

func (c *HTTPClient) command(ctx context.Context, name string, val string) error {
	params := url.Values{"command": {name}}
	if val != "" {
		params.Set("val", val)
	}
	_, err := c.call(ctx, params)
	return err
}

// Pause force la pause.
func (c *HTTPClient) Pause(ctx context.Context) error {
	return c.command(ctx, "pl_forcepause", "")
}

// Resume force la reprise.
func (c *HTTPClient) Resume(ctx context.Context) error {
	return c.command(ctx, "pl_forceresume", "")
}

// Seek saute à une position absolue (arrondie à la seconde, limite de l'API HTTP).
func (c *HTTPClient) Seek(ctx context.Context, positionSec float64) error {
	if positionSec < 0 || math.IsNaN(positionSec) {
		positionSec = 0
	}
	return c.command(ctx, "seek", strconv.Itoa(int(math.Round(positionSec))))
}

// SetRate change la vitesse de lecture.
func (c *HTTPClient) SetRate(ctx context.Context, rate float64) error {
	if rate <= 0 || math.IsNaN(rate) {
		rate = 1
	}
	return c.command(ctx, "rate", strconv.FormatFloat(rate, 'f', -1, 64))
}

// Close ne fait rien : le client HTTP ne possède pas le process VLC.
func (c *HTTPClient) Close() error { return nil }

// WaitReady attend que l'interface HTTP réponde (VLC met un instant à démarrer).
func (c *HTTPClient) WaitReady(ctx context.Context, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	var last error
	for {
		reqCtx, cancel := context.WithTimeout(ctx, 500*time.Millisecond)
		_, err := c.Status(reqCtx)
		cancel()
		if err == nil {
			return nil
		}
		last = err
		if ctx.Err() != nil {
			return ctx.Err()
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("vlc: interface HTTP muette après %s: %w", timeout, last)
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(100 * time.Millisecond):
		}
	}
}
