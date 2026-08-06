# Rapport de recherche : OSD dans VLC sans script Lua (agent Sonnet, 2026-08-06)

## Verdict : INFAISABLE (par RC/marq ou HTTP), avec la contrainte 0-dépendance

Testé en local sur VLC **3.0.20 "Vetinari"** (build Windows officiel,
`C:\Program Files\VideoLAN\VLC\vlc.exe`). Aucun mécanisme réseau natif ne permet
de pousser un texte OSD arbitraire dans VLC sans installer un script Lua custom.

## 1. Piste RC/marq — testée en local, morte

Lancement : `vlc.exe silence.wav --intf qt --extraintf=rc --rc-host=127.0.0.1:45454
--audio-visual=visual --effect-list=spectrum --sub-source=marq
--marq-marquee=init_test --marq-timeout=0 --marq-position=8 --qt-start-minimized`
(WAV silencieux généré à la main, 20 s, RIFF 8 kHz/8-bit). `--rc-unix` n'existe pas
sous Windows ; seul `--rc-host` (TCP) fonctionne.

Connexion telnet brute au port 45454 : la commande `help` liste ~40 commandes fixes
(play, seek, volume, snapshot…) — **aucune mention de `marq` ni de `@`**. Tests
explicites, tous rejetés par le même message `Commande "X" inconnue` :
- `@marq marq-marquee texte` → inconnue
- `marq-marquee texte` → inconnue
- `@marq-marquee texte` → inconnue

`vlc.exe -l` confirme que le module chargé par `--extraintf=rc` est bien
`liboldrc_plugin.dll` (nom interne `oldrc`, alias `rc`) — pas de module distinct
« nouveau RC » à essayer. La syntaxe `@nom var valeur` documentée sur d'anciens
forums (2010-2013, VLC 1.1/2.x) a été **retirée de la table de commandes RC** dans
la branche 3.0 stable : le handler ne fait plus de dispatch générique vers les
variables de module, seulement les commandes codées en dur. Un snapshot RC
(`snapshot`, réussi, `returned 0`) confirme par ailleurs qu'aucun texte marquee
n'apparaît sur le rendu — cohérent avec l'absence de contrôle actif.

Donc même en acceptant `--sub-source=marq` au lancement (texte **statique**, fixé
une fois pour toutes en ligne de commande), il est impossible de le faire changer
en cours de lecture via RC dans cette version. Idem probable sur VLC 3.0.x/4.x
récents en général (le retrait est ancien, pas spécifique à ce build).

## 2. Interface HTTP — aucune capacité OSD

VLC embarque déjà son interface HTTP via des scripts Lua **fournis avec
l'installation** (`lua/intf/http.lua`, `lua/http/requests/*.lua` — c'est ce que
`--extraintf=http` charge, donc pas un script "tiers" à installer). Recherche
exhaustive (`grep -ri "marq\|osd"`) dans tout `lua/` de l'install VLC : **zéro
résultat**. `requests/status.json`/`status.xml` n'exposent que playback state,
volume, position — aucun endpoint ni commande texte/OSD, documenté ou caché.

## 3. Alternatives 0-dépendance : aucune viable pour du texte éphémère

- **Sous-titre dynamique (`--sub-file`)** : VLC ne surveille pas le fichier .srt en
  continu ; changer le fichier ne fait rien tant que la piste n'est pas rechargée
  (`strack`), ce qui interrompt/relance le flux sous-titre — inadapté à un message
  éphémère non disruptif.
- **Hotkeys via RC (`key raccourci`)** : simule des raccourcis clavier prédéfinis
  (ex. `key vol-up` affiche "Volume 50%") mais ne permet pas d'injecter du texte
  arbitraire — chaque raccourci a un OSD fixe câblé dans VLC.
- **dbus** : Linux only, hors sujet (client Windows/macOS).
- Aucune combinaison de flags/commandes ne contourne l'absence du dispatch RC
  générique sans toucher au Lua.

## Recommandation

Fermer la piste VLC-natif pour l'OSD. Passer à la solution de repli déjà prévue
au ticket VS-020 : **toasts applicatifs côté client** (GDI côté Windows, natif
côté macOS), affichés par-dessus ou à côté de la fenêtre VLC, pilotés par les
mêmes événements WebSocket (`pause`, `seek`, `ready`, …) que le reste du client.
Ça évite toute dépendance à la version VLC de l'ami et reste dans l'esprit
handmade/0-dépendance de l'ADR-008. Documenter ce choix dans un ADR court
(critère d'acceptation du ticket) puis retirer/refermer la piste RC de la spec.

Nettoyage : instance VLC de test fermée proprement (aucun processus orphelin),
aucun fichier créé hors du scratchpad et de ce rapport.
