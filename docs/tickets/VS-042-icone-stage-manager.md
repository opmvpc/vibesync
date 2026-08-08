---
id: VS-042
titre: Icône absente dans Stage Manager (Régisseur) macOS
statut: ouvert
priorité: basse
dépend-de: []
créé: 2026-08-08
mis-à-jour: 2026-08-08
---

Constat Thibault (capture) : icône OK dans le Dock mais carré générique dans
Stage Manager. Pistes : cache LaunchServices (lsregister/killall Dock),
CFBundleIconName absent de l Info.plist (chemin moderne AppKit), ou
représentation manquante. Reproduire, diagnostiquer, corriger.
