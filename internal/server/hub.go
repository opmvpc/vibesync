package server

import (
	"context"
	"errors"
	"log/slog"
	"sync"
	"time"
)

// Plafonds dépassés au moment de rejoindre une salle (§Comportements serveur,
// point 6).
var (
	errRoomFull     = errors.New("server: salle pleine")
	errTooManyRooms = errors.New("server: trop de salles ouvertes")
)

// Bornes de la période du ramasse-miettes des salles en attente de reprise :
// une salle survit au plus `linger + gcEvery`, ce qui est une granularité
// largement suffisante pour une fenêtre qui se compte en minutes.
const (
	gcMinPeriod = 5 * time.Second
	gcMaxPeriod = time.Minute
)

// Hub détient les salles : création à la volée au premier hello ; quand une
// salle se vide, elle est conservée avec son état (gelé en pause) pendant
// `linger` — pour qu'un retour après un crash reprenne la séance — puis
// détruite par le ramasse-miettes (§Modèle, VS-021).
//
// Une salle en attente de reprise reste comptée dans `maxRooms` : c'est le
// choix simple côté anti-abus (on ne peut pas ouvrir 50 salles fantômes de plus
// que le plafond), au prix d'un serveur saturé qui refuse des salles neuves
// pendant au plus `linger`. Le ramasse-miettes est passé avant tout contrôle de
// plafond pour qu'une salle déjà expirée ne bloque jamais personne.
type Hub struct {
	clock       Clock
	log         *slog.Logger
	maxRooms    int
	maxRoomSize int
	// linger est la fenêtre de reprise d'une salle vide ; ≤ 0 = destruction
	// immédiate (cf. RoomLingerDisabled).
	linger time.Duration
	// info est ce que le serveur dit de lui-même dans chaque welcome.
	info buildInfo

	mu    sync.Mutex
	rooms map[string]*Room

	// onRoomDestroyed, s'il est défini, est appelé (hors verrou) après la
	// destruction d'une salle. Utilisé par les tests pour se synchroniser sans
	// attente active ; nil en production.
	onRoomDestroyed func(name string)
}

func newHub(clock Clock, log *slog.Logger, maxRooms, maxRoomSize int, linger time.Duration, info buildInfo) *Hub {
	return &Hub{
		clock:       clock,
		log:         log,
		maxRooms:    maxRooms,
		maxRoomSize: maxRoomSize,
		linger:      linger,
		info:        info,
		rooms:       make(map[string]*Room),
	}
}

// join place un membre dans la salle demandée (créée si besoin, ou reprise si
// elle attendait un retour). Le verrou du hub est tenu pendant l'insertion pour
// qu'une salle ne puisse pas être détruite au moment précis où quelqu'un la
// rejoint — et pour qu'une reprise de session ne puisse pas s'entrelacer avec
// une autre arrivée.
//
// Le troisième retour est la connexion zombie remplacée par une reprise de
// session, à fermer par l'appelant une fois les verrous relâchés.
func (h *Hub) join(roomName, userName, session string, latencyMs int64, out sink) (*Room, *member, sink, error) {
	// Hors verrou : les salles expirées ne doivent pas peser sur les plafonds.
	h.gc()

	h.mu.Lock()
	defer h.mu.Unlock()

	room, ok := h.rooms[roomName]
	if !ok {
		if len(h.rooms) >= h.maxRooms {
			h.log.Warn("plafond de salles atteint", "max", h.maxRooms, "room", roomName)
			return nil, nil, nil, errTooManyRooms
		}
		room = newRoom(roomName, h.clock, h.log, h.info)
		h.rooms[roomName] = room
		h.log.Info("salle créée", "room", roomName)
	} else if room.size() >= h.maxRoomSize && !room.canResume(userName, session) {
		// Une reprise de session ne fait pas grossir la salle : le plafond ne
		// doit pas empêcher un membre déjà compté de récupérer sa place.
		h.log.Warn("plafond de membres atteint", "max", h.maxRoomSize, "room", roomName)
		return nil, nil, nil, errRoomFull
	}
	m, replaced, err := room.join(userName, session, latencyMs, out)
	if err != nil {
		if !ok {
			// La salle venait d'être créée pour rien.
			delete(h.rooms, roomName)
			h.log.Info("salle détruite", "room", roomName)
		}
		return nil, nil, nil, err
	}
	return room, m, replaced, nil
}

// leave retire le membre ; la salle devenue vide entre en attente de reprise ou
// est détruite si le linger est désactivé.
func (h *Hub) leave(room *Room, m *member) {
	if room.leave(m) {
		h.retireIfEmpty(room)
	}
}

func (h *Hub) retireIfEmpty(room *Room) {
	now := h.clock.Now()
	h.mu.Lock()
	// Re-vérification sous le verrou du hub : personne ne peut avoir rejoint
	// entre-temps, puisque join tient ce même verrou.
	empty := h.rooms[room.name] == room && room.markEmpty(now)
	destroyed := empty && h.linger <= 0
	if destroyed {
		delete(h.rooms, room.name)
	}
	hook := h.onRoomDestroyed
	h.mu.Unlock()

	switch {
	case destroyed:
		h.log.Info("salle détruite", "room", room.name)
		if hook != nil {
			hook(room.name)
		}
	case empty:
		h.log.Info("salle vide conservée pour reprise", "room", room.name, "linger", h.linger)
	}
}

// gc détruit les salles dont la fenêtre de reprise est écoulée.
func (h *Hub) gc() {
	if h.linger <= 0 {
		return // les salles vides sont déjà détruites à la volée
	}
	now := h.clock.Now()
	h.mu.Lock()
	var dead []string
	for name, room := range h.rooms {
		if room.expired(now, h.linger) {
			delete(h.rooms, name)
			dead = append(dead, name)
		}
	}
	hook := h.onRoomDestroyed
	h.mu.Unlock()

	for _, name := range dead {
		h.log.Info("salle détruite (fenêtre de reprise écoulée)", "room", name, "linger", h.linger)
		if hook != nil {
			hook(name)
		}
	}
}

// gcPeriod est la période de passage du ramasse-miettes.
func (h *Hub) gcPeriod() time.Duration {
	period := h.linger / 4
	if period < gcMinPeriod {
		period = gcMinPeriod
	}
	if period > gcMaxPeriod {
		period = gcMaxPeriod
	}
	return period
}

// gcLoop passe le ramasse-miettes jusqu'à l'annulation du contexte.
func (h *Hub) gcLoop(ctx context.Context) {
	ticker := time.NewTicker(h.gcPeriod())
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			h.gc()
		}
	}
}

// roomCount renvoie le nombre de salles vivantes (celles en attente de reprise
// comprises).
func (h *Hub) roomCount() int {
	h.mu.Lock()
	defer h.mu.Unlock()
	return len(h.rooms)
}

// room renvoie une salle par nom (nil si absente).
func (h *Hub) room(name string) *Room {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.rooms[name]
}
