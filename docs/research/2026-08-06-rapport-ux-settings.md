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

---

# Mot de passe mémorisé (VS-025)

`secret.c` : DPAPI `CryptProtectData`/`CryptUnprotectData`, portée du compte
Windows, entropie applicative constante `"vibesync.v1"` (elle cloisonne nos
blobs de ceux d'une autre appli DPAPI, elle ne remplace pas la clé de l'OS),
`CRYPTPROTECT_UI_FORBIDDEN` — jamais d'invite surprise. Le blob part en
hexadécimal dans `password_enc=`. Zéro ligne de cryptographie maison.

**Un seul point d'écriture.** `ini_flush()` remplace tous les appels directs à
`ini_save_file` : il chiffre (ou supprime) `password_enc`, écrit `retenir_mdp`,
et retire systématiquement une éventuelle clé `password` en clair héritée. Le
panneau Réglages passe par la même porte — un chemin qui oublierait de chiffrer
n'existe plus par construction, plutôt que par vigilance.

**UX.** Case « Se souvenir du mot de passe (chiffré par Windows) », cochée par
défaut, dessinée à la main (coche en deux segments), pilotable au clavier.
Décocher efface l'entrée **immédiatement**, pas à la fermeture. Blob
indéchiffrable (autre compte, autre machine, corruption) → champ vide, aucun
message, une ligne `OutputDebugString`.

**Hygiène mémoire.** `app->password` passe d'une copie en arène à un `StrBuf`
effaçable ; `SecureZeroMemory` sur le `hello` encodé dès l'envoi (il portait le
clair), sur le tampon rendu par DPAPI avant `LocalFree`, sur le clair
déchiffré une fois recopié dans le champ, à la déconnexion et à la sortie.
Limite assumée : tant qu'il est affiché, le mot de passe vit dans `UiText`.

## QA

