# Provisionnement de la VM Windows 11 ARM64 (UTM / Mac M1 Pro)

Date : 2026-08-07
Objet : installer par script la toolchain de build/test du client Windows dans la
VM Win11 ARM64 pilotable en SSH, cloner le dépôt, faire tourner la suite.

Livrable associé : `scripts/provision-vm.ps1` (rejouable, idempotent, aucun droit
admin requis, aucun installeur — que des archives officielles).

---

## 1. Résultat en une ligne

`ui\win32\build.bat test` passe : **1437 vérifications, 0 échec**, 13/13 vecteurs
de conformité rejoués. `go build`, `go vet` et `go test ./...` passent aussi.
La cible `asan` est en revanche **inutilisable sur cette machine** (voir §5) et,
pire, elle **rapportait un faux succès** — c'est le point de vigilance principal.

## 2. Ce qui est installé

Tout est sous `C:\Users\OPMVPC\tools\`, en portable, sans écriture hors du profil
utilisateur.

| Outil | Version | Chemin | Archive |
|---|---|---|---|
| Git | 2.55.0.windows.3 | `C:\Users\OPMVPC\tools\git\cmd\git.exe` | `MinGit-2.55.0.3-arm64.zip` (ARM64 natif) |
| Go | go1.26.5 windows/arm64 | `C:\Users\OPMVPC\tools\go\bin\go.exe` | `go1.26.5.windows-arm64.zip` |
| llvm-mingw | 20260616, clang 22.1.8 | `C:\Users\OPMVPC\tools\llvm-mingw\bin\` | `llvm-mingw-20260616-ucrt-aarch64.zip` |
| VLC | 3.0.23 | `C:\Users\OPMVPC\tools\vlc\vlc.exe` | `vlc-3.0.23-winarm64.zip` (ARM64 natif) |

- `go env GOPATH` = `C:\Users\OPMVPC\go` (défaut), `GOROOT` = `C:\Users\OPMVPC\tools\go`.
- Dépôt cloné dans `C:\Users\OPMVPC\vibesync` (HEAD au clonage : `0c00460`).
- PATH **utilisateur** persistant (HKCU\Environment, écrit via
  `[Environment]::SetEnvironmentVariable(...,'User')` et non `setx`, qui tronque
  à 1024 caractères) enrichi de :
  `tools\git\cmd`, `tools\go\bin`, `tools\llvm-mingw\bin`, `%USERPROFILE%\go\bin`.
  **Aucun redémarrage nécessaire** : sshd reconstruit l'environnement depuis le
  registre à chaque nouvelle session, donc la session SSH *suivante* voit le PATH.

`winget` n'a pas été tenté : tout passe par `Invoke-WebRequest` depuis la VM.
Espace disque après coup : 9,3 Go libres, dont 345 Mo de cache d'archives
(`tools\.cache`, purgeable via `-PurgeCache`).

## 3. `build.bat` n'a PAS eu besoin d'être modifié pour `test`

`ui\win32\build.bat` cherche clang d'abord dans le chemin en dur
`C:\Users\thibs\tools\llvm-mingw\bin`, puis **retombe sur le PATH** :

```bat
set "CC=C:\Users\thibs\tools\llvm-mingw\bin\x86_64-w64-mingw32-clang.exe"
if not exist "%CC%" set "CC=x86_64-w64-mingw32-clang.exe"
```

Mettre `tools\llvm-mingw\bin` dans le PATH suffit donc : `build.bat` et
`build.bat test` fonctionnent tels quels. Rien n'a été committé ni modifié.

## 4. Résultats

### `ui\win32\build.bat test`

Compilation propre (`-Werror` compris), puis exécution :

```
== base / json / protocol / vlc / engine (unités) / ini / adresse serveur
== semver / politique de connexion / file de chat hors ligne
== reprise salle vierge / suspension du buffering / dossiers médias
== net / ui / édition de texte / secret (DPAPI) / ini : secret
== vlc (HTTP réel)
== net (cycle réel)        stress fermeture/envoi : 100 itérations, 100 connectées
== net (saturation de file) 125 messages reçus, 1 erreurs (dont 1 saturation)
== vecteurs de conformité  01..13 : ok  (13/13)

1437 vérifications, 0 échec(s)
[test] OK
```

Y compris les tests qui touchent réellement l'OS (DPAPI, Winsock, WinHTTP) :
l'émulation x86_64 de Windows-on-ARM les encaisse sans broncher.

### `ui\win32\build.bat` (release)

```
[release] version 0.2.0
[release] vibesync.exe
        258048 octets
```

252 Ko — très loin du budget de 10 Mo (`scripts/check-size.ps1`).

### Go, à la racine du clone

```
go version go1.26.5 windows/arm64
go build ./...   -> 0
go vet ./...     -> 0
go test ./...    -> 0
  internal/client     ok   5,4 s
  internal/protocol   ok   1,6 s
  internal/server     ok   2,5 s
  internal/vlc        ok   2,9 s
  internal/webui      ok   4,6 s
  internal/ws         ok  17,4 s
  test/e2e            ok  78,8 s
  test/real           ok   9,1 s
