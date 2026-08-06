// Commande genicon — générateur d'icônes vibesync, Go stdlib pur.
//
// Redessine par le code EXACTEMENT le design de assets/icon.svg (mêmes
// constantes géométriques, mêmes couleurs) et produit :
//
//	assets/png/icon-{16,24,32,48,64,128,256}.png
//	assets/vibesync.ico   (conteneur ICO, PNG embarqués — valides depuis Vista)
//
// Principes (ADR-008, philosophie handmade) :
//   - aucune dépendance : image, image/draw, image/png uniquement ;
//   - rastérisation maison par fonctions de distance signée (SDF) sur des
//     polygones convexes, dilatées d'un rayon r → coins arrondis exacts
//     (somme de Minkowski avec un disque = stroke-linejoin="round" en SVG) ;
//   - anti-aliasing par supersampling ×4 (16 échantillons/pixel, chaque
//     échantillon prend la couleur de la forme la plus haute) ;
//   - déterministe : aucune map, aucun aléa, aucune horloge → même sortie
//     bit à bit à chaque exécution.
//
// Usage : go run ./tools/genicon [-out assets]
package main

import (
	"bytes"
	"encoding/binary"
	"flag"
	"fmt"
	"image"
	"image/draw"
	"image/png"
	"math"
	"os"
	"path/filepath"
)

// ---------------------------------------------------------------------------
// Design — toutes les valeurs sont exprimées dans l'espace de dessin 256×256
// de assets/icon.svg. Toute modification ici doit être répercutée dans le SVG.
// ---------------------------------------------------------------------------

const (
	canvas = 256.0 // côté de l'espace de dessin

	bgPad    = 8.0  // marge autour du squircle
	bgRadius = 56.0 // rayon des coins du squircle
	bgRim    = 2.0  // liseré violet sur le bord du squircle

	triLen  = 90.0  // triangle « play » : base → pointe
	triHigh = 103.0 // triangle « play » : hauteur de la base
	triRad  = 12.0  // rayon d'arrondi des trois coins

	seamGap   = 17.0 // largeur du couloir de fond entre les deux triangles
	seamSlide = 22.0 // décalage des triangles le long de la couture (effet hélice)

	superSample = 4 // ×4 → 16 échantillons par pixel
)

// Palette (voir docs/research/captures/ui-*.png — thème sombre, accent #8b5cf6).
var (
	bgTop    = rgb(0x26, 0x20, 0x38) // haut du dégradé de fond
	bgBottom = rgb(0x12, 0x10, 0x16) // bas du dégradé de fond
	rimColor = rgb(0x8b, 0x5c, 0xf6) // accent violet, mixé à 30 % sur le bord
	rimMix   = 0.30

	triAtop = rgb(0xb7, 0x9e, 0xff) // triangle violet : clair
	triAbot = rgb(0x76, 0x35, 0xe8) // triangle violet : profond
	triBtop = rgb(0xff, 0xff, 0xff) // triangle pâle : blanc
	triBbot = rgb(0xc7, 0xb8, 0xfa) // triangle pâle : lavande
)

var icoSizes = []int{16, 24, 32, 48, 64, 128, 256}

// ---------------------------------------------------------------------------
// Vecteurs et distances signées
// ---------------------------------------------------------------------------

type vec struct{ x, y float64 }

func (a vec) add(b vec) vec     { return vec{a.x + b.x, a.y + b.y} }
func (a vec) sub(b vec) vec     { return vec{a.x - b.x, a.y - b.y} }
func (a vec) mul(k float64) vec { return vec{a.x * k, a.y * k} }
func (a vec) dot(b vec) float64 { return a.x*b.x + a.y*b.y }
func (a vec) len() float64      { return math.Hypot(a.x, a.y) }
func (a vec) norm() vec         { return a.mul(1 / a.len()) }

// sdSegment renvoie la distance de p au segment [a,b].
func sdSegment(p, a, b vec) float64 {
	ab := b.sub(a)
	t := clamp(p.sub(a).dot(ab)/ab.dot(ab), 0, 1)
	return p.sub(a.add(ab.mul(t))).len()
}

