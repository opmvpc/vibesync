package ws

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"math"
	"net"
	"runtime"
	"strings"
	"testing"
	"time"
)

// --- harnais mémoire ---------------------------------------------------

// memConn est un net.Conn minimal branché sur des tampons mémoire : il permet
// de tester le framing sans pile réseau ni goroutine.
type memConn struct {
	r io.Reader
	w bytes.Buffer
}

func (m *memConn) Read(p []byte) (int, error)  { return m.r.Read(p) }
func (m *memConn) Write(p []byte) (int, error) { return m.w.Write(p) }
func (m *memConn) Close() error                { return nil }
func (m *memConn) LocalAddr() net.Addr         { return dummyAddr{} }
func (m *memConn) RemoteAddr() net.Addr        { return dummyAddr{} }
func (m *memConn) SetDeadline(time.Time) error { return nil }

func (m *memConn) SetReadDeadline(time.Time) error  { return nil }
func (m *memConn) SetWriteDeadline(time.Time) error { return nil }

type dummyAddr struct{}

func (dummyAddr) Network() string { return "mem" }
func (dummyAddr) String() string  { return "mem" }

// newMemConn crée une Conn lisant in et écrivant dans un tampon consultable.
func newMemConn(isServer bool, in []byte) (*Conn, *memConn) {
	m := &memConn{r: bytes.NewReader(in)}
	return newConn(m, nil, nil, isServer), m
}

// testMaskKey est la clé utilisée par buildFrame.
var testMaskKey = [4]byte{0xCA, 0xFE, 0xBA, 0xBE}

// rawHeader fabrique un en-tête de trame avec une longueur annoncée
// arbitraire — y compris mensongère.
func rawHeader(fin bool, rsv byte, opcode byte, mask bool, length uint64) []byte {
	var out []byte
	b0 := opcode | rsv<<4
	if fin {
		b0 |= 0x80
	}
	out = append(out, b0)
	var maskBit byte
	if mask {
		maskBit = 0x80
	}
	switch {
	case length < 126:
		out = append(out, maskBit|byte(length))
	case length <= 0xFFFF:
		out = append(out, maskBit|126)
		out = binary.BigEndian.AppendUint16(out, uint16(length))
	default:
		out = append(out, maskBit|127)
		out = binary.BigEndian.AppendUint64(out, length)
	}
	if mask {
		out = append(out, testMaskKey[:]...)
	}
	return out
}

// buildFrame fabrique une trame brute, en dehors de l'implémentation, pour
// tester le décodeur sur des cas que l'encodeur ne produit jamais.
func buildFrame(fin bool, rsv byte, opcode byte, mask bool, payload []byte) []byte {
	out := rawHeader(fin, rsv, opcode, mask, uint64(len(payload)))
	body := append([]byte(nil), payload...)
	if mask {
		maskBytes(testMaskKey, 0, body)
	}
	return append(out, body...)
}

// allocatedBytes mesure les octets alloués par fn. Les tests du package ne
// tournent pas en parallèle : la mesure est représentative.
func allocatedBytes(fn func()) uint64 {
	var before, after runtime.MemStats
	runtime.GC()
	runtime.ReadMemStats(&before)
	fn()
	runtime.ReadMemStats(&after)
	return after.TotalAlloc - before.TotalAlloc
}

// --- round-trip --------------------------------------------------------

func TestRoundTripPayloadSizes(t *testing.T) {
	sizes := []int{0, 1, 125, 126, 127, 65535, 65536, 70000}
	for _, n := range sizes {
		payload := make([]byte, n)
		for i := range payload {
			payload[i] = byte(i * 7)
		}
		for _, srv := range []bool{false, true} {
			// L'émetteur est client (masque) ou serveur (ne masque pas).
			enc, wire := newMemConn(srv, nil)
			if err := enc.WriteMessage(BinaryMessage, payload); err != nil {
				t.Fatalf("taille %d: WriteMessage: %v", n, err)
			}
			dec, _ := newMemConn(!srv, wire.w.Bytes())
			mt, got, err := dec.ReadMessage()
			if err != nil {
				t.Fatalf("taille %d: ReadMessage: %v", n, err)
			}
			if mt != BinaryMessage {
				t.Fatalf("taille %d: type = %d", n, mt)
			}
			if !bytes.Equal(got, payload) {
				t.Fatalf("taille %d: charge utile différente (len %d)", n, len(got))
			}
			// Vérifie le bit de masquage réellement posé sur le fil.
			raw := wire.w.Bytes()
			masked := raw[1]&0x80 != 0
			if masked == srv {
				t.Fatalf("taille %d: bit MASK = %v pour isServer=%v", n, masked, srv)
			}
		}
	}
}

