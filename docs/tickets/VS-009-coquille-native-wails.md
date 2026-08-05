---
id: VS-009
titre: Coquille native Wails (Windows ici, macOS sur le Mac demain)
statut: ouvert
priorité: haute
dépend-de: [VS-006]
créé: 2026-08-05
mis-à-jour: 2026-08-05
---

## Contexte

ADR-005 : l'UI web embarquée est affichée dans une fenêtre native Wails v2 au lieu du
navigateur. À faire APRÈS l'intégration (VS-006) pour ne pas perturber les agents.

## Critères d'acceptation

- [ ] Fenêtre native Windows affichant l'UI (AssetServer branché sur internal/webui)
- [ ] Flag `--browser` conservé (mode secours sans Wails)
- [ ] Build Windows : exe unique fonctionnel testé sur la machine
- [ ] `scripts/build-macos.sh` + doc `docs/build-macos.md` (Go, Wails, Xcode CLT,
      commande, sortie .app, signature ad hoc) prêts pour le Mac de demain
- [ ] Vérifier l'état de Wails v3 : si stable, ADR d'amendement, sinon rester v2

## Journal du ticket

- 2026-08-05 : créé suite au pivot « client natif » demandé par Thibault.
