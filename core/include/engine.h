// engine.h — moteur de synchronisation, machine à états PURE.
//
// Port C de internal/client (implémentation Go de référence), conforme à
// docs/protocol.md §Comportements client et gelé par test/vectors/*.json.
//
// Aucune dépendance à net.c ni vlc.c : le moteur reçoit des observations
// (pong, roomState, statut VLC, tic d'horloge) et rend des décisions
// (commandes VLC, messages serveur). C'est ce qui le rend rejouable contre les
// vecteurs de conformité.
#ifndef VS_ENGINE_H
#define VS_ENGINE_H

#include "base.h"

// --- constantes de synchronisation (docs/protocol.md) ---
#define VS_POLL_INTERVAL_NS (200 * 1000000LL)
// Zone morte élargie à 1,5 s par VS-038 : au-dessus du bruit de la position
// rendue par VLC (±0,15 s) et du perceptible. La vitesse n'est JAMAIS utilisée
// pour corriger la dérive ; au-delà de la zone morte c'est un micro-seek, et
// seulement si la dérive PERSISTE (médiane des VS_DRIFT_SAMPLES derniers polls).
#define VS_DEAD_ZONE_SEC 1.5
// Au-delà de ce seuil, seek immédiat : on ne consulte pas la médiane (réveil de
// veille, lecteur qui décroche).
#define VS_HARD_SEEK_SEC 5.0
// Historique de dérive : 5 polls ≈ 1 s. Le micro-seek exige un historique PLEIN
// dont la médiane dépasse la zone morte (docs/protocol.md §Persistance).
#define VS_DRIFT_SAMPLES 5
#define VS_USER_SEEK_SEC 3.0
#define VS_GRACE_NS (500 * 1000000LL)
#define VS_USER_HOLD_NS (2000 * 1000000LL)
#define VS_PAUSED_SEEK_SEC 0.6
#define VS_MIN_RATE 0.25
#define VS_MAX_RATE 4.0
#define VS_PING_EVERY_NS (2000 * 1000000LL)
#define VS_REPORT_EVERY_NS (1000 * 1000000LL)
#define VS_OFFSET_SAMPLES 5
#define VS_BACKOFF_MIN_NS (1000 * 1000000LL)
#define VS_BACKOFF_MAX_NS (10000 * 1000000LL)
// Départ de lecture : au-delà de cet écart, on cale VLC par un seek AVANT de
// jouer — un demi-seconde d'écart est sous la zone morte, plus rien ne le
// résorberait ensuite.
#define VS_START_SEEK_SEC 0.3
// Détection de buffering neutralisée pendant 2 s après tout seek (commandé ou
// utilisateur) et toute transition play/pause : ces actions figent
// mécaniquement la position (docs/protocol.md §Comportements client).
#define VS_BUFFERING_SUSPEND_NS (2000 * 1000000LL)
// Anti-masquage : une nouvelle suspension ne peut pas démarrer moins de 1 s
// après la fin de la précédente, et une suspension en cours n'est jamais
// prolongée. Sans ces deux règles, des seeks de correction en boucle
// masqueraient indéfiniment un VLC réellement figé.
#define VS_BUFFERING_COOLDOWN_NS (1000 * 1000000LL)
// File des chats composés hors ligne : au-delà, les plus anciens sont
// abandonnés (docs/protocol.md §File d'attente hors ligne).
#define VS_CHAT_QUEUE_MAX 20
// Salle vierge : un lecteur local au-delà de ce seuil déclenche UNE reprise.
#define VS_VIRGIN_RESUME_SEC 5.0

// Bornes des horodatages epoch en millisecondes acceptés (1970 → 2100). Toute
// valeur hors bornes est rejetée : les soustractions de l'offset d'horloge
// doivent rester loin d'un débordement signé (comportement indéfini).
#define VS_MS_MIN 0LL
#define VS_MS_MAX 4102444800000LL

b32 vs_valid_epoch_ms(i64 ms);

// --- état observé de VLC ---
typedef enum {
    VS_PLAY_STOPPED = 0,
    VS_PLAY_PLAYING,
    VS_PLAY_PAUSED,
} VsPlayState;

typedef struct {
    VsPlayState state;
    f64 position_sec;  // position fine (position × length)
    f64 length_sec;
    f64 rate;
    StrBuf file_name;
} VsStatus;

b32 vs_status_loaded(const VsStatus *s);
const char *vs_play_state_name(VsPlayState s);