func TestRoundTripText(t *testing.T) {
	enc, wire := newMemConn(false, nil)
	msg := "héllo wörld — ✓ 𝄞"
	if err := enc.WriteMessage(TextMessage, []byte(msg)); err != nil {
		t.Fatalf("WriteMessage: %v", err)
	}
	dec, _ := newMemConn(true, wire.w.Bytes())
	mt, got, err := dec.ReadMessage()
	if err != nil {
		t.Fatalf("ReadMessage: %v", err)
	}
	if mt != TextMessage || string(got) != msg {
		t.Fatalf("got (%d, %q)", mt, got)
	}
}

func TestWriteFragmentsRoundTrip(t *testing.T) {
	enc, wire := newMemConn(false, nil)
	if err := enc.WriteFragments(TextMessage, []byte("Hel"), []byte("lo, "), []byte("mönde ✓")); err != nil {
		t.Fatalf("WriteFragments: %v", err)
	}
	// Trois trames attendues : text non-FIN, continuation non-FIN, continuation FIN.
	raw := wire.w.Bytes()
	if raw[0] != opText {
		t.Fatalf("première trame: b0 = 0x%02X", raw[0])
	}
	dec, _ := newMemConn(true, raw)
	mt, got, err := dec.ReadMessage()
	if err != nil {
		t.Fatalf("ReadMessage: %v", err)
	}
	if mt != TextMessage || string(got) != "Hello, mönde ✓" {
		t.Fatalf("got (%d, %q)", mt, got)
	}
}

func TestReadFragmentedWithInterleavedControl(t *testing.T) {
	var wire []byte
	wire = append(wire, buildFrame(false, 0, opText, true, []byte("Hel"))...)
	wire = append(wire, buildFrame(true, 0, opPing, true, []byte("bip"))...)
	wire = append(wire, buildFrame(false, 0, opContinuation, true, []byte("lo, "))...)
	wire = append(wire, buildFrame(true, 0, opPong, true, []byte("bop"))...)
	wire = append(wire, buildFrame(true, 0, opContinuation, true, []byte("monde"))...)

	c, out := newMemConn(true, wire)
	var pongs []string
	c.OnPong = func(p []byte) { pongs = append(pongs, string(p)) }

	mt, got, err := c.ReadMessage()
	if err != nil {
		t.Fatalf("ReadMessage: %v", err)
	}
	if mt != TextMessage || string(got) != "Hello, monde" {
		t.Fatalf("got (%d, %q)", mt, got)
	}
	if len(pongs) != 1 || pongs[0] != "bop" {
		t.Fatalf("pongs = %q", pongs)
	}
	// Le ping reçu doit avoir déclenché un pong automatique (serveur : non masqué).
	reply := out.w.Bytes()
	if len(reply) != 2+3 || reply[0] != 0x80|opPong || reply[1] != 3 || string(reply[2:]) != "bip" {
		t.Fatalf("réponse au ping = % X", reply)
	}
	if len(pongs) > 0 && pongs[0] != "bop" {
		t.Fatal("pong inattendu")
	}
}

func TestEmptyMessageIsNonNil(t *testing.T) {
	c, _ := newMemConn(true, buildFrame(true, 0, opBinary, true, nil))
	_, got, err := c.ReadMessage()
	if err != nil {
		t.Fatalf("ReadMessage: %v", err)
	}
	if got == nil || len(got) != 0 {
		t.Fatalf("got = %v", got)
	}
}

// --- erreurs de protocole ----------------------------------------------

func expectProtoErr(t *testing.T, c *Conn, wantCode uint16, what string) {
	t.Helper()
	_, _, err := c.ReadMessage()
	var pe *ProtocolError
	if !errors.As(err, &pe) {
		t.Fatalf("%s: erreur = %v, attendu *ProtocolError", what, err)
	}
	if pe.Code != wantCode {
		t.Fatalf("%s: code = %d, attendu %d", what, pe.Code, wantCode)
	}
	// L'erreur est collante.
	if _, _, err2 := c.ReadMessage(); !errors.Is(err2, err) {
		t.Fatalf("%s: seconde lecture = %v, attendu la même erreur", what, err2)
	}
}

