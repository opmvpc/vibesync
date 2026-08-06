---
id: VS-026
titre: Dossiers médias + sélection auto du fichier d'un participant (à la Syncplay)
statut: ouvert
priorité: haute
dépend-de: [VS-018]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

Demande de Thibault (capture Syncplay à l'appui) : configurer ses dossiers de
médias pour qu'un double-clic sur le fichier déclaré par un autre participant le
retrouve et l'ouvre automatiquement chez soi. Aucun changement de protocole : les
noms de fichiers circulent déjà via `setFile`/`users`.

## Critères d'acceptation

- [ ] Réglages : liste de dossiers médias (défaut : Downloads de l'utilisateur),
      ajout via dialogue natif de dossier, suppression, persistée dans l'ini
- [ ] Double-clic sur le fichier d'un participant → recherche du nom exact
      (insensible à la casse) dans les dossiers configurés, récursive et bornée
      (profondeur ≤ 6, ≤ 50 000 entrées, hors thread UI) → trouvé : lancement VLC
      dessus ; introuvable : message clair + raccourci Réglages ; plusieurs
      correspondances : la plus grosse taille gagne (heuristique simple, loggée)
- [ ] Bandeau à l'arrivée en salle : « X regarde <fichier> — cliquer pour l'ouvrir »
      si un participant a un fichier et pas moi (fermable)
- [ ] Avertissement existant de durées divergentes inchangé (le serveur s'en charge)
- [ ] Tests : recherche (arborescence temporaire : trouvé/absent/doublons/casse/
      bornes), persistance ini des dossiers ; build.bat test + asan verts ; budget
- [ ] macOS : à porter au polissage Swift (NSOpenPanel dossier, ~/Downloads défaut)

## Journal du ticket

- 2026-08-06 : créé.
- 2026-08-06 : assigné à l'agent C (après son port VS-017/024), en cours.
