package protocol

import (
	"testing"
)

func TestRoundTripControl(t *testing.T) {
	raw, err := Encode(TypeControl, Control{Action: ActionSeek, PositionSec: 42.5})
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	env, err := Decode(raw)
	if err != nil {
		t.Fatalf("Decode: %v", err)
	}
	if env.Type != TypeControl {
		t.Fatalf("type = %q, attendu %q", env.Type, TypeControl)
	}
	c, err := DecodeData[Control](env)
	if err != nil {
		t.Fatalf("DecodeData: %v", err)
	}
	if c.Action != ActionSeek || c.PositionSec != 42.5 {
		t.Fatalf("round-trip incorrect: %+v", c)
	}
}

func TestRoundTripWelcome(t *testing.T) {
	in := Welcome{
		SelfID: "u1",
		Room:   "soirée",
		State:  RoomState{Paused: true, PositionSec: 12.25, Rate: 1, RefServerMs: 1000, SetBy: "u1"},
		Users: []User{
			{ID: "u1", Name: "thib", Ready: true, File: &FileInfo{Name: "ep1.mkv", DurationSec: 1200, SizeBytes: 700_000_000}},
			{ID: "u2", Name: "ami", LatencyMs: 35},
		},
	}
	raw, err := Encode(TypeWelcome, in)
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	env, err := Decode(raw)
	if err != nil {
		t.Fatalf("Decode: %v", err)
	}
	out, err := DecodeData[Welcome](env)
	if err != nil {
		t.Fatalf("DecodeData: %v", err)
	}
	if out.SelfID != in.SelfID || out.Room != in.Room || out.State != in.State {
		t.Fatalf("round-trip incorrect: %+v", out)
	}
	if len(out.Users) != 2 || out.Users[0].File == nil || out.Users[0].File.Name != "ep1.mkv" {
		t.Fatalf("users incorrects: %+v", out.Users)
	}
	if out.Users[1].File != nil {
		t.Fatalf("file de u2 devrait être nil (omitempty)")
	}
}

func TestDecodeErrors(t *testing.T) {
	if _, err := Decode([]byte("pas du json")); err == nil {
		t.Fatal("JSON invalide accepté")
	}
	if _, err := Decode([]byte(`{"data":{}}`)); err == nil {
		t.Fatal("enveloppe sans type acceptée")
	}
	env := Envelope{Type: TypePing, Data: []byte(`{"t":"pas un nombre"}`)}
	if _, err := DecodeData[Ping](env); err == nil {
		t.Fatal("data invalide acceptée")
	}
}
