// json.h — parseur et écrivain JSON minimaux mais corrects, sans allocation
// hors arène. Conçus pour des entrées semi-fiables (notre serveur, VLC local) :
// profondeur bornée, tailles bornées, refus propre au lieu du crash.
#ifndef VS_JSON_H
#define VS_JSON_H

#include "base.h"

#define JSON_MAX_DEPTH 32
#define JSON_MAX_INPUT VS_MB(8)
// Budget de valeurs : un document dense (des centaines de milliers de petites
// valeurs) doit être refusé proprement, pas épuiser l'arène de travail.
#define JSON_MAX_VALUES 100000
// Le parseur refuse aussi de dépasser cette fraction de l'arène fournie.
#define JSON_ARENA_BUDGET_NUM 3
#define JSON_ARENA_BUDGET_DEN 4

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonKind;

typedef struct JsonValue JsonValue;
struct JsonValue {
    JsonKind kind;
    Str8 key;     // clé, si le nœud est membre d'un objet
    Str8 string;  // valeur, si JSON_STRING (déjà déséchappée, UTF-8)
    f64 number;
    b32 boolean;
    JsonValue *first;  // premier enfant (tableau/objet)
    JsonValue *last;
    JsonValue *next;  // frère suivant
    isize count;      // nombre d'enfants
};

typedef enum {
    JSON_OK = 0,
    JSON_ERR_EMPTY,
    JSON_ERR_TOO_BIG,
    JSON_ERR_DEPTH,
    JSON_ERR_SYNTAX,
    JSON_ERR_STRING,
    JSON_ERR_NUMBER,
    JSON_ERR_TRAILING,
    JSON_ERR_UTF8,
    JSON_ERR_BUDGET,
} JsonError;

const char *json_error_text(JsonError e);

// json_parse analyse `text` dans l'arène `a`. Renvoie NULL en cas d'erreur
// (le code est déposé dans `err`, qui peut être NULL). Aucune allocation hors
// de l'arène ; l'arène peut être remise à zéro d'un bloc ensuite.
JsonValue *json_parse(Arena *a, Str8 text, JsonError *err);

// --- accès (tous tolérants au NULL) ---
// json_get renvoie la DERNIÈRE occurrence de la clé, comme encoding/json en
// Go : une clé dupliquée dans un message piégé doit produire le même
// comportement chez tous les clients.
JsonValue *json_get(const JsonValue *obj, const char *key);
JsonValue *json_at(const JsonValue *arr, isize index);
f64 json_num(const JsonValue *v, f64 def);
i64 json_i64(const JsonValue *v, i64 def);
b32 json_bool(const JsonValue *v, b32 def);
Str8 json_str(const JsonValue *v, Str8 def);
// Raccourcis sur un membre d'objet.
f64 json_get_num(const JsonValue *obj, const char *key, f64 def);
i64 json_get_i64(const JsonValue *obj, const char *key, i64 def);
b32 json_get_bool(const JsonValue *obj, const char *key, b32 def);
Str8 json_get_str(const JsonValue *obj, const char *key, Str8 def);

// ------------------------------------------------------------- écriture ---

typedef struct {
    Builder out;
    isize depth;
    b32 has_item[JSON_MAX_DEPTH + 2];
    b32 pending_key;  // une clé vient d'être écrite : pas de virgule avant la valeur
    b32 overflow;     // profondeur dépassée : la sortie est marquée invalide
} JsonWriter;

void jw_init(JsonWriter *w, Arena *a);
void jw_obj_begin(JsonWriter *w);
void jw_obj_end(JsonWriter *w);
void jw_arr_begin(JsonWriter *w);
void jw_arr_end(JsonWriter *w);
void jw_key(JsonWriter *w, const char *key);
void jw_str(JsonWriter *w, Str8 v);
void jw_cstr(JsonWriter *w, const char *v);
void jw_num(JsonWriter *w, f64 v);
void jw_i64(JsonWriter *w, i64 v);
void jw_bool(JsonWriter *w, b32 v);
void jw_null(JsonWriter *w);
// Raccourcis clé/valeur.
void jw_kv_str(JsonWriter *w, const char *key, Str8 v);
void jw_kv_num(JsonWriter *w, const char *key, f64 v);
void jw_kv_i64(JsonWriter *w, const char *key, i64 v);
void jw_kv_bool(JsonWriter *w, const char *key, b32 v);
Str8 jw_result(const JsonWriter *w);

#endif // VS_JSON_H
