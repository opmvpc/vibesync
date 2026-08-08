// vlc_core.c — moitié PORTABLE du pilotage VLC (ADR-010, VS-030).
//
// Libellés d'erreur, base64, analyse d'une réponse HTTP, construction de la
// requête et de la ligne de commande, lecture de status.json : rien qui touche
// au système. Winsock, CreateProcessW et la localisation de l'exécutable
// vivent dans vlc_win32.c.
#include "vlc.h"

#include <string.h>

const char *vlc_error_text(VlcError e) {
    switch (e) {
        case VLC_OK: return "ok";
        case VLC_ERR_NOT_FOUND: return "exécutable VLC introuvable (installez VLC ou renseignez VIBESYNC_VLC)";
        case VLC_ERR_SPAWN: return "démarrage de VLC impossible";
        case VLC_ERR_SOCKET: return "socket indisponible";
        case VLC_ERR_CONNECT: return "interface HTTP de VLC injoignable";
        case VLC_ERR_SEND: return "envoi vers VLC impossible";
        case VLC_ERR_RECV: return "réponse de VLC illisible";
        case VLC_ERR_HTTP: return "statut HTTP inattendu";
        case VLC_ERR_AUTH: return "authentification refusée par l'interface HTTP";
        case VLC_ERR_JSON: return "status.json illisible";
        case VLC_ERR_TIMEOUT: return "interface HTTP de VLC muette";
    }
    return "erreur inconnue";
}

const char *vlc_error_hint(VlcError e) {
    switch (e) {
        case VLC_OK: return "";
        case VLC_ERR_NOT_FOUND: return "installez VLC, ou indiquez son chemin dans Réglages";
        case VLC_ERR_SPAWN: return "Windows a refusé de démarrer VLC : vérifiez le chemin dans Réglages";
        case VLC_ERR_SOCKET: return "aucun port local disponible ; redémarrez la machine si cela persiste";
        case VLC_ERR_AUTH:
            // Un mot de passe d'interface figé dans le vlcrc (ce que pose
            // Syncplay) écrase le nôtre : VLC répond 401 à chaque requête.
            return "un mot de passe d'interface figé dans la configuration de VLC remplace le nôtre — "
                   "videz-le, détails dans vibesync.log";
        case VLC_ERR_CONNECT:
        case VLC_ERR_TIMEOUT:
        case VLC_ERR_SEND:
        case VLC_ERR_RECV:
        case VLC_ERR_HTTP:
        case VLC_ERR_JSON:
            return "configuration VLC personnalisée probable (Syncplay) — VLC a été fermé, "
                   "détails et marche à suivre dans vibesync.log";
    }
    return "";
}

// ------------------------------------------------------------------ base64 ---

