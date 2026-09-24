# Passe de durcissement — rapport de validation

Branche `claude/serene-allen-loflan`, repartie de `main` = `1eab8ef`.
Dix commits, 38 fichiers touchés. Suite pytest : **105 → 121**.
Dernière CI complète vérifiée : run #88 sur `55b2d92`, **cinq jobs au vert**,
les deux builds ESP32 compris. Les commits postérieurs ne touchent que la
documentation.

Ce document dit ce qui a ete corrige, comment chaque defaut a ete REPRODUIT
avant d'etre touche, et — ce qui compte autant — ce qui ne l'a pas ete et ce
que cette passe ne prouve pas.

## Regle appliquee a chaque ligne du tableau

Aucun defaut n'a ete corrige sur la foi de son enonce. Pour chacun : ecrire
d'abord le test qui ECHOUE sur le code courant, corriger, puis REINTRODUIRE le
defaut et constater que le test redevient rouge. Un correctif dont le test ne
sait pas redevenir rouge ne protege rien — c'est ainsi qu'on trouve les tests
vides, et cette passe en a trouve.

Un point du brief ne s'est pas confirme. Il est signale comme tel : le
rapporter a la meme valeur que corriger le reste.

## Tableau de validation

| ID | Gravite | Defaut | Reproduction | Correction | Test ajoute | Resultat |
|---|---|---|---|---|---|---|
| **H-1** | P0 | `resetToDefaults()` et `factoryReset()` ecrivaient le `cfg` global, qui decrit le materiel REELLEMENT initialise. `factoryReset()` le detruisait **sans rien persister** : jusqu'au reboot, l'instrument pilotait du vrai materiel sur une configuration d'usine arbitraire | `harden_storage_defaults_never_touch_the_active_config` : `memcmp` sur les 5132 octets → abort (exit 134) | `makeDefaultConfig(RuntimeConfig&)` remplit la structure qu'on lui donne ; les deux resets preparent un candidat, le persistent, et l'activation se fait au redemarrage | `test_harden_storage.cpp` (12 tests) + verrou textuel sur les deux chemins de `ConfigStorage.cpp` | **Corrige**, 2 mutations tuees |
| **H-2** | P0 | Corollaire trouve en corrigeant H-1 : un `.tmp` orphelin ne faisait pas echouer `isFirstBoot()`, il le faisait **mentir**. Le boot promeut un `.tmp` quand le fichier final manque — donc le demarrage suivant un reset usine RESSUSCITAIT la configuration effacee | Lecture du chemin de boot, puis test de `isFirstBoot()` avec residus | `factoryReset()` efface `.tmp` et `.bak` ; `isFirstBoot()` repond « aucune configuration RECUPERABLE » | idem H-1 | **Corrige** |
| **H-3** | P1 | `ConfigCommit` prenait un verrou puis ecrivait `active` **quand meme** si le verrou etait refuse, avec un simple avertissement `config_lock_timeout`. Or le verrou n'echoue que si une autre tache tient la configuration : ecrire alors lui livre 5132 octets a moitie recopies | `harden_commit_lock_refused_leaves_active_bit_identical` : `memcmp` → echec. 5 des 9 tests rouges avant | Sortie anticipee : pas un octet ecrit, `activated=false`, `unlock()` jamais appele sur un verrou non pris, `restartRequired=true` | `test_harden_commit.cpp` (9 tests) | **Corrige**, 4 mutations tuees |
| **H-4** | P1 | La sauvegarde dite atomique faisait `remove(final)` puis `rename(tmp,final)` : un rename rate detruisait **les deux copies**, et la recuperation du `.tmp` au boot ne servait a rien puisqu'il venait d'etre efface | `harden_storage_failed_promotion_keeps_a_readable_config` : « fichiers restants = 0 » → abort | Sequence a trois temps avec `.bak` : le fichier vivant est RENOMME, jamais supprime, et restaure si la promotion echoue. Logique extraite dans `ConfigPersist.{h,cpp}`, pure, avec operations de FS injectees — c'est ce qui la rend testable | idem H-1, avec un faux systeme de fichiers en memoire | **Corrige**, 3 mutations tuees |
| **H-5** | P1 | Sous-defaut trouve **par son propre test** : une nouvelle tentative de sauvegarde apres un echec effacait le `.bak`, alors seule copie de l'ancienne configuration | Partie 3 du test ci-dessus, rouge sur la premiere version du correctif | Le `.bak` n'est libere que si la configuration finale est presente | idem | **Corrige** |
| **H-6** | P1 | `volatile bool` utilise comme primitive de synchronisation pour deux demandes inter-taches. Le defaut concret : `if (flag) { flag = false; ... }` n'est pas atomique — une demande posee entre la lecture et l'effacement est perdue (un CC121 Reset All Controllers disparait sans trace) | **Echec de COMPILATION** : aucune primitive de prise atomique n'existait. C'est la limite honnete d'un test hote, et elle est dite | `portMUX_TYPE` dedie + `take...()` qui lisent et effacent en une section critique, sur le modele du `takePanicRequest()` deja en place | `test_harden_tasks.cpp` (8 points d'entree) | **Corrige** — contrat verrouille, **course NON rejouee** (voir Limites) |
| **H-7** | P1 | `AutoCalibrator` ecrivait la configuration active champ par champ (384 ecritures) avant de persister : un echec a mi-parcours la laissait ni dans l'ancien etat ni dans le nouveau | 20 assertions rouges reparties sur 8 tests | Candidat sur le tas → validation → persistance → activation par le meme commit transactionnel que la voie web | `test_harden_autocal.cpp` (13 tests) | **Corrige**, 4 mutations tuees |
| **H-8** | — | **Point du brief NON CONFIRME.** Plusieurs sites signales comme des ecritures de `cfg` pendant le balayage de calibration sont des **lectures**. Verifie a l'execution, par comparaison de toute la structure a chaque tour de `update()` (>50 points) : la machine a etats n'ecrivait deja jamais `cfg` | — | Aucune. Les deux tests correspondants sont conserves comme VERROUS, pas presentes comme des reproductions | idem H-7 | **Pas un defaut** |
| **H-9** | P1 | `WEBOP_WIFI_CONNECT` ne lisait que `res.saved`. Apres H-3, un verrou refuse donne `saved && !activated` : le code repondait « Connecting… » et laissait `cfg` porter les anciens identifiants. Le prochain `POST /api/config`, qui construit son candidat depuis `cfg`, aurait **reecrit les anciens identifiants en flash** — perte de donnees silencieuse | Lecture du chemin apres integration de H-3 | Le chemin distingue `saved` de `activated` et programme le redemarrage controle | `test_p1_wifi_commit_never_treats_saved_as_activated` (source), mutation verifiee | **Corrige** |
| **H-10** | P1 | Les deux `apply*` du calibrateur prennent des parametres **optionnels** : les oublier compile sans un avertissement, et le commit remplacerait alors la configuration active **hors verrou** — H-3 contourne par un defaut silencieux | Revue du diff d'integration | Les deux appels passent le garde ; le cas `saved && !applied` declenche le reboot controle | `test_p1_autocal_apply_commits_under_the_configuration_lock` | **Corrige** |
| **H-11** | P2 | Allocations suivies d'une gestion d'echec soignee mais **INATTEIGNABLE** : sur ESP32 un `new` ordinaire qui echoue ne rend pas nullptr, il abandonne. Cas le plus net : `new RuntimeConfig(cfg)` suivi de `if (candidatePtr == nullptr) { … 500 … }` — protection ecrite, relue en revue, morte | Lecture : le seul chemin pouvant produire le nul tuait le programme avant | `std::nothrow` sur les 8 allocations concernees ; les modes degrades existaient deja et deviennent joignables | `test_harden_memory.cpp` + regle de source `test_p2_no_bare_new_in_firmware_sources` | **Corrige**, mutation verifiee |
| **H-12** | P2 | Trouve pendant le balayage : `lockConfig()` rendait `true` quand le mutex valait nullptr. Vrai avant `begin()` ; **faux** si sa creation a echoue faute de tas, cas ou les taches AsyncTCP tournent. Le verrou annoncait alors un succes a `ConfigCommit` sans rien proteger | Lecture du code, distinction des deux cas | Les deux situations sont distinguees ; l'echec est **ferme** | `test_p2_config_lock_fails_closed…`, mutation verifiee | **Corrige** |
| **H-13** | P2 | `MidiFilePlayer::begin()` ecrasait `_events` sans le liberer : 16 Ko de fuite au deuxieme appel | Ecrit en redigeant son propre test | `begin()` idempotente, et rend desormais si elle a pu allouer | `test_harden_memory.cpp` | **Corrige** |
| **H-14** | P2 | `processCommands()` drainait la file ENTIEREMENT a chaque `update()` : sous rafale WebSocket, une passe pouvait enchainer des dizaines de transactions I2C et affamer `loop()` | `command_work_is_bounded_per_update_and_nothing_is_lost` : 24 commandes appliquees en une passe → echec | Travail borne par passe, commandes DIFFEREES et jamais perdues, panic hors borne, garde anti-famine sur les Note Off | `test_harden_tasks.cpp` | **Corrige**, 3 mutations tuees |
| **H-15** | P2 | `new` nu dans les deux files : allocation ratee → dereferencement nul | `a_queue_without_storage_refuses_instead_of_crashing` : la file clampee a 1 acceptait la commande | Etat degrade unique et observable (`storageAvailable()`, `queues_ok`). Le panic et les Note Off survivent : ils ne vivent pas dans le tableau alloue | idem H-14 | **Corrige**, mutation retirant la garde → **SIGFPE**, soit le plantage evite |
| **H-16** | — | **Deux tests DEFINIS mais jamais appeles** vivaient dans le depot depuis des semaines. Ils compilaient, se relisaient, ne protegeaient rien | Trouves a la main au cycle precedent, en comparant 61 definitions a la liste des appels | Detection automatisee des deux morts : debranchement et vacuite | `tests/test_suite_integrity.py` (6 tests, chacun prouve par mutation) | **Automatise**. Zero test mort ou vide aujourd'hui |
| **H-17** | P1 | **Le firmware ne compile pas dans le dialecte que le depot demande.** `platformio.ini` demande `gnu++17` ; le builder Arduino ajoute `-std=gnu++11` APRES, et gcc retient le dernier. Les deux builds hote sont en C++17 : toute une famille d'erreurs leur est invisible | **Echec reel du build ESP32 en CI** : un agregat ayant recu des initialiseurs de membre par defaut n'est plus un agregat en C++11 | Construction par defaut au lieu de la liste positionnelle. Et surtout : verification locale du dialecte reel | `tests/test_firmware_dialect.py` — mutation faite avec l'erreur REELLE, reproduite a l'identique en local | **Corrige**. Deuxieme occurrence du meme piege dans ce depot |