func TestProtocolErrors(t *testing.T) {
	tests := []struct {
		name     string
		isServer bool
		wire     []byte
		code     uint16
	}{
		{"bits RSV non nuls", true, buildFrame(true, 0x4, opText, true, []byte("x")), CloseProtocolError},
		{"contrôle fragmenté", true, buildFrame(false, 0, opPing, true, []byte("x")), CloseProtocolError},
		{"contrôle > 125", true, buildFrame(true, 0, opPing, true, make([]byte, 126)), CloseProtocolError},
		{"trame client non masquée", true, buildFrame(true, 0, opText, false, []byte("x")), CloseProtocolError},
		{"trame serveur masquée", false, buildFrame(true, 0, opText, true, []byte("x")), CloseProtocolError},
		{"opcode de données réservé", true, buildFrame(true, 0, 0x3, true, nil), CloseProtocolError},
		{"opcode de contrôle réservé", true, buildFrame(true, 0, 0xB, true, nil), CloseProtocolError},
		{"continuation orpheline", true, buildFrame(true, 0, opContinuation, true, []byte("x")), CloseProtocolError},
		{"texte invalide UTF-8", true, buildFrame(true, 0, opText, true, []byte{0x48, 0xC3, 0x28}), CloseInvalidFramePayloadData},
		{"close 1 octet", true, buildFrame(true, 0, opClose, true, []byte{0x03}), CloseProtocolError},
		{"close code invalide", true, buildFrame(true, 0, opClose, true, []byte{0x03, 0xEE}), CloseProtocolError},
		{"close raison invalide UTF-8", true, buildFrame(true, 0, opClose, true, []byte{0x03, 0xE8, 0xC3, 0x28}), CloseInvalidFramePayloadData},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			c, out := newMemConn(tc.isServer, tc.wire)
			expectProtoErr(t, c, tc.code, tc.name)
			// Une trame close portant le code doit avoir été émise au pair.
			raw := out.w.Bytes()
			if len(raw) < 4 {
				t.Fatalf("aucune trame close émise (% X)", raw)
			}
			off := 2
			if !tc.isServer {
				off += 4 // clé de masquage
			}
			body := append([]byte(nil), raw[off:]...)
			if !tc.isServer {
				maskBytes([4]byte(raw[2:6]), 0, body)
			}
			if raw[0] != 0x80|opClose {
				t.Fatalf("trame émise b0 = 0x%02X", raw[0])
			}
			if got := binary.BigEndian.Uint16(body); got != tc.code {
				t.Fatalf("code de close émis = %d, attendu %d", got, tc.code)
			}
		})
	}
}

func TestDataFrameInsideFragmentedMessage(t *testing.T) {
	var wire []byte
	wire = append(wire, buildFrame(false, 0, opText, true, []byte("a"))...)
	wire = append(wire, buildFrame(true, 0, opText, true, []byte("b"))...)
	c, _ := newMemConn(true, wire)
	expectProtoErr(t, c, CloseProtocolError, "données au milieu d'un fragment")
}

func TestLyingLengthDoesNotOverAllocate(t *testing.T) {
	// En-tête annonçant 1 Tio, suivi de trois octets seulement. Limite de
	// lecture levée : seule la lecture incrémentale protège la mémoire.
	wire := append(rawHeader(true, 0, opBinary, true, 1<<40), 1, 2, 3)
	c, _ := newMemConn(true, wire)
	c.SetReadLimit(0)

	var err error
	alloc := allocatedBytes(func() { _, _, err = c.ReadMessage() })
	if !errors.Is(err, io.ErrUnexpectedEOF) {
		t.Fatalf("erreur = %v, attendu io.ErrUnexpectedEOF", err)
	}
	if alloc > 32<<10 {
		t.Fatalf("%d octets alloués pour 3 octets réellement reçus", alloc)
	}
}

func TestLength64HighBitRejected(t *testing.T) {
	wire := []byte{0x82, 0x80 | 127, 0xFF, 0, 0, 0, 0, 0, 0, 1, 0xCA, 0xFE, 0xBA, 0xBE}
	c, _ := newMemConn(true, wire)
	expectProtoErr(t, c, CloseProtocolError, "longueur 64 bits négative")
}

