package client

import (
	"strconv"
	"strings"
)

// Comparaison de versions applicatives (VS-023). Volontairement minimale : le
// garde-fou dur de compatibilité reste la version de protocole, refusée par le
// serveur. Ici on ne cherche qu'à dire « le serveur est plus récent que moi »
// pour proposer un téléchargement — jamais pour bloquer quoi que ce soit.
//
// Format accepté : `major[.minor[.patch]]`, chiffres seulement, avec un « v »
// initial optionnel et un suffixe ignoré (`-rc1`, `+build`). Les composants
// absents valent 0. Tout le reste (« dev », vide, texte) est illisible : dans
// le doute, on ne dit rien.

// maxVersionPart borne chaque composant : au-delà, c'est une saisie absurde
// plutôt qu'une version (et Atoi déborderait sur les entrées très longues).
const maxVersionPart = 1_000_000

// parseVersion découpe une version en (major, minor, patch).
func parseVersion(s string) ([3]int, bool) {
	var out [3]int
	s = strings.TrimSpace(s)
	s = strings.TrimPrefix(s, "v")
	// Suffixe de pré-release ou de build : ignoré, il ne participe pas à
	// l'ordre (« 1.2.3-rc1 » est traitée comme « 1.2.3 »).
	if i := strings.IndexAny(s, "-+ "); i >= 0 {
		s = s[:i]
	}
	if s == "" {
		return out, false
	}
	parts := strings.Split(s, ".")
	if len(parts) > 3 {
		return out, false
	}
	for i, p := range parts {
		if p == "" {
			return out, false
		}
		n, err := strconv.Atoi(p)
		if err != nil || n < 0 || n > maxVersionPart {
			return out, false
		}
		out[i] = n
	}
	return out, true
}

// NewerVersion dit si `remote` est strictement plus récente que `local`.
// Renvoie false dès que l'une des deux est illisible : un client « dev » ou un
// serveur muet ne doit jamais provoquer d'invitation à mettre à jour.
func NewerVersion(remote, local string) bool {
	r, okR := parseVersion(remote)
	l, okL := parseVersion(local)
	if !okR || !okL {
		return false
	}
	for i := range r {
		if r[i] != l[i] {
			return r[i] > l[i]
		}
	}
	return false
}
