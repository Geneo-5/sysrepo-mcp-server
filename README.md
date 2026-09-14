# sysrepo-mcp

Serveur MCP (Model Context Protocol) qui expose les fonctionnalités de
[sysrepo](https://github.com/sysrepo/sysrepo) à un agent IA. Il permet de
configurer un système via des données sysrepo (modèles YANG) et d'en
contrôler le statut en temps réel.

> **Note** : Ce projet est un squelette d'intégration. Aucune fonctionnalité
> n'est implémentée à ce stade. La documentation décrit l'architecture cible
> et les choix techniques.

## Fonctionnalités

- **Configuration** : l'agent IA pourra modifier la configuration du système
  via l'API sysrepo (modèles YANG).
- **Monitoring** : le serveur exposera les status et événements du système
  (notifications sysrepo, états des interfaces, routes, ...).
- **Sécurité** : authentification et autorisation de l'agent avant les
  opérations sensibles ; journalisation de toutes les opérations.

## Architecture

```
┌─────────────┐      ┌──────────────┐      ┌─────────────┐
│   AI Agent  │◄────►│  MCP Server  │◄────►│   sysrepo   │
│ (OpenHands, │      │  (ce projet) │      │  Library    │
│  etc.)      │      └──────────────┘      │ (linked)    │
└─────────────┘                            └─────────────┘
                                             │
                                             ▼
                                    ┌──────────────┐
                                    │ YANG Models  │
                                    │ (sysrepo-mcp │
                                    │  + custom)  │
                                    └──────────────┘
```

- **libyang** : moteur YANG (parsing / validation des schémas).
- **sysrepo** : bibliothèque C de gestion du datastore de configuration NETCONF.
  Le serveur utilise sysrepo **en mode librairie** (linked at build time),
  accédant directement aux fichiers du datastore (`/etc/sysrepo/data/`).
  Le serveur implémente son propre modèle YANG (`yang/sysrepo-mcp.yang`) pour la
  configuration du serveur et son état opérationnel.
- **fcgi2** : bibliothèque FastCGI (`libfcgi`) pour le transport. **Obligatoire**.
- **json-c** : bibliothèque JSON pour le parsing/la génération de messages JSON-RPC.
- **utils** / **stroll** : bibliothèques utilitaires eTux.
- **ebuild** : système de construction Makefile.

## Construction

La construction se fait **uniquement via Docker** (recommandé pour CI et agents IA).
Le workflow standard est :

```sh
make -C docker build    # télécharge extern/ + construit l'image
make -C docker run      # shell interactif : les libs sont toutes là
make                    # compile le binaire build/sysrepo-mcp
make install            # installe /usr/local/bin/sysrepo-mcp
```

`make config` ouvre l'interface `menuconfig` du système eBuild pour
configurer le projet (adresse d'écoute, port, journalisation, etc.) ;
les valeurs par défaut sont définies dans `config.in`.
Il est aussi possible de générer une configuration sans interface
interactive avec `make defconfig`.

> **Note** : Toutes les dépendances sont fournies via l'image Docker.
> Aucune installation manuelle n'est requise sur le système hôte.

### Prérequis

Le système de build doit fournir :

- Docker avec BuildKit (pour construire l'image)
- eBuild (le système de construction — présent dans `extern/ebuild/`)
- GCC (8+) ou Clang (dans l'image Docker)
- les bibliothèques, compilées et installées dans l'image :
  **fcgi2**, **json-c**, **libyang**, **sysrepo**, **stroll**, **utils** (eTux)
- `pkg-config` et `kconfig-frontends` (pour `make config` dans l'image)

Le dossier `extern/` contient les sources des dépendances utilisées pour :
- Construire l'image Docker (les bibliothèques y sont compilées et installées)
- Servir de référence pour les agents IA (code source consultable)

> **Important** : le dossier `extern/` est un dossier de sources téléchargées.
> Il ne fait pas partie du dépôt Git (voir `.gitignore`) et **ne doit jamais
> être modifié**. Les sources sont téléchargées automatiquement via
> `make -C docker extern`.

Pour CI et agents IA, le workflow complet (build, test, run) est décrit
dans [docker/README.md](docker/README.md).

## Utilisation

```sh
./build/sysrepo-mcp [options]

  --help          Aide
  --version       Version
```

Le serveur écoute par défaut sur une socket FastCGI. En production, un
reverse proxy (lighttpd, nginx) forward les requêtes HTTP vers sysrepo-mcp.

## Documentation

La documentation HTML (guide d'installation, architecture du pont MCP ↔
sysrepo, API) est générée avec Sphinx :

```sh
make doc         # génère doc/ (HTML)
```

## Structure du projet

```
├── docker/                 # Environnement de build Docker
│   ├── Dockerfile          # image de build (toutes les dépendances)
│   ├── Makefile            # cibles build / run / test / extern
│   └── README.md           # doc du workflow Docker
├── extern/                 # Sources des dépendances (téléchargées, non versionnées)
│   ├── ebuild/             # système de build
│   ├── utils/              # utilitaires eTux
│   ├── stroll/             # structures de données
│   ├── fcgi2/              # FastCGI
│   ├── libyang/            # moteur YANG
│   └── sysrepo/            # librairie NETCONF
├── include/sysrepo/mcp/    # En-têtes publics
├── src/                    # Code source (squelette)
├── sphinx/                 # Sources de la documentation (Sphinx)
├── yang/                   # Modèles YANG du projet
│   └── sysrepo-mcp.yang    # Modèle YANG du serveur sysrepo-mcp
├── Makefile                # Point d'entrée du build (ebuild)
└── ebuild.mk               # Déclaration du binaire
```

## Limites

- **Pas de SSE** : Server-Sent Events ne sera pas implémenté car incompatible avec
  le protocole FastCGI utilisé pour le transport.
- **Transport FastCGI uniquement** : Le serveur est conçu pour fonctionner derrière
  un reverse proxy (lighttpd, nginx) via FastCGI. Aucun support pour socket Unix
  ou TCP direct.

## Sécurité

- Les opérations de l'agent **seront** validées contre les permissions de
  `agent.allowed_modules` (à implémenter).
- Le transport MCP doit être protégé (TLS au niveau du reverse proxy).
- Chaque opération de configuration **sera** journalisée (à implémenter).

## License

Ce projet est distribué sous les termes de la licence **LGPL-3.0**
(voir [COPYING.LESSER](COPYING.LESSER)).
