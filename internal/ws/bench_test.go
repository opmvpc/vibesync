package ws

import (
	"io"
	"testing"
)

// nullConn absorbe les écritures : il isole le coût de l'encodage du coût de
// la pile réseau et évite qu'un tampon ne grossisse pendant la mesure.
type nullConn struct{ memConn }

func (n *nullConn) Write(p []byte) (int, error) { return len(p), nil }

func benchWriteMessage(b *testing.B, size int, isServer bool) {
	b.Helper()
	payload := make([]byte, size)
	c := newConn(&nullConn{}, nil, nil, isServer)
	b.SetBytes(int64(size))
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if err := c.WriteMessage(BinaryMessage, payload); err != nil {
			b.Fatal(err)
		}
	}
}

func BenchmarkWriteMessage(b *testing.B) {
	b.Run("serveur/64", func(b *testing.B) { benchWriteMessage(b, 64, true) })
	b.Run("serveur/4096", func(b *testing.B) { benchWriteMessage(b, 4096, true) })
	b.Run("client-masque/64", func(b *testing.B) { benchWriteMessage(b, 64, false) })
	b.Run("client-masque/4096", func(b *testing.B) { benchWriteMessage(b, 4096, false) })
}

// repeatReader rejoue indéfiniment le même bloc d'octets.
type repeatReader struct {
	buf []byte
	off int
}

func (r *repeatReader) Read(p []byte) (int, error) {
	if len(r.buf) == 0 {
		return 0, io.EOF
	}
	n := copy(p, r.buf[r.off:])
	r.off += n
	if r.off == len(r.buf) {
		r.off = 0
	}
	return n, nil
}

func benchReadMessage(b *testing.B, size int) {
	b.Helper()
	payload := make([]byte, size)
	enc, wire := newMemConn(false, nil) // client : trames masquées
	if err := enc.WriteMessage(BinaryMessage, payload); err != nil {
		b.Fatal(err)
	}
	c := newConn(&nullConn{memConn{r: &repeatReader{buf: wire.w.Bytes()}}}, nil, nil, true)
	b.SetBytes(int64(size))
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if _, _, err := c.ReadMessage(); err != nil {
			b.Fatal(err)
		}
	}
}

func BenchmarkReadMessage(b *testing.B) {
	b.Run("64", func(b *testing.B) { benchReadMessage(b, 64) })
	b.Run("4096", func(b *testing.B) { benchReadMessage(b, 4096) })
}

func BenchmarkMaskBytes(b *testing.B) {
	buf := make([]byte, 4096)
	key := [4]byte{1, 2, 3, 4}
	b.SetBytes(int64(len(buf)))
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		maskBytes(key, 0, buf)
	}
}
