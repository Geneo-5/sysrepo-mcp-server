# sysrepo-mcp-server

## Description

Serveur MCP (Model Context Protocol) en C qui expose les fonctionnalités de
[sysrepo](https://github.com/sysrepo/sysrepo) à un agent IA. Ce projet permet
de configurer un système via des données sysrepo et d'en contrôler le statut
en temps réel.

## Objectifs

- **Configuration**: Permettre à un agent IA de modifier la configuration du
  système via l'API sysrepo (YANG models).
- **Monitoring**: Exposer les status et événements du système (notifications
  sysrepo, états des interfaces, routes, etc.).
- **Sécurité**: Assurer une authentification et autorisation appropriées pour
  les opérations de l'agent.
- **Extensibilité**: Permettre l'ajout de nouveaux modules de configuration
  et de monitoring.

## Architecture

```
┌─────────────┐      ┌──────────────┐      ┌─────────────┐
│   AI Agent  │◄────►│  MCP Server  │◄────►│   sysrepo   │
│ (OpenHands  │      │  (C Library) │      │   Daemon    │
│  SDK, etc.) │      └──────────────┘      └─────────────┘
└─────────────┘                                │
                                               ▼
                                      ┌──────────────┐
                                      │ YANG Models  │
                                      │ (Configuration)│
                                      └──────────────┘
```

- **sysrepo**: Système de stockage de configuration basé sur les modèles YANG
- **MCP Server**: Couche d'abstraction qui expose les opérations sysrepo via
  le protocole Model Context Protocol
- **AI Agent**: Peut interagir avec le serveur pour lire/modifier la
  configuration et recevoir des notifications

## Règles importantes

- Le dossier `extern/` est un dossier de sources externes utilisé comme code de
  référence. **Il ne doit jamais être modifié.**

## Dépendances

Toutes les dépendances sont incluses dans le dossier `extern/` et versionnées
comme sous-modules Git.

### Librairies externes

| Librairie | Description | Usage dans le projet |
|-----------|-------------|----------------------|
| **ebuild** | Système de construction Makefile pour projets C | Système de build |
| **utils** | Librairie utilitaire eTux | Fonctions communes (gestion de fichiers, sockets, événements, timers, etc.) |
| **stroll** | Librairie de structures de données | Listes, hash tables, heap, buffers, allocation, etc. |
| **fcgi2** | FastCGI library | Serveur FastCGI pour exposer le MCP over FastCGI |
| **libyang** | Bibliothèque de parsing YANG | Parsing et validation des schémas YANG |
| **sysrepo** | Système de configuration NETCONF | API de configuration et monitoring YANG |

### Emplacement

```
extern/
├── ebuild/      # Système de build (Makefile system)
├── utils/       # eTux utilities (fd, file, net, thread, timer, etc.)
├── stroll/      # Data structures (lists, hashes, heaps, buffers)
├── fcgi2/       # FastCGI library
├── libyang/     # YANG schema parsing/validation
└── sysrepo/     # sysrepo configuration datastore API
```

## Système de build

Ce projet utilise le système de construction **ebuild**, un framework Makefile
pour projets C. Ce système est standard dans l'écosystème eTux.

### Construire le projet

```bash
# Configuration
make configure

# Construction
make

# Installation (optionnel)
sudo make install

# Nettoyage
make clean
```

### Règles ebuild

Le système de build suit la structure standard d'ebuild :

- `Makefile` principal à la racine du projet
- `src/` : code source C
- `include/` : en-têtes publics
- `tests/` : tests unitaires
- `sphinx/` : documentation

## Structure du projet

```
sysrepo-mcp-server/
├── extern/                   # Dépendances (sous-modules Git)
│   ├── ebuild/              # Système de build
│   ├── utils/               # eTux utilities
│   ├── stroll/              # Data structures
│   ├── fcgi2/               # FastCGI library
│   ├── libyang/             # YANG parsing
│   └── sysrepo/             # sysrepo API
├── src/                      # Code source du serveur
│   ├── main.c               # Point d'entrée
│   ├── mcp_server.c         # Implémentation du serveur MCP
│   ├── sysrepo_bridge.c     # Pont vers sysrepo API
│   └── ...
├── include/                  # En-têtes publics
├── tests/                    # Tests unitaires
├── models/                   # Modèles YANG utilisés
├── Makefile                  # Configuration ebuild
└── README.md                 # Documentation utilisateur
```

## Prérequis de construction

- GCC (version 8+) ou Clang
- pkg-config
- Python 3 (pour certains scripts ebuild)
- Documentation : make, man (optionnel)

Les dépendances sont fournies dans `extern/` et n'ont pas besoin d'être
installées séparément.

## Construction et installation

Le projet utilise un Makefile ebuild. Voir le README.md à la racine du
dossier `extern/ebuild/` pour les détails du système de build.

```bash
# Configuration (optionnelle, détecte les dépendances)
make configure

# Construction
make

# Installation (optionnel)
sudo make install

# Nettoyage
make clean
```

## Fonctionnalités prévues

1. **Lecture de configuration**: Récupérer la configuration courante d'un
   modèle YANG.
2. **Écriture de configuration**: Appliquer des modifications de
   configuration.
3. **Notifications en temps réel**: Recevoir et transmettre les notifications
   sysrepo (events, erreurs, changements d'état).
4. **Gestion des sessions**: Gérer les sessions sysrepo depuis l'agent IA.
5. **Authentification**: Vérifier les permissions de l'agent avant
   d'exécuter des opérations sensibles.

## Utilisation

```bash
./sysrepo-mcp-server [--config /path/to/config.json]
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
