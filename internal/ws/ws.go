// Package ws implémente le protocole WebSocket (RFC 6455) en Go stdlib pur,
// sans aucune dépendance tierce (ADR-008).
//
// Périmètre couvert :
//   - handshake serveur ([Upgrade]) et client ([Dial]) ;
//   - framing complet : masquage, fragmentation (lecture et écriture),
//     longueurs 7/16/64 bits, trames de contrôle, close handshake ;
//   - validation UTF-8 des messages texte et des raisons de close (§5.6, §8.1).
//
// Volontairement non implémenté :
//   - permessage-deflate et toute autre extension (Sec-WebSocket-Extensions
//     est refusé s'il apparaît dans la réponse du serveur) ;
//   - négociation de sous-protocole (Sec-WebSocket-Protocol) ;
//   - proxies HTTP et suivi des redirections côté [Dial] ;
//   - cookies / authentification (à passer via DialConfig.Header).
//
// Écarts assumés à la lettre de la RFC : les longueurs de trame non minimales
// (126 annoncé pour moins de 126 octets) sont acceptées en lecture, et la
// validité UTF-8 d'un message texte n'est vérifiée qu'une fois tous ses
// fragments réassemblés (pas de rejet au premier octet fautif).
//
// Modèle de concurrence — direct, sans goroutine ni channel cachés :
//   - les écritures ([Conn.WriteMessage], [Conn.WritePing], [Conn.WritePong],
//     [Conn.WriteClose]) sont sérialisées par un mutex : elles sont sûres
//     depuis plusieurs goroutines ;
//   - les lectures ([Conn.ReadMessage]) ne le sont pas : une seule goroutine
//     lectrice à la fois ;
//   - [Conn.Close] lit et écrit : il s'appelle depuis la goroutine lectrice
//     (ou quand aucune lecture n'est en cours). Pour débloquer un lecteur
//     depuis une autre goroutine, utiliser [Conn.CloseNow].
package ws

import (
	"errors"
	"fmt"
)

// Types de messages applicatifs acceptés par [Conn.WriteMessage] et renvoyés
// par [Conn.ReadMessage].
const (
	TextMessage   = 1
	BinaryMessage = 2
)

// Opcodes RFC 6455 §5.2.
const (
	opContinuation = 0x0
	opText         = 0x1
	opBinary       = 0x2
	opClose        = 0x8
	opPing         = 0x9
	opPong         = 0xA
)

// Codes de fermeture RFC 6455 §7.4.1.
const (
	CloseNormalClosure           uint16 = 1000
	CloseGoingAway               uint16 = 1001
	CloseProtocolError           uint16 = 1002
	CloseUnsupportedData         uint16 = 1003
	CloseNoStatusReceived        uint16 = 1005 // jamais émis sur le fil
	CloseAbnormalClosure         uint16 = 1006 // jamais émis sur le fil
	CloseInvalidFramePayloadData uint16 = 1007
	ClosePolicyViolation         uint16 = 1008
	CloseMessageTooBig           uint16 = 1009
	CloseMandatoryExtension      uint16 = 1010
	CloseInternalServerErr       uint16 = 1011
	CloseServiceRestart          uint16 = 1012
	CloseTryAgainLater           uint16 = 1013
	CloseBadGateway              uint16 = 1014
	CloseTLSHandshake            uint16 = 1015 // jamais émis sur le fil
)

// maxControlPayload est la taille maximale du corps d'une trame de contrôle
// (RFC 6455 §5.5).
const maxControlPayload = 125

// ErrClosed est renvoyé quand la connexion a déjà été fermée localement.
var ErrClosed = errors.New("ws: connexion fermée")

// CloseError décrit la fermeture reçue du pair. [Conn.ReadMessage] le renvoie
// (enveloppé, à tester avec errors.As) quand une trame close arrive.
type CloseError struct {
	Code   uint16
	Reason string
}

func (e *CloseError) Error() string {
	if e.Reason == "" {
		return fmt.Sprintf("ws: fermeture reçue (code %d)", e.Code)
	}
	return fmt.Sprintf("ws: fermeture reçue (code %d: %s)", e.Code, e.Reason)
}

// IsCloseCode indique si err est un [CloseError] dont le code figure parmi
// codes (ou, si codes est vide, si err est un [CloseError] tout court).
func IsCloseCode(err error, codes ...uint16) bool {
	var ce *CloseError
	if !errors.As(err, &ce) {
		return false
	}
	if len(codes) == 0 {
		return true
	}
	for _, c := range codes {
		if ce.Code == c {
			return true
		}
	}
	return false
}

// ProtocolError signale une violation du protocole détectée en lecture. Le
// code de fermeture associé a été envoyé au pair avant de renvoyer l'erreur.
type ProtocolError struct {
	Code uint16
	Msg  string
}

func (e *ProtocolError) Error() string {
	return "ws: " + e.Msg
}

func protoErr(code uint16, msg string) *ProtocolError {
	return &ProtocolError{Code: code, Msg: msg}
}

// validCloseCode applique RFC 6455 §7.4 : codes enregistrés utilisables sur le
// fil + plages application (3000-3999) et privée (4000-4999). fromServer
// indique que le code est émis par un serveur : 1010 (extension obligatoire
// absente) est réservé au client (§7.4.1).
func validCloseCode(code uint16, fromServer bool) bool {
	if fromServer && code == CloseMandatoryExtension {
		return false
	}
	switch {
	case code >= 1000 && code <= 1003:
		return true
	case code >= 1007 && code <= 1014:
		return true
	case code >= 3000 && code <= 4999:
		return true
	default:
		return false
	}
}
