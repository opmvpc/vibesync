package client

import "testing"

func TestNewerVersion(t *testing.T) {
	cases := []struct {
		nom           string
		remote, local string
		veutBanniere  bool
	}{
		{"identiques", "1.2.3", "1.2.3", false},
		{"patch plus récent", "1.2.4", "1.2.3", true},
		{"patch plus ancien", "1.2.2", "1.2.3", false},
		{"mineure plus récente", "1.3.0", "1.2.9", true},
		{"majeure plus récente", "2.0.0", "1.99.99", true},
		{"majeure plus ancienne", "1.0.0", "2.0.0", false},
		{"composants manquants", "0.3", "0.2.9", true},
		{"composant manquant à droite", "0.2", "0.2.0", false},
		{"préfixe v toléré", "v1.2.4", "1.2.3", true},
		{"pré-release d'un triplet supérieur", "1.2.4-rc1", "1.2.3", true},
		// Ordre semver : à triplet égal, la pré-release précède la stable.
		{"pré-release ne dépasse pas la stable", "1.2.3-rc1", "1.2.3", false},
		{"la stable dépasse la pré-release", "1.2.3", "1.2.3-rc1", true},
		{"pré-release contre pré-release : on ne tranche pas", "1.2.3-rc2", "1.2.3-rc1", false},
		{"pré-release plus ancienne", "1.2.2-rc1", "1.2.3", false},
		{"tiret sans suffixe", "1.2.3-", "1.2.3", false},
		{"métadonnées de build hors de l'ordre", "1.2.3+build7", "1.2.3", false},
		{"métadonnées sur un triplet supérieur", "1.2.4+build7", "1.2.3", true},
		{"métadonnées après pré-release", "1.2.3+b", "1.2.3-rc1", true},
		{"numéros à plusieurs chiffres", "1.10.0", "1.9.0", true},
		{"client dev : jamais de bannière", "9.9.9", "dev", false},
		{"serveur dev", "dev", "1.0.0", false},
		{"serveur muet", "", "1.0.0", false},
		{"client muet", "1.0.0", "", false},
		{"texte pourri", "on-verra-plus-tard", "1.0.0", false},
		{"trop de composants", "1.2.3.4", "1.0.0", false},
		{"composant vide", "1..3", "1.0.0", false},
		{"négatif", "-1.2.3", "1.0.0", false},
		{"absurdement grand", "99999999999999999999.0.0", "1.0.0", false},
		{"espaces", "  1.2.4  ", "1.2.3", true},
	}
	for _, tc := range cases {
		t.Run(tc.nom, func(t *testing.T) {
			if got := NewerVersion(tc.remote, tc.local); got != tc.veutBanniere {
				t.Fatalf("NewerVersion(%q, %q) = %v, attendu %v",
					tc.remote, tc.local, got, tc.veutBanniere)
			}
		})
	}
}

// La comparaison doit être antisymétrique : jamais deux versions plus récentes
// l'une que l'autre.
func TestNewerVersionAntisymetrique(t *testing.T) {
	versions := []string{"0.0.1", "0.1.0", "0.2.0", "1.0.0", "1.0.1", "1.2.3-rc1", "1.2.3", "v2.0.0"}
	for _, a := range versions {
		for _, b := range versions {
			if NewerVersion(a, b) && NewerVersion(b, a) {
				t.Fatalf("%q et %q sont chacune plus récente que l'autre", a, b)
			}
		}
	}
}