// --- état de salle (serveur) ---
typedef struct {
    b32 paused;
    f64 position_sec;
    f64 rate;
    i64 ref_server_ms;
    StrBuf set_by;
} VsRoomState;

typedef struct {
    i64 t;
    i64 server_ms;
} VsPong;

// --- décisions ---
typedef enum {
    VS_CMD_PAUSE = 0,
    VS_CMD_RESUME,
    VS_CMD_SEEK,
    VS_CMD_RATE,
} VsCmdKind;

typedef struct {
    VsCmdKind kind;
    f64 value;
} VsCmd;

const char *vs_cmd_name(VsCmdKind k);

typedef enum {
    VS_ACT_PLAY = 0,
    VS_ACT_PAUSE,
    VS_ACT_SEEK,
} VsAction;

const char *vs_action_name(VsAction a);

typedef enum {
    VS_MSG_PING = 0,
    VS_MSG_SET_READY,
    VS_MSG_SET_FILE,
    VS_MSG_CONTROL,
    VS_MSG_REPORT,
    VS_MSG_CHAT,
} VsMsgKind;

const char *vs_msg_name(VsMsgKind k);

typedef struct {
    VsMsgKind kind;
    i64 t;             // ping
    b32 ready;         // setReady
    StrBuf name;       // setFile
    f64 duration_sec;  // setFile
    i64 size_bytes;    // setFile
    VsAction action;   // control
    f64 position_sec;  // control, report
    b32 paused;        // report
    b32 buffering;     // report
    StrBuf text;       // chat
} VsMsg;

#define VS_MAX_CMDS 8
#define VS_MAX_MSGS 8

#define VS_MAX_MSGS_QUEUED (VS_MAX_MSGS + VS_CHAT_QUEUE_MAX)

typedef struct {
    VsCmd cmds[VS_MAX_CMDS];
    isize cmd_count;
    // Le welcome peut rendre d'un coup setFile + setReady + ping + control de
    // reprise + toute la file de chat hors ligne : la capacité en tient compte.
    VsMsg msgs[VS_MAX_MSGS_QUEUED];
    isize msg_count;
    b32 dropped;  // débordement : ne doit jamais arriver, sinon bug
    // Reprise « salle vierge » : le moteur reste pur, il signale seulement
    // qu'un toast « Reprise à … » est à afficher, à la position donnée.
    b32 have_resume_toast;
    f64 resume_toast_sec;
} VsOutput;

void vs_output_reset(VsOutput *o);

// --- état interne ---
typedef enum {
    VS_PHASE_IDLE = 0,
    VS_PHASE_CONNECTING,
    VS_PHASE_CONNECTED,
} VsPhase;

typedef enum {
    VS_CORRECT_NONE = 0,
    VS_CORRECT_SEEK,
} VsCorrection;

// Ce que le moteur croit que VLC est en train de faire.
typedef struct {
    b32 valid;
    b32 paused;
    f64 pos;
    i64 at;
    f64 rate;
} VsExpectation;

// Détecteur de buffering : la position n'avance plus alors que VLC se déclare
// en lecture (l'interface HTTP de VLC n'expose pas d'état de buffering).
typedef struct {
    b32 have;
    b32 buffering;
    i64 stall_from;  // VS_TIME_ZERO = pas de stagnation en cours
    f64 last_pos;
    i64 last_at;
    // suspend_until : instant jusqu'auquel la détection est neutralisée. Le
    // VERDICT courant survit à la suspension — envoyer un seek ne prouve pas
    // que la lecture est repartie, et le seek de correction envoyé justement
    // parce que le lecteur décroche effacerait sinon le diagnostic à chaque
    // fois. Survit aussi à buf_reset, comme en Go.
    i64 suspend_until;
} VsBufferDetect;