```

`staticcheck` n'est pas installé (hors périmètre de la mission) ; à ajouter via
`go install honnef.co/go/tools/cmd/staticcheck@latest` si l'on veut la QA
complète de CLAUDE.md dans la VM — `%USERPROFILE%\go\bin` est déjà dans le PATH
pour ça.

## 5. Point de vigilance nº1 : `build.bat asan` rapporte un faux succès

`build.bat asan` affiche `[asan] OK` alors que **le binaire ne démarre même pas**.
Deux défauts cumulés :

**(a) La DLL du runtime ASan n'est pas où `build.bat` la cherche.** La cible fait
`set "PATH=C:\Users\thibs\tools\llvm-mingw\bin;%PATH%"` — chemin en dur absent
ici, et de toute façon le mauvais sous-répertoire : dans llvm-mingw le runtime
vit sous le triplet cible, pas dans le `bin\` de l'hôte.

```
C:\Users\OPMVPC\tools\llvm-mingw\x86_64-w64-mingw32\bin\libclang_rt.asan_dynamic-x86_64.dll
```

Sans elle, le processus meurt au chargement avec `0xC0000135`
(STATUS_DLL_NOT_FOUND), soit un code de sortie de **-1073741515**.

**(b) `if errorlevel 1` est FAUX pour un code de sortie négatif.** cmd compare en
signé : `-1073741515 >= 1` est faux, donc la branche `ECHEC` n'est jamais prise
et `[asan] OK` s'affiche. **Un crash Windows (DLL manquante, violation d'accès,
`abort()`) passe donc actuellement pour un succès — y compris dans la cible
`:test`.** C'est le vrai bug, indépendant de l'ARM64.

Mesuré :

```
ui\win32\build\vibesync_tests_asan.exe test\vectors > asan.log 2>&1
ASAN_RC=-1073741515      ASAN_SIZE=0     (zéro octet de sortie)
ui\win32\build\vibesync_tests.exe      test\vectors > plain.log 2>&1
PLAIN_RC=0               PLAIN_SIZE=997
```

### Diff proposé (NON committé)

```diff
--- a/ui/win32/build.bat
+++ b/ui/win32/build.bat
@@ :test
   "%ROOT%build\vibesync_tests.exe" "%VECTORS%"
