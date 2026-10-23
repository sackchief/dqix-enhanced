# Audit des fichiers — DQIX Enhanced

Date de mise à jour : 2026-10-23  
Base fusionnée examinée : main à 1920a22f70e81872e0ebb67391b58498236a8629  
Branche de l’évolution GUI : feat/appimage-gui-builder

## Résumé

DQIX Enhanced recompile en code natif le programme d'une ROM Dragon Quest IX fournie par l'utilisateur. tools/build-game.sh extrait la ROM, produit du C à l'aide de recomp/, compile le runtime SDL et assemble un répertoire game/. Une CI sans ROM peut vérifier les sources, fabriquer l'AppImage constructeur et tester la toolchain, mais elle ne doit ni précompiler ni publier le jeu issu d'une ROM personnelle.

L'AppImage est un **constructeur Linux x86_64**, pas un binaire de jeu précompilé. Son démarrage par défaut ouvre une interface graphique pour choisir le fichier ROM, le dossier de travail et démarrer la compilation locale.

## Arborescence et rôle des composants

| Chemin | Rôle | Considérations d’audit |
|---|---|---|
| README.md, ROADMAP.md | Présentation, contrôles et état du projet | La documentation générale reste orientée vers le build Windows ; les instructions AppImage spécialisées se trouvent dans appimage-audit.md. |
| build.mk | Compilation des modules générés, runtime et bibliothèque dqixcore.so | Nécessite un compilateur C, Make, SDL2 et ses en-têtes. |
| tools/build-game.sh | Pipeline de build Linux à partir d'une ROM | Écrit build/ et game/; copie la ROM fournie vers game/data/dqix.nds. |
| tools/extract_rom.py | Extraction du code ARM9, ARM7 et overlays | Les données extraites proviennent de la ROM et restent dans le workspace local. |
| recomp/build.py, recomp/arm7.py, recomp/insn.py | Recompilation ARM vers du C | recomp/build.py dépend de PyYAML. Le code généré depuis la ROM ne doit pas être publié. |
| runtime/ | Console DS, rendu, son, contrôles, réseau et sessions | Le frontend SDL et le rendu OpenGL sont utilisés sous Linux. |
| third_party/libslirp/ | Pile réseau utilisateur incluse | Compilée depuis le code du dépôt ; respecter la licence BSD de cette dépendance. |
| config/usa/arm9/ | Symboles et relocations de référence | Ne remplace pas la ROM nécessaire au build complet. |
| skins/, extras/, wfc/ | Habillages et données facultatives | La publication doit exclure les ressources de jeu sensibles ajoutées localement. |
| tests/semantics/run.py | Tests sémantiques du recompiler | S'exécute sans ROM avec Clang/LLVM, GCC et PyYAML. |
| tests/gpu3d/t3d.c, tests/verify_decode.py | Banc de test 3D et vérification du décodeur | Le second dépend de modules extraits ; ce ne sont pas des tests de lancement du jeu complet. |
| packaging/linux/AppRun | Point d'entrée de l'AppImage | Valide le checksum de la toolchain, la décompacte une fois dans le cache utilisateur, configure l'environnement et ouvre l'interface graphique par défaut. Garde un mode CLI et --self-test. |
| packaging/linux/gui.py | Interface de construction Tk | Sélecteurs de fichier ROM et dossier workspace, bouton Build, journal de compilation, barre de progression par étapes et bouton pour lancer le jeu construit. |
| packaging/linux/environment.yml | Dépendances de la toolchain | Versions contraintes de Python, PyYAML, Tk/Tcl, Make, GCC, sysroot, SDL2, pkg-config, curl, conda-pack et setuptools. |
| packaging/linux/dqix-enhanced-builder.desktop | Intégration bureau Linux | Terminal=false permet d'ouvrir la GUI sans fenêtre de terminal. |
| packaging/linux/dqix-enhanced-builder.svg | Icône neutre du constructeur | N'utilise pas d'icône extraite d'une ROM. |
| .github/workflows/linux-ci-appimage.yml | CI et création d'artefact | Tests sans ROM, cache Conda et appimagetool, packaging, --self-test, sommes SHA-256 et artefact. |
| .github/workflows/release-appimage.yml | Publication manuelle | Valide le run source et ses sommes de contrôle avant de créer une Release. |
| audit-files.md, appimage-audit.md | Documentation d'audit | Doivent rester synchronisés avec les étapes de lancement et de packaging décrites ici. |
| .gitignore | Exclusion des ROMs, fichiers générés, objets et données locales | Ne pas neutraliser ses exclusions pour faciliter un build CI. |

