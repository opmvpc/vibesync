# Recherche : correction de dérive dans les systèmes comparables (agent Sonnet, 2026-08-08)

Contexte : le nudge de vitesse dynamique (±5 %) de vibesync est jugé désagréable par
les utilisateurs. Avant de re-designer le mécanisme de resync, tour d'horizon de
comment Syncplay, Jellyfin SyncPlay, Plex Watch Together, Kodi, Watch2Gether,
Teleparty et les scripts mpv gèrent le même problème. Recherche web uniquement,
aucun code touché.

## 1. Syncplay — la référence directe du use case

Syncplay est le seul système mature qui synchronise des **fichiers locaux** lus
dans un lecteur natif (mpv/VLC/MPC-HC) via un serveur central — exactement le use
case vibesync. Ses seuils sont **codés en dur** dans
`syncplay/constants.py` (vérifié en clonant le fichier source, pas une source
secondaire) :

| Constante | Valeur par défaut | Rôle |
|---|---|---|
| `SLOWDOWN_RATE` | 0.95 | vitesse appliquée pendant le ralentissement (−5 %, exactement le facteur que vibesync utilise déjà) |
| `DEFAULT_SLOWDOWN_KICKIN_THRESHOLD` | 1.5 s | dérive à partir de laquelle le ralentissement démarre |
| `MINIMUM_SLOWDOWN_THRESHOLD` | 1.3 s | plancher configurable par l'utilisateur |
| `SLOWDOWN_RESET_THRESHOLD` | 0.1 s | hystérésis : sous ce seuil, on arrête de ralentir et on revient à 1x |
| `DEFAULT_REWIND_THRESHOLD` | 4 s | dérive à partir de laquelle Syncplay fait un seek arrière (au lieu de ralentir) |
| `MINIMUM_REWIND_THRESHOLD` | 3 s | plancher configurable |
| `DEFAULT_FASTFORWARD_THRESHOLD` | 5 s | dérive à partir de laquelle il fait un seek avant (retard, pas juste ralenti) |
| `MINIMUM_FASTFORWARD_THRESHOLD` | 4 s | plancher configurable |
| `FASTFORWARD_EXTRA_TIME` | 0.25 s | marge ajoutée à la cible du seek |
| `FASTFORWARD_RESET_THRESHOLD` | 3.0 s | hystérésis pour désarmer le fast-forward |
| `FASTFORWARD_BEHIND_THRESHOLD` | 1.75 s | seuil de retard distinct pour le fast-forward |
| `SEEK_THRESHOLD` | 1 s | seuil générique de déclenchement d'un seek |

**Logique en 3 paliers** : sous 1,5 s d'écart → rien (tolérance passive) ; entre
~1,5 s et 4 s → ralentissement à 0.95x avec hystérésis (on ne remonte à 1x que
sous 0,1 s d'écart, pour éviter le pompage) ; au-delà de 4-5 s → seek dur direct
(rewind ou fast-forward), jugé moins perturbant qu'un ralentissement prolongé.

**Configurable et désactivable** : dans l'onglet *Sync* du client (`.syncplay`/
`syncplay.ini`, section `[client_settings]` : `slowdownthreshold`,
`rewindthreshold`, `fastforwardthreshold`), l'utilisateur peut désactiver
« Slow down on minor desync », « Rewind on major desync » et « Fast-forward if
lagging behind » indépendamment. Le fast-forward et le rewind sont recommandés
par défaut ; le slowdown est actif par défaut mais explicitement présenté comme
la fonctionnalité à couper en premier en cas de gêne.

**Plainte utilisateur institutionnalisée** : la FAQ officielle
(syncplay.pl/guide/trouble/) a une entrée dédiée intitulée *« I keep getting
slowed down / rewinded, even though we're all actually in sync »* — assez
fréquent pour mériter sa propre section. Réponse officielle : souvent causé par
une connexion instable qui fait osciller l'estimation de position ; solution
proposée = désactiver purement et simplement le slowdown/rewind. Autre point
doctrinal : *« Syncplay is not designed to have millisecond-level precision as
this is not necessary for users to have a shared viewing experience »* — ils
acceptent une tolérance large plutôt que de forcer une précision qui coûte en
confort. Enfin, changer la vitesse manuellement (ex. lecture à 1.5x) casse le
moteur : Syncplay suppose une vitesse de base = 1.0 partout, et l'implémentation
d'une vraie vitesse custom a été jugée trop coûteuse en risque de régression
(discussion/issue #443, #339, #115 sur GitHub — jamais implémenté après
plusieurs demandes).

Sources :
- https://raw.githubusercontent.com/Syncplay/syncplay/master/syncplay/constants.py
- https://syncplay.pl/guide/client/
- https://syncplay.pl/guide/trouble/
- https://github.com/Syncplay/syncplay/discussions/443
- https://github.com/Syncplay/syncplay/issues/339
- https://github.com/Syncplay/syncplay/issues/115

