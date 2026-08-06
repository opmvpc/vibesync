// net.h — client WebSocket (ws:// et wss://) via WinHTTP.
//
// Mode synchrone sur un thread réseau dédié : plus simple et plus robuste que
// les callbacks asynchrones du pool de threads WinHTTP. Le thread pousse ses
// événements dans une file à créneaux fixes (SPSC, protégée par un SRWLOCK et
// signalée par un événement auto-reset), vidée par la boucle principale.
//
// L'émission se fait depuis le thread appelant sous verrou : WinHTTP autorise
// un envoi et une réception concurrents sur le même handle WebSocket.
#ifndef VS_NET_H
#define VS_NET_H

#include "base.h"

// Taille maximale d'un message applicatif (les messages du protocole sont
// courts ; au-delà, la connexion est signalée en erreur plutôt que de faire
// grossir la mémoire sans borne).
#define NET_MSG_MAX VS_KB(16)
#define NET_QUEUE_SLOTS 16

typedef enum {
    NET_EV_NONE = 0,
    NET_EV_CONNECTED,
    NET_EV_MESSAGE,
    NET_EV_CLOSED,
    NET_EV_ERROR,
} NetEventKind;

typedef struct {
    NetEventKind kind;
    u32 code;  // code Win32/WebSocket associé à l'erreur ou à la fermeture
    isize len;
    u8 data[NET_MSG_MAX];
} NetSlot;

// URL décomposée (fonction pure, testable sans réseau).
typedef struct {
    b32 secure;
    char host[256];
    int port;
    char path[1024];  // chemin + query, commence par '/'
} NetUrl;

b32 net_parse_url(Str8 url, NetUrl *out);

typedef struct {
    // file d'événements (thread réseau → boucle principale)
    NetSlot slots[NET_QUEUE_SLOTS];
    isize head;  // lu par la boucle principale
    isize tail;  // écrit par le thread réseau
    isize dropped;

    // Un SRWLOCK tient dans un pointeur et vaut SRWLOCK_INIT à zéro : on le
    // stocke tel quel pour garder windows.h hors de cet en-tête.
    void *lock;
    void *send_lock;
    void *wakeup;  // HANDLE d'événement auto-reset

    void *thread;
    void *session;  // HINTERNET WinHttpOpen
    void *connect;  // HINTERNET
    void *request;  // HINTERNET
    void *websock;  // HINTERNET (upgrade complété)

    volatile long stop;
    volatile long running;
    NetUrl url;
} Net;

// La structure fait quelques centaines de kio (file de messages à créneaux
// fixes) : l'allouer dans une arène, jamais sur la pile.

// net_init prépare la structure (pas de connexion).
void net_init(Net *n);
// net_connect démarre le thread réseau vers `url`. 0 si l'URL est invalide.
b32 net_connect(Net *n, Str8 url);
// net_send_text envoie un message texte complet. 0 en cas d'échec (la
// connexion doit alors être refermée et relancée).
b32 net_send_text(Net *n, Str8 text);
// net_poll copie le prochain événement dans `out`. 0 si la file est vide.
b32 net_poll(Net *n, NetSlot *out);
// net_wakeup_handle renvoie le HANDLE à attendre pour être réveillé.
void *net_wakeup_handle(Net *n);
// net_close ferme proprement (close WebSocket puis arrêt du thread).
void net_close(Net *n);
// net_destroy ferme la connexion et libère l'événement de réveil.
void net_destroy(Net *n);

#endif // VS_NET_H
