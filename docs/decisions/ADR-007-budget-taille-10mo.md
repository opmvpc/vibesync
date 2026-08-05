---
id: ADR-007
titre: Budget taille client < 10 Mo → Windows en WPF (.NET Framework 4.8)
statut: accepté
date: 2026-08-05
---

## Contexte

Thibault impose : « le binaire des clients doit faire moins de 10 méga ». Un client =
app UI native + cœur Go headless (ADR-006). WinUI 3 (choix initial ADR-006) est
incompatible avec ce budget : ~100 Mo en self-contained, et la variante légère exige
d'installer .NET + Windows App SDK runtimes chez chaque ami.

## Décision

- **Budget : < 10 Mo au total par client** (UI + core), vérifié par script
  (`scripts/check-size.ps1` / CI) qui échoue au-delà.
- **Windows : WPF ciblant .NET Framework 4.8** — préinstallé sur Windows 10 19045 et
  Windows 11 → distribution framework-dependent, exe UI ~2-3 Mo, aucun runtime à
  installer. Look moderne assuré par un thème sombre custom (styles WPF faits main,
  pas de grosse lib de thème sauf si le budget le permet). Amende le volet Windows
  d'ADR-006 ; le pattern core headless + façade native reste inchangé.
- **Core Go** : build `-ldflags "-s -w" -trimpath` ; assets de la web UI de debug
  exclus des builds de prod via build tag (`//go:build withdebugui` pour les inclure).
- **macOS** : SwiftUI inchangé (runtime Swift dans l'OS, app ~2 Mo + core).
- Réserve si dépassement : compression UPX du core (à tester contre les antivirus
  et Gatekeeper avant adoption).

## Alternatives écartées

- WinUI 3 : voir contexte (taille/runtimes).
- .NET moderne (8/9) self-contained + trimming : WPF ne trimme pas bien, ~40-60 Mo.
- Lazarus/FreePascal : binaires très petits mais écosystème/compétence agents faibles.

## Conséquences

- L'esthétique Fluent sous WPF 4.8 demande un vrai travail de styles à la main.
- C# 7.3 max sur .NET Framework par défaut (langage plus ancien, acceptable).
- Le budget taille devient un critère d'acceptation de VS-011/VS-012 et un garde-fou CI.
