# Rapport — édition de texte et réglages in-app (VS-018)

Périmètre : `ui/win32/**`. Captures : `docs/research/captures/`.

## Édition de texte

`UiText` gagne une **ancre** à côté du caret : sélection = `[min, max]`. Tout le
modèle (`ui_text_move|select_all|word_bounds|hit|…`, déclaré dans `ui.h`) est du
calcul pur sur trois entiers, **sans GDI**, donc testable. Le seul pont vers le
dessin est `UiTextMetrics` : un rappel qui donne la largeur du préfixe
`[0, off)` — `GetTextExtentPoint32W` en production, police fictive dans les
tests. Le hit-test cherche par **dichotomie** sur les frontières de points de
code (≈9 mesures GDI par clic, pas une par caractère) et ne renvoie jamais un
offset au milieu d'un caractère UTF-8.

Livré : clic, glissé continu, double-clic (mot, avec les trois classes
espace/mot/ponctuation de Windows — un accent est une lettre), Ctrl+A,
Maj+flèches/Origine/Fin, Ctrl+flèches et Ctrl+Retour/Suppr par mot, Ctrl+C/X/V
(`CF_UNICODETEXT`), frappe et collage remplaçant la sélection, fond de sélection
en teinte accent, défilement horizontal persistant, curseur en I. **Un mot de
passe se sélectionne mais ne se copie pas.**

## Réglages

Panneau modal (engrenage tracé en primitives, sur les deux écrans). L'écran du
dessous reste dessiné mais **sourd** : un `input_locked`, pas une machine à
états de plus. Voile par `AlphaBlend` chargée à la demande (`msimg32.dll`),
aucun ajout à l'édition de liens.

Serveur, pseudo, salle et **chemin de VLC**, dans le même
`%APPDATA%\vibesync.ini`. `settings_save` fusionne dans l'ini chargé au lieu
d'en repartir de zéro : plus de clé perdue. Le chemin est validé à la frappe et
refusé à l'enregistrement s'il n'existe pas. La détection auto est mesurée **au
démarrage, avant** d'appliquer le réglage — sinon elle renverrait le réglage à
l'utilisateur.

**Transverse** : `vlc.c` intact, le réglage passe par
`SetEnvironmentVariableW("VIBESYNC_VLC")` que `vlc_locate()` lit déjà en
premier. Un `vlc_locate(override)` explicite serait plus propre le jour où ce
fichier sera rouvert.

## QA

`build.bat test` et `asan` : **1 058 vérifications, 0 échec**, 12 vecteurs
verts. 48 nouvelles vérifications d'édition (largeurs inégales, frontières
UTF-8, glissé, mots, suppressions, collage filtré, capacité).

**Essai réel** sur l'exe, piloté par messages Win32 postés : glissé
sélectionnant une partie de l'URL, double-clic prenant `soiree` dans
`soiree-film`, engrenage, chemin invalide refusé avec message, « Détecter »,
enregistrement, `vlc=` retrouvé dans l'ini (restauré ensuite).

**Icône** : `assets/vibesync.ico` dans `vibesync.rc`, chargée en
`hIcon`/`hIconSm` ; extraction depuis l'exe vérifiée. **217 088 octets
(212 Ko)**, 43 % du budget.

---

# UX de connexion (VS-022) et versions (VS-023)

## La boucle « Nouvelle tentative… » : cause trouvée

`engine_session_lost()` remet la phase à `VS_PHASE_CONNECTING`. Or `main.c`
l'appelait sur **toute** fermeture de socket — y compris celle qui SUIT un
`error` fatal, déjà traité par `engine_disconnected()`. Un mauvais mot de passe
repassait donc en CONNECTING et relançait le backoff, indéfiniment.

Le correctif ne touche pas `engine.c` : la politique de reconnexion sort de
`main.c` pour aller dans **`conn.c`**, une petite machine à états pure
(`IDLE / TRYING / WAITING / OPEN / REFUSED`). `conn_on_socket_down()` est un
no-op en `REFUSED` et en `IDLE` — la règle « un refus ne relance jamais rien »
est désormais une ligne de code, et six vérifications de test.

## Adresses

`conn_normalize_url()` porte `internal/webui/address.go` : hôte nu → `wss://…/ws`,
local → `ws://`, http→ws, https→wss, chemin absent → `/ws`, fragment et
userinfo retirés. Les **14 cas de `address_test.go`** sont rejoués tels quels,
plus 6 cas propres au client (IPv6 entre crochets, `//hôte`, query). L'adresse
retenue est réécrite dans le champ à la connexion : l'utilisateur voit ce qui
part.

## Joignabilité

`health.c` : `GET /healthz` en WinHTTP sur un **thread dédié** (DNS et TLS
bloquent), résultat versionné — une réponse à une adresse déjà quittée est
jetée. Pastille verte/rouge avec latence ou motif traduit (DNS, TLS, port
fermé, délai). Déclenchée au démarrage, à la sortie du champ Serveur, sur
« Tester », et une fois à la première panne. Quand `ws://` échoue mais que le
même hôte répond en TLS, le client le dit et propose la bascule d'un clic.

## Erreurs actionnables

`bad_password`, `name_taken`, `version_mismatch` : arrêt net, retour au
formulaire éditable, message qui dit quoi corriger, et **focus + sélection sur
le champ fautif** (`UiFieldRef`). Le backoff garde un bouton **Annuler** avec
compte à rebours.

## Versions

`build.bat` lit `VERSION` et passe `-DVIBESYNC_VERSION_RAW=0.2.0` — un jeton
brut mis en chaîne par le préprocesseur, donc zéro échappement de guillemets à
travers `cmd.exe`. `protocol.c` lit `welcome.serverVersion` / `downloadUrl`
(additifs : un serveur muet reste valide) ; `proto_semver_cmp` compare. Version
en pied de l'écran de connexion, `client · serveur · protocole` en salle,
bannière fermable et `ShellExecuteW` sur une URL http(s) validée.

## QA

`build.bat test` et `asan` : **1 116 vérifications, 0 échec** (58 ajoutées),
12 vecteurs verts. **231 424 octets (226 Ko)**, 45 % du budget.

**Essai réel** contre `cmd/vibesync-server` local (bâti à la version 9.9.9 avec
`VIBESYNC_PASSWORD`) : hôte nu sondé et normalisé à l'écran ; mot de passe
refusé → message précis, focus sur le champ, bouton réactivé, et **capture
identique 8 secondes plus tard** — plus aucune boucle ; bon mot de passe →
salle avec `client v0.2.0 · serveur v9.9.9 · protocole v1` et la bannière de
mise à jour.