typedef struct {
    VsPhase phase;
    StrBuf self_id;
    b32 ready;

    // horloge serveur
    i64 offsets[VS_OFFSET_SAMPLES];
    isize offset_count;
    b32 have_offset;
    i64 offset_ms;
    i64 latency_ms;

    // état de salle de référence
    VsRoomState room_state;
    b32 have_state;
    VsRoomState pending_rs;  // roomState d'autrui mémorisé pendant le hold
    b32 have_pending_rs;

    // fenêtres temporelles (VS_TIME_ZERO = « zéro », rien n'est avant)
    i64 grace_until;
    i64 hold_until;
    i64 user_hold_until;

    // lecteur
    VsStatus status;
    b32 have_status;
    b32 vlc_error;
    VsExpectation expect;
    VsBufferDetect buf;
    b32 buffering;
    f64 applied_rate;
    f64 drift;
    // Historique des |drift| des derniers polls corrigeables EN LECTURE : le
    // micro-seek exige que sa médiane dépasse la zone morte, ce qui interdit au
    // bruit de mesure de déclencher un recalage (docs/protocol.md §Persistance
    // de la dérive). Vidé à chaque seek émis, dès que la lecture s'interrompt
    // et à toute invalidation de la référence.
    f64 drifts[VS_DRIFT_SAMPLES];
    isize drift_count;
    VsCorrection correcting;

    // fichier déclaré
    StrBuf file_name;
    f64 file_duration_sec;
    i64 file_size_bytes;
    b32 have_file;

    // tâches périodiques
    i64 last_ping;
    i64 last_report;

    // File des chats composés hors ligne : SEULS les chats sont rejoués. Ni
    // setReady ni setFile (l'état courant est re-déclaré à chaque welcome), et
    // JAMAIS un control — une action périmée écraserait la salle.
    // La file est LIÉE À LA SALLE : changer de salle ou se déconnecter
    // volontairement la vide sans envoi (docs/protocol.md §File d'attente).
    StrBuf chat_queue[VS_CHAT_QUEUE_MAX];
    isize chat_queue_count;

    // Mémoire de séance, par salle et par processus : conditions cumulatives de
    // la reprise « salle vierge ».
    StrBuf session_room;  // salle visée (posée par engine_set_room)
    b32 had_session;      // déjà connecté à CETTE salle dans CE processus
    // Dernière position de salle OBSERVÉE (échantillonnée à chaque tic tant que
    // la salle est réellement pilotée). Elle survit à la coupure : c'est elle
    // qu'on propose en reprise, telle quelle — pas une projection, pas la
    // position brute de VLC.
    f64 last_room_pos;
    b32 have_last_room_pos;
} VsEngine;

void engine_init(VsEngine *e);

// --- transitions de connexion ---
// engine_set_room déclare la salle visée. Changer de salle vide la file de chat
// sans envoi et oublie la mémoire de séance : ni les messages ni la position
// d'une salle ne doivent fuir vers une autre.
void engine_set_room(VsEngine *e, Str8 room);
void engine_connecting(VsEngine *e);   // tentative de connexion en cours
void engine_session_lost(VsEngine *e);  // session perdue : référence invalidée
void engine_disconnected(VsEngine *e);  // arrêt volontaire

// --- entrées serveur ---
// self_ready : NULL si la vue serveur du ready n'est pas connue.
void engine_on_welcome(VsEngine *e, i64 now, Str8 self_id, const VsRoomState *st,
                       const b32 *self_ready, VsOutput *out);
void engine_on_pong(VsEngine *e, i64 now, VsPong p);
void engine_on_roomstate(VsEngine *e, i64 now, const VsRoomState *rs);
void engine_on_self_ready(VsEngine *e, b32 ready);

// --- entrées lecteur ---
void engine_open_file(VsEngine *e, Str8 name, i64 size_bytes, VsOutput *out);
void engine_on_vlc_status(VsEngine *e, i64 now, const VsStatus *st, VsOutput *out);
void engine_on_vlc_error(VsEngine *e);

// --- tic d'horloge : lève le hold, décide des corrections, tâches périodiques ---
void engine_on_tick(VsEngine *e, i64 now, VsOutput *out);

// --- actions de l'utilisateur venues de l'UI ---
void engine_user_control(VsEngine *e, i64 now, VsAction action, f64 position_sec, b32 use_pos, VsOutput *out);
void engine_set_ready(VsEngine *e, b32 ready, VsOutput *out);
// engine_chat envoie un message… ou le met en file s'il est composé hors ligne.
void engine_chat(VsEngine *e, Str8 text, VsOutput *out);

// --- file de chat hors ligne (affichage « en attente » dans l'UI) ---
isize engine_pending_chat_count(const VsEngine *e);
Str8 engine_pending_chat(const VsEngine *e, isize index);

// --- lecture d'état (UI, tests) ---
f64 engine_expected_position(const VsEngine *e, i64 now);
f64 engine_room_rate(const VsEngine *e);
i64 engine_now_server_ms(const VsEngine *e, i64 now);
f64 engine_clamp_position(f64 pos, f64 duration);
b32 engine_sanitize_roomstate(const VsRoomState *in, VsRoomState *out);
// engine_next_backoff applique le doublement borné 1 s → 10 s.
i64 engine_next_backoff(i64 current_ns);

#endif // VS_ENGINE_H
