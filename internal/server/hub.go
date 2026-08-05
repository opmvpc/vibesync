package server

import (
	"errors"
	"log/slog"
	"sync"
)

// Plafonds dépassés au moment de rejoindre une salle (§Comportements serveur,
// point 6).
var (
	errRoomFull     = errors.New("server: salle pleine")
	errTooManyRooms = errors.New("server: trop de salles ouvertes")
)

// Hub détient les salles : création à la volée au premier hello, destruction
// dès qu'une salle devient vide.
type Hub struct {
	clock       Clock
	log         *slog.Logger
	maxRooms    int
	maxRoomSize int

	mu    sync.Mutex
	rooms map[string]*Room

	// onRoomDestroyed, s'il est défini, est appelé (hors verrou) après la
	// destruction d'une salle. Utilisé par les tests pour se synchroniser sans
	// attente active ; nil en production.
	onRoomDestroyed func(name string)
}

func newHub(clock Clock, log *slog.Logger, maxRooms, maxRoomSize int) *Hub {
	return &Hub{
		clock:       clock,
		log:         log,
		maxRooms:    maxRooms,
		maxRoomSize: maxRoomSize,
		rooms:       make(map[string]*Room),
	}
}

// join place un membre dans la salle demandée (créée si besoin). Le verrou du
// hub est tenu pendant l'insertion pour qu'une salle ne puisse pas être
// détruite (vide) au moment précis où quelqu'un la rejoint.
func (h *Hub) join(roomName, userName string, latencyMs int64, out sink) (*Room, *member, error) {
	h.mu.Lock()
	defer h.mu.Unlock()

	room, ok := h.rooms[roomName]
	if !ok {
		if len(h.rooms) >= h.maxRooms {
			h.log.Warn("plafond de salles atteint", "max", h.maxRooms, "room", roomName)
			return nil, nil, errTooManyRooms
		}
		room = newRoom(roomName, h.clock, h.log)
		h.rooms[roomName] = room
		h.log.Info("salle créée", "room", roomName)
	} else if room.size() >= h.maxRoomSize {
		h.log.Warn("plafond de membres atteint", "max", h.maxRoomSize, "room", roomName)
		return nil, nil, errRoomFull
	}
	m, err := room.join(userName, latencyMs, out)
	if err != nil {
		if !ok {
			// La salle venait d'être créée pour rien.
			delete(h.rooms, roomName)
			h.log.Info("salle détruite", "room", roomName)
		}
		return nil, nil, err
	}
	return room, m, nil
}

// leave retire le membre et détruit la salle si elle est vide.
func (h *Hub) leave(room *Room, m *member) {
	if room.leave(m) {
		h.dropIfEmpty(room)
	}
}

func (h *Hub) dropIfEmpty(room *Room) {
	h.mu.Lock()
	destroyed := false
	// Re-vérification sous le verrou du hub : personne ne peut avoir rejoint
	// entre-temps, puisque join tient ce même verrou.
	if room.size() == 0 && h.rooms[room.name] == room {
		delete(h.rooms, room.name)
		h.log.Info("salle détruite", "room", room.name)
		destroyed = true
	}
	hook := h.onRoomDestroyed
	h.mu.Unlock()

	if destroyed && hook != nil {
		hook(room.name)
	}
}

// roomCount renvoie le nombre de salles vivantes.
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
