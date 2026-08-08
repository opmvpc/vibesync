# Guide vibesync — Windows

Pour regarder un film en même temps que tes potes, chacun de son côté, avec VLC
qui reste calé sur la même image chez tout le monde.

## Ce qu'il te faut

- **VLC** installé (le vrai, [videolan.org](https://www.videolan.org/vlc/)) —
  vibesync ne lit rien lui-même, il pilote ton VLC.
- Le fichier vidéo, en local sur ton PC. Chacun a sa propre copie ; vibesync
  n'envoie jamais le fichier à personne.
- L'adresse du serveur (donnée par la personne qui l'héberge), un pseudo, et le
  nom de la salle à rejoindre — mets-vous d'accord dessus avant.

## Installation

1. Va sur la page des versions :
   `https://github.com/opmvpc/vibesync/releases/latest` — dans la section
   **Assets**, prends le fichier `.exe`, du genre `vibesync-0.2.3.exe` (le
   numéro de version est dans le nom, il change à chaque nouvelle version).
2. Pas d'installateur : c'est un seul fichier `.exe` (moins de 500 Ko). Mets-le
   où tu veux (Bureau, Documents...) et double-clique dessus.

![Écran connexion](captures/win-connexion.png)

## Rejoindre une salle

1. Au lancement, renseigne :
   - **Serveur** : l'adresse donnée par ton hôte (ex. `wss://vibesync.exemple.com/ws`)
   - **Pseudo** : ton nom, tel que les autres le verront
   - **Salle** : le nom convenu avec tes amis (invente-le, la salle se crée
     toute seule si elle n'existe pas encore)
2. Valide. Ces infos sont mémorisées : tu n'auras plus à les retaper la
   prochaine fois.

![Écran salle](captures/win-salle.png)

3. Une fois dans la salle, clique **Ouvrir un fichier...** et choisis ta vidéo.
   VLC se lance tout seul, en pause, prêt à démarrer.
4. Quand tu es prêt, clique **Je suis prêt**. La lecture ne démarre que quand
   **tout le monde** a cliqué ce bouton.
5. À partir de là, c'est automatique : play, pause, avance/recul décidés par
   n'importe qui dans la salle se répercutent chez tout le monde. Un petit
   indicateur montre l'écart (drift) entre ton VLC et la salle.

## Dépannage

**Windows affiche un avertissement au premier lancement** (SmartScreen,
« Windows a protégé votre ordinateur », ou une demande du pare-feu) — normal,
vibesync n'est pas signé numériquement. Clique **Informations
complémentaires** puis **Exécuter quand même** (SmartScreen), et **Autoriser
l'accès** si le pare-feu demande une confirmation réseau.

### ⚠️ L'antivirus a supprimé le fichier ? C'est une fausse alerte

Windows Defender fait parfois disparaître `vibesync.exe` juste après le
téléchargement, avec un message du genre **« Menace trouvée »** ou
**« Trojan:Win32/Wacatac.C!ml »**. Le fichier n'est pas infecté.

**Pourquoi ça arrive** — vibesync n'est pas signé numériquement (un certificat
d'éditeur coûte plusieurs centaines d'euros par an ; c'est un projet gratuit
entre potes). Sans signature, Defender ne juge plus l'identité de l'éditeur
mais le comportement, « à la tête du client » : un tout petit programme
(260 Ko), fraîchement publié, téléchargé par peu de monde, et qui ouvre une
connexion réseau — c'est exactement le portrait-robot statistique d'un logiciel
malveillant. Le suffixe `!ml` dans le nom de la menace veut littéralement dire
« détecté par apprentissage automatique », c'est-à-dire par ressemblance, pas
par identification d'un virus connu.

**Comment récupérer le fichier**

1. Ouvre **Sécurité Windows** (menu Démarrer, tape « Sécurité Windows »).
2. **Protection contre les virus et menaces** → **Historique de protection**.
3. Repère la ligne `vibesync.exe` (la plus récente), clique dessus pour la
   déplier.
4. Menu **Actions** → **Restaurer** — ou **Autoriser sur l'appareil** si
   l'entrée est encore en quarantaine sans avoir été supprimée.
5. Si le fichier a été supprimé pour de bon plutôt que mis en quarantaine :
   re-télécharge-le depuis la page des versions, puis refais **Actions** →
   **Autoriser**.

Si tu ne trouves pas l'entrée, tu peux aussi déclarer une exclusion :
**Protection contre les virus et menaces** → **Gérer les paramètres** →
**Ajouter ou supprimer des exclusions** → **Ajouter une exclusion** →
**Fichier**, et désigne `vibesync.exe`.

**À savoir : ça peut recommencer à chaque nouvelle version.** L'autorisation
porte sur *ce* fichier précis, pas sur « vibesync » en général. Un nouveau
`vibesync-0.2.5.exe` est un fichier différent aux yeux de Defender : il peut
falloir refaire la manip. Chaque version est par ailleurs signalée à Microsoft
comme faux positif, ce qui règle en général le problème sous quelques jours —
mais la fenêtre entre la publication et la correction reste possible.

**Dans le doute, vérifie par toi-même** : dépose le fichier sur
[virustotal.com](https://www.virustotal.com/) — il est analysé par ~70 moteurs
antivirus. Une poignée de détections heuristiques isolées face à une majorité
de « clean », c'est la signature typique d'un faux positif. Le code source
complet est public sur [github.com/opmvpc/vibesync](https://github.com/opmvpc/vibesync),
et les `.exe` publiés sont construits par GitHub Actions directement depuis ce
code.

**« VLC introuvable »** — vibesync cherche VLC aux emplacements habituels
(`Program Files`). Deux solutions :
- installer VLC normalement si ce n'est pas déjà fait ;
- si VLC est ailleurs (installation portable, autre disque), définir la
  variable d'environnement `VIBESYNC_VLC` avec le chemin complet vers
  `vlc.exe`, puis relancer vibesync.

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
