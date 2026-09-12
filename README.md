# sysrepo-mcp-server

Serveur MCP (Model Context Protocol) qui expose les fonctionnalités de
[sysrepo](https://github.com/sysrepo/sysrepo) à un agent IA. Il permet de
configurer un système via des données sysrepo (modèles YANG) et d'en
contrôler le statut en temps réel.

## Fonctionnalités

- **Configuration** : l'agent IA peut modifier la configuration du système
  via l'API sysrepo (modèles YANG).
- **Monitoring** : le serveur expose les status et événements du système
  (notifications sysrepo, états des interfaces, routes, ...).
- **Sécurité** : authentification et autorisation de l'agent avant les
  opérations sensibles ; journalisation de toutes les opérations.

## Architecture

```
┌─────────────┐      ┌──────────────┐      ┌─────────────┐
│   AI Agent  │◄────►│  MCP Server  │◄────►│   sysrepo   │
│ (OpenHands, │      │  (ce projet) │      │   Daemon    │
│  etc.)      │      └──────────────┘      └─────────────┘
└─────────────┘                                │
                                               ▼
                                      ┌──────────────┐
                                      │ YANG Models  │
                                      │(configuration)│
                                      └──────────────┘
```

- **libyang** : moteur YANG (parsing / validation des schémas).
- **sysrepo** : datastore de configuration NETCONF.
- **fcgi2** : bibliothèque FastCGI (`libfcgi`) pour le transport.
- **utils** / **stroll** : bibliothèques utilitaires eTux.
- **ebuild** : système de construction Makefile.

## Prérequis

Pour utiliser l'environnement de build :

- [Docker](https://www.docker.com/) (BuildKit est requis, activé par défaut
  sur les versions récentes).

Toutes les autres dépendances (compilateur, autotools, cmake, libconfig,
python3, lighttpd, ...) sont installées par l'image Docker. Les sources des
bibliothèques externes (`extern/`) sont téléchargées automatiquement.

## Build

Le build s'effectue depuis le dossier `docker/` :

```sh
# 1. Télécharge les sources extern/ (libyang, sysrepo, ebuild, stroll,
#    utils, fcgi2) puis construit l'image Docker
make -C docker build

# 2. Vérification de zéro : nettoie extern/ et reconstruit tout sans cache
make -C docker extern-clean
make -C docker build-nc
```

L'image `sysrepo-mcp-server:latest` est ensuite utilisée comme
*environnement de build* : elle contient toutes les dépendances compilées
(installées sous `/usr/local`), mais pas le projet lui-même. Le projet est
compilé à l'exécution, sur le répertoire monté en lecture/écriture.

### Cibles disponibles

| Cible | Description |
|-------|------------|
| `make -C docker extern`        | Télécharge les sources `extern/` (sans build). |
| `make -C docker build`         | Télécharge `extern/` + build de l'image Docker (avec cache). |
| `make -C docker build-nc`      | Idem, sans cache Docker. |
| `make -C docker extern-clean`  | Supprime `extern/` (les sources seront retéléchargées). |
| `make -C docker run`           | Ouvre un shell interactif dans le conteneur. |
| `make -C docker test`          | Build de l'image, puis `make test` du projet dans le conteneur. |

### Scripts d'aide

Les scripts du dossier `scripts/` s'exécutent *dans* l'image :

- `scripts/build.sh` : configure, compile et installe le projet
  (`build/sysrepo-mcp-server`).
- `scripts/test.sh` : lance `make test` (build + smoke test + suite pytest si
  présente).

## Utilisation

```sh
./build/sysrepo-mcp-server [options]

  --help          Aide
  --version       Version
  --config PATH   Fichier de configuration (format libconfig)
```

Le fichier de configuration de référence est
[`docker/config.cfg`](docker/config.cfg) (syntaxe [libconfig](http://www.cksystem.com/libconfig/)) :

```
server {
    host "0.0.0.0";
    port 8000;
    transport "tcp";            // "tcp" ou "unix"

    sysrepo {
        socket_path "/var/run/sysrepo/sysrepod.sock";
        username "sysrepo-mcp";
        connection_timeout 5000;
    };

    logging {
        level "info";           // emerg|alert|crit|err|warning|notice|info|debug
        use_syslog yes;
    };

    agent {
        api_key "";
        name "openhands-agent";
        allowed_modules = [ "ietf-interfaces", "ietf-routing" ];
    };
};
```

## Structure du projet

```
├── docker/                 # Environnement de build (Docker)
│   ├── Dockerfile          # Image de build (toutes les dépendances)
│   ├── Makefile            # Cibles build / build-nc / run / test / extern
│   └── config.cfg          # Exemple de configuration
├── extern/                 # Sources des dépendances (téléchargées, non versionnées)
│   ├── ebuild/             #   système de build
│   ├── utils/              #   utilitaires eTux
│   ├── stroll/             #   structures de données
│   ├── fcgi2/              #   FastCGI
│   ├── libyang/            #   moteur YANG
│   └── sysrepo/            #   datastore NETCONF
├── include/sysrepo/mcp/    # En-têtes publics
├── src/                    # Code source
├── scripts/                # Scripts build / test (exécutés dans le conteneur)
├── sphinx/                 # Documentation
├── Makefile                # Point d'entrée du build (ebuild)
└── ebuild.mk               # Déclaration du binaire
```

**Important** : le dossier `extern/` est un dossier de sources téléchargées.
Il ne fait pas partie du dépôt Git (voir `.gitignore`) et **ne doit jamais
être modifié**.

## Sécurité

- Les opérations de l'agent sont validées contre les permissions de
  `agent.allowed_modules`.
- Le transport MCP doit être protégé (TLS ou socket restreint).
- Chaque opération de configuration est journalisée.

## License

Ce projet est distribué sous les termes de la licence LGPL-3.0
(voir [COPYING.LESSER](COPYING.LESSER)) et la licence BSD 3-Clause
(voir [COPYING.txt](COPYING.txt)).
