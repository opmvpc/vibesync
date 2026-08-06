---
id: VS-026
titre: Dossiers médias + sélection auto du fichier d'un participant (à la Syncplay)
statut: terminé
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

- [x] Réglages : liste de dossiers médias (défaut : Downloads de l'utilisateur),
      ajout via dialogue natif de dossier, suppression, persistée dans l'ini
- [x] Double-clic sur le fichier d'un participant → recherche du nom exact
      (insensible à la casse) dans les dossiers configurés, récursive et bornée
      (profondeur ≤ 6, ≤ 50 000 entrées, hors thread UI — sur le thread VLC) →
      trouvé : lancement VLC dessus ; introuvable : bandeau clair + raccourci
      Réglages ; plusieurs correspondances : la plus grosse taille gagne (loggée)
- [x] Bandeau à l'arrivée en salle : « X regarde <fichier> — cliquer pour l'ouvrir »
      si un participant a un fichier et pas moi (fermable, non ressuscité pour le
      même fichier)
- [x] Avertissement existant de durées divergentes inchangé (le serveur s'en charge)
- [x] Tests : 25 checks (sérialisation ini, arborescence temporaire réelle :
      trouvé/casse/absent/partiel/homonymes/bornes/300 fichiers) ; test + asan
      verts (1 282 checks) ; 242 Ko (48 % du budget)
- [x] macOS : à porter au polissage Swift (NSOpenPanel dossier, ~/Downloads
      défaut) → suivi via VS-015

## Journal du ticket

- 2026-08-06 : créé.
- 2026-08-06 : assigné à l'agent C (après son port VS-017/024), en cours.
- 2026-08-06 : livré (3a70fa0) — media.c (FindFirstFileW borné, reparse points
  ignorés, CompareStringOrdinal), bandeaux notice_bar empilables. Essai réel
  2 clients concluant (fichier retrouvé 2 niveaux sous le dossier, VLC lancé).
  Dans v0.2.0. Terminé (port mac via VS-015).
- 2026-08-06 (soir) : macOS livré (MediaLibrary.swift — recherche bornée
  identique, hors thread UI, jeton de génération anti-résultat périmé ;
  SettingsView + bandeaux). Commit 73499fc.