-  if errorlevel 1 (
-    echo [test] ECHEC
+  rem `if errorlevel 1` est FAUX pour un code negatif : cmd compare en signe, et
+  rem un crash Windows (0xC0000135 = -1073741515) passait pour un succes.
+  if not "%errorlevel%"=="0" (
+    echo [test] ECHEC ^(code %errorlevel%^)
     exit /b 1
   )
@@ :asan
   echo [asan] execution
-  set "PATH=C:\Users\thibs\tools\llvm-mingw\bin;%PATH%"
+  rem La DLL du runtime ASan n'est pas dans <toolchain>\bin (qui ne contient que
+  rem les binaires de l'hote) mais dans <toolchain>\x86_64-w64-mingw32\bin. On la
+  rem localise depuis %CC%, qu'il soit un chemin absolu ou un nom resolu par le
+  rem PATH : marche pour un hote x86_64 comme pour un hote aarch64.
+  set "CCFULL=%CC%"
+  for /f "delims=" %%D in ('where "%CC%" 2^>nul') do set "CCFULL=%%D"
+  for %%D in ("%CCFULL%") do set "LLVMROOT=%%~dpD.."
+  set "PATH=%LLVMROOT%\x86_64-w64-mingw32\bin;%LLVMROOT%\bin;%PATH%"
   "%ROOT%build\vibesync_tests_asan.exe" "%VECTORS%"
-  if errorlevel 1 (
-    echo [asan] ECHEC
+  if not "%errorlevel%"=="0" (
+    echo [asan] ECHEC ^(code %errorlevel%^)
     exit /b 1
   )
```

(Dans `if not "%errorlevel%"=="0" ( ... %errorlevel% ... )`, les deux expansions
ont lieu au parsing de l'instruction `if`, donc **après** l'exécution de la ligne
précédente : pas besoin de `setlocal enabledelayedexpansion`.)

Attention : ce diff rendra `build.bat asan` **rouge sur cette VM** — c'est voulu,
il devient honnête (§6). Sur la machine x86_64 de Thibault il doit rester vert.

## 6. Point de vigilance nº2 : ASan est un cul-de-sac sur Windows ARM64

Une fois la bonne DLL dans le PATH, l'ASan x86_64 démarre puis meurt en
initialisation :

```
==11252==interception_win: unhandled instruction at 0x7ffbf0066013: 4c 8b ca f6 c1 07 74 1b
AddressSanitizer: CHECK failed: asan_malloc_win.cpp:228
  "(((HEAP_ALLOCATE_UNSUPPORTED_FLAGS & dwFlags) != 0 && "unsupported flags")) != (0)"
  #2 HeapAlloc  asan_malloc_win.cpp:227
  #12 __asan::AsanInitInternal()  asan_rtl.cpp:520
```

L'interception de fonctions d'ASan désassemble les prologues des DLL système pour
y poser ses trampolines ; sous l'émulation x86_64 de Windows-on-ARM, les
`ucrtbase.dll`/`ntdll.dll` traversés ne présentent pas des prologues qu'elle sait
réécrire (`unhandled instruction`), et l'interception de `HeapAlloc` part en
`CHECK failed`. **Ce n'est pas contournable par configuration.**

Le repli naturel — compiler la suite en **aarch64 natif** — est fermé aussi :
llvm-mingw ne livre pas de runtime ASan pour cette cible.

```
ld.lld: error: could not open '.../lib/aarch64-w64-windows-gnu/libclang_rt.asan_dynamic.dll.a'
ld.lld: error: could not open '.../lib/aarch64-w64-windows-gnu/libclang_rt.asan_dynamic_runtime_thunk.a'
```

**Conséquence :** cette VM sert à valider le client Windows **fonctionnellement**
(`build.bat test`, release, tests OS réels), pas sous sanitizers. La couverture
asan+ubsan du C portable reste celle du job `client-macos` de la CI (ADR-010),
ce qui est cohérent : `core/src/*.c` est exactement le même code des deux côtés.
Seul le C Win32 (`ui/win32/src/*.c`) reste hors sanitizer, et le restera tant que
le seul Windows disponible est de l'ARM64.

## 7. Point de vigilance nº3 : PowerShell 5.1 lit les .ps1 en ANSI

Le premier jet de `provision-vm.ps1` ne parsait pas. Cause : trois tirets
cadratins « — » (U+2014). En UTF-8 c'est `E2 80 94` ; PowerShell 5.1, devant un
`.ps1` **sans BOM**, décode en cp1252, et l'octet `0x94` y vaut U+201D, le
guillemet double fermant typographique — que l'analyseur traite comme un vrai `"`.
Toutes les chaînes suivantes se décalent d'un cran et le fichier explose une
centaine de lignes plus bas, avec des messages qui ne pointent pas la cause.

Le script est donc **volontairement en ASCII pur**, et le dit dans son en-tête.
Garde-fou :

```bash
grep -n '[^ -~\t]' scripts/provision-vm.ps1   # doit ne rien renvoyer
```

Vaut pour tout `.ps1` du dépôt destiné à PowerShell 5.1 (`check-size.ps1`,
`run-real-sandbox.ps1`) : soit ASCII, soit UTF-8 **avec** BOM.

## 8. Autres pièges rencontrés (SSH → cmd)

- **`%errorlevel%` derrière `&` ne vaut rien.** `ssh ... "truc.bat & echo RC=%errorlevel%"`
  est parsé d'un bloc : `%errorlevel%` est expansé *avant* que `truc.bat` tourne,
  et affiche invariablement `RC=0`. C'est ce qui a d'abord masqué l'échec asan.
  → passer par un `.bat` déposé en `scp`, où chaque ligne est parsée à son tour.
- **Un `.bat` qui en appelle un autre sans `call` ne revient jamais.** `ui\win32\build.bat`
  depuis un script wrapper transfère le contrôle définitivement ; il faut
  `call ui\win32\build.bat`.
- **Les commandes SSH multilignes vers cmd sont peu fiables** : tout ce qui
  dépasse une ligne part dans un `.bat` copié en `scp`.
- **`$ProgressPreference = 'SilentlyContinue'`** est indispensable avant
  `Invoke-WebRequest` en PS 5.1 (le rendu de la barre de progression coûte ~10x
  le temps de téléchargement).
- **`tar.exe`** (bsdtar, présent de base depuis Win10 1803) décompresse les zip
  bien plus vite que `Expand-Archive` ; le script l'utilise avec repli sur
  `Expand-Archive`.

## 9. Rejouer

```powershell
# depuis le Mac
scp -i ~/.ssh/vibesync_vm_ed25519 scripts/provision-vm.ps1 OPMVPC@192.168.64.2:C:/Users/OPMVPC/
ssh -i ~/.ssh/vibesync_vm_ed25519 OPMVPC@192.168.64.2 \
    "powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\OPMVPC\provision-vm.ps1"
```

Idempotence vérifiée : un second passage ne retélécharge ni ne réinstalle rien
(chaque étape teste la version en place), se termine en quelques secondes et se
contente de faire `git fetch`. Monter une version = bouger le paramètre
correspondant en tête de script (`-GoVersion`, `-LlvmMingwTag`, …).
