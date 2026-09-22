# sysrepo-mcp — instructions pour agents

Serveur MCP (Model Context Protocol) en C qui expose le datastore YANG
[sysrepo](https://github.com/sysrepo/sysrepo) à un agent IA.

> **Source de vérité.** L'état du code et la liste des tâches restantes sont
> dans `sphinx/todo.rst`. Ce fichier-ci ne duplique pas la roadmap : il décrit
> les règles de travail dans le dépôt. Si les deux divergent, `sphinx/todo.rst`
> fait foi, et la divergence est un bug à corriger.

## Règles impératives

1. **Ne jamais modifier `extern/`.** C'est une destination de téléchargement,
   pas un arbre de sources. Contenu exclu de Git, peuplé par
   `make -C docker extern`, monté en lecture seule par le Dockerfile. Il sert
   de code de référence consultable.
2. **Ne jamais compiler en local.** Toute compilation passe par Docker, via
   `scripts/build-docker.sh` ou `make -C docker run`.
3. **Ne pas reconstruire l'image sans raison.** `--force` re-télécharge tout et
   prend plusieurs minutes. Justifié uniquement si une version d'`extern/` a
   bougé dans `docker/Makefile`, si le `Dockerfile` a changé, ou si un paquet
   système manque.
4. **Transport FastCGI uniquement.** Pas de SSE, pas de WebSocket, pas de
   listener HTTP direct. Ces décisions sont documentées et closes.
5. **Mettre à jour la documentation dans le même commit que le code.** Une
   doc qui promet plus que le code ne livre est le défaut le plus coûteux de
   ce dépôt ; il a déjà fallu le corriger une fois.

## Ce qui marche, ce qui ne marche pas

Résumé au moment de la rédaction. Le détail est dans `sphinx/todo.rst`.

| Domaine | État |
|---|---|
| Build eBuild + Docker | fonctionnel |
| Documentation Sphinx | fonctionnelle, `-W` propre |
| Suite de tests | 260+ tests via lighttpd sur le port 80 |
| Transport FastCGI, JSON-RPC, codes HTTP | fonctionnel |
| Cycle de vie MCP (`initialize`, `tools/list`, enveloppe `content`) | fonctionnel |
| Sessions (`Mcp-Session-Id`, TTL, `DELETE`) | fonctionnel, **local au processus** |
| Outils datastore, RPC, actions | fonctionnels |
| Notifications (abonnement, file, `sr_notif_poll`) | fonctionnel |
| Gestion des modules, introspection | fonctionnel |
| Authentification, NACM | **absents** |
| elog | **absent**, `fprintf(stderr)` à la place |
| `get_help` : ranges, patterns, valeurs par défaut | **implémenté** |

> **Aucun contrôle d'accès n'est appliqué.** Un agent obtient les droits de
> l'utilisateur système du serveur. Ne pas exposer cette version.

> **`max-procs` doit valoir 1.** Sessions, abonnements et files d'attente
> vivent dans le processus qui les a créés.

## Architecture

```
Agent IA  ──HTTP/TLS──►  Reverse proxy  ──FastCGI──►  sysrepo-mcp
(client MCP)             (lighttpd/nginx)             │ linké
                                                      ▼
                                             libsysrepo + libyang
                                                      ▼
                                  /etc/sysrepo (modules YANG, startup)
                                  /dev/shm     (running, verrous, events)
```

- **sysrepo est une bibliothèque linkée**, pas un démon. Il n'y a plus de
  démon sysrepo depuis la version 2. Le datastore `running` vit en mémoire
  partagée, pas dans un fichier.
- **Le proxy peut démarrer le serveur lui-même** (lighttpd `bin-path`) : la
  socket arrive alors sur le descripteur 0 et les options de socket de
  `config.in` sont inutilisées.

Détail : `sphinx/architecture.rst`. Interface MCP : `sphinx/api.rst`.

## Dépendances

Téléchargées dans `extern/` par `make -C docker extern`, compilées et
installées dans `/usr/local` par le Dockerfile.

| Librairie | Version | Usage |
|---|---|---|
| **ebuild** | master | Système de build (non linké) |
| **libyang** | 5.8.6 | Parsing YANG, arbres `lyd_node`, export `LYD_JSON` |
| **sysrepo** | 5.1.0 | API datastore |
| **fcgi2** | 2.4.7 | Transport FastCGI (`libfcgi`) |
| **stroll** | master | Structures de données |
| **utils** | master | Utilitaires eTux (fd, file, net, thread, timer) |
| **elog** | master | Journalisation (syslog, fichier, rotation) |

**json-c** ne vient *pas* d'`extern/` : c'est le paquet Debian
`libjson-c-dev` installé dans l'image.

Les versions sont épinglées dans `docker/Makefile`. libyang 5.8.6 et sysrepo
5.1.0 forment la paire utilisée par Netopeer2 2.8.7 ; elles vont ensemble et
ne se bumpent pas séparément.

## Sessions et notifications : pièges connus

- **Une session est locale au processus.** Elle porte ses abonnements sysrepo
  et sa file d'attente. `max-procs` doit rester à 1 tant que le stockage n'est
  pas partagé (roadmap, milestone 1).
- **`SR_SUBSCR_NO_THREAD` est structurant.** Les abonnements sont créés sans
  fil d'exécution sysrepo, et `sr_subscription_process_events()` est appelé au
  début de chaque requête. Le serveur reste mono-thread : pas de verrou sur la
  file, et elle ne change jamais pendant la construction d'une réponse.
  Repasser à un thread sysrepo obligerait à verrouiller la file.
- **`sr_notif_send_tree()` doit être appelé avec `wait = 0`.** Attendre
  bloquerait sur les abonnements de ce même processus, qui ne sont servis
  qu'entre deux requêtes : le serveur s'attendrait lui-même.
- **La session sysrepo d'un abonnement doit lui survivre.** Toujours
  `sr_unsubscribe()` avant `sr_session_stop()`.

## API sysrepo : pièges connus

Le code se trompait sur ces points ; ne pas les réintroduire.

- **`sr_val_t` est déprécié** en amont au profit de `struct lyd_node`. Utiliser
  `sr_get_data()` et non `sr_get_items()` : un tableau de `sr_val_t` ne
  représente pas un arbre, et le reconstruire à la main est une impasse.
- **`sr_edit_commit()` n'existe pas.** Le cycle est `sr_edit_batch()` puis
  `sr_apply_changes()`, avec `sr_discard_changes()` en cas d'échec.
- **`sr_set_item_str()` écrit un seul nœud** depuis sa valeur textuelle. On ne
  peut pas lui passer un document JSON sérialisé.
- **`LYD_PARSE_STRICT` est obligatoire** pour parser un edit. Sans lui, libyang
  ignore silencieusement les nœuds qu'il ne connaît pas, et un agent qui a mal
  orthographié une feuille s'entend répondre que son edit a réussi.
- **`SR_ERR_LY` n'est pas une erreur interne.** Elle vient toujours de ce que
  le client a fourni : un XPath que libyang ne compile pas, ou des données qui
  ne collent pas au schéma. La mapper sur `-32603` dit à l'agent d'abandonner
  quand il devrait corriger sa requête.
- **`lys_nodetype2str()` renvoie `"RPC"` en majuscules.** Normaliser, sinon
  un agent qui filtre sur `"rpc"` rate toutes les opérations.
- **Le datastore d'une session n'est pas implicite.** Une session ouverte sur
  `SR_DS_RUNNING` continue de répondre depuis `running` jusqu'à
  `sr_session_switch_ds()`. C'est la cause habituelle d'une lecture
  opérationnelle vide.
- **NACM se délègue à sysrepo** : `sr_nacm_init()`, `sr_nacm_set_user()`,
  `sr_nacm_check_operation()`, `sr_nacm_destroy()`, dans
  `<sysrepo/netconf_acm.h>`. Ne pas réimplémenter RFC 8341.

## FastCGI : pièges connus

- **Ne pas mélanger les deux API.** `fcgi_stdio.h` remappe `printf` sur son
  propre flux, qui n'est pas la `FCGX_Request` acceptée par la boucle. Une
  réponse écrite avec `printf` n'atteint jamais le client. Choisir une API et
  s'y tenir : `FCGX_*` avec `FCGX_FPrintF(request.out, ...)`.
- **Détecter l'environnement avec `FCGX_IsCGI()`.** Les paramètres de requête
  arrivent dans `request.envp`, pas dans l'environnement du processus : tester
  `getenv("REQUEST_METHOD")` ne marche pas.
- **`stdout` appartient au corps de la réponse**, `stderr` est capturé par le
  proxy. Le seul journal fiable est syslog.
- **Toujours répondre.** Corps vide, JSON invalide, méthode inconnue : il faut
  produire une réponse, sinon le client attend son timeout.

## Système de build

eBuild, un framework Makefile standard dans l'écosystème eTux.

```bash
scripts/build-docker.sh              # binaire
scripts/build-docker.sh --doc        # + documentation (html, pdf, info, man)
scripts/build-docker.sh --doc-html   # + HTML seul
scripts/build-docker.sh --test       # + suite de tests
scripts/build-docker.sh --force      # re-télécharge extern/ et reconstruit l'image
```

Sorties : `build/sysrepo-mcp`, `build/doc/{html,pdf,info,man}/`.

La documentation se construit avec `-W` : un avertissement Sphinx est une
erreur. Vérifier avant de commiter une modification de `sphinx/`.

## Configuration

Deux niveaux, à ne pas confondre :

- **`config.in`** (Kconfig) : options figées à la compilation. Transport, type
  de credential, interrupteurs de contrôle d'accès, chemin du dépôt sysrepo,
  journalisation, et les limites de session (nombre maximum, TTL d'inactivité,
  taille de la file de notifications).
- **`yang/sysrepo-mcp.yang`** : configuration modifiable à chaud (les clés
  d'API) et état opérationnel. Lue et écrite via le datastore, donc soumise à
  NACM comme n'importe quelle donnée.

Les limites de session sont en Kconfig parce que le magasin de sessions est un
tableau fixe dans le processus. Les déplacer vers le modèle YANG fait partie du
milestone 1.

## Structure

```
sysrepo-mcp/
├── extern/                  # Dépendances téléchargées (hors Git, immuables)
├── src/main.c               # Serveur FastCGI
├── include/sysrepo/mcp/     # En-têtes publics (extraits par Doxygen)
├── yang/sysrepo-mcp.yang    # Modèle YANG du serveur
├── tests/                   # Suite pytest
├── docker/                  # Dockerfile, Makefile, lighttpd.conf
├── scripts/                 # build-docker.sh, test.sh
├── sphinx/                  # Documentation RST + Doxyfile
├── config.in                # Options Kconfig
├── Makefile                 # Point d'entrée eBuild
└── ebuild.mk                # Déclaration du binaire
```

## Licence

LGPL-3.0-only. Chaque fichier source porte son tag
`SPDX-License-Identifier: LGPL-3.0-only`. Les bibliothèques linkées ont
d'autres licences (sysrepo et libyang en BSD-3-Clause) : voir
`sphinx/license.rst`.
