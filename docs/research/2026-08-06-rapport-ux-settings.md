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