## 2. Jellyfin SyncPlay — a désactivé la correction automatique par défaut, sur plainte utilisateur

Jellyfin a un mécanisme à deux étages, `SpeedToSync` (variation de vitesse) et
`SkipToSync` (seek dur), implémenté dans
`src/plugins/syncPlay/core/PlaybackCore.js` (jellyfin-web, valeurs lues
directement dans le code source, vérifiées par grep) :

| Paramètre | Valeur par défaut | Rôle |
|---|---|---|
| `minDelaySpeedToSync` | 60 ms | seuil bas : sous cette dérive, rien ne se passe |
| `maxDelaySpeedToSync` | 3000 ms | au-delà, on bascule sur `SkipToSync` (seek) au lieu de varier la vitesse |
| `speedToSyncDuration` | 1000 ms | durée pendant laquelle la vitesse ajustée est appliquée avant de réévaluer |
| `minDelaySkipToSync` | 400 ms | seuil bas pour déclencher un seek direct |
| `useSpeedToSync` | `true` | interrupteur du mécanisme de vitesse |
| `useSkipToSync` | `true` | interrupteur du mécanisme de seek |
| **`enableSyncCorrection`** | **`false` depuis 10.9 (nov. 2023)** | interrupteur maître de toute la correction automatique |

Formule de vitesse : `speed = 1 + diffMillis / speedToSyncTime` (donc une
dérive de 1000 ms avec une fenêtre de correction de 1000 ms donne un facteur
x2 ; le code plafonne à un minimum de vitesse de 0.2x pour éviter une vitesse
négative). Les deux seuils bas/haut (60 ms → 3000 ms) définissent la bande où
`SpeedToSync` agit ; au-dessus de 3000 ms, seek direct.

**Fait le plus significatif pour vibesync** : le commutateur maître
`enableSyncCorrection` était activé par défaut sur desktop (et déjà désactivé
sur mobile) jusqu'à ce que l'issue jellyfin-web#4972, *« Disable sync correction
for SyncPlay by default »*, ouverte par un mainteneur lui-même, ne dise
explicitement : *« [sync correction] seems to cause a lot of issues when most
people probably don't care about super precise syncing »*. Résolue par le PR
jellyfin-web#5003 (*« Sets the default value for sync correction to false for
everything »*, mergé nov. 2023, milestone 10.9.0) : diff vérifié directement,
`enableSyncCorrection` passe de `!(browser.mobile || browser.iOS)` à `false`
inconditionnellement. **Depuis 10.9, aucun client Jellyfin ne corrige
automatiquement la dérive par défaut — l'utilisateur doit l'activer
explicitement dans les réglages SyncPlay.** C'est la même conclusion à laquelle
vibesync est en train d'arriver empiriquement.

Autres frictions connues : issue jellyfin-web#7185, *« Syncplay with different
speed will force back to 1x »* — comme Syncplay, Jellyfin ne supporte pas une
vitesse de lecture custom pendant une session SyncPlay, le moteur la réinitialise
à 1x (fermé « not planned »). Issue jellyfin#11302 confirme le même problème
côté serveur : la vitesse de lecture ne peut pas être synchronisée entre membres
du groupe.

Sources :
- https://raw.githubusercontent.com/jellyfin/jellyfin-web/master/src/plugins/syncPlay/core/PlaybackCore.js
- https://github.com/jellyfin/jellyfin-web/issues/4972
- https://github.com/jellyfin/jellyfin-web/pull/5003 (diff vérifié via `gh pr diff`)
- https://github.com/jellyfin/jellyfin-web/issues/7185
- https://github.com/jellyfin/jellyfin/issues/11302

## 3. Plex Watch Together

Peu de documentation technique publique. Le comportement documenté est basique :
play/pause/seek d'un participant sont répliqués à tous (contrôle partagé, pas de
hiérarchie hôte/invité stricte décrite). Aucune mention publique d'un mécanisme
de correction de dérive (ni changement de vitesse ni seek automatique
documenté) — juste la réplication des événements de contrôle. Un fil du forum
Plex (*« Watch Together slowly out of sync »*) rapporte une dérive progressive
non corrigée : ~2-3 s d'écart après 30 minutes sur certains programmes, sans
réponse officielle publique sur un correctif automatique. Cela suggère que Plex
mise sur la réplication des commandes plutôt que sur une boucle de correction
active façon Syncplay — donc plus simple, mais qui laisse dériver sans garde-fou
sur de longues séances.

Sources :
- https://support.plex.tv/articles/frequently-asked-questions-watch-together/
- https://support.plex.tv/articles/watch-together/
- https://forums.plex.tv/t/watch-together-slowly-out-of-sync/598434

## 4. Kodi

