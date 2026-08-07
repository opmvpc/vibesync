// auto.h — pilotage de l'application par variables d'environnement (VS-029).
//
// Miroir exact de ui/macos/Sources/VibeSync/UI/AutoPilot.swift : le client C
// n'a pas plus de ligne de commande utile qu'une app AppKit, et sans mode
// pilotable aucun harnais ne peut prouver qu'il détecte une action faite DANS
// VLC. On lui donne donc trois choses, et rien de plus :
//
//   1. de quoi se connecter sans interaction (URL, pseudo, salle, mot de
//      passe, fichier à ouvrir) ;
//   2. un fichier d'état JSON réécrit chaque seconde, sur lequel le script
//      fonde ses assertions ;
//   3. un fichier de commandes que le script remplit ligne à ligne
//      (`play`, `pause`, `seek 42`, `ready`, `chat coucou`, `open …`, `quit`)
//      et que l'application consomme au fil de sa boucle.
//
// Un fichier de commandes plutôt qu'un scénario minuté : le script décide QUAND
// passer à l'étape suivante, une fois son point de contrôle vérifié.
//
// **Rien n'est actif hors mode auto** : sans `VIBESYNC_AUTO_URL`,
// `auto_from_env` rend 0 et l'application se comporte comme d'habitude.
#ifndef VS_AUTO_H
#define VS_AUTO_H

#include "base.h"

// Période de réécriture du fichier d'état (comme AutoPilot.statusEverySec).
#define VS_AUTO_STATUS_PERIOD_MS 1000
// Période de relecture du fichier de commandes.
#define VS_AUTO_PUMP_MS 250
// Borne de lecture du fichier de commandes : un script en écrit quelques lignes.
#define VS_AUTO_CMDS_MAX VS_KB(64)

typedef struct {
    b32 on;  // 0 = mode normal, aucun changement de comportement
    Str8 url;
    Str8 name;
    Str8 room;
    Str8 password;
    Str8 file;         // média ouvert au démarrage (vide = aucun)
    Str8 status_path;  // état JSON (vide = pas d'état publié)
    Str8 cmds_path;    // fichier de commandes (vide = pas de pilotage)
    Str8 scenario;     // étiquette libre recopiée dans l'état

    isize cmds_done;    // lignes déjà exécutées
    i64 last_status_ms; // dernière publication d'état
} AutoPilot;

typedef enum {
    AUTO_CMD_NONE = 0,
    AUTO_CMD_PLAY,
    AUTO_CMD_PAUSE,
    AUTO_CMD_SEEK,
    AUTO_CMD_READY,
    AUTO_CMD_CHAT,
    AUTO_CMD_OPEN,
    AUTO_CMD_QUIT,
} AutoCmdKind;

typedef struct {
    AutoCmdKind kind;
    f64 value;  // seek : position en secondes
    b32 flag;   // ready : valeur visée
    Str8 text;  // chat / open : pointe DANS la ligne analysée
} AutoCmd;

// auto_from_env lit la configuration du pilote dans l'environnement.
// Renvoie 0 (et laisse `out` à zéro) si VIBESYNC_AUTO_URL est absent ou vide.
// [frontière plateforme]
b32 auto_from_env(Arena *a, AutoPilot *out);

// auto_parse analyse une ligne de commande. 0 : ligne vide, commentaire (#) ou
// verbe inconnu — on ignore plutôt que d'échouer, le fichier est écrit par un
// script.
b32 auto_parse(Str8 line, AutoCmd *out);

// auto_read_text lit un fichier texte entier (borné à VS_AUTO_CMDS_MAX).
// Chaîne vide si le fichier n'existe pas encore. [frontière plateforme]
Str8 auto_read_text(Arena *a, Str8 path);

// auto_write_atomic écrit `content` dans `path` par fichier temporaire puis
// renommage : le lecteur ne voit jamais de contenu tronqué.
// [frontière plateforme]
b32 auto_write_atomic(Arena *scratch, Str8 path, Str8 content);

#endif  // VS_AUTO_H