Tests : aller-retour sur 4 clairs (accents, ponctuation), blob strictement
hexadécimal, **le clair n'apparaît pas dans le blob** (vérifié sur les aiguilles
≥ 4 octets — sous ce seuil la recherche se déclenche par hasard, le premier jet
du test était faux), deux chiffrements du même clair diffèrent, clair vide
refusé, 7 formes de blobs invalides rejetées sans toucher la sortie,
`secret_wipe`, et les règles ini (`ini_remove`, clé absente du fichier écrit,
ini d'une version antérieure).

**1 166 vérifications, 0 échec** (`test` et `asan`), 20 exécutions consécutives
sans intermittence. Chiffres établis sur les **12 vecteurs commités** : un 13ᵉ
vecteur non suivi (`13-reprise-salle-vierge.json`) est apparu pendant la
session et échoue — il vise `engine.c`, hors de ce lot. **236 544 octets
(231 Ko)**, 46 % du budget.

**Essai réel** : saisie du mot de passe → connexion → fermeture ; l'ini contient
`password_enc=01000000d08c9ddf…` et **ni le clair ni de clé `password=`** ;
relance → champ prérempli masqué, case cochée ; décochage → `retenir_mdp=0` et
`password_enc` **disparue** du fichier.

---

# Règles de sync portées du Go (VS-017 part client, VS-024)

## Suspension du buffering

`VsBufferDetect` gagne `suspend_until`. `buf_suspend()` oublie le stall en cours
et neutralise la détection 2 s, sans jamais raccourcir une suspension plus
lointaine ; **le verdict courant survit** — sinon le seek de correction envoyé
justement parce que le lecteur décroche effacerait le diagnostic à chaque fois.
`buf_reset()` oublie le stall mais pas la suspension, comme `Reset` en Go.
Points de suspension alignés sur la référence : `arm()` (pause, reprise, seek
commandés), action détectée dans VLC, et control émis par l'UI.

## File de chat hors ligne

20 messages max dans le moteur ; au-delà, les plus anciens tombent. Vidée dans
l'ordre au welcome, **après** les re-déclarations. Ni `setReady`, ni `setFile`,
ni surtout `control` ne sont rejoués. L'UI lit la file à chaque frame et la rend
en gris « · en attente » **sous** l'historique, sans jamais l'y mêler : l'écho du
serveur après reconnexion produirait sinon des doublons.

## Ready préservé au welcome

Le client C avait le bug corrigé côté Go : `engine_on_welcome` faisait
`e->ready = *self_ready`, or le serveur vient de créer un membre neuf, donc
« pas prêt ». Toute reconnexion effaçait le ready. Le paramètre est désormais
ignoré (l'état local fait foi), `users` reste la resynchronisation.

## Reprise salle vierge

`virgin_resume()` : `setBy` vide + position 0 + lecteur local > 5 s → UN
`control seek` via `emit_user_control()`, donc hold post-action armé et
buffering suspendu comme n'importe quelle action. Toast « Reprise à HH:MM:SS »
signalé par `VsOutput.have_resume_toast` — le moteur reste pur.

## Le vecteur 13 : format ambigu, pas une divergence de moteur

Le générateur Go a deux conventions — `event()` **jette** ce que le moteur émet
en réaction, `eventKeep()` le **garde** pour le pas de trace suivant — mais le
fichier golden n'enregistre pas laquelle a servi : les welcome de `12-coupure`
(jeté) et de `13-reprise` (gardé) sont structurellement identiques. Aucune règle
unique ne peut donc satisfaire les deux vecteurs.

Le rejeu C lit maintenant un booléen `keepOutput` sur l'événement (défaut :
jeter, comportement actuel). **Avec `"keepOutput": true` sur le welcome et le
pong à 6400 ms, les 13 vecteurs passent, `test` comme `asan`** — vérifié sur une
copie en scratch, le fichier du dépôt n'a pas été touché : c'est un fichier
généré, un correctif à la main sauterait à la prochaine régénération.

**Correctif attendu côté Go** (3 lignes) : ajouter
`KeepOutput bool \`json:"keepOutput,omitempty"\`` à `vectorEvent`, le mettre à
`true` dans `eventKeep`, régénérer.

## QA

**1 244 vérifications, 0 échec** en `test` et en `asan` sur les 13 vecteurs
(vecteur 13 marqué) ; sur les vecteurs du dépôt tels quels, seul le 13 échoue,
pour la raison ci-dessus. 44 vérifications ajoutées, indépendantes de la
convention de rejeu : file bornée et ordre de composition, rejeu au welcome sans
control ni double setReady, ready local préservé, reprise émise/non émise selon
`setBy`, position de salle et seuil des 5 s, ordre setReady/ping puis control,
faux buffering évité pendant 2 s puis détection qui reprend, verdict conservé,
fenêtre jamais raccourcie. **237 568 octets (232 Ko)**, 46 % du budget.

**Essai réel** contre le serveur local : en salle → serveur coupé → deux
messages tapés, affichés en gris « en attente » → serveur relancé → les deux
partent **dans l'ordre** et reviennent en lignes normales, sans doublon.

## Resserrages post-review (spec `af4c9f8`)

Les trois règles resserrées sont en place, et le vecteur 13 régénéré passe.

- **Anti-masquage du buffering** : `buf_suspend` refuse désormais deux cas —
  suspension déjà en cours (elle va à son terme, jamais prolongée) et moins
  d'1 s après la fin de la précédente. Un refus ne touche à rien : effacer le
  stall en cours suffirait à masquer. Mesure : un VLC figé avec un seek de
  correction **à chaque poll de 200 ms** est diagnostiqué en **2,8 s** (bien
  sous les 5 s exigées).
- **File de chat liée à la salle** : vidée sans envoi par `engine_set_room` sur
  changement de salle et par `engine_disconnected` (départ volontaire) ; elle ne
  survit qu'à `engine_session_lost`, la reconnexion automatique.
- **Reprise vierge stricte** : conditions cumulatives (salle sans control + déjà
  connecté à CETTE salle dans CE processus + position de salle connue > 5 s), et
  seek à la **dernière position de salle observée** (échantillonnée à chaque tic
  tant que la salle est pilotée), pas à la position VLC brute. La décision est
  prise **avant** `engine_on_roomstate` — c'est le piège : le welcome vierge
  écrasait la mémoire avant usage. Une salle vierge n'alimente jamais
  l'échantillon, donc deux redémarrages d'affilée reproposent la bonne position.

---

# Dossiers médias et ouverture du fichier d'un participant (VS-026)

## Recherche

`media.c` : parcours `FindFirstFileW` récursif **borné** — profondeur ≤ 6,
≤ 50 000 entrées, points de reparse ignorés (une jonction circulaire ferait
tourner la recherche jusqu'à la borne). Comparaison de nom **exacte et
insensible à la casse** par `CompareStringOrdinal` (ordinal, pas de surprise de
locale). Un seul tampon de chemin de 32 768 unités est empilé/dépilé pendant la
descente au lieu d'allouer par niveau. Homonymes : **le plus gros gagne** — entre
un extrait et le film, c'est le film ; le nombre de correspondances et d'entrées
visitées est tracé pour pouvoir contester l'heuristique.

Tout cela est bloquant, donc exécuté **sur le thread VLC**, qui enchaîne
directement sur le lancement quand la recherche aboutit. La fenêtre ne gèle
jamais.

## UX

Réglages : liste de dossiers (ajout par `IFileOpenDialog` + `FOS_PICKFOLDERS`,
retrait par ligne), défaut = **Téléchargements** (`FOLDERID_Downloads`, repli
`%USERPROFILE%\Downloads`). Persistés dans `dossiers_medias`, joints par `|` —
caractère interdit dans un chemin Windows, donc ni échappement ni ambiguïté.

Double-clic sur la ligne d'un participant qui a déclaré un fichier (survol mis
en évidence) → recherche → VLC. Introuvable → bandeau orange « … introuvable —
cliquer pour ouvrir les Réglages », soit le raccourci demandé. À l'arrivée en
salle, bandeau cyan « X regarde Y — cliquer pour l'ouvrir chez vous », fermable
et non ressuscité pour le même fichier. Les trois bandeaux (mise à jour, watch,
introuvable) passent par un `notice_bar` commun et s'empilent sans rien masquer.

## QA

**1 282 vérifications, 0 échec** en `test` et en `asan`, **13/13 vecteurs
verts**. 25 vérifications ajoutées pour VS-026 : aller-retour de la
sérialisation (accents, entrées vides, surplus), puis recherche sur une
arborescence temporaire réelle — trouvé, casse inversée, absent, pas de
correspondance partielle, homonymes départagés par la taille, dossier
inexistant, barre finale, **profondeur 8 non atteinte / profondeur maximale
atteinte**, 300 fichiers sans troncature. **247 808 octets (242 Ko)**, 48 % du
budget.

**Essai réel** à deux clients contre le serveur local : le client A ouvre un WAV
niché deux niveaux sous le dossier configuré ; le client B arrive, voit le
bandeau « thibault regarde soiree-test.wav », double-clique la ligne du
participant → le fichier est retrouvé, VLC démarre dessus et B déclare le même
média (`0:00 / 0:45`).