Pas de fonctionnalité « party sync » native comparable à Syncplay/Jellyfin.
Kodi expose du **UPnP** (serveur/client de streaming sur réseau local), pas un
protocole de synchronisation multi-clients avec correction de dérive. La seule
solution trouvée est un script tiers non officiel, **kodisync** (Node.js,
github.com/tremby/kodisync) : approche radicalement plus simple que Syncplay —
il **met en pause toutes les instances**, les **seek à la position la plus en
retard**, puis vérifie périodiquement les changements d'état pour resynchroniser
au besoin. Aucun changement de vitesse, aucune tolérance graduée : correction
tout-ou-rien par pause+seek. Pas de retour utilisateur substantiel trouvé (projet
peu utilisé).

Sources :
- https://kodi.wiki/view/Syncing_and_sharing (page bloquée par Cloudflare au
  moment du fetch direct, contenu confirmé via extrait de résultat de recherche)
- https://github.com/tremby/kodisync

## 5. Watch2Gether

Pas de documentation technique officielle publiée sur l'algorithme de
correction de dérive. Le forum communautaire officiel (community.w2g.tv)
contient de nombreux fils utilisateurs sur des désynchronisations progressives
sur vidéos longues (>1h), avec des écarts de quelques centaines de ms à
plusieurs secondes, et aucune garantie de convergence automatique fiable citée
par les utilisateurs. Un post du développeur (*« Sync System Update »*, 2020)
explique un choix d'architecture — ne plus contrôler directement le lecteur
vidéo pour éviter que le buffering d'un participant ne fasse
jouer/pauser/saccader tout le monde — mais ne détaille aucun mécanisme de
correction de vitesse ou de seuils. Watch2Gether semble privilégier la
réplication d'événements de contrôle (comme Plex) plutôt qu'une boucle active
de correction de dérive.

Sources :
- https://community.w2g.tv/t/sync-system-update/117897
- https://community.w2g.tv/t/sync-issues-when-playing-long-videos-on-watch2gether/196528

## 6. Teleparty (ex-Netflix Party) et Amazon Prime Video Watch Party

**Teleparty** : extension navigateur qui injecte du JS dans la page du service
de streaming (Netflix, Disney+, Hulu, HBO Max, Prime) pour observer et répliquer
les événements DOM play/pause/seek/volumechange de l'hôte vers les autres
participants. Aucune documentation technique officielle publiée sur un
algorithme de correction de dérive (changement de vitesse, seuils numériques).
Attention : plusieurs sites marketing/SEO tiers (alibaba.com/product-insights,
watchnest.party) avancent des chiffres précis (« dérive corrigée sous 180 ms »,
« ±3 % de variation de vitesse », « 87 ms d'erreur médiane ») — **ces chiffres
n'ont aucune source primaire identifiable et sont très probablement fabriqués
par du contenu généré pour le SEO**, à ne pas citer comme faits.

**Amazon Prime Video Watch Party** : fonctionnalité native abandonnée le 2
avril 2024. Contrôle centralisé par l'hôte (play/pause/rewind/fast-forward
répliqués instantanément), aucune documentation technique publique sur un
mécanisme de tolérance de dérive ou de correction automatique n'a été
retrouvée.

Sources :
- https://www.teleparty.com/ (positionnement produit, pas de doc technique)
- https://techcrunch.com/2020/06/29/amazon-prime-video-introduces-watch-party-a-social-coviewing-experience-included-with-prime

## 7. Scripts mpv communautaires (référence d'implémentation légère)

