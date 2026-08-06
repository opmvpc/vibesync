# Rapport — interface graphique du client Windows (VS-014, passe 2)

Périmètre : `ui/win32/**`. Captures : `docs/research/captures/`.

## Fichiers

`src/ui.c|h` (~1 000 lignes) : UI immediate-mode GDI complète — palette sombre,
boutons (primaire / fantôme / danger / actif), champs de saisie **maison**
(caret clignotant, Retour, Tab, Origine/Fin, Suppr, Ctrl+V, masquage des mots de
passe, défilement horizontal), barre de position scrubbable, liste de
participants, chat avec retour à la ligne et molette, bandeau de notification,
coins arrondis, états survol/pressé/désactivé, focus visible. `src/ini.c|h` :
réglages `%APPDATA%\vibesync.ini` (clé=valeur maison, UTF-8, BOM toléré).
`src/main.c` refondu : fenêtre, back-buffer DIB + BitBlt, threads, moteur.
`vibesync.rc` + `vibesync.manifest` (DPI Per-Monitor v2, UTF-8, chemins longs).

## Décisions

- **Trois threads, une règle** : le moteur ne vit que sur le thread UI. Le
  réseau réveille l'UI par `PostMessage(WM_APP_NET)` (nouveau `net_set_notify`) ;
  un thread VLC absorbe tout le HTTP bloquant (statut, commandes, lancement de
  VLC jusqu'à 20 s) et publie sous verrou + `PostMessage`. L'UI ne bloque jamais.
- **CPU nul au repos** : aucun timer permanent. Timer moteur (200 ms) seulement
  si connecté ou VLC lancé ; timer UI (100 ms) seulement si quelque chose bouge
  (lecture, caret, bandeau). Mesure : **0 ms de CPU sur 8 s** sur l'écran de
  connexion, 12,5 Mo de mémoire de travail.
- **Zéro contrôle Win32** : tout est dessiné, y compris les glyphes ▶/❚❚ tracés
  en primitives — net à tous les DPI, sans police d'icônes.
- **Écriture PNG maison** pour les captures (`--capture`) : filtre PNG *Up* +
  deflate RLE en Huffman fixe, ~110 lignes. 1,9 Mo → **100 Ko** par image.
- Sélecteur de fichier `IFileOpenDialog` (COM) avec filtres vidéo/audio ;
  barre de titre sombre via `DwmSetWindowAttribute` si disponible.

## QA

`build.bat test` et `build.bat asan` : **1 010 vérifications, 0 échec**, dont les
**12 vecteurs de conformité toujours verts**. Nouveaux tests : ini (analyse,
accents, clé répétée, plafond d'entrées, aller-retour disque, BOM, fichier
absent), formatage du temps (`0:00`, `1:23`, `1:23:45`, bornes, valeur non
finie), troncature UTF-8 d'un champ de saisie.

**Essai réel** contre `cmd/vibesync-server` local, deux clients : connexion,
salle affichée avec les deux participants, chat aller-retour par le serveur,
vrai VLC lancé sur un WAV de 45 s — le média arrive **en pause à 0:00**, durée
lue (`0:00 / 0:45`), aucun VLC orphelin à la fermeture. Captures :
`ui-connexion.png`, `ui-salle.png` (état riche), `ui-salle-vlc.png` (réel).

**Taille** : `vibesync.exe` = **176 640 octets** (172 Ko), soit ~1/3 du budget de
500 Ko. **Démarrage** : **47 ms** jusqu'au premier rendu (mesuré par
`GetTickCount64`, tracé sur stderr et `OutputDebugString`).
