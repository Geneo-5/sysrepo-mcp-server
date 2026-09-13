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

## Construction

La construction se fait **sur un système où les dépendances sont déjà
installées** (voir [Prérequis](#prérequis)). Le workflow standard est :

```sh
make config     # génère build/.config + build/config.h (Kconfig menuconfig)
make            # compile le binaire build/sysrepo-mcp-server
make install    # installe /usr/local/bin/sysrepo-mcp-server
```

`make config` ouvre l'interface `menuconfig` du système eBuild pour
configurer le projet (adresse d'écoute, port, chemin du socket sysrepo,
journalisation ...) ; les valeurs par défaut sont définies dans `config.in`.
Il est aussi possible de générer une configuration sans interface
interactive avec `make defconfig`.

### Prérequis

Le système de build doit fournir :

- eBuild (le système de construction — présent dans `extern/ebuild/` ou
  `/usr/share/ebuild/`),
- GCC (8+) ou Clang,
- les bibliothèques, compilées et installées :
  **libyang** (≥ 5.8), **sysrepo** (≥ 5.1), **fcgi2** (`libfcgi`),
  **stroll**, **utils** (eTux), **libconfig**,
- `pkg-config` et `kconfig-frontends` (pour `make config`).

Ces dépendances peuvent être obtenues deux façons :

1. **Compiler à la main** les sources de `extern/` sur le système hôte
   (`make -C docker extern` télécharge les sources ; chaque bibliothèque a
   sa commande de build respective) ;
2. **Réutiliser l'image Docker de build** ([docker/](docker/README.md)) :
   elle contient toutes les dépendances installées, et sert de
   *système où les libs sont installées* pour CI et agents IA :

   ```sh
   make -C docker build    # télécharge extern/ + construit l'image
   make -C docker run      # shell interactif : les libs sont toutes là
   ```

Pour CI et agents IA, le workflow complet (build, test, run) est décrit
dans [docker/README.md](docker/README.md).

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

## Documentation

La documentation HTML (guide d'installation, architecture du pont MCP ↔
sysrepo, API) est générée avec Sphinx :

```sh
make doc         # génère doc/ (HTML)
```

## Structure du projet

```
├── docker/                 # Environnement de build Docker (CI / agents IA)
│   ├── Dockerfile          #   image de build (toutes les dépendances)
│   ├── Makefile            #   cibles build / build-nc / run / test / extern
│   ├── README.md           #   doc du workflow Docker
│   └── config.cfg          #   exemple de configuration
├── extern/                 # Sources des dépendances (téléchargées, non versionnées)
│   ├── ebuild/             #   système de build
│   ├── utils/              #   utilitaires eTux
│   ├── stroll/             #   structures de données
│   ├── fcgi2/              #   FastCGI
│   ├── libyang/            #   moteur YANG
│   └── sysrepo/            #   datastore NETCONF
├── include/sysrepo/mcp/    # En-têtes publics
├── src/                    # Code source
├── sphinx/                 # Sources de la documentation (Sphinx)
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
