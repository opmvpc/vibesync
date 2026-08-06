// net.h — client WebSocket (ws:// et wss://) via WinHTTP.
//
// Mode synchrone sur un thread réseau dédié : plus simple et plus robuste que
// les callbacks asynchrones du pool de threads WinHTTP.
//
// Règles de concurrence (revue sécurité mémoire) :
//   - `net_connect` / `net_close` / `net_destroy` / `net_poll` s'appellent
//     depuis le thread principal uniquement ; `net_send_text` depuis n'importe
//     quel thread.
//   - TOUT accès aux handles WinHTTP (création, publication, envoi,
//     destruction) se fait sous `lock`, avec un état de cycle de vie explicite
//     (DEAD → CONNECTING → OPEN → CLOSING → DEAD). Le seul appel hors verrou
//     est la réception bloquante, sur une copie locale du handle : WinHTTP
//     garantit que le handle survit à l'opération en cours après un close.
//   - `net_close` ferme les handles (ce qui débloque la réception) PUIS joint
//     le thread SANS timeout : rien n'est libéré tant que le thread vit.
//   - `net_close_graceful` ne ferme AUCUN handle : elle n'émet qu'un envoi
//     (autorisé concurremment d'une réception, contrairement à un close), pose
//     l'intention d'arrêt et attend que le thread réseau constate la close du
//     pair et ferme ses handles lui-même. Repli sur `net_close` uniquement si
//     le pair reste muet.
//   - La file d'événements ne perd jamais rien : nœuds de taille exacte dans
//     une arène dédiée, remise à zéro quand la file se vide. Si l'arène sature
//     (consommateur bloqué), la connexion est fermée avec une erreur explicite
//     plutôt que de jeter un roomState.
#ifndef VS_NET_H
#define VS_NET_H

#include "base.h"

// Taille maximale d'un message applicatif. Les messages du protocole sont
// courts ; au-delà, la connexion est signalée en erreur.
#define NET_MSG_MAX VS_KB(64)
// Mémoire de la file d'événements en attente (bornée, jamais silencieuse).
#define NET_QUEUE_ARENA VS_MB(4)
// NET_CLOSE_GRACE_MS : plafond d'attente de la close echo du serveur après un
// départ volontaire (net_close_graceful). Le thread réseau meurt dès qu'elle
// arrive, donc en pratique bien moins.
#define NET_CLOSE_GRACE_MS 250

typedef enum {
    NET_EV_NONE = 0,
    NET_EV_CONNECTED,
    NET_EV_MESSAGE,
    NET_EV_CLOSED,
    NET_EV_ERROR,
} NetEventKind;

// Codes d'erreur propres au client (au-delà des codes Win32).
#define NET_ERR_MSG_TOO_BIG 0xE0000001u
#define NET_ERR_QUEUE_FULL 0xE0000002u
#define NET_ERR_NO_MEMORY 0xE0000003u

// NetSlot est la copie remise à l'appelant par net_poll.
typedef struct {
    NetEventKind kind;
    u32 code;
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

typedef enum {
    NET_STATE_DEAD = 0,
    NET_STATE_CONNECTING,
    NET_STATE_OPEN,
    NET_STATE_CLOSING,
} NetState;

typedef struct NetEvent NetEvent;

typedef struct {
    // Un SRWLOCK tient dans un pointeur et vaut SRWLOCK_INIT à zéro : on le
    // stocke tel quel pour garder windows.h hors de cet en-tête.
    void *lock;        // état + handles WinHTTP
    void *queue_lock;  // file d'événements
    void *wakeup;      // HANDLE d'événement auto-reset

    Arena *queue_arena;
    NetEvent *head;
    NetEvent *tail;
    isize queued;
    isize dropped;  // doit rester à 0 : la saturation ferme la connexion

    void *thread;    // thread principal uniquement
    void *session;   // HINTERNET, sous `lock`
    void *connect;   // HINTERNET, sous `lock`
    void *request;   // HINTERNET, sous `lock`
    void *websock;   // HINTERNET, sous `lock`
    NetState state;  // sous `lock`

    volatile long stop;
    NetUrl url;

    // Réveil du thread UI : PostMessageW(notify_hwnd, notify_msg, 0, 0) à
    // chaque événement mis en file (en plus de l'événement auto-reset).
    void *notify_hwnd;
    unsigned notify_msg;
} Net;

// net_poll écrit dans un NetSlot de 64 Kio : allouer le Net et le NetSlot dans
// une arène, jamais sur la pile.

// net_init prépare la structure (pas de connexion). 0 si les ressources
// système manquent.
b32 net_init(Net *n);
// net_connect démarre le thread réseau vers `url`. Toute connexion précédente
// est fermée et jointe d'abord : il n'y a jamais deux threads réseau.
b32 net_connect(Net *n, Str8 url);
// net_send_text envoie un message texte complet. 0 en cas d'échec (la
// connexion doit alors être refermée et relancée).
b32 net_send_text(Net *n, Str8 text);
// net_poll copie le prochain événement dans `out`. 0 si la file est vide.
b32 net_poll(Net *n, NetSlot *out);
// net_wakeup_handle renvoie le HANDLE à attendre pour être réveillé.
void *net_wakeup_handle(Net *n);
// net_set_notify fait poster `msg` à `hwnd` à chaque événement (thread UI).
void net_set_notify(Net *n, void *hwnd, unsigned msg);
// net_state renvoie l'état courant du cycle de vie.
NetState net_state(Net *n);
// net_close ferme la connexion et joint le thread réseau (idempotent).
void net_close(Net *n);
// net_close_graceful signale un DÉPART VOLONTAIRE (bouton « Quitter la salle »,
// fermeture de la fenêtre) : trame de fermeture WebSocket 1000, attente bornée
// que le thread réseau constate la close echo et se retire, puis net_close.
// Sans elle, le serveur garde un membre zombie jusqu'à son timeout de lecture
// (60 s) et le pseudo reste pris — exactement le retour terrain de VS-028.
// Thread principal uniquement. Ne ferme aucun handle elle-même (voir les règles
// de concurrence en tête de fichier).
void net_close_graceful(Net *n, i64 grace_ms);
// net_destroy ferme puis libère l'événement de réveil et l'arène de la file.
void net_destroy(Net *n);

#endif // VS_NET_H