isize base64_encode(const u8 *in, isize n, char *out, isize cap) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    isize need = 4 * ((n + 2) / 3);
    if (cap < need + 1) return 0;
    isize o = 0;
    isize i = 0;
    while (i + 3 <= n) {
        u32 v = ((u32)in[i] << 16) | ((u32)in[i + 1] << 8) | in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63];
        out[o++] = tbl[v & 63];
        i += 3;
    }
    isize rest = n - i;
    if (rest == 1) {
        u32 v = (u32)in[i] << 16;
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rest == 2) {
        u32 v = ((u32)in[i] << 16) | ((u32)in[i + 1] << 8);
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

// -------------------------------------------------------------------- HTTP ---

static b32 header_value(Str8 headers, const char *name, Str8 *out) {
    Str8 key = str8_from_cstr(name);
    isize i = 0;
    while (i < headers.len) {
        isize eol = i;
        while (eol < headers.len && headers.data[eol] != '\n') eol++;
        Str8 line = str8_sub(headers, i, eol - i);
        if (line.len > 0 && line.data[line.len - 1] == '\r') line.len--;
        isize colon = str8_find_char(line, ':', 0);
        if (colon > 0) {
            Str8 k = str8_trim(str8_sub(line, 0, colon));
            if (k.len == key.len) {
                b32 same = 1;
                for (isize j = 0; j < k.len; j++) {
                    u8 a = k.data[j], b = key.data[j];
                    if (a >= 'A' && a <= 'Z') a = (u8)(a + 32);
                    if (b >= 'A' && b <= 'Z') b = (u8)(b + 32);
                    if (a != b) {
                        same = 0;
                        break;
                    }
                }
                if (same) {
                    *out = str8_trim(str8_sub(line, colon + 1, -1));
                    return 1;
                }
            }
        }
        i = eol + 1;
    }
    return 0;
}

static b32 hex_to_i64(Str8 s, i64 *out) {
    s = str8_trim(s);
    // Une extension de chunk (« 1a;foo ») est tolérée.
    isize semi = str8_find_char(s, ';', 0);
    if (semi >= 0) s = str8_sub(s, 0, semi);
    s = str8_trim(s);
    if (s.len == 0 || s.len > 15) return 0;
    i64 v = 0;
    for (isize i = 0; i < s.len; i++) {
        u8 c = s.data[i];
        i64 nib;
        if (c >= '0' && c <= '9') nib = c - '0';
        else if (c >= 'a' && c <= 'f') nib = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nib = c - 'A' + 10;
        else return 0;
        v = v * 16 + nib;
    }
    *out = v;
    return 1;
}

static b32 dechunk(Arena *a, Str8 in, Str8 *out) {
    Builder b;
    builder_init(&b, a, in.len + 1);
    isize i = 0;
    for (;;) {
        isize eol = i;
        while (eol < in.len && in.data[eol] != '\n') eol++;
        if (eol >= in.len) return 0;
        i64 size = 0;
        if (!hex_to_i64(str8_sub(in, i, eol - i), &size)) return 0;
        i = eol + 1;
        if (size == 0) break;
        if (i + size > in.len) return 0;
        builder_bytes(&b, in.data + i, (isize)size);
        i += (isize)size;
        // CRLF de fin de bloc
        while (i < in.len && (in.data[i] == '\r' || in.data[i] == '\n')) i++;
    }
    *out = builder_result(&b);
    return 1;
}

b32 http_parse_response(Arena *a, Str8 raw, int *status_code, Str8 *body) {
    *status_code = 0;
    *body = str8_lit("");
    if (raw.len < 12) return 0;
    if (!str8_starts_with(raw, str8_lit("HTTP/"))) return 0;
    isize sp = str8_find_char(raw, ' ', 0);
    if (sp < 0 || sp + 4 > raw.len) return 0;
    i64 code = 0;
    if (!str_to_i64(str8_sub(raw, sp + 1, 3), &code)) return 0;
    *status_code = (int)code;

    // Fin des en-têtes : \r\n\r\n (ou \n\n, tolérance).
    isize hdr_end = -1;
    isize skip = 0;
    for (isize i = 0; i + 1 < raw.len; i++) {
        if (raw.data[i] == '\n' && raw.data[i + 1] == '\n') {
            hdr_end = i;
            skip = 2;
            break;
        }
        if (i + 3 < raw.len && raw.data[i] == '\r' && raw.data[i + 1] == '\n' && raw.data[i + 2] == '\r' &&
            raw.data[i + 3] == '\n') {
            hdr_end = i;
            skip = 4;
            break;
        }
    }
    if (hdr_end < 0) return 0;
    Str8 headers = str8_sub(raw, 0, hdr_end);
    Str8 rest = str8_sub(raw, hdr_end + skip, -1);

    Str8 te;
    if (header_value(headers, "Transfer-Encoding", &te) && te.len >= 7) {
        // « chunked » éventuellement précédé d'autres codages
        b32 chunked = 0;
        for (isize i = 0; i + 7 <= te.len; i++) {
            if (memcmp(te.data + i, "chunked", 7) == 0 || memcmp(te.data + i, "Chunked", 7) == 0) {
                chunked = 1;
                break;
            }
        }
        if (chunked) return dechunk(a, rest, body);
    }
    Str8 cl;
    if (header_value(headers, "Content-Length", &cl)) {
        i64 n = 0;
        if (!str_to_i64(cl, &n) || n < 0) return 0;
        if (n > rest.len) return 0;  // réponse tronquée
        *body = str8_sub(rest, 0, (isize)n);
        return 1;
    }
    *body = rest;  // fermeture de connexion = fin du corps
    return 1;
}

Str8 vlc_build_request(Arena *a, Str8 path, Str8 auth_b64, int port) {
    Builder b;
    builder_init(&b, a, 256);
    builder_cstr(&b, "GET ");
    builder_str(&b, path);
    builder_cstr(&b, " HTTP/1.1\r\nHost: " VLC_HOST ":");
    builder_i64(&b, port);
    builder_cstr(&b, "\r\nAuthorization: Basic ");
    builder_str(&b, auth_b64);
    builder_cstr(&b, "\r\nUser-Agent: vibesync\r\nAccept: application/json\r\nConnection: close\r\n\r\n");
    return builder_result(&b);
}

// ------------------------------------------------ sélecteur de l'exécutable ---

// path_parent rend le répertoire contenant `path`, chaîne vide s'il n'y en a
// pas. Les deux séparateurs sont acceptés : un utilisateur qui colle un chemin
// depuis ailleurs écrit parfois des barres obliques, et Windows les accepte.
//
// Les deux racines sont des cas à part. « \foo » → « \ » et « C:\vlc.exe » →
// « C:\ » : couper à sec donnerait « C: », qui ne désigne PAS la racine du
// lecteur sous Windows mais son répertoire courant — un dossier d'ouverture
// imprévisible.
static Str8 path_parent(Str8 path) {
    isize i = path.len;
    while (i > 0 && path.data[i - 1] != '\\' && path.data[i - 1] != '/') i--;
    if (i == 0) return str8_lit("");
    isize cut = i - 1;  // longueur sans le séparateur final
    if (cut == 0) return str8_sub(path, 0, 1);
    if (cut == 2 && path.data[1] == ':') return str8_sub(path, 0, 3);
    return str8_sub(path, 0, cut);
}

Str8 vlc_browse_initial_dir(Arena *a, Str8 current, Str8 program_files, VlcDirExistsFn exists, void *ctx) {
    Str8 cur = str8_trim(current);
    // Ce que l'utilisateur a déjà saisi prime : même si le nom de fichier est
    // faux, son dossier est presque toujours le bon endroit où chercher.
    if (cur.len > 0) {
        if (exists(ctx, cur)) return str8_copy(a, cur);
        Str8 parent = path_parent(cur);
        if (parent.len > 0 && exists(ctx, parent)) return str8_copy(a, parent);
    }
    Str8 pf = str8_trim(program_files);
    if (pf.len > 0) {
        // « C:\ » a déjà son séparateur : ne pas le doubler.
        b32 sep = pf.data[pf.len - 1] == '\\' || pf.data[pf.len - 1] == '/';
        Str8 dir = str8_cat(a, pf, sep ? str8_lit("VideoLAN\\VLC") : str8_lit("\\VideoLAN\\VLC"));
        if (exists(ctx, dir)) return dir;
        if (exists(ctx, pf)) return str8_copy(a, pf);
    }
    return str8_lit("");
}

// -------------------------------------------------------------- status.json ---

static f64 clamp01(f64 v) {
    if (!f64_is_finite(v) || v < 0) return 0;
    if (v > 1) return 1;
    return v;
}

static void lower_ascii(Str8 s, char *out, isize cap) {
    isize n = VS_MIN(s.len, cap - 1);
    for (isize i = 0; i < n; i++) {
        u8 c = s.data[i];
        out[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    out[n] = 0;
}

// meta_file_name reprend l'ordre de préférence de la référence Go.
static Str8 meta_file_name(const JsonValue *root) {
    const JsonValue *info = json_get(root, "information");
    const JsonValue *cat = json_get(info, "category");
    const JsonValue *meta = json_get(cat, "meta");
    if (!meta) return str8_lit("");
    static const char *keys[] = {"filename", "title", "now_playing"};
    for (isize i = 0; i < (isize)(sizeof(keys) / sizeof(keys[0])); i++) {
        Str8 v = json_get_str(meta, keys[i], str8_lit(""));
        if (str8_trim(v).len > 0) return v;
    }
    return str8_lit("");
}

b32 vlc_parse_status(Arena *scratch, Str8 body, VsStatus *out) {
    JsonError err = JSON_OK;
    JsonValue *root = json_parse(scratch, body, &err);
    if (!root || root->kind != JSON_OBJECT) return 0;

    memset(out, 0, sizeof(*out));
    out->state = VS_PLAY_STOPPED;

    f64 length = json_get_num(root, "length", 0);
    if (f64_is_finite(length) && length > 0) out->length_sec = length;

    f64 rate = json_get_num(root, "rate", 0);
    out->rate = (f64_is_finite(rate) && rate > 0) ? rate : 1;

    char st[32];
    lower_ascii(json_get_str(root, "state", str8_lit("")), st, sizeof(st));
    if (strcmp(st, "playing") == 0) out->state = VS_PLAY_PLAYING;
    else if (strcmp(st, "paused") == 0) out->state = VS_PLAY_PAUSED;
    else out->state = VS_PLAY_STOPPED;

    if (out->length_sec > 0) {
        // Position fine : `time` n'a qu'une résolution d'une seconde.
        out->position_sec = clamp01(json_get_num(root, "position", 0)) * out->length_sec;
    } else {
        f64 t = json_get_num(root, "time", 0);
        if (f64_is_finite(t) && t > 0) out->position_sec = t;
    }
    strbuf_set(&out->file_name, meta_file_name(root));
    return 1;
}

// ------------------------------------------------------------------ client ---

void vlc_client_init(VlcClient *c, int port, Str8 password) {
    memset(c, 0, sizeof(*c));
    c->port = port;
    isize n = VS_MIN(password.len, (isize)sizeof(c->password) - 1);
    memcpy(c->password, password.data, (size_t)n);
    c->password[n] = 0;
    // Basic auth : utilisateur vide, mot de passe = celui de l'interface.
    u8 raw[128];
    isize m = 0;
    raw[m++] = ':';
    for (isize i = 0; i < n && m < (isize)sizeof(raw); i++) raw[m++] = (u8)c->password[i];
    base64_encode(raw, m, c->auth_b64, (isize)sizeof(c->auth_b64));
}

// vlc_build_command — TOUT ce dont on dépend est forcé explicitement.
//
// Le vlcrc de l'utilisateur gagne sur les défauts de VLC, jamais sur la ligne
// de commande. Un VLC configuré par Syncplay faisait échouer l'attache HTTP et
// laissait un VLC orphelin en train de jouer (VS-029, retour terrain). Chaque
// drapeau ci-dessous neutralise un réglage qui peut venir du vlcrc :
//
//   --extraintf=http     notre besoin : l'interface de pilotage. Sur la ligne
//                        de commande, elle REMPLACE l'`extraintf` du vlcrc
//                        (l'interface lua de Syncplay, typiquement).
//   --lua-intf=http      filet : si le vlcrc a fait de `luaintf` l'interface
//                        PRINCIPALE (`intf=luaintf`), notre extraintf ne la
//                        remplace pas — au moins elle exécutera notre script
//                        http et non syncplay.lua.
//   --no-one-instance                          les deux : sinon le média est
//   --no-one-instance-when-started-from-file   renvoyé à l'instance VLC déjà
//                        ouverte — qui JOUE — pendant que le process qu'on
//                        vient de lancer s'en va, et notre attache expire sur
//                        un port que plus personne n'écoute. Le second vaut
//                        VRAI par défaut chez VLC : le désactiver n'est pas
//                        redondant, c'est la cause racine la plus probable.
//   --no-playlist-enqueue    sinon le média est enfilé au lieu d'être ouvert.
//   --playlist-autostart     sinon rien ne démarre et le statut reste
//                            « stopped » : la préparation tourne dans le vide.
//   --start-paused       l'autoplay est dompté AVANT l'attache, pas après :
//                        même si l'attache échoue, rien ne part en lecture
//                        sauvage chez l'utilisateur (l'autre moitié du retour
//                        terrain). vlc_prepare_paused reste nécessaire — il
//                        constate l'état — mais converge immédiatement.
//   --no-random --no-loop --no-repeat  le moteur de sync raisonne sur un média
//                        unique joué une fois ; un vlcrc en lecture aléatoire
//                        ou en boucle le ferait mentir.
//   --no-play-and-exit   VLC ne doit pas disparaître en fin de média : la
//                        salle continue d'exister.
//   --no-video-title-show  confort, déjà là avant VS-029.
//
// Volontairement ABSENT : `--intf=<module>`. Forcer l'interface principale
// obligerait à parier sur son nom (qt/qt4 selon la version) et un nom inconnu
// empêche VLC de démarrer — le remède serait pire que le mal. Tous les
// drapeaux ci-dessus sont des options du cœur de VLC (libvlc-module.c),
// présentes depuis VLC 2.x, dont le bloc Windows pour one-instance*.
Str8 vlc_build_command(Arena *a, Str8 binary, Str8 file_path, int port, Str8 password) {
    Builder cmd;
    builder_init(&cmd, a, 1024);
    builder_cstr(&cmd, "\"");
    builder_str(&cmd, binary);
    builder_cstr(&cmd, "\" --extraintf=http --lua-intf=http --http-host=" VLC_HOST " --http-port=");
    builder_i64(&cmd, port);
    builder_cstr(&cmd, " --http-password=");
    builder_str(&cmd, password);
    builder_cstr(&cmd, " --no-one-instance --no-one-instance-when-started-from-file"
                       " --no-playlist-enqueue --playlist-autostart --start-paused"
                       " --no-random --no-loop --no-repeat --no-play-and-exit"
                       " --no-video-title-show \"");
    builder_str(&cmd, file_path);
    builder_cstr(&cmd, "\"");
    return builder_result(&cmd);
}