// sdConvex renvoie la distance signée de p au polygone convexe pts
// (négative à l'intérieur). Les sommets sont donnés dans le sens horaire.
// Dilater ce résultat d'un rayon r (sd <= r) produit le polygone à coins
// arrondis, identique au rendu SVG fill + stroke-linejoin="round".
func sdConvex(p vec, pts []vec) float64 {
	d := math.Inf(1)
	inside := true
	n := len(pts)
	for i := 0; i < n; i++ {
		a, b := pts[i], pts[(i+1)%n]
		if e := sdSegment(p, a, b); e < d {
			d = e
		}
		// Produit vectoriel : négatif = p à droite de l'arête (intérieur,
		// sommets horaires en repère écran y vers le bas → signe inversé).
		edge := b.sub(a)
		if edge.x*(p.y-a.y)-edge.y*(p.x-a.x) < 0 {
			inside = false
		}
	}
	if inside {
		return -d
	}
	return d
}

func clamp(x, lo, hi float64) float64 {
	if x < lo {
		return lo
	}
	if x > hi {
		return hi
	}
	return x
}

// ---------------------------------------------------------------------------
// Géométrie du logo
// ---------------------------------------------------------------------------

// Le logo : deux triangles « play » identiques, l'un pointant à droite (violet,
// en haut à gauche), l'autre déduit par rotation de 180° autour du centre
// (lavande, en bas à droite, pointant à gauche). Leurs pointes se font face de
// part et d'autre d'un couloir de fond en diagonale, large de seamGap unités —
// aucun chevauchement, donc symétrie de rotation exacte à toutes les tailles.
var (
	center = vec{canvas / 2, canvas / 2}
	bgQuad []vec // rectangle « noyau » du squircle (à dilater de bgRadius)
	triA   []vec // triangle violet, pointe à droite
	triB   []vec // triangle pâle, pointe à gauche
	triAp0 vec   // dégradé de A : point de départ
	triAp1 vec   // dégradé de A : point d'arrivée
	triBp0 vec
	triBp1 vec
)

func init() {
	// Squircle : rectangle inset de bgRadius, dilaté de bgRadius → coins ronds.
	lo := bgPad + bgRadius
	hi := canvas - bgPad - bgRadius
	bgQuad = []vec{{lo, lo}, {hi, lo}, {hi, hi}, {lo, hi}}

	// Couture : arête « bas-droite » du triangle pointant à droite, allant de
	// (-L/2, +H/2) à (+L/2, 0). Sa normale sortante dirige l'écartement.
	edge := vec{triLen, -triHigh / 2}.norm() // direction de la couture
	normal := vec{edge.y, -edge.x}           // normale sortante (vers le bas-droite)
	if normal.y < 0 {
		normal = normal.mul(-1)
	}

	// Distance du centre du triangle à cette arête.
	h := (triHigh * triLen / 4) / math.Hypot(triHigh/2, triLen)
	// Écartement des deux centres le long de la normale : les deux bords
	// arrondis se font alors face à exactement seamGap unités de distance,
	// sans jamais se chevaucher (symétrie de rotation parfaite, aucun des
	// deux triangles n'est rogné par l'autre).
	sep := 2*(h+triRad) + seamGap
	offset := normal.mul(sep).add(edge.mul(seamSlide))

	ca := center.sub(offset.mul(0.5)) // centre du triangle violet (haut-gauche)
	cb := center.add(offset.mul(0.5)) // centre du triangle pâle (bas-droite)

	hw, hh := triLen/2, triHigh/2
	triA = []vec{
		{ca.x - hw, ca.y - hh}, // base, haut
		{ca.x + hw, ca.y},      // pointe, droite
		{ca.x - hw, ca.y + hh}, // base, bas
	}
	// B = A tourné de 180° autour du centre du canevas.
	triB = make([]vec, 3)
	for i, p := range triA {
		triB[i] = vec{canvas - p.x, canvas - p.y}
	}
	// Dégradés : même axe lumineux (haut-gauche → bas-droite) pour les deux.
	triAp0 = vec{ca.x - hw, ca.y - hh}
	triAp1 = vec{ca.x + hw, ca.y + hh}
	triBp0 = vec{cb.x - hw, cb.y - hh}
	triBp1 = vec{cb.x + hw, cb.y + hh}
}

