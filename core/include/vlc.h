// vlc.h — pilotage de VLC local : localisation de l'exécutable, lancement
// (CreateProcessW avec interface HTTP sur 127.0.0.1, port et mot de passe
// aléatoires), requêtes HTTP/1.1 en Winsock direct, lecture de status.json.
//
// Voir ADR-003 et internal/vlc (implémentation Go de référence).
#ifndef VS_VLC_H
#define VS_VLC_H

#include "base.h"
#include "engine.h"
#include "json.h"

typedef enum {
    VLC_OK = 0,
    VLC_ERR_NOT_FOUND,   // exécutable introuvable
    VLC_ERR_SPAWN,       // CreateProcessW en échec
    VLC_ERR_SOCKET,      // socket/WSAStartup
    VLC_ERR_CONNECT,     // interface HTTP injoignable
    VLC_ERR_SEND,
    VLC_ERR_RECV,
    VLC_ERR_HTTP,        // code de statut inattendu
    VLC_ERR_AUTH,        // 401 : mot de passe refusé
    VLC_ERR_JSON,        // status.json illisible
    VLC_ERR_TIMEOUT,     // interface muette
} VlcError;

const char *vlc_error_text(VlcError e);
// vlc_error_hint donne la PISTE à afficher à l'utilisateur (chaîne vide s'il
// n'y en a pas d'utile). « interface HTTP muette » sans piste, c'est un mur :
// la cause la plus fréquente est une configuration VLC personnalisée
// (VS-029), et l'utilisateur ne peut pas la deviner.
const char *vlc_error_hint(VlcError e);

#define VLC_HOST "127.0.0.1"
// Préparation du média (pause + position 0), cf. internal/vlc.Prepare.
#define VLC_PREPARE_TIMEOUT_MS 15000
#define VLC_PREPARE_POLL_MS 20
// Le seek HTTP est arrondi à la seconde : viser mieux que la demi-seconde
// n'aurait pas de sens.
#define VLC_START_TOLERANCE 0.5

typedef struct {
    int port;
    char password[64];
    char auth_b64[128];  // « Basic <base64(:mot de passe)> »
    void *process;       // HANDLE du process lancé (NULL si non lancé par nous)
    b32 keep_alive;      // ne pas tuer VLC à la fermeture
} VlcClient;

// --- fonctions pures (testables sans réseau ni process) ---

// vlc_parse_status convertit un status.json en VsStatus, avec le même
// assainissement que la référence Go (position fine = position × length,
// fraction bornée à [0,1], rate ≤ 0 ramené à 1, durée < 0 ignorée).
b32 vlc_parse_status(Arena *scratch, Str8 body, VsStatus *out);

// http_parse_response découpe une réponse HTTP brute : code de statut et
// corps (Content-Length ou Transfer-Encoding: chunked). 0 si la réponse est
// tronquée ou malformée.
b32 http_parse_response(Arena *a, Str8 raw, int *status_code, Str8 *body);

// base64_encode écrit l'encodage de `n` octets (out doit tenir 4*((n+2)/3)+1).
isize base64_encode(const u8 *in, isize n, char *out, isize cap);

// vlc_build_request fabrique la requête GET (chemin + query déjà formés).
Str8 vlc_build_request(Arena *a, Str8 path, Str8 auth_b64, int port);

// VlcDirExistsFn : prédicat « ce répertoire existe ». Il est INJECTÉ pour que
// le choix du dossier d'ouverture du sélecteur reste une fonction pure, donc
// testable sans toucher au disque ni ouvrir de boîte de dialogue.
typedef b32 (*VlcDirExistsFn)(void *ctx, Str8 dir);

// vlc_browse_initial_dir choisit le répertoire où ouvrir le sélecteur de
// vlc.exe, du plus au moins pertinent :
//   1. le chemin déjà saisi s'il désigne un répertoire existant,
//   2. sinon le répertoire qui le contient s'il existe (cas normal : le champ
//      contient « …\VLC\vlc.exe », on ouvre sur « …\VLC »),
//   3. sinon <program_files>\VideoLAN\VLC s'il existe,
//   4. sinon <program_files> s'il existe,
//   5. sinon la chaîne vide : à Windows de décider (dernier dossier utilisé).
// Le résultat vit dans `a`. `program_files` peut être vide.
Str8 vlc_browse_initial_dir(Arena *a, Str8 current, Str8 program_files, VlcDirExistsFn exists, void *ctx);

// vlc_build_command fabrique la ligne de commande de lancement. Séparée du
// lancement pour être vérifiable sans process : ces drapeaux sont la partie
// fragile de VS-029 (blindage contre le vlcrc de l'utilisateur), et une
// régression y est invisible autrement.
Str8 vlc_build_command(Arena *a, Str8 binary, Str8 file_path, int port, Str8 password);

// --- pilotage réel ---

// vlc_locate cherche l'exécutable : %VIBESYNC_VLC%, puis les emplacements
// standards de Windows.
b32 vlc_locate(Arena *a, Str8 *out_path);

// vlc_client_init prépare un client sur un port/mot de passe donnés (utile
// pour se rebrancher sur un VLC déjà lancé).
void vlc_client_init(VlcClient *c, int port, Str8 password);

// vlc_launch lance VLC sur `file_path`, attend que son interface HTTP réponde
// puis force pause + position 0 (cf. vlc_prepare_paused). En cas d'échec, le
// process lancé est arrêté : pas d'orphelin. timeout_ms ≤ 0 → 20 000.
VlcError vlc_launch(Arena *scratch, VlcClient *c, Str8 binary, Str8 file_path, i64 timeout_ms);

// vlc_prepare_paused force pause + position 0 et ne rend la main qu'une fois
// l'état « en pause » observé (docs/protocol.md §Chargement de fichier).
VlcError vlc_prepare_paused(VlcClient *c, Arena *scratch, i64 timeout_ms);

VlcError vlc_status(VlcClient *c, Arena *scratch, VsStatus *out);
VlcError vlc_pause(VlcClient *c, Arena *scratch);
VlcError vlc_resume(VlcClient *c, Arena *scratch);
VlcError vlc_seek(VlcClient *c, Arena *scratch, f64 position_sec);
VlcError vlc_set_rate(VlcClient *c, Arena *scratch, f64 rate);
// vlc_apply exécute une décision du moteur.
VlcError vlc_apply(VlcClient *c, Arena *scratch, VsCmd cmd);

void vlc_close(VlcClient *c);

#endif // VS_VLC_H
