---
id: VS-029
titre: Attache VLC en échec chez l'utilisateur — « aucun fichier ouvert », pause VLC non synchronisée
statut: ouvert
priorité: critique
dépend-de: [VS-026]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

Retour terrain de Thibault (première vraie séance C↔C, machine avec un VLC perso
configuré par Syncplay) :
1. Double-clic sur le fichier d'un ami (VS-026) : VLC s'ouvre ET JOUE (l'autoplay
   n'a pas été dompté → l'attache HTTP a échoué), l'app affiche « aucun fichier
   ouvert », aucun contrôle disponible.
2. Pause faite DANS VLC : non propagée aux autres (et contrôles UI indisponibles).
   Peut être une conséquence de 1 (pas de statut VLC → pas de détection d'action),
   mais la détection d'action utilisateur du client C n'a JAMAIS été validée en
   réel (le test sandbox utilise le client Go de référence) — à prouver.

L'essai réel de l'agent C passait sur sa machine → cause environnementale probable
(vlcrc perso : extraintf/lua Syncplay, one-instance, mdp http lua…). Exigence de
Thibault : play/pause/seek doivent marcher DEPUIS VLC et DEPUIS l'UI, dans les
deux sens.

## Critères d'acceptation

- [ ] Cause racine de l'échec d'attache identifiée et corrigée (reproduire avec un
      vlcrc à la Syncplay : lua intf configurée, one-instance, etc. ; candidats :
      fusion extraintf vs config utilisateur, --lua-intf résiduel, mdp http)
- [ ] Lancement VLC blindé contre la config utilisateur : forcer explicitement ce
      dont on dépend (`--no-lua` n'est pas possible — l'intf http EST du lua —
      mais neutraliser ce qui interfère : `--lua-intf` vide/notre intf,
      `--one-instance` désactivé, options en `--no-*` explicites)
- [ ] En cas d'échec d'attache : le VLC lancé est FERMÉ (pas de VLC orphelin qui
      joue), message d'erreur actionnable dans l'app avec la cause, entrée
      détaillée dans %APPDATA%\vibesync.log
- [ ] Détection d'action utilisateur (pause/play/seek faits dans VLC) prouvée en
      réel client C : scénario sandbox étendu au client C (au minimum un mode
      headless/CLI du client C piloté par le harnais, ou test manuel scripté
      documenté) — le trou « la sandbox ne teste que le client Go » est comblé
- [ ] Contrôles UI (boutons + timeline) et actions VLC vérifiés dans les DEUX sens
      sur une séance à 2 clients C
- [ ] build.bat test + asan verts, 13/13 vecteurs, budget < 500 Ko

## Journal du ticket

- 2026-08-06 : créé (retour terrain critique).
- 2026-08-06 (soir) : blindage livré (8b2982d, écrit à l'aveugle depuis le Mac,
  CI verte) — vlc_build_command avec 9 drapeaux ajoutés dont
  --no-one-instance-when-started-from-file (VRAI par défaut chez VLC : cause
  racine la plus probable — le média part vers l'instance déjà ouverte qui
  joue, pendant que notre process meurt), --start-paused, --lua-intf=http,
  --no-playlist-enqueue ; échec d'attache → VLC tué + toast « cause — piste »
  + trace vs_log dans %APPDATA%\vibesync.log. Gel des 15 drapeaux en test.
  RESTE (Windows réel requis — VM Win11 UTM en préparation ou retour du PC) :
  repro vlcrc Syncplay, critères 4-5 (détection d'action utilisateur prouvée
  en réel client C, séance 2 clients C), asan/budget locaux. NOTE : le driver
  Go internal/vlc/launch.go n'a pas les 9 nouveaux drapeaux — à aligner.