// ---------------------------------------------------------------------------
// Couleurs
// ---------------------------------------------------------------------------

type col struct{ r, g, b float64 }

func rgb(r, g, b uint8) col { return col{float64(r), float64(g), float64(b)} }

func mix(a, b col, t float64) col {
	return col{a.r + (b.r-a.r)*t, a.g + (b.g-a.g)*t, a.b + (b.b-a.b)*t}
}

// gradient projette p sur l'axe [p0,p1] et interpole c0→c1.
func gradient(p, p0, p1 vec, c0, c1 col) col {
	axis := p1.sub(p0)
	t := clamp(p.sub(p0).dot(axis)/axis.dot(axis), 0, 1)
	return mix(c0, c1, t)
}

// background renvoie la couleur du squircle au point p, et false si p est
// hors du squircle (pixel transparent).
func background(p vec) (col, bool) {
	d := sdConvex(p, bgQuad)
	if d > bgRadius {
		return col{}, false
	}
	base := gradient(p, vec{0, bgPad}, vec{0, canvas - bgPad}, bgTop, bgBottom)
	if d > bgRadius-bgRim {
		return mix(base, rimColor, rimMix), true // liseré violet
	}
	return base, true
}

// shade renvoie la couleur d'un échantillon. Les deux triangles étant
// disjoints, l'ordre de dessin n'a aucune importance. Chaque échantillon est
// opaque ou transparent ; l'anti-aliasing vient de la moyenne des 16
// échantillons du pixel.
func shade(p vec) (col, bool) {
	bg, inCard := background(p)
	if !inCard {
		return col{}, false // hors du squircle : rien ne déborde
	}
	if sdConvex(p, triA) <= triRad {
		return gradient(p, triAp0, triAp1, triAtop, triAbot), true
	}
	if sdConvex(p, triB) <= triRad {
		return gradient(p, triBp0, triBp1, triBtop, triBbot), true
	}
	return bg, true
}

// ---------------------------------------------------------------------------
// Rastérisation
// ---------------------------------------------------------------------------

// render dessine l'icône à la taille demandée, avec superSample² échantillons
// par pixel. Le résultat est en alpha prémultiplié (format image.RGBA).
func render(size int) *image.RGBA {
	img := image.NewRGBA(image.Rect(0, 0, size, size))
	draw.Draw(img, img.Bounds(), image.Transparent, image.Point{}, draw.Src)

	step := canvas / float64(size*superSample) // unités de dessin par échantillon
	total := float64(superSample * superSample)

	for py := 0; py < size; py++ {
		for px := 0; px < size; px++ {
			var sr, sg, sb, sa float64
			for sy := 0; sy < superSample; sy++ {
				for sx := 0; sx < superSample; sx++ {
					p := vec{
						(float64(px*superSample+sx) + 0.5) * step,
						(float64(py*superSample+sy) + 0.5) * step,
					}
					if c, ok := shade(p); ok {
						sr += c.r
						sg += c.g
						sb += c.b
						sa++
					}
				}
			}
			if sa == 0 {
				continue
			}
			a := sa / total * 255
			// Prémultiplication : les canaux sont déjà pondérés par la
			// couverture puisque seuls les échantillons opaques ont contribué.
			set(img, px, py, sr/total, sg/total, sb/total, a)
		}
	}
	return img
}

func set(img *image.RGBA, x, y int, r, g, b, a float64) {
	ai := round8(a)
	i := img.PixOffset(x, y)
	img.Pix[i+0] = minU8(round8(r), ai)
	img.Pix[i+1] = minU8(round8(g), ai)
	img.Pix[i+2] = minU8(round8(b), ai)
	img.Pix[i+3] = ai
}

func round8(v float64) uint8 {
	n := int(math.Round(v))
	if n < 0 {
		n = 0
	}
	if n > 255 {
		n = 255
	}
	return uint8(n)
}

