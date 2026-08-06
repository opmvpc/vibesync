# Guide vibesync — macOS

Pour regarder un film en même temps que tes potes, chacun de son côté, avec VLC
qui reste calé sur la même image chez tout le monde.

## Ce qu'il te faut

- Un **Mac Apple Silicon** (M1 ou plus récent).
- **VLC** installé dans `/Applications` (le vrai, [videolan.org](https://www.videolan.org/vlc/))
  — vibesync ne lit rien lui-même, il pilote ton VLC.
- Le fichier vidéo, en local sur ton Mac. Chacun a sa propre copie ; vibesync
  n'envoie jamais le fichier à personne.
- L'adresse du serveur (donnée par la personne qui l'héberge), un pseudo, et le
  nom de la salle à rejoindre — mets-vous d'accord dessus avant.

## Installation

1. Télécharge `VibeSync.app` (ou l'archive qui la contient) depuis les
   releases GitHub : `https://github.com/opmvpc/vibesync/releases/latest`
   *(lien à confirmer par la personne qui t'a envoyé ce guide)*.
2. Déplace `VibeSync.app` dans `/Applications` (ou où tu veux).

## Contourner Gatekeeper (première fois seulement)

L'app n'est pas signée par un compte développeur Apple ni notarisée. Un
double-clic normal sera refusé par macOS la toute première fois. La manœuvre :

1. **Clic droit** (ou Ctrl+clic) sur **VibeSync.app** → **Ouvrir**.
2. Une boîte de dialogue prévient que l'éditeur n'est pas identifié → clique
   à nouveau **Ouvrir**.

C'est à faire **une seule fois** : tous les lancements suivants se font
normalement, par simple double-clic.

![Écran connexion](captures/mac-connexion.png)

## Rejoindre une salle

1. Au lancement, renseigne :
   - **Serveur** : l'adresse donnée par ton hôte (ex. `wss://vibesync.exemple.com/ws`)
   - **Pseudo** : ton nom, tel que les autres le verront
   - **Salle** : le nom convenu avec tes amis (invente-le, la salle se crée
     toute seule si elle n'existe pas encore)
2. Valide. Ces infos sont mémorisées (préférences macOS) : tu n'auras plus à
   les retaper la prochaine fois.

![Écran salle](captures/mac-salle.png)

3. Une fois dans la salle, clique **Ouvrir un fichier...** et choisis ta vidéo.
   VLC se lance tout seul, en pause, prêt à démarrer.
4. Quand tu es prêt, clique **Je suis prêt**. La lecture ne démarre que quand
   **tout le monde** a cliqué ce bouton.
5. À partir de là, c'est automatique : play, pause, avance/recul décidés par
   n'importe qui dans la salle se répercutent chez tout le monde. Un petit
   indicateur montre l'écart (drift) entre ton VLC et la salle.

## Dépannage

**« VibeSync.app est endommagée et ne peut pas être ouverte »** (au lieu du
message Gatekeeper normal) — arrive si le fichier a perdu son attribut de
quarantaine dans un état bizarre après téléchargement. Dans le Terminal :

```sh
xattr -dr com.apple.quarantine /Applications/VibeSync.app
```

puis relance normalement (clic droit → Ouvrir si besoin).

**« VLC introuvable »** — vibesync cherche VLC dans `/Applications/VLC.app`.
Deux solutions :
- installer VLC normalement à cet emplacement si ce n'est pas déjà fait ;
- si VLC est ailleurs, lancer l'app depuis un Terminal avec la variable
  `VIBESYNC_VLC` pointant vers le binaire VLC :
  ```sh
  VIBESYNC_VLC=/chemin/vers/VLC open /Applications/VibeSync.app
  ```

**« Fichiers différents entre amis »** (toast d'avertissement) — le serveur
compare la durée des fichiers déclarés par chacun. Si elles diffèrent de plus
de 2 secondes, tu reçois un avertissement (pas un blocage) : vérifiez que vous
avez bien la même version/coupe du fichier, sinon la sync sera approximative
en fin de vidéo.

**Désync persistante** — le moteur corrige en douceur les petits écarts
(vitesse légèrement ajustée le temps de rattraper) et fait un saut direct pour
les gros écarts. Si ça ne se résorbe jamais :
- vérifie ta connexion réseau (une latence très instable perturbe l'estimation
  d'horloge) ;
- vérifie que ton fichier est exactement la même version que celle des autres
  (voir point précédent) ;
- une coupure réseau de quelques secondes se répare toute seule (reconnexion
  automatique) ; au-delà, rejoins à nouveau la salle avec le même pseudo, ta
  place est reprise proprement.
