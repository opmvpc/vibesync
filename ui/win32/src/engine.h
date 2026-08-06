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
#define VS_DEAD_ZONE_SEC 0.1
#define VS_HARD_SEEK_SEC 2.0
#define VS_NUDGE_FAST 1.05
#define VS_NUDGE_SLOW 0.95
#define VS_NUDGE_EXIT_SEC 0.03
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

typedef struct {
    VsCmd cmds[VS_MAX_CMDS];
    isize cmd_count;
    VsMsg msgs[VS_MAX_MSGS];
    isize msg_count;
    b32 dropped;  // débordement : ne doit jamais arriver, sinon bug
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
    VS_CORRECT_NUDGE,
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
    b32 nudging;
    f64 drift;
    VsCorrection correcting;

    // fichier déclaré
    StrBuf file_name;
    f64 file_duration_sec;
    i64 file_size_bytes;
    b32 have_file;

    // tâches périodiques
    i64 last_ping;
    i64 last_report;
} VsEngine;

void engine_init(VsEngine *e);

// --- transitions de connexion ---
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
void engine_chat(VsEngine *e, Str8 text, VsOutput *out);

// --- lecture d'état (UI, tests) ---
f64 engine_expected_position(const VsEngine *e, i64 now);
f64 engine_room_rate(const VsEngine *e);
i64 engine_now_server_ms(const VsEngine *e, i64 now);
f64 engine_clamp_position(f64 pos, f64 duration);
b32 engine_sanitize_roomstate(const VsRoomState *in, VsRoomState *out);
// engine_next_backoff applique le doublement borné 1 s → 10 s.
i64 engine_next_backoff(i64 current_ns);

#endif // VS_ENGINE_H
