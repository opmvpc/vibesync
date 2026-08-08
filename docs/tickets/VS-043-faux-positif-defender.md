---
id: VS-043
titre: Faux positif Defender Wacatac.C!ml sur vibesync.exe
statut: ouvert
priorité: haute
dépend-de: []
créé: 2026-08-08
mis-à-jour: 2026-08-08
---

Defender supprime vibesync.exe téléchargé (Trojan:Win32/Wacatac.C!ml — !ml =
heuristique machine learning ; profil déclencheur : exe 260 Ko non signé qui
fait du réseau). À faire : (1) soumettre le binaire de release à Microsoft
(https://www.microsoft.com/en-us/wdsi/filesubmission) — levé sous 24-72 h en
général ; (2) guide amis Windows : encadré expliquant le faux positif et la
restauration depuis l historique de protection ; (3) étudier les métadonnées
VERSIONINFO enrichies + à terme une signature de code (coût/bénéfice).
Chaque nouvelle release peut re-déclencher (hash différent).
