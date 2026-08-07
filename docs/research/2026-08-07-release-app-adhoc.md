# Publication de la .app macOS en release GitHub (signature ad hoc)

2026-08-07 — sous-agent codegen. Aucun commit, aucun tag, aucune release réelle.

## Décision appliquée

La `.app` macOS est distribuée en release GitHub avec une **signature ad hoc**
(`codesign --sign -`), sans compte Apple Developer et **sans notarisation**.
Conséquence assumée : au premier lancement les amis passent par clic droit →
Ouvrir, ou par Réglages Système → Confidentialité et sécurité → « Ouvrir quand
même » sur les macOS récents.

## Fichiers modifiés

- `.github/workflows/ci.yml`
- `docs/guide-amis-macos.md`
- `docs/build-macos.md`

## Ce qui change dans la CI

### Job `client-macos` (macos-latest)

Trois étapes ajoutées après le build + garde-fou budget (garde-fou **inchangé**,
toujours `du -sk` sur `VibeSync.app` avec la limite 10 240 Ko) :

1. **Vérification de la signature** — `codesign --verify --deep --strict`, plus
   un `grep -q "Signature=adhoc"` sur `codesign -dv`, plus un contrôle
   `lipo -archs` qui exige `arm64`.
   Motivation : `scripts/build-macos.sh` signe **déjà** le bundle (ligne 99 :
   `codesign --force --sign - --timestamp=none`) mais **avale l'échec** (`|| echo
   "codesign a échoué (le bundle reste utilisable en local)"`). Tolérable en
   local, inacceptable pour un artefact distribué : une `.app` non signée est
   refusée par Gatekeeper même après clic droit → Ouvrir. On **ne resigne donc
   pas** (pas de double signature), on vérifie et on échoue dur.
   Le contrôle `arm64` évite qu'un runner `macos-latest` devenu autre chose
   publie un zip nommé `...-arm64.zip` contenant du x86_64.
2. **Archivage** (`if: startsWith(github.ref, 'refs/tags/v')`) —
   `ditto -c -k --keepParent ui/macos/build/VibeSync.app VibeSync-macos-arm64.zip`.
   `ditto` et pas `zip` : seul lui préserve xattrs, bit d'exécution et
   signature à travers l'archive.
3. **`actions/upload-artifact@v4`** (même condition) sous le nom
   `vibesync-macos`, exactement le pattern déjà utilisé pour l'exe Windows.

Sur push/PR ordinaires, les deux dernières étapes sont sautées : le job coûte
la même chose qu'avant.

### Job `release`

- `needs: [qa-go, client-windows, client-macos]` (ajout de `client-macos`).
- Second `actions/download-artifact@v4` pour `vibesync-macos`.
- **Un seul job crée la release et y attache les deux fichiers** — c'est le
  moyen le plus simple d'éviter la course « deux jobs sur la même release » :
  il n'y a pas de second créateur.
- L'étape devient rejouable : `gh release view` → si la release existe déjà
  (re-run d'un tag), `gh release upload --clobber` ; sinon `gh release create
  --generate-notes` comme avant. Aucune action marketplace ajoutée (ADR-008,
  philosophie handmade) : uniquement `gh`, déjà utilisé.

## Validations faites en local (sur ce Mac)

| Vérification | Résultat |
| --- | --- |
| `bash scripts/build-macos.sh` | OK — `VibeSync.app`, 1,1 Mo, binaire 1 142 304 o, version 0.2.0 |
| `codesign --verify --deep --strict` | `valid on disk` + `satisfies its Designated Requirement` |
| `codesign -dv` | `Signature=adhoc`, `flags=0x2(adhoc)`, `Identifier=org.vibesync.client` |
| `lipo -archs` | `arm64` |
| `ditto -c -k --keepParent` | zip produit, 320 270 o |
| Aller-retour `ditto -x -k` puis re-vérif | signature toujours valide, `-rwxr-xr-x` sur le binaire |
| Bloc shell de l'étape de vérification, exécuté tel quel | OK |
| YAML | `python3 -c "yaml.safe_load(...)"` → OK, 4 jobs |
| `bash -n` sur tous les blocs `run:` non-Windows | OK |

Non fait volontairement : aucun tag poussé, aucune release créée, aucun
`gh release` exécuté.

## Points de vigilance pour la review

1. **Le budget 10 Mo porte sur la `.app`, pas sur le zip.** Le zip fait ~0,3 Mo,
   le bundle 1,1 Mo : marge énorme, mais la mesure reste au bon endroit.
2. **`release` dépend maintenant de `client-macos`.** Un échec macOS (suite C
   asan/ubsan, `swift test`, signature) bloque désormais aussi la publication de
   l'exe Windows. C'est le comportement voulu pour une release cohérente, mais
   c'est un changement de couplage — à valider explicitement.
3. **`gh release upload --clobber` en cas de re-run** écrase des assets déjà
   téléchargeables. Souhaitable pour rattraper un run raté ; à connaître.
4. **Chemin non testé de bout en bout** : personne ne peut vérifier
   `upload-artifact`/`download-artifact` du zip sans pousser un tag. Le point
   subtil (l'artefact GitHub rezippe le fichier, et `download-artifact@v4`
   défait exactement cette couche → on récupère le zip `ditto` intact) est
   correct par construction mais mérite un premier tag de vérification, par
   exemple `v0.2.0-rc1`, avant la vraie release.
5. **`build-macos.sh` reste tolérant à l'échec de `codesign`.** Volontairement
   laissé tel quel (usage local), la CI compensant par sa vérification. Si on
   préfère un échec dur partout, c'est une ligne à changer dans le script — mais
   cela casserait un build local sur une machine sans outils de signature.
6. **`--deep` dans `codesign`** : la mission le mentionnait pour la signature.
   Le bundle n'a aucun code imbriqué (un seul exécutable, pas de framework, pas
   de bundle interne), donc `--deep` à la signature est inutile — il est utilisé
   côté `--verify`, où il a un sens. Rien à corriger, juste à ne pas prendre pour
   un oubli.
7. **Version du bundle** : `build-macos.sh` lit le fichier `VERSION` de la
   racine (0.2.0 aujourd'hui) — pas le nom du tag. Comme pour l'exe Windows
   (`build.bat`), **le fichier `VERSION` doit être à jour avant de poser le
   tag**, sinon la release `vX.Y.Z` embarque une app qui s'annonce autrement, ce
   que la bannière de mise à jour (VS-036) prend au sérieux.
8. **Nom du zip figé dans deux endroits** : le workflow et
   `docs/guide-amis-macos.md`. Si on le renomme, renommer aux deux endroits (et
   dans `docs/build-macos.md`).
