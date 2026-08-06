// protocol.h — encodage/décodage des messages de docs/protocol.md (v1)
// au-dessus de json.c. Aucune E/S : des chaînes en entrée, des chaînes en
// sortie, tout dans une arène.
#ifndef VS_PROTOCOL_H
#define VS_PROTOCOL_H

#include "base.h"
#include "engine.h"
#include "json.h"

#define VS_PROTOCOL_VERSION 1
// Jeton de reprise de session : 16 octets aléatoires en hexadécimal.
#define VS_SESSION_TOKEN_BYTES 16
#define VS_SESSION_TOKEN_LEN (VS_SESSION_TOKEN_BYTES * 2)
// VS_SESSION_TOKEN_MAX borne un jeton RELU des réglages (le nôtre en fait 32,
// mais le fichier est éditable à la main et peut venir d'une autre version).
// Même valeur que maxSessionTokenLen du client Go de référence : au-delà, le
// serveur refuserait le hello.
#define VS_SESSION_TOKEN_MAX 128

// --- client → serveur ---

// proto_encode_hello construit le premier message de la session. `password` et
// `session` sont omis s'ils sont vides.
Str8 proto_encode_hello(Arena *a, Str8 name, Str8 room, Str8 password, Str8 session);
// proto_encode_msg encode une décision du moteur (ping, setReady, setFile,
// control, report, chat).
Str8 proto_encode_msg(Arena *a, const VsMsg *m);

// proto_session_token remplit `out` (VS_SESSION_TOKEN_LEN+1 octets) avec un
// jeton hexadécimal tiré du générateur du système (BCryptGenRandom).
b32 proto_session_token(char *out, isize cap);
// proto_session_token_valid dit si un jeton relu des réglages est réutilisable
// tel quel : hexadécimal (casse indifférente), longueur paire, au moins
// VS_SESSION_TOKEN_BYTES octets et au plus VS_SESSION_TOKEN_MAX caractères —
// les mêmes règles que validSessionToken() du client Go. Un jeton tronqué ou
// bricolé à la main est refusé et remplacé : mieux vaut un jeton neuf qu'un
// jeton que le serveur rejettera.
b32 proto_session_token_valid(Str8 token);
// proto_hex encode `n` octets en hexadécimal minuscule dans `out`.
void proto_hex(const u8 *bytes, isize n, char *out);

// --- serveur → client ---

typedef enum {
    VS_IN_UNKNOWN = 0,
    VS_IN_WELCOME,
    VS_IN_PONG,
    VS_IN_ROOMSTATE,
    VS_IN_USERS,
    VS_IN_CHATEVENT,
    VS_IN_TOAST,
    VS_IN_ERROR,
} VsInKind;

typedef struct {
    Str8 id;
    Str8 name;
    b32 ready;
    f64 position_sec;
    i64 latency_ms;
    b32 has_file;
    Str8 file_name;
    f64 file_duration_sec;
    i64 file_size_bytes;
} VsUser;

#define VS_MAX_USERS 64

typedef struct {
    VsInKind kind;
    Str8 type;   // type brut, même si inconnu (forward-compat)
    b32 invalid;  // type connu mais champs obligatoires absents ou mal typés

    // welcome
    Str8 self_id;
    Str8 room;
    b32 have_self_ready;
    b32 self_ready;
    // Champs additifs (VS-023) : version applicative du serveur et adresse de
    // téléchargement des clients. Absents des serveurs plus anciens — leur
    // absence n'invalide pas le welcome.
    Str8 server_version;
    Str8 download_url;

    // welcome.state / roomState
    VsRoomState state;
    b32 have_state;

    // welcome.users / users
    VsUser *users;
    isize user_count;

    // pong
    VsPong pong;

    // chatEvent / toast / error
    Str8 from;
    Str8 text;
    Str8 level;
    Str8 code;
    i64 server_ms;
} VsInMsg;

// proto_decode analyse une enveloppe {type, data}. Renvoie NULL si le message
// est illisible (JSON invalide, enveloppe sans type) ; un type inconnu donne
// un message de kind VS_IN_UNKNOWN, à ignorer sans fermer la connexion.
VsInMsg *proto_decode(Arena *a, Str8 raw);

// proto_fill remplit un message à partir d'un type et du nœud `data` déjà
// analysé (utilisé par le rejeu des vecteurs de conformité, qui portent les
// mêmes données sans l'enveloppe).
void proto_fill(Arena *a, Str8 type, const JsonValue *data, VsInMsg *m);

// proto_error_is_fatal dit si un code d'erreur serveur interdit de réessayer.
b32 proto_error_is_fatal(Str8 code);

// proto_newer_version dit si la version `server` est strictement plus récente
// que `client` — la seule question que posent les deux clients, pour proposer
// une mise à jour. Portage exact de `NewerVersion` (internal/client/version.go).
//
// Format : `major[.minor[.patch]]`, chiffres seulement, « v » initial toléré,
// suffixe de pré-version (« -rc1 ») et métadonnées de build (« +sha ») admis,
// espaces de bord rognés. Renvoie 0 dès que l'une des deux est ILLISIBLE
// (« dev », vide, texte, plus de trois composants) : un build non versionné ne
// doit jamais provoquer d'invitation à mettre à jour. À triplet égal, une
// pré-version est antérieure à la version nue ; deux pré-versions du même
// triplet ne sont pas départagées. Pas un moteur semver complet.
b32 proto_newer_version(Str8 server, Str8 client);

#endif // VS_PROTOCOL_H