## Ce que cette passe NE prouve PAS

- **Rien n'a tourne sur un ESP32 avec des peripheriques physiques.** Les 77
  lignes de `HARDWARE_TEST_MATRIX.md` restent `NOT TESTED — requires hardware`,
  et une garde de CI interdit desormais d'en changer une sans inscrire quand et
  sur quel firmware l'essai a eu lieu.
- **La course de H-6 n'a pas ete rejouee.** Un test hote est mono-tache, et
  `portENTER_CRITICAL` est un no-op dans le stub. Ce qui est verrouille est le
  CONTRAT qui rend la course impossible, pas son absence.
- **Trois fichiers ne sont compilables sur aucun build hote** —
  `ConfigStorage.cpp`, `WebConfigurator.cpp`, le sketch — parce qu'ils
  dependent d'ArduinoJson, d'ESPAsyncWebServer ou du framework Arduino. Ils ont
  ete **RELUS**, jamais compiles en local ; seul le build ESP32 de la CI les
  compile. C'est precisement la ou H-17 s'etait cache.
- **Les trois risques electriques de la fenetre de reset** (sens du tirage de
  `/OE`, GPIO13 en pull-up faible au reset, largeurs d'impulsion reelles)
  restent hors de portee du logiciel : aucun code ne s'execute pendant cette
  fenetre. Voir [BRINGUP.md](BRINGUP.md), etape 0.

## Niveaux de verification atteints

| Element | Niveau |
|---|---|
| Sources de production compilables sur hote | **executees** (pytest, 121 passed) et **compilees en gnu++11**, le dialecte reel |
| `ConfigStorage.cpp`, `WebConfigurator.cpp`, sketch | **relus**, compiles uniquement par le build ESP32 de la CI |
| Build ESP32 (espressif32 6.10.0 et 6.11.0) | **compile en CI** |
| Materiel | **rien** |

## Ce qui n'a pas ete fait, et pourquoi

- **Protection de branche sur `main`** : recommandee, non appliquee. Modifier
  les regles administratives du depot demande une autorisation explicite. La
  recommandation : exiger les 5 jobs de `firmware-ci.yml` avant fusion.
- **`takePendingNoteOff()` balaie 128 bits sous section critique.** Examine :
  quelques centaines de nanosecondes, pas d'appel imbrique. Un `__builtin_ctz`
  serait plus court mais reecrirait une fonction correcte et deja verrouillee.
  Signale, non modifie.
- **La relecture du fichier final apres promotion** (H-4) : une corruption
  survenant pendant le `rename` ne serait pas detectee. Couteux en flash,
  hors perimetre.
- **Les lectures de `cfg` sans verrou par la machine a etats de calibration** :
  elles s'executent dans la tache `loop()`, la meme qui execute le commit, donc
  elles ne peuvent pas se dechirer contre lui.