func TestNonMinimalLengthRejected(t *testing.T) {
	// Format 16 bits pour une charge utile qui tient sur 7 bits.
	small := []byte{0x82, 0x80 | 126, 0x00, 0x0A, 0xCA, 0xFE, 0xBA, 0xBE}
	small = append(small, make([]byte, 10)...)
	c, _ := newMemConn(true, small)
	expectProtoErr(t, c, CloseProtocolError, "longueur 16 bits non minimale")

	// Format 64 bits pour une charge utile qui tient sur 16 bits.
	big := []byte{0x82, 0x80 | 127, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0xCA, 0xFE, 0xBA, 0xBE}
	big = append(big, make([]byte, 256)...)
	c2, _ := newMemConn(true, big)
	expectProtoErr(t, c2, CloseProtocolError, "longueur 64 bits non minimale")

	// La borne exacte reste acceptée : 126 octets en 16 bits, 65536 en 64 bits.
	c3, _ := newMemConn(true, buildFrame(true, 0, opBinary, true, make([]byte, 126)))
	if _, got, err := c3.ReadMessage(); err != nil || len(got) != 126 {
		t.Fatalf("126 octets: %d lus, err = %v", len(got), err)
	}
	c4, _ := newMemConn(true, buildFrame(true, 0, opBinary, true, make([]byte, 65536)))
	if _, got, err := c4.ReadMessage(); err != nil || len(got) != 65536 {
		t.Fatalf("65536 octets: %d lus, err = %v", len(got), err)
	}
}

func TestReadLimitOverflow(t *testing.T) {
	// Un premier fragment, puis une continuation annonçant MaxInt64 : la
	// somme len(data)+length déborderait et passerait sous la limite.
	wire := buildFrame(false, 0, opBinary, true, make([]byte, 100))
	wire = append(wire, rawHeader(true, 0, opContinuation, true, math.MaxInt64)...)
	c, _ := newMemConn(true, wire)
	c.SetReadLimit(1000)

	var err error
	alloc := allocatedBytes(func() { _, _, err = c.ReadMessage() })
	var pe *ProtocolError
	if !errors.As(err, &pe) || pe.Code != CloseMessageTooBig {
		t.Fatalf("erreur = %v, attendu ProtocolError 1009", err)
	}
	if alloc > 32<<10 {
		t.Fatalf("%d octets alloués", alloc)
	}
}

func TestDefaultReadLimit(t *testing.T) {
	c0, _ := newMemConn(true, nil)
	if c0.readLimit != defaultReadLimit {
		t.Fatalf("limite par défaut = %d, attendu %d", c0.readLimit, defaultReadLimit)
	}

	// Message fragmenté sans FIN qui dépasse le défaut : refusé avant lecture.
	wire := buildFrame(false, 0, opBinary, true, make([]byte, 100))
	wire = append(wire, rawHeader(false, 0, opContinuation, true, 2<<20)...)
	c, _ := newMemConn(true, wire)

	var err error
	alloc := allocatedBytes(func() { _, _, err = c.ReadMessage() })
	var pe *ProtocolError
	if !errors.As(err, &pe) || pe.Code != CloseMessageTooBig {
		t.Fatalf("erreur = %v, attendu ProtocolError 1009", err)
	}
	if alloc > 32<<10 {
		t.Fatalf("%d octets alloués alors que rien n'a été reçu du fragment", alloc)
	}
}

func TestCloseCodeRoleDependent(t *testing.T) {
	// 1010 est réservé au client : un client qui le reçoit d'un serveur doit
	// couper.
	cli, _ := newMemConn(false, buildFrame(true, 0, opClose, false, []byte{0x03, 0xF2}))
	expectProtoErr(t, cli, CloseProtocolError, "1010 venant d'un serveur")

	// Le même code venant d'un client est légitime.
	srv, _ := newMemConn(true, buildFrame(true, 0, opClose, true, []byte{0x03, 0xF2}))
	if _, _, err := srv.ReadMessage(); !IsCloseCode(err, CloseMandatoryExtension) {
		t.Fatalf("erreur = %v, attendu CloseError 1010", err)
	}
	// Et un serveur ne peut pas l'émettre.
	s2, _ := newMemConn(true, nil)
	if err := s2.WriteClose(CloseMandatoryExtension, ""); err == nil {
		t.Fatal("un serveur a pu émettre un close 1010")
	}
}

