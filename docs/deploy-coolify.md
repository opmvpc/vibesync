# Déployer le serveur sur Coolify

Le serveur (`cmd/vibesync-server`) est un unique binaire Go stdlib, packagé en
image Docker minimale (`Dockerfile` à la racine, ~18 Mo, Alpine + utilisateur
non-root). Ce guide part d'une instance Coolify déjà installée et opérationnelle.

## 1. Créer la ressource

1. Dans Coolify : **Projects** → ton projet → **+ New Resource**.
2. Type de ressource : **Docker Compose**.
3. Source : dépôt Git — `github.com/opmvpc/vibesync` (privé). Connecte le GitHub
   App Coolify au repo si ce n'est pas déjà fait (accès en lecture seule suffit).
4. Branche : `main`.
5. Fichier compose : `docker-compose.yml` (racine du repo). Coolify build l'image
   depuis le `Dockerfile` du repo — aucun registre externe à configurer.

Le `docker-compose.yml` du repo :

```yaml
services:
  vibesync:
    build: .
    restart: unless-stopped
    environment:
      - VIBESYNC_PASSWORD=${VIBESYNC_PASSWORD:-}
      - VIBESYNC_LOG=${VIBESYNC_LOG:-info}
    expose:
      - "8080"
```

Le service **expose** le port 8080 en interne au réseau Coolify — il n'y a
**aucun port TCP brut à publier** ni à mapper toi-même : c'est Traefik (le proxy
que Coolify gère) qui route le trafic public vers ce port.

## 2. Attacher un domaine

1. Dans la ressource, onglet **Domains** (ou **General** selon la version) :
   renseigne un domaine ou sous-domaine pointant vers ton instance Coolify en
   DNS (A/AAAA vers l'IP du serveur, ou CNAME selon ton hébergeur).
2. Coolify configure automatiquement Traefik comme reverse proxy et provisionne
   un certificat **Let's Encrypt** pour ce domaine — le TLS est géré pour toi,
   pas de manipulation manuelle de certificats.
3. Une fois le certificat émis, le serveur est joignable en
   `wss://ton-domaine.tld/ws` (WebSocket) et `https://ton-domaine.tld/healthz`
   (healthcheck). C'est cette URL `wss://.../ws` que tes amis renseigneront dans
   leur client.

## 3. Variables d'environnement

À définir dans l'onglet **Environment Variables** de la ressource Coolify (elles
sont injectées dans le `docker-compose.yml` via `${VAR}`) :

| Variable | Rôle | Défaut |
|---|---|---|
| `VIBESYNC_PASSWORD` | mot de passe global optionnel, exigé au `hello` de chaque client. Vide = serveur ouvert à qui a l'URL. | vide |
| `VIBESYNC_LOG` | niveau de log : `debug`, `info`, `warn`, `error` | `info` |
| `VIBESYNC_MAX_CLIENTS` | connexions simultanées max | `200` |
| `VIBESYNC_MAX_ROOMS` | salles vivantes simultanées max | `50` |
| `VIBESYNC_MAX_ROOM_SIZE` | membres max par salle | `20` |

Les trois plafonds `VIBESYNC_MAX_*` ne sont pas dans le `docker-compose.yml` par
défaut mais sont lus par le serveur si tu les ajoutes en variables d'environnement
Coolify — utile pour éviter un abus si le serveur est ouvert sans mot de passe.

`VIBESYNC_ADDR` (adresse d'écoute HTTP, défaut `:8080`) n'a en principe pas besoin
d'être modifiée : le `Dockerfile` fixe déjà `ENV VIBESYNC_ADDR=:8080` et
`docker-compose.yml` s'aligne dessus (`expose: 8080`). Ne la change que si tu sais
pourquoi et que tu mets aussi à jour l'`expose`.

## 4. Healthcheck

L'image embarque un `HEALTHCHECK` Docker natif (`wget` vers
`http://127.0.0.1:8080/healthz` toutes les 30 s) — Coolify l'utilise pour
déterminer si le conteneur est sain, aucune configuration supplémentaire requise.

## 5. Déployer

Bouton **Deploy** dans Coolify. Le build se fait via le `Dockerfile` multi-stage
du repo (compilation Go dans un premier stage, image finale Alpine minimale) —
compte quelques dizaines de secondes.

## 6. Vérification post-déploiement

```bash
curl -i https://ton-domaine.tld/healthz
```

Réponse attendue : `200 OK` avec le corps `ok`. Si tu obtiens une erreur TLS ou
une 502 :

- vérifie que le domaine a bien fini de propager en DNS et que le certificat
  Let's Encrypt est émis (statut visible dans l'onglet du domaine, côté Coolify) ;
- vérifie dans les logs du conteneur (onglet **Logs**) que le serveur a bien
  démarré (`VIBESYNC_LOG=debug` temporairement si besoin de détails).

Pour vérifier le WebSocket lui-même, le plus simple est de lancer un client
(`vibesync.exe` ou `VibeSync.app`) avec `wss://ton-domaine.tld/ws` comme adresse
serveur et de constater la connexion.

## 7. Mettre à jour

Un `git push` sur `main` (ou un nouveau tag) suffit si le **déploiement
automatique** est activé côté Coolify (webhook GitHub). Sinon, ou pour forcer une
mise à jour immédiate : bouton **Redeploy** dans la ressource — Coolify rebuild
l'image depuis le dernier commit de la branche configurée et remplace le
conteneur (`restart: unless-stopped` garantit qu'il repart automatiquement).

Le serveur ne stocke aucun état persistant à disque (les salles vivent en
mémoire et se recréent à la demande) : un redeploy ferme simplement toutes les
connexions actives, les clients se reconnectent seuls (backoff automatique) et
retrouvent leur salle au prochain `hello`.