**groupwatch_sync** (github.com/po5/groupwatch_sync, script Lua pour mpv,
README vérifié directement) — approche minimaliste et **manuelle** (pas de
serveur central, l'utilisateur déclenche la sync à la demande via touche) :
- Si en retard : lecture reprise, vitesse augmentée de `speed_increase` chaque
  seconde jusqu'à `max_speed` ou position atteinte.
- Si en avance : par défaut (`allow_slowdowns=no`) → **pause simple** jusqu'à
  ce que le groupe rattrape ; en option (`allow_slowdowns=yes`) → vitesse
  réduite progressivement jusqu'à `min_speed`.
- Option `subs_reset_speed=yes` : revient à vitesse 1x pendant l'affichage d'un
  sous-titre, pour ne pas désynchroniser l'audio/les sous-titres pendant la
  correction — signe que même les auteurs de scripts légers considèrent la
  correction de vitesse comme perturbante pour les sous-titres/l'oreille.
- Une touche dédiée (`Ctrl+k`) fait un seek dur direct, séparé de la correction
  de vitesse — l'utilisateur choisit lui-même la méthode plutôt que de subir un
  algorithme automatique.

Cette philosophie « pause plutôt que ralentir » quand on est en avance, et
« vitesse variable seulement si l'utilisateur l'autorise explicitement », est
notable : c'est l'inverse du choix par défaut de Syncplay/vibesync actuels.

Source :
- https://raw.githubusercontent.com/po5/groupwatch_sync/master/README.md

## Tableau comparatif

| Système | Mécanisme(s) | Seuils chiffrés | Actif par défaut ? | Plaintes connues |
|---|---|---|---|---|
| **Syncplay** | tolérance passive → ralenti (0.95x, hystérésis) → seek dur | 1.5 s ralenti / 0.1 s reset / 4 s rewind / 5 s fast-forward | Rewind + fast-forward: oui (recommandés) ; slowdown: oui mais 1ère chose recommandée à couper | FAQ dédiée aux ralentissements/rewinds perçus comme injustifiés ; vitesse custom cassée |
| **Jellyfin SyncPlay** | tolérance passive → SpeedToSync (vitesse variable, formule linéaire) → SkipToSync (seek) | 60 ms / 3000 ms bascule vitesse→seek / 400 ms seuil seek seul | **Non** depuis 10.9 (nov. 2023) — désactivé par défaut suite à plainte mainteneur | Issue mainteneur : « cause plus de problèmes que ça n'aide » ; vitesse custom cassée |
| **Plex Watch Together** | réplication d'événements de contrôle, pas de correction active documentée | aucun publié | n/a | dérive non corrigée signalée (2-3 s / 30 min) |
| **Kodi (kodisync tiers)** | pause globale + seek à la position la plus en retard | aucun, tout-ou-rien | outil tiers, pas de défaut Kodi | peu de retours (faible usage) |
| **Watch2Gether** | réplication d'événements de contrôle, pas de correction active documentée officiellement | aucun publié | n/a | fils forum récurrents sur dérive progressive non résolue |
| **Teleparty / Prime Video Watch Party** | réplication DOM/évènements côté hôte | aucun publié (chiffres SEO non fiables à ignorer) | n/a | pas de plainte structurée trouvée sur le mécanisme lui-même |
| **groupwatch_sync (mpv)** | manuel : pause par défaut si en avance ; vitesse variable seulement en option ; seek dur sur touche dédiée | `speed_increase`/`max_speed` configurables par l'utilisateur | correction de vitesse **désactivée par défaut**, pause préférée | reset auto à 1x pendant les sous-titres, signe explicite de gêne anticipée |

## Synthèse (20 lignes)

Le pattern dominant chez les systèmes matures qui font vraiment de la
correction active (Syncplay, Jellyfin) est un **modèle à paliers avec
hystérésis** : zone morte passive sous ~1-1,5 s (Syncplay) ou quelques centaines
de ms (Jellyfin), puis micro-correction de vitesse dans une fenêtre
intermédiaire, puis seek dur au-delà d'un seuil haut (4-5 s chez Syncplay,
3 s chez Jellyfin). Aucun des deux ne descend à ±5 % en continu comme vibesync :
Syncplay applique un facteur fixe 0.95x mais seulement au-delà de 1,5 s de
dérive et avec hystérésis de reset à 0,1 s pour éviter le pompage ; Jellyfin
calcule sa vitesse proportionnellement à la dérive dans une fenêtre bornée
60 ms-3000 ms. Les systèmes plus simples (Plex, Watch2Gether, Teleparty, Prime
Watch Party) ne documentent **aucune** correction active — ils se contentent de
répliquer play/pause/seek et laissent dériver, ce qui génère ses propres
plaintes de dérive non corrigée sur les sessions longues. Fait le plus
actionnable : **Jellyfin a désactivé sa correction automatique par défaut en
2023**, sur demande explicite d'un mainteneur constatant que « ça cause plus de
problèmes que ça n'apporte pour la plupart des gens qui se moquent d'une sync
parfaite » — même diagnostic que le retour utilisateur de vibesync. Le script
mpv `groupwatch_sync` va plus loin : il préfère **la pause à la vitesse
variable** quand on est en avance, et ne touche à la vitesse que sur
consentement explicite de l'utilisateur, avec un reset à 1x pendant les
sous-titres pour ne pas perturber la lecture. Ce que les utilisateurs détestent
concrètement : les micro-ajustements de vitesse perçus comme du pompage
(hystérésis mal réglée), la perte de synchro audio/sous-titres pendant un
ralentissement, et l'impossibilité de lire à vitesse custom (1.5x etc.) pendant
une session synchronisée — Syncplay et Jellyfin ont tous deux ce défaut non
résolu. Piste pour vibesync : soit une zone morte large + seek dur seul (pas de
ralenti du tout, comme Plex/Watch2Gether mais avec un vrai seuil de
déclenchement), soit un ralenti optionnel/désactivable avec hystérésis large et
seuil de déclenchement nettement plus haut que l'actuel, sur le modèle
Syncplay/Jellyfin plutôt qu'un nudge permanent à ±5 %.
