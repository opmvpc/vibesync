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
// initial optionnel, un suffixe de pré-release (`-rc1`) et des métadonnées de
// build (`+sha`). Les composants absents valent 0. Tout le reste (« dev »,
// vide, texte) est illisible : dans le doute, on ne dit rien.
//
// Ordre : à triplet égal, une pré-release est ANTÉRIEURE à la version nue
// (1.2.3-rc1 < 1.2.3), comme le veut semver — sinon un serveur en release
// candidate croirait dépasser la stable qu'il précède. Les métadonnées de build
// ne comptent pas dans l'ordre. Deux pré-releases du même triplet ne sont pas
// départagées : pas de bannière (leur ordre alphabétique mentirait sur rc10 vs
// rc2, et cela ne vaut pas la complexité).

// maxVersionPart borne chaque composant : au-delà, c'est une saisie absurde
// plutôt qu'une version (et Atoi déborderait sur les entrées très longues).
const maxVersionPart = 1_000_000

// version est un triplet numérique, plus le fait d'être une pré-release.
type version struct {
	parts [3]int
	pre   bool
}

// parseVersion découpe une version.
func parseVersion(s string) (version, bool) {
	var out version
	s = strings.TrimSpace(s)
	s = strings.TrimPrefix(s, "v")
	// Métadonnées de build : hors de l'ordre, on les coupe d'abord.
	if i := strings.IndexByte(s, '+'); i >= 0 {
		s = s[:i]
	}
	// Pré-release : elle ne change pas le triplet mais le déclasse.
	if i := strings.IndexByte(s, '-'); i >= 0 {
		out.pre = len(s) > i+1
		s = s[:i]
	}
	if strings.ContainsAny(s, " \t") || s == "" {
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
		out.parts[i] = n
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
	for i := range r.parts {
		if r.parts[i] != l.parts[i] {
			return r.parts[i] > l.parts[i]
		}
	}
	// Même triplet : seule une stable dépasse une pré-release.
	return !r.pre && l.pre
}
