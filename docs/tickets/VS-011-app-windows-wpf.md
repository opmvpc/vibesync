---
id: VS-011
titre: App Windows native — WPF .NET Framework 4.8 (Win10 ≥ 19045 et Win11)
statut: ouvert
priorité: haute
dépend-de: [VS-010]
créé: 2026-08-05
mis-à-jour: 2026-08-05
---

## Contexte

ADR-006 (façade native du cœur headless) amendé par ADR-007 (budget < 10 Mo) :
WinUI 3 remplacé par **WPF ciblant .NET Framework 4.8** (préinstallé Win10/11, exe
minuscule, aucun runtime à installer chez les amis). Projet dans `ui/windows/`.
Machine de dev : Windows 10 Pro 19045, SDK .NET 8 (installé le 05-08) pour builder
`net48` (ajouter le PackageReference `Microsoft.NETFramework.ReferenceAssemblies`).

## Critères d'acceptation

- [ ] Spawn du core (`vibesync-core --headless`), lecture uiPort/uiToken sur stdout,
      reconnexion/crash gérés, arrêt propre du core à la fermeture
- [ ] Écrans : connexion (serveur/pseudo/salle, mémorisés), salle (participants+ready+latence,
      bouton Prêt, contrôles play/pause/seek avec barre, chat, toasts, indicateur de drift),
      sélection de fichier (dialogue natif)
- [ ] Thème sombre moderne fait main (styles WPF), textes français
- [ ] Build CLI reproductible (`dotnet build -c Release`), doc dans le repo
- [ ] **Taille : UI + core < 10 Mo au total** (script `scripts/check-size.ps1`)
- [ ] Testé sur la machine Win10 locale ; compat Win11 raisonnée
- [ ] Tests unitaires du ViewModel (parsing /ui, machine à états) exécutés en CLI

## Journal du ticket

- 2026-08-05 : créé (pivot natif ADR-006).
- 2026-08-05 : WinUI 3 → WPF net48 (ADR-007, budget 10 Mo). SDK .NET 8 installé.