func minU8(a, b uint8) uint8 {
	if a < b {
		return a
	}
	return b
}

// ---------------------------------------------------------------------------
// Conteneur ICO
// ---------------------------------------------------------------------------

// writeICO assemble un fichier .ico : ICONDIR (6 o) + N × ICONDIRENTRY (16 o)
// + les images brutes. Les images sont ici des PNG complets, acceptés par le
// shell Windows depuis Vista (et par tous les compilateurs de ressources).
func writeICO(path string, images [][]byte, sizes []int) error {
	if len(images) != len(sizes) {
		return fmt.Errorf("genicon: %d images pour %d tailles", len(images), len(sizes))
	}
	var buf bytes.Buffer
	// ICONDIR : réservé=0, type=1 (icône), nombre d'images.
	binary.Write(&buf, binary.LittleEndian, uint16(0))
	binary.Write(&buf, binary.LittleEndian, uint16(1))
	binary.Write(&buf, binary.LittleEndian, uint16(len(images)))

	offset := 6 + 16*len(images) // début des données image
	for i, sz := range sizes {
		dim := byte(sz) // 256 est codé 0 (un octet ne va que jusqu'à 255)
		if sz >= 256 {
			dim = 0
		}
		buf.WriteByte(dim)                                              // largeur
		buf.WriteByte(dim)                                              // hauteur
		buf.WriteByte(0)                                                // palette : 0 = truecolor
		buf.WriteByte(0)                                                // réservé
		binary.Write(&buf, binary.LittleEndian, uint16(1))              // plans
		binary.Write(&buf, binary.LittleEndian, uint16(32))             // bits/pixel
		binary.Write(&buf, binary.LittleEndian, uint32(len(images[i]))) // taille
		binary.Write(&buf, binary.LittleEndian, uint32(offset))         // offset
		offset += len(images[i])
	}
	for _, img := range images {
		buf.Write(img)
	}
	return os.WriteFile(path, buf.Bytes(), 0o644)
}

// ---------------------------------------------------------------------------

func main() {
	out := flag.String("out", "assets", "répertoire de sortie")
	flag.Parse()

	pngDir := filepath.Join(*out, "png")
	if err := os.MkdirAll(pngDir, 0o755); err != nil {
		fail(err)
	}

	blobs := make([][]byte, 0, len(icoSizes))
	for _, size := range icoSizes {
		img := render(size)
		var buf bytes.Buffer
		enc := png.Encoder{CompressionLevel: png.BestCompression}
		if err := enc.Encode(&buf, img); err != nil {
			fail(err)
		}
		data := buf.Bytes()
		name := filepath.Join(pngDir, fmt.Sprintf("icon-%d.png", size))
		if err := os.WriteFile(name, data, 0o644); err != nil {
			fail(err)
		}
		fmt.Printf("%-28s %6d o\n", name, len(data))
		blobs = append(blobs, data)
	}

	ico := filepath.Join(*out, "vibesync.ico")
	if err := writeICO(ico, blobs, icoSizes); err != nil {
		fail(err)
	}
	info, err := os.Stat(ico)
	if err != nil {
		fail(err)
	}
	fmt.Printf("%-28s %6d o  (%d entrées)\n", ico, info.Size(), len(icoSizes))

	// Rappel des sommets, pour garder assets/icon.svg synchronisé.
	fmt.Println("\ngéométrie (espace 256) — à refléter dans assets/icon.svg :")
	fmt.Printf("  triangle violet : %s\n", svgPoints(triA))
	fmt.Printf("  triangle pâle   : %s\n", svgPoints(triB))
	fmt.Printf("  rayon d'arrondi : %g   (stroke-width=%g)\n", triRad, 2*triRad)
}

func svgPoints(pts []vec) string {
	s := ""
	for i, p := range pts {
		if i > 0 {
			s += " "
		}
		s += fmt.Sprintf("%.2f,%.2f", p.x, p.y)
	}
	return s
}

func fail(err error) {
	fmt.Fprintln(os.Stderr, "genicon:", err)
	os.Exit(1)
}
