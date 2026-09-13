# sysrepo-mcp

## Description

Serveur MCP (Model Context Protocol) en C qui expose les fonctionnalités de
[sysrepo](https://github.com/sysrepo/sysrepo) à un agent IA. Ce projet permet
de configurer un système via des données sysrepo et d'en contrôler le statut
en temps réel.

> **Note** : Ce projet est encore en phase de développement. La documentation
> est un draft et peut être incomplète ou sujette à modification. Les APIs
> et configurations ne sont pas figées.

## Objectifs

- **Configuration** : Permettre à un agent IA de modifier la configuration du
  système via l'API sysrepo (YANG models).
- **Monitoring** : Exposer les status et événements du système (notifications
  sysrepo, états des interfaces, routes, etc.).
- **Sécurité** : Assurer une authentification et autorisation appropriées pour
  les opérations de l'agent.
- **Extensibilité** : Permettre l'ajout de nouveaux modules de configuration
  et de monitoring.

## Architecture

```
+-------------+      +----------------------+      +-----------------+
|   AI Agent  |<---->|  Reverse Proxy       |<---->|  sysrepo-mcp    |
| (OpenHands  |      |  (lighttpd)          |<---->|  (Stream)       |
|  SDK, etc.) |      |                      |      +-----------------+
+-------------+      |  - HTTP/HTTPS        |                  |
                     |  - TLS termination   |                  |
                     |  - Load balancing    |                  |
                     |  - Rate limiting     |                  |
                     +----------------------+                  |
                                                             \(\bigtriangledown\)
                                                    +-----------------+
                                                    |  Sysrepo Lib. |
                                                    |  (linked)     |
                                                    +--------+--------+
                                                             |
                                                             \(\bigtriangledown\)
                                                    +-----------------+
                                                    |  YANG Models    |
                                                    |  (Datastore)    |
                                                    +-----------------+
```

- **sysrepo** : Bibliothèque C liée au binaire qui fournit les opérations
  NETCONF sur les modèles YANG
- **MCP Server** : Couche d'abstraction qui expose les opérations sysrepo via
  le protocole Model Context Protocol
- **Reverse Proxy (lighttpd)** : Gère HTTP/HTTPS, TLS, et forward les requêtes
  vers sysrepo-mcp via un flux (socket Unix ou TCP)
- **AI Agent** : Peut interagir avec le serveur pour lire/modifier la
  configuration et recevoir des notifications

## Règles importantes

- Le dossier `extern/` est un dossier de sources externes utilisé comme code de
  référence. **Il ne doit jamais être modifié.**
- **Toute compilation doit se faire dans l'image Docker** via
  `scripts/build-docker.sh`. Ne jamais compiler localement.

## Dépendances

Toutes les dépendances sont incluses dans le dossier `extern/` et versionnées
comme sous-modules Git.

### Librairies externes

| Librairie | Description | Usage dans le projet |
|-----------|-------------|----------------------|
| **ebuild** | Système de construction Makefile pour projets C | Système de build |
| **utils** | Librairie utilitaire eTux | Fonctions communes (fd, file, net, thread, timer, etc.) |
| **stroll** | Librairie de structures de données | Listes, hash tables, heap, buffers, allocation, etc. |
| **fcgi2** | FastCGI library | Serveur FastCGI pour exposer le MCP over FastCGI |
| **libyang** | Bibliothèque de parsing YANG | Parsing et validation des schémas YANG |
| **sysrepo** | Système de configuration NETCONF | API de configuration et monitoring YANG |
| **elog** | Logging system | Gestion des logs (syslog, fichiers, rotation) |

### Emplacement

```
extern/
├── ebuild/      # Système de build (Makefile system)
├── utils/       # eTux utilities (fd, file, net, thread, timer, etc.)
├── stroll/      # Data structures (lists, hashes, heaps, buffers)
├── fcgi2/       # FastCGI library
├── libyang/     # YANG schema parsing/validation
├── sysrepo/     # sysrepo configuration datastore API
└── elog/        # Logging system (syslog, file, rotation)
```

## Système de build

Ce projet utilise le système de construction **ebuild**, un framework Makefile
pour projets C. Ce système est standard dans l'écosystème eTux.

### Construire les sources (binaire)

**Toute compilation doit passer par Docker :**

```bash
# Compiler le binaire uniquement
./scripts/build-docker.sh

# Rebuild forcé (télécharge les sources et reconstruit l'image Docker)
./scripts/build-docker.sh --force

# Avec smoke tests
./scripts/build-docker.sh --test
```

### Construire la documentation

La génération de documentation est **stabilisée** : `scripts/build-docker.sh --doc`
produit du HTML, du PDF et des pages de man sans erreur.

```bash
# Compiler + documentation (HTML, PDF, info, man)
./scripts/build-docker.sh --doc

# Documentation HTML uniquement
./scripts/build-docker.sh --doc-html

# Documentation PDF uniquement
./scripts/build-docker.sh --doc-pdf

# Pages de man uniquement
./scripts/build-docker.sh --doc-man
```

La documentation générée se trouve dans :

- **HTML** : `build/doc/html/index.html`
- **PDF** : `build/doc/pdf/sysrepo-mcp.pdf`
- **Info** : `build/doc/info/sysrepo-mcp.info`
- **Man** : `build/doc/man/`

### Rebuild de l'image Docker

**L'image Docker ne doit être reconstruite que si c'est vraiment nécessaire.**
Les sources des dépendances (dossiers dans `extern/`) changent rarement.

Pour forcer un rebuild complet (téléchargement des sources + reconstruction de l'image) :

```bash
./scripts/build-docker.sh --force
```

À éviter sauf si :
- Les sous-modules `extern/` ont été mis à jour (`git submodule update`)
- Le `Dockerfile` a été modifié
- Une dépendance système a changé (paquets texlive, etc.)

## Structure du projet

```
sysrepo-mcp/
├── extern/                   # Dépendances (sous-modules Git)
│   ├── ebuild/              # Système de build
│   ├── utils/               # eTux utilities
│   ├── stroll/              # Data structures
│   ├── fcgi2/               # FastCGI library
│   ├── libyang/             # YANG parsing
│   ├── sysrepo/             # sysrepo API
│   └── elog/                # Logging system
├── src/                      # Code source du serveur
│   └── main.c               # Point d'entrée
├── include/                  # En-têtes publics
├── sphinx/                   # Documentation Sphinx (sources RST)
├── docker/                   # Dockerfile et Makefile pour le build Docker
├── config.in                # Configuration Kconfig (valeurs par défaut)
├── scripts/
│   └── build-docker.sh      # Script de compilation dans Docker
├── Makefile                  # Configuration ebuild
├── ebuild.mk                # Extension du système ebuild
└── README.md                 # Documentation utilisateur
```

## Prérequis de construction

- Docker (avec le daemon en cours d'exécution)
- GCC (version 8+) ou Clang (dans l'image Docker)
- pkg-config (dans l'image Docker)

Les dépendances sont fournies dans `extern/` et n'ont pas besoin d'être
installées séparément.

## Utilisation

```bash
./sysrepo-mcp [--help] [--version]
```

Le serveur démarrera et exécutera le protocole MCP sur une socket Unix ou TCP
(par défaut, configurable via les arguments ou le fichier de configuration).

## Sécurité

- Les opérations de l'agent doivent être validées contre des politiques
  d'accès.
- Les connexions MCP doivent être chiffrées (TLS).
- Journalisation sécurisée des opérations de configuration.

## Licence

Ce projet est distribué sous les termes de la licence BSD 3-Clause (voir
[COPYING.txt](COPYING.txt) et [COPYING.LESSER](COPYING.LESSER)).
