# sysrepo-mcp

Serveur MCP (Model Context Protocol) en C qui expose le datastore YANG
[sysrepo](https://github.com/sysrepo/sysrepo) à un agent IA : lire et modifier
la configuration, lire l'état opérationnel, exécuter des RPC et des actions,
explorer les schémas YANG.

> **État du projet.** Le serveur est fonctionnel : transport FastCGI, cycle de
> vie MCP, sessions (`Mcp-Session-Id`), outils de configuration, RPC, actions,
> notifications, gestion des modules et introspection des schémas. Ce qui
> manque : l'**authentification** et **NACM**, elog, et un stockage de sessions
> partagé. La roadmap détaillée, qui fait autorité sur l'état réel du code, est
> dans [`sphinx/todo.rst`](sphinx/todo.rst).

> **Ne pas exposer cette version à un agent non fiable.** Aucun contrôle
> d'accès n'est appliqué : un agent obtient les droits de l'utilisateur système
> qui exécute le serveur.

> **`max-procs` doit valoir 1.** Une session, ses abonnements aux notifications
> et sa file d'attente vivent dans le processus FastCGI qui les a créés. Avec
> plusieurs processus, deux requêtes consécutives d'un même agent atterrissent
> dans des processus différents et la session est introuvable.

## Fonctionnalités

- **Configuration** : lire, modifier et supprimer la configuration via l'API
  sysrepo (`sr_get_config`, `sr_edit_config`, `sr_delete_config`).
- **Monitoring** : lire l'état opérationnel (`sr_get_operational`).
- **Opérations** : exécuter des RPC et des actions YANG.
- **Notifications** : s'abonner aux notifications d'un module sur une session
  et récupérer, à la demande, tout ce qui est arrivé depuis le dernier appel
  (`sr_notif_subscribe`, `sr_notif_poll`). Par scrutation : ce transport ne
  peut rien pousser.
- **Modules** : lister, installer et désinstaller des modules YANG.
- **Introspection** : explorer un schéma (`get_tree`) et documenter un nœud
  (`get_help`), pour qu'un agent construise des XPath valides sans lire le
  YANG.
- **Sécurité** *(à faire)* : clé d'API par agent, associée à un utilisateur
  NACM, et journalisation de chaque modification.

## Architecture

```
┌─────────────┐  HTTP  ┌───────────────┐ FastCGI ┌───────────────┐
│   Agent IA  │◄──────►│ Reverse proxy │◄───────►│  sysrepo-mcp  │
│ (client MCP)│  TLS   │ (lighttpd,    │         │  (ce projet)  │
└─────────────┘        │  nginx)       │         └───────┬───────┘
                       └───────────────┘                 │ linké
                                                         ▼
                                                 ┌───────────────┐
                                                 │  libsysrepo   │
                                                 │  + libyang    │
                                                 └───────┬───────┘
                                                         ▼
                                            ┌──────────────────────┐
                                            │ /etc/sysrepo (YANG,  │
                                            │ startup) + /dev/shm  │
                                            │ (running, verrous)   │
                                            └──────────────────────┘
```

Deux choix structurants :

- **Transport FastCGI uniquement.** Le serveur ne parle jamais HTTP lui-même :
  un reverse proxy termine HTTP et TLS, puis relaie en FastCGI. Pas de socket
  publique, pas de SSE (voir *Limites*).
- **sysrepo en bibliothèque.** `libsysrepo` est linkée dans le binaire et
  appelée directement. Il n'y a aucun démon : sysrepo n'en a plus depuis la
  version 2.

Détail complet dans [`sphinx/architecture.rst`](sphinx/architecture.rst).

## Dépendances

| Bibliothèque | Version | Origine | Rôle |
|---|---|---|---|
| **eBuild** | master | `extern/` | Système de build (non linké) |
| **libyang** | 5.8.6 | `extern/` | Moteur YANG |
| **sysrepo** | 5.1.0 | `extern/` | API datastore YANG |
| **fcgi2** | 2.4.7 | `extern/` | Transport FastCGI (`libfcgi`) |
| **stroll** | master | `extern/` | Structures de données |
| **utils** | master | `extern/` | Utilitaires eTux |
| **elog** | master | `extern/` | Journalisation |
| **json-c** | `libjson-c-dev` | paquet Debian | JSON-RPC |

> **Important** : `extern/` est une destination de téléchargement, pas un
> arbre de sources. Il est exclu de Git (voir `.gitignore`), peuplé par
> `make -C docker extern`, et **ne doit jamais être modifié**. Le Dockerfile
> monte chaque bibliothèque en lecture seule et compile dans une copie
> jetable.

## Construction

La construction se fait **uniquement via Docker**.

```sh
make -C docker build    # télécharge extern/ + construit l'image
make -C docker run      # shell interactif, toutes les libs sont présentes
make                    # compile build/sysrepo-mcp
make install PREFIX=/usr/local
```

Ou depuis l'hôte, en une commande :

```sh
scripts/build-docker.sh              # binaire
scripts/build-docker.sh --doc        # binaire + documentation
scripts/build-docker.sh --test       # binaire + suite de tests
scripts/build-docker.sh --force      # re-télécharge extern/ et reconstruit l'image
```

`make config` ouvre l'interface `menuconfig` d'eBuild ; les options et leurs
valeurs par défaut sont dans `config.in`. `make defconfig` génère une
configuration par défaut sans interaction.

> Toutes les options de `config.in` sont des options de **compilation**. La
> configuration modifiable à chaud (les clés d'API) est dans le modèle YANG
> `yang/sysrepo-mcp.yang`.

Le workflow Docker complet est décrit dans [docker/README.md](docker/README.md).

## Utilisation

```sh
sysrepo-mcp [--help] [--version]
```

Sans argument, le processus attend d'être démarré comme application FastCGI
par un serveur web. Exemple lighttpd :

```
server.modules += ( "mod_fastcgi" )

fastcgi.server = (
    "/mcp" => (
        "sysrepo-mcp" => (
            "socket"      => "/var/run/sysrepo-mcp.sock",
            "bin-path"    => "/usr/local/bin/sysrepo-mcp",
            "check-local" => "disable",
            "max-procs"   => 1
        )
    )
)
```

### Exemple de session

```sh
# 1. Ouvrir une session : l'identifiant revient dans l'en-tête
curl -i -X POST http://localhost/mcp \
     -H 'Content-Type: application/json' \
     -d '{"jsonrpc":"2.0","id":1,"method":"initialize",
          "params":{"protocolVersion":"2025-06-18"}}'

# 2. S'abonner aux notifications, en répétant l'en-tête
curl -X POST http://localhost/mcp \
     -H 'Content-Type: application/json' \
     -H 'Mcp-Session-Id: <identifiant>' \
     -d '{"jsonrpc":"2.0","id":2,"method":"tools/call",
          "params":{"name":"sr_notif_subscribe",
                    "arguments":{"module":"oven"}}}'

# 3. Relever ce qui est arrivé depuis la dernière fois
curl -X POST http://localhost/mcp \
     -H 'Content-Type: application/json' \
     -H 'Mcp-Session-Id: <identifiant>' \
     -d '{"jsonrpc":"2.0","id":3,"method":"tools/call",
          "params":{"name":"sr_notif_poll","arguments":{}}}'

# 4. Fermer la session
curl -X DELETE http://localhost/mcp -H 'Mcp-Session-Id: <identifiant>'
```

## Documentation

```sh
scripts/build-docker.sh --doc        # HTML, PDF, info, man
```

| Format | Sortie |
|---|---|
| HTML | `build/doc/html/index.html` |
| PDF | `build/doc/pdf/sysrepo-mcp.pdf` |
| Info | `build/doc/info/sysrepo-mcp.info` |
| Man | `build/doc/man/` |

Les sources sont dans `sphinx/` : installation, architecture, référence API,
licence, roadmap.

## Structure du projet

```
├── docker/                 # Environnement de build Docker
│   ├── Dockerfile          # image de build (toutes les dépendances)
│   ├── Makefile            # cibles build / build-nc / run / test / extern
│   └── lighttpd.conf       # proxy FastCGI pour les tests
├── extern/                 # Sources des dépendances (téléchargées, hors Git)
├── include/sysrepo/mcp/    # En-têtes publics
├── scripts/                # build-docker.sh, test.sh
├── sphinx/                 # Documentation (RST + Doxyfile)
├── src/                    # Code source du serveur
├── tests/                  # Suite pytest
├── yang/                   # Modèle YANG du serveur
├── config.in               # Options Kconfig (compilation)
├── Makefile                # Point d'entrée eBuild
└── ebuild.mk               # Déclaration du binaire
```

## Limites

- **Pas de SSE.** Le serveur répond à un POST par un unique objet JSON, ce que
  la liaison Streamable HTTP de MCP autorise explicitement. C'est un choix de
  périmètre, pas une impossibilité technique : FastCGI sait diffuser, mais le
  buffering des proxies et le modèle `max-procs` s'y prêtent mal.
  Conséquence : **les notifications sysrepo ne peuvent pas être poussées vers
  l'agent**. Elles ne sont pas perdues pour autant — le serveur s'abonne à sa
  place, met en file, et lui remet le tout quand il appelle `sr_notif_poll`.
  Le sampling et l'elicitation MCP, eux, sont hors périmètre.
- **`max-procs = 1`.** Les sessions sont locales au processus ; voir la
  roadmap, milestone 1.
- **Pas de transport direct.** HTTP, TLS et limitation de débit restent la
  responsabilité du reverse proxy.
- **Pas de contrôle d'accès.** Voir la roadmap, milestone 2.

## Licence

LGPL-3.0-only. Voir [COPYING.LESSER](COPYING.LESSER) et [COPYING.txt](COPYING.txt).

Les bibliothèques linkées ont leurs propres licences : sysrepo et libyang sont
en BSD-3-Clause, json-c en MIT, fcgi2 sous licence FastCGI. Détail dans
`sphinx/license.rst`.
