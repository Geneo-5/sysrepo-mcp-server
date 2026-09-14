# sysrepo-mcp

## Description

Serveur MCP (Model Context Protocol) en C qui expose les fonctionnalités de
[sysrepo](https://github.com/sysrepo/sysrepo) à un agent IA. Ce projet permet
de configurer un système via des données sysrepo (modèles YANG) et d'en
contrôler le statut en temps réel.

> **Note** : Ce projet est en phase de développement actif. La documentation
> et le code sont stabilisés mais l'implémentation des fonctionnalités sysrepo
> est en cours. Voir la section [Tâches à faire](#taches-à-faire) pour l'état actuel.

## Objectifs

- **Configuration** : Permettre à un agent IA de modifier la configuration du
  système via l'API sysrepo (YANG models).
- **Monitoring** : Exposer les status et événements du système (états des interfaces,
  routes, etc.) via les opérations sysrepo.
- **Sécurité** : Assurer une authentification et autorisation appropriées pour
  les opérations de l'agent.
- **Extensibilité** : Permettre l'ajout de nouveaux modules de configuration
  et de monitoring.

## Architecture

```
+-------------+      +----------------------+      +-----------------+
|   AI Agent  |<---->|  Reverse Proxy       |<---->|  sysrepo-mcp    |
| (OpenHands  |      |  (lighttpd/nginx)    |      | (FastCGI)       |
|  SDK, etc.) |      |                      |      +-----------------+
+-------------+      |  - HTTP/HTTPS        |                  |
                     |  - TLS termination   |                  |
                     |  - Load balancing    |                  |
                     |  - Rate limiting     |                  |
                     +----------------------+                  |
                                                             \bigtriangledown
                                                    +-----------------+
                                                    |  Sysrepo Lib. |
                                                    |  (linked)     |
                                                    +--------+--------+
                                                             |
                                                             \bigtriangledown
                                                    +-----------------+
                                                    |  YANG Models    |
                                                    |  (Datastore)    |
                                                    +-----------------+
```

- **sysrepo** : Bibliothèque C liée au binaire qui fournit les opérations
  NETCONF sur les modèles YANG
- **MCP Server** : Couche d'abstraction qui expose les opérations sysrepo via
  le protocole Model Context Protocol
- **Reverse Proxy (lighttpd/nginx)** : Gère HTTP/HTTPS, TLS, et forward les requêtes
  vers sysrepo-mcp via **FastCGI** (obligatoire)
- **AI Agent** : Peut interagir avec le serveur pour lire/modifier la
  configuration et recevoir des notifications

## Règles importantes

- Le dossier `extern/` est un dossier de sources externes utilisé comme code de
  référence. **Il ne doit jamais être modifié.**
- **Toute compilation doit se faire dans l'image Docker** via
  `scripts/build-docker.sh`. Ne jamais compiler localement.
- **Transport** : FastCGI uniquement (fcgi2 obligatoire). SSE n'est PAS implémenté
  (incompatible avec FastCGI).

## Dépendances

Toutes les dépendances sont incluses dans le dossier `extern/` et versionnées
comme sous-modules Git.

### Librairies externes

| Librairie | Description | Usage dans le projet |
|-----------|-------------|----------------------|
| **ebuild** | Système de construction Makefile pour projets C | Système de build |
| **utils** | Librairie utilitaire eTux | Fonctions communes (fd, file, net, thread, timer, etc.) |
| **stroll** | Librairie de structures de données | Listes, hash tables, heap, buffers, allocation, etc. |
| **fcgi2** | FastCGI library | **Serveur FastCGI pour exposer le MCP** (obligatoire) |
| **libyang** | Bibliothèque de parsing YANG | Parsing et validation des schémas YANG, export LYD_JSON |
| **sysrepo** | Système de configuration NETCONF | API de configuration et monitoring YANG (linked library) |
| **json-c** | JSON library | Parsing/génération des messages JSON-RPC |
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

La génération de documentation est **stabilisée** :

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
│   └── main.c               # Point d'entrée (FastCGI server)
├── yang/                     # Modèles YANG du projet
│   └── sysrepo-mcp.yang     # Modèle YANG du serveur (API keys + server-state)
├── include/sysrepo/mcp/     # En-têtes publics
├── docker/                   # Dockerfile et Makefile pour le build Docker
├── config.in                # Configuration Kconfig (build-time options)
├── scripts/
│   └── build-docker.sh      # Script de compilation dans Docker
├── Makefile                 # Configuration ebuild
├── ebuild.mk                # Extension du système ebuild
├── sphinx/                  # Documentation Sphinx (sources RST)
└── README.md                # Documentation utilisateur
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

Le serveur démarrera comme un **serveur FastCGI** et attendra les connexions
du reverse proxy (lighttpd, nginx).

### Configuration du reverse proxy (lighttpd)

```nginx
fastcgi.server = (
    "/mcp" => (
        "sysrepo-mcp" => (
            "socket" => "/var/run/sysrepo-mcp.sock",
            "check-local" => "disable",
            "bin-path" => "/usr/local/bin/sysrepo-mcp",
            "max-procs" => 4
        )
    )
)
```

## Sécurité

- Les opérations de l'agent **doivent** être validées contre les permissions
  (implémentation en cours via NACM).
- Le transport MCP **doit** être protégé (TLS au niveau du reverse proxy).
- Chaque opération de configuration **doit** être journalisée.

## Licence

Ce projet est distribué sous les termes de la licence **LGPL-3.0**
(voir [COPYING.LESSER](COPYING.LESSER)).

---

## Tâches à faire

### Terminé

- Creer la structure du projet (Makefile, ebuild.mk, Docker, etc.)
- Definir le modele YANG (yang/sysrepo-mcp.yang) avec:
  - list api-key pour les cles API avec utilisateurs NACM
  - container server-state pour l'etat operationnel du serveur
- Mettre a jour la documentation Sphinx:
  - sphinx/architecture.rst : Architecture FastCGI, diagrammes, configuration
  - sphinx/api.rst : Reference API complete avec exemples (oven, sysrepo-mcp)
  - sphinx/install.rst : Instructions d'installation
  - sphinx/license.rst : Licence LGPL-3.0
- Nettoyer config.in : Supprimer les options runtime (dans YANG), garder build-time
- Implemente le squelette du serveur FastCGI dans src/main.c
- Configurer les dependances dans ebuild.mk (fcgi2, json-c, sysrepo)
- Verifier que le code compile avec scripts/build-docker.sh
- Maintenir AGENTS.md avec l'etat actuel du projet
- Creer des tests unitaires pour chaque outil MCP
- Tester avec des modeles YANG concrets (oven.yang, etc.)
- Tester le reverse proxy (lighttpd/nginx) avec FastCGI

#### Serveur FastCGI de base
- Implemente la gestion complete des connexions FastCGI
- Ajouter la gestion des erreurs et logging via elog
- Implemente le parsing des headers HTTP (Authorization, etc.)

#### Integration sysrepo
- Initialiser la connexion sysrepo (sr_conn_open())
- Implemente sr_get_config() avec:
  - Parsing du xpath (format: /module:container)
  - Appel a sr_get_items() ou sr_get_subtree()
  - Retour en format LYD_JSON via libyang (lyd_print_fd avec LYD_JSON)
- Implemente sr_edit_config() avec:
  - Parsing du config JSON (format compatible libyang/sysrepo)
  - Appel a sr_set_item() / sr_set_items()
  - Validation du schema YANG
- Implemente sr_get_operational() avec:
  - Appel a sr_get_items() sur le datastore operational
  - Retour en format LYD_JSON
- Implemente sr_module_install() et sr_module_uninstall()
- Implemente sr_execute_rpc() avec:
  - Parsing du xpath RPC (format: /module:rpc-name)
  - Appel a sr_rpc_send()
- Implemente sr_action() avec:
  - Parsing du xpath action (format: /module:action-name)
  - Appel a l'action YANG via sysrepo

#### Gestion de la configuration
- Implemente la lecture/ecriture de la configuration runtime via YANG:
  - Charger les API keys depuis le datastore sysrepo
  - Valider les cles API contre le datastore
  - Mettre a jour server-state dynamiquement
- Implemente la configuration build-time depuis config.in:
  - Transport FastCGI (Unix socket ou TCP)
  - Options de logging
  - Chemins sysrepo

#### Authentification et ACL
- Implemente l'authentification Bearer token:
  - Validation des cles API (header Authorization: Bearer <key>)
  - Association cle -> utilisateur NACM
- Implemente l'authentification par cookie (optionnelle)
- Integrer sysrepo NACM pour le controle d'acces:
  - Verification des permissions par utilisateur NACM
  - Filtrage par module
  - Filtrage par operation (read/write)

#### Outils systeme
- Implemente get_status() avec:
  - Version du serveur
  - Uptime
  - Nombre de sessions actives
  - Maximum de sessions configure
- Implemente get_tree() via libyang:
  - Recuperation du schema YANG
  - Construction de l'arbre hierarchique
  - Inclusion des descriptions (optionnel)
- Implemente get_help() via libyang:
  - Recuperation de la documentation d'un node
  - Parsing des descriptions YANG
  - Validation des valeurs possibles (enum, etc.)

#### Tests et validation
- Creer des tests unitaires pour chaque outil MCP
- Tester avec des modeles YANG concrets (oven.yang, etc.)
- Valider la compatibilite avec les agents IA (OpenHands, etc.)
- Tester le reverse proxy (lighttpd/nginx) avec FastCGI

### Backlog (futur)

- Support pour la validation de schema avancee
- Integration avec des modeles YANG custom
- Support pour les notifications sysrepo (si compatible FastCGI)
- Metriques et monitoring avance
- Documentation en francais complete
