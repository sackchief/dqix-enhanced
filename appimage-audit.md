# Audit AppImage Linux — DQIX Enhanced

Date de mise à jour : 2026-10-23  
Cible : Linux x86_64  
Branche d'évolution GUI : feat/appimage-gui-builder

## Décision de packaging

Le script tools/build-game.sh exige une ROM .nds fournie par l'utilisateur, extrait son code et place le résultat compilé dans game/. L'AppImage distribuée est donc un **constructeur Linux portable avec interface graphique**, et non le jeu précompilé.

L'AppImage embarque les sources du dépôt qui sont nécessaires à la construction et une toolchain Linux packagée. Elle ne contient aucune ROM, aucun code généré depuis une ROM, ni aucun binaire de jeu. Le build complet reste local dans le dossier workspace choisi par l'utilisateur.

## Interface utilisateur

L'AppImage s'ouvre par défaut sur une fenêtre **DQIX Enhanced — Constructeur Linux**.

- **Fichier ROM (.nds)** : bouton de sélection qui ouvre une boîte de dialogue de fichier, filtrée sur les ROM Nintendo DS.
- **Dossier workspace** : bouton de sélection de dossier et champ modifiable permettant de choisir l'endroit où garder les sources de travail, les résultats du build, le jeu et les sauvegardes. Le workspace peut aussi être saisi directement s'il n'existe pas encore.
- **Construire** : prépare/synchronise les sources dans le workspace choisi et exécute tools/build-game.sh dans un worker, sans bloquer la fenêtre.
- **Barre de progression** : avance selon les grandes étapes détectées dans la sortie du script (validation, extraction, recompilation, compilation, assemblage). Ce sont des jalons estimés : le recompiler et Make ne publient pas de pourcentage d'avancement commun. Pendant les étapes longues de recompilation et de compilation, la barre passe temporairement en mode animé indéterminé, puis revient aux jalons déterminés.
- **Journal du build** : affiche en direct stdout/stderr afin de voir les diagnostics de compilation.
- **Lancer le jeu** : devient disponible après un build réussi (ou lorsqu'un jeu préexistant est trouvé dans le workspace).

En cas de build en échec, la fenêtre reste ouverte, réactive le bouton Construire et signale l'erreur tout en laissant le journal consultable. Le marqueur .dqix-enhanced-appimage-workspace identifie un workspace géré par cette AppImage. Pour éviter une écriture inattendue, un workspace non vide sans ce marqueur demande confirmation avant de mettre à jour ses fichiers source.

## Dépendances packagées

| Dépendance | Utilité | Stratégie |
|---|---|---|
| Python 3.11 | UI, extraction et recompiler | Version contrainte et incluse dans la toolchain Conda. |
| Tk/Tcl | Fenêtre, sélecteur de fichier, sélecteur de dossier, widgets et barre de progression | Runtime Tk versionné dans l'environnement. Requiert une session graphique hôte. |
| PyYAML | Métadonnées importées par le recompiler | Version contrainte, incluse. |
| GNU Make | Orchestration de build.mk | Version contrainte, incluse. |
| GCC Linux x86_64 et wrappers | Compilation du C généré et runtime | Toolchain incluse, avec environnement sysroot contraint. |
| Sysroot Linux | En-têtes et bibliothèques de compilation ciblés | Packagé avec l'environnement de compilateur. |
| SDL2, en-têtes et sdl2-config | Fenêtre, audio, entrées et frontend | Version contrainte, incluse pour la compilation et le runtime. |
| pkg-config | Recherche des métadonnées des bibliothèques | Inclus. |
| curl | Téléchargement optionnel du message DQVC au build | Inclus ; l'échec du téléchargement facultatif est non fatal dans le script actuel. |
| third_party/libslirp | Pile réseau intégrée | Compilée à partir des sources du dépôt, plutôt que dépendre d'une libslirp système. |
| Clang, lld et LLVM | Tests du recompiler | Présents dans le job CI, non nécessaires dans l'AppImage runtime. |
| Pilote OpenGL, serveur de fenêtre, audio et noyau | Exécution et rendu graphique | Restent dépendances de l'hôte et ne doivent pas être copiés aveuglément dans l'AppImage. |

Les versions directes de l'environnement sont précisées dans packaging/linux/environment.yml. setup-micromamba cache l'environnement et les paquets téléchargés selon le fichier de configuration ; conda-pack crée ensuite une archive relocalisable. Au premier lancement, AppRun la décompresse dans le cache utilisateur, vérifie son SHA-256, exécute conda-unpack et initialise les scripts d'activation.

Le cache améliore les lancements suivants. Ce mécanisme reproduit l'environnement effectivement livré dans l'artefact ; sans lockfile complet des dépendances transitives, une résolution Conda effectuée à une date ultérieure ne garantit pas nécessairement un binaire bit-à-bit identique.

## Parcours de lancement

1. Sans argument, AppRun prépare la toolchain puis lance packaging/linux/gui.py.
2. L'utilisateur choisit sa ROM et son workspace, puis clique sur **Construire**.
3. Le constructeur synchronise les sources et lance tools/build-game.sh en tâche de fond.
4. L'interface affiche les lignes du build et avance la barre de progression sur les jalons annoncés.
5. À la fin, l'utilisateur peut lancer le jeu compilé depuis l'interface.

Le workspace par défaut est $XDG_DATA_HOME/dqix-enhanced-linux/workspace (ou ~/.local/share/dqix-enhanced-linux/workspace si XDG_DATA_HOME n'est pas défini). L'utilisateur peut choisir un autre dossier. Le workspace contient la ROM copiée, le code extrait, les sources locales, game/, les sauvegardes et les réglages : il doit rester privé.

Pour les scripts ou l'automatisation, le mode CLI reste disponible : --build-only ROM.nds, --rebuild [ROM.nds], ROM.nds pour construire puis lancer, --self-test pour les vérifications et --help pour le rappel de commande.

## CI, artefact et publication

Le workflow .github/workflows/linux-ci-appimage.yml :

1. exécute les contrôles Python/shell et les tests sémantiques sans ROM ;
2. crée l'environnement versionné sur Ubuntu 22.04 et le packe avec conda-pack ;
3. met en cache l'environnement et appimagetool épinglé par SHA-256 ;
4. assemble l'AppDir contenant AppRun, le fichier desktop, l'icône générique, les sources publiques, gui.py et l'archive de toolchain ;
5. crée l'AppImage et l'exécute avec --self-test, en mode extraction sans FUSE ;
6. publie l'AppImage, SHA256SUMS et build-info.txt comme artefact CI.

Le workflow manuel .github/workflows/release-appimage.yml récupère un artefact de run CI approuvé, vérifie ses checksums et crée une Release sous un nouveau tag. Les ROMs, sauvegardes, dossiers de jeu et fichiers générés ne doivent jamais être ajoutés à cet artefact.

Le test --self-test valide les imports Python/Tk, la syntaxe de l'interface, le compilateur, Make, SDL2 et un petit programme de liaison SDL2. En environnement CI sans affichage, il n'ouvre pas de fenêtre. Le test d'intégration visuelle et le build du vrai jeu nécessitent encore un test manuel sur Linux avec une ROM personnelle.

## Compatibilité et limites

- L'artefact est x86_64 ; une cible AArch64 nécessitera son propre job, environnement et artefact.
- La compilation de l'AppImage utilise Ubuntu 22.04, tandis que la CI de test du recompiler utilise Ubuntu 24.04. La glibc et certains composants de l'hôte imposent encore des limites de compatibilité.
- Les sélecteurs Tk nécessitent une session graphique X11 ou Wayland compatible. Sans affichage, utiliser --self-test ou les modes CLI.
- La progression du build est basée sur les étapes textuelles du script : elle n'indique pas le pourcentage exact du travail restant au sein d'une longue étape.
- Enhanced requiert un pilote OpenGL 3.3 compatible selon docs/TECHNICAL.md.
- Le premier build peut être long et utilise de l'espace disque local.
- L'environnement AppImage n'inclut pas une ROM ni le jeu construit.

## Contrôles avant publication

- Attendre la réussite du test et du packaging AppImage dans GitHub Actions.
- Vérifier l'exécution de --self-test, les checksums et l'absence de ROMs dans l'artefact.
- Sur une session graphique Linux, lancer l'AppImage sans arguments et tester les deux sélecteurs, le bouton Construire, la progression et le journal.
- Construire à partir d'une ROM locale et tester le démarrage, le son, les contrôles, le rendu et les sauvegardes.
- Essayer un workspace vide et un workspace existant ; confirmer que les erreurs de build sont lisibles et n'arrêtent pas l'interface.