## Flux de build Linux avec interface graphique

1. L'utilisateur lance l'AppImage ; AppRun vérifie l'archive de toolchain et l'extrait dans $XDG_CACHE_HOME/dqix-enhanced-appimage/toolchain (ou le cache utilisateur par défaut).
2. L'interface Tk affiche un sélecteur de fichier .nds et un champ de workspace doté d'un sélecteur de dossier.
3. Quand l'utilisateur clique sur **Construire**, le constructeur copie/met à jour les sources dans le workspace choisi. Les dossiers générés et les données locales tels que build/, game/, ROMs et sauvegardes sont préservés durant la synchronisation.
4. L'interface exécute tools/build-game.sh en tâche de fond, affiche le journal et met à jour la barre de progression à partir des étapes que le script annonce.
5. Après réussite, le bouton **Lancer le jeu** démarre game/play.sh dans ce workspace.

La barre de progression exprime des **jalons de pipeline**, pas un pourcentage précis du compilateur : les étapes de recompilation et de compilation n'exposent pas actuellement une progression interne détaillée. La barre s'anime durant ces étapes longues, puis revient à une progression déterminée au jalon suivant. Le journal reste visible pour diagnostiquer les échecs.

## Risques et règles de distribution

- Le build complet dépend d'une ROM locale : aucun workflow public ne doit recevoir ou publier cette ROM, le code extrait, game/ ni les binaires issus de cette ROM.
- Le workspace sélectionné peut contenir une copie de la ROM, le jeu, les sauvegardes, les paramètres et les fichiers générés. Ne pas publier ou partager ce dossier.
- L'utilisateur doit choisir un workspace dédié. Pour les dossiers non vides qui n'ont pas le marqueur du constructeur, la GUI demande confirmation avant d'écrire ou de mettre à jour les sources.
- Tk/Tcl est inclus dans l'environnement Conda de l'AppImage ; l'affichage lui-même a toujours besoin d'une session graphique Linux fonctionnelle. Les pilotes noyau, serveur de fenêtres, audio et pilotes GPU restent fournis par l'hôte.
- Le mode Enhanced demande un pilote OpenGL compatible, notamment OpenGL 3.3 selon docs/TECHNICAL.md.
- Le test CI --self-test vérifie l'import Tk sans ouvrir une fenêtre, les dépendances et un petit lien SDL2. Il ne peut pas automatiser le choix interactif de fichiers ni garantir un gameplay fonctionnel sans écran graphique et ROM.

## Vérifications après cette évolution

- Exécuter la CI et confirmer le smoke-test de l'AppImage mise à jour.
- Sur un bureau Linux, ouvrir l'AppImage sans arguments et vérifier le sélecteur .nds, le sélecteur de dossier, le bouton Build, la progression, le journal et la gestion d'erreur.
- Effectuer un build complet avec une ROM dumpée légalement ; vérifier le lancement du jeu, le son, les entrées et les sauvegardes.
- Tester un workspace vierge, un workspace existant avec marqueur et un dossier non vide sans marqueur.
- Contrôler que les archives d'artefact et les releases ne contiennent aucun .nds, .sav, code C généré depuis la ROM ou dossier game/.