func TestReadLimit(t *testing.T) {
	c, _ := newMemConn(true, buildFrame(true, 0, opBinary, true, make([]byte, 200)))
	c.SetReadLimit(100)
	expectProtoErr(t, c, CloseMessageTooBig, "dépassement de SetReadLimit")

	// Limite atteinte par accumulation de fragments.
	var wire []byte
	wire = append(wire, buildFrame(false, 0, opBinary, true, make([]byte, 60))...)
	wire = append(wire, buildFrame(true, 0, opContinuation, true, make([]byte, 60))...)
	c2, _ := newMemConn(true, wire)
	c2.SetReadLimit(100)
	expectProtoErr(t, c2, CloseMessageTooBig, "dépassement cumulé de SetReadLimit")

	// Pile à la limite : accepté.
	c3, _ := newMemConn(true, buildFrame(true, 0, opBinary, true, make([]byte, 100)))
	c3.SetReadLimit(100)
	if _, got, err := c3.ReadMessage(); err != nil || len(got) != 100 {
		t.Fatalf("lecture à la limite: %d octets, err = %v", len(got), err)
	}
}

func TestReadCloseFrame(t *testing.T) {
	body := append([]byte{0x03, 0xF3}, "au revoir"...) // 1011
	c, out := newMemConn(true, buildFrame(true, 0, opClose, true, body))
	_, _, err := c.ReadMessage()
	var ce *CloseError
	if !errors.As(err, &ce) {
		t.Fatalf("erreur = %v, attendu *CloseError", err)
	}
	if ce.Code != CloseInternalServerErr || ce.Reason != "au revoir" {
		t.Fatalf("close = %d %q", ce.Code, ce.Reason)
	}
	if !IsCloseCode(err, CloseInternalServerErr) || IsCloseCode(err, CloseNormalClosure) {
		t.Fatal("IsCloseCode incohérent")
	}
	// Écho automatique du même code, sans raison.
	raw := out.w.Bytes()
	if len(raw) != 4 || raw[0] != 0x80|opClose || binary.BigEndian.Uint16(raw[2:]) != CloseInternalServerErr {
		t.Fatalf("écho de close = % X", raw)
	}
}

func TestReadCloseWithoutBody(t *testing.T) {
	c, _ := newMemConn(true, buildFrame(true, 0, opClose, true, nil))
	_, _, err := c.ReadMessage()
	var ce *CloseError
	if !errors.As(err, &ce) || ce.Code != CloseNoStatusReceived {
		t.Fatalf("erreur = %v", err)
	}
}

// --- écriture : validations --------------------------------------------

func TestWriteRejects(t *testing.T) {
	c, _ := newMemConn(true, nil)
	if err := c.WriteMessage(9, nil); err == nil {
		t.Fatal("type de message invalide accepté")
	}
	if err := c.WriteMessage(TextMessage, []byte{0xC3, 0x28}); err == nil {
		t.Fatal("texte non UTF-8 accepté")
	}
	if err := c.WriteFragments(TextMessage, []byte{0xE2, 0x9C}); err == nil {
		t.Fatal("fragment texte tronqué accepté")
	}
	if err := c.WritePing(make([]byte, 126)); err == nil {
		t.Fatal("ping de 126 octets accepté")
	}
	if err := c.WriteClose(1006, ""); err == nil {
		t.Fatal("code de close 1006 accepté")
	}
	if err := c.WriteClose(CloseNormalClosure, string([]byte{0xC3, 0x28})); err == nil {
		t.Fatal("raison de close non UTF-8 acceptée")
	}
}

func TestWriteFragmentsSplitRune(t *testing.T) {
	// Une rune coupée entre deux fragments reste valide.
	c, wire := newMemConn(false, nil)
	r := []byte("é✓")
	if err := c.WriteFragments(TextMessage, r[:1], r[1:3], r[3:]); err != nil {
		t.Fatalf("WriteFragments: %v", err)
	}
	dec, _ := newMemConn(true, wire.w.Bytes())
	_, got, err := dec.ReadMessage()
	if err != nil || string(got) != "é✓" {
		t.Fatalf("got %q, err %v", got, err)
	}
}

func TestWriteAfterCloseFails(t *testing.T) {
	c, _ := newMemConn(true, nil)
	if err := c.WriteClose(CloseNormalClosure, "fini"); err != nil {
		t.Fatalf("WriteClose: %v", err)
	}
	if err := c.WriteMessage(TextMessage, []byte("x")); !errors.Is(err, ErrClosed) {
		t.Fatalf("erreur = %v, attendu ErrClosed", err)
	}
	if err := c.WriteClose(CloseNormalClosure, "encore"); err != nil {
		t.Fatalf("second WriteClose: %v", err)
	}
}

