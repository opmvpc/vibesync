---
id: VS-043
titre: Faux positif Defender Wacatac.C!ml sur vibesync.exe
statut: fait
priorité: haute
dépend-de: []
créé: 2026-08-08
mis-à-jour: 2026-08-09
---

Defender supprime vibesync.exe téléchargé (Trojan:Win32/Wacatac.C!ml — !ml =
heuristique machine learning ; profil déclencheur : exe 264 Ko non signé qui
fait du réseau). Chaque nouvelle release peut re-déclencher (hash différent) :
l'autorisation locale comme le verdict Microsoft portent sur un hash, pas sur
« vibesync ».

Livré (les trois leviers gratuits) :

- **Guide amis Windows** (`docs/guide-amis-windows.md`) : encadré « L'antivirus
  a supprimé le fichier ? C'est une fausse alerte » — pourquoi (non signé +
  heuristique ML, sens du suffixe `!ml`), restauration pas-à-pas (Sécurité
  Windows → Historique de protection → Actions → Restaurer / Autoriser sur
  l'appareil), exclusion en secours, mention explicite que c'est **à refaire à
  chaque version**, et vérification indépendante par VirusTotal.
- **Métadonnées VERSIONINFO** (`ui/win32/vibesync.rc`) : bloc complet
  (CompanyName, FileDescription, ProductName, InternalName, OriginalFilename,
  LegalCopyright, Comments avec l'URL du dépôt, File/ProductVersion). Version
  tirée de la **même source unique que le code C**, le fichier `VERSION` :
  `build.bat` passe désormais à windres le jeton brut *et* les trois nombres
  séparés (FILEVERSION veut `0,2,4,0`, qu'aucune macro ne sait dériver de
  « 0.2.4 »). Sans injection → « dev » / 0.0.0, comme côté C.
- **Recherche** (`docs/research/2026-08-09-vs043-defender.md`) : procédure de
  soumission WDSI pas-à-pas (portail, quel fichier, « Software developer »,
  cases à cocher, texte prêt à coller, pièges connus), à rejouer à chaque
  release ; synthèse des options de signature.

Décision (à valider par Thibault) : **rester à 0 €**. Azure Trusted Signing
(~10 $/mois, intégrable CI) est **inéligible** — réservé depuis avril 2025 aux
organisations US/Canada avec 3 ans d'existence vérifiable, onboarding individuel
suspendu. Certum Open Source (~85-115 € puis ~30 €/an) est ouvert aux
individus mais impose un jeton matériel → signature manuelle, incompatible avec
le pipeline « tag → Actions → assets ».

Validation (VM Win11 par SSH, clone jetable) : `build.bat` release OK, 270 336
octets (< 500 Ko) ; `build.bat test` OK, 1647 vérifications / 15 vecteurs ;
`Get-Item .VersionInfo` sur le binaire produit → toutes les chaînes présentes,
FileVersion 0.2.4 / FileVersionRaw 0.2.4.0 ; chemins de repli vérifiés (windres
sans `-D` compile ; sans fichier VERSION → FileVersion « dev », 0.0.0.0).

Reste à faire (manuel, hors code) : soumettre l'asset `vibesync-<version>.exe`
de la prochaine release à WDSI + VirusTotal, en suivant la procédure du rapport.