func TestPingAfterCloseSentIsIgnored(t *testing.T) {
	// Après notre close, un ping du pair ne doit pas casser l'attente de son
	// écho de fermeture.
	wire := append(buildFrame(true, 0, opPing, true, []byte("x")),
		buildFrame(true, 0, opClose, true, []byte{0x03, 0xE8})...)
	c, out := newMemConn(true, wire)
	if err := c.WriteClose(CloseNormalClosure, ""); err != nil {
		t.Fatalf("WriteClose: %v", err)
	}
	if _, _, err := c.ReadMessage(); !IsCloseCode(err, CloseNormalClosure) {
		t.Fatalf("erreur = %v", err)
	}
	if n := out.w.Len(); n != 4 {
		t.Fatalf("%d octets émis, attendu la seule trame close", n)
	}
}

func TestCloseReasonTruncated(t *testing.T) {
	c, wire := newMemConn(true, nil)
	long := strings.Repeat("é", 200) // 400 octets
	if err := c.WriteClose(CloseNormalClosure, long); err != nil {
		t.Fatalf("WriteClose: %v", err)
	}
	raw := wire.w.Bytes()
	if raw[1] > maxControlPayload {
		t.Fatalf("trame de close de %d octets", raw[1])
	}
	if !isValidUTF8(raw[4:]) {
		t.Fatal("raison tronquée au milieu d'une rune")
	}
}

func isValidUTF8(b []byte) bool { var a utf8Acc; return a.write(b) && a.complete() }

// --- primitives --------------------------------------------------------

func TestMaskBytesChunked(t *testing.T) {
	key := [4]byte{1, 2, 3, 4}
	data := make([]byte, 137)
	for i := range data {
		data[i] = byte(i)
	}
	ref := append([]byte(nil), data...)
	maskBytes(key, 0, ref)

	// Même résultat en masquant par tranches arbitraires.
	got := append([]byte(nil), data...)
	pos := 0
	for _, n := range []int{1, 2, 3, 7, 13, 32, 79} {
		if pos+n > len(got) {
			n = len(got) - pos
		}
		pos = maskBytes(key, pos, got[pos:pos+n])
	}
	if pos != len(got) {
		t.Fatalf("pos = %d", pos)
	}
	if !bytes.Equal(ref, got) {
		t.Fatal("masquage par tranches != masquage en un bloc")
	}
	// Involution.
	maskBytes(key, 0, got)
	if !bytes.Equal(got, data) {
		t.Fatal("le démasquage ne restitue pas l'original")
	}
}

func TestUTF8Acc(t *testing.T) {
	cases := []struct {
		frags []string
		ok    bool
	}{
		{[]string{"abc"}, true},
		{[]string{"é", "✓"}, true},
		{[]string{"\xc3", "\xa9"}, true},
		{[]string{"\xf0\x9d", "\x84\x9e"}, true},
		{[]string{"\xc3"}, false},         // rune incomplète en fin de flux
		{[]string{"\xc3", "\x28"}, false}, // continuation invalide
		{[]string{"\xff"}, false},
		{[]string{"ok\xed\xa0\x80"}, false}, // surrogate
	}
	for _, tc := range cases {
		var a utf8Acc
		ok := true
		for _, f := range tc.frags {
			if !a.write([]byte(f)) {
				ok = false
				break
			}
		}
		if ok {
			ok = a.complete()
		}
		if ok != tc.ok {
			t.Fatalf("fragments %q: ok = %v, attendu %v", tc.frags, ok, tc.ok)
		}
	}
}

func TestGrowSlice(t *testing.T) {
	b := growSlice(nil, 10)
	if len(b) != 10 || cap(b) != 10 {
		t.Fatalf("len %d cap %d", len(b), cap(b))
	}
	b = b[:3]
	b2 := growSlice(b, 5)
	if len(b2) != 8 || &b2[0] != &b[0] {
		t.Fatal("capacité non réutilisée")
	}
}

func TestTruncateUTF8(t *testing.T) {
	if got := truncateUTF8("héllo", 3); got != "hé" {
		t.Fatalf("got %q", got)
	}
	if got := truncateUTF8("abc", 10); got != "abc" {
		t.Fatalf("got %q", got)
	}
}
