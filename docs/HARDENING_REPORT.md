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
  lignes que `HARDWARE_TEST_MATRIX.md` comptait a la fin de CETTE passe (80
  depuis la suivante) sont restees `NOT TESTED — requires hardware`, et une
  garde de CI interdit desormais d'en changer une sans inscrire quand et sur
  quel firmware l'essai a eu lieu.
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

---

# Passe de finalisation — rapport de validation

Suite de la passe ci-dessus, apres fusion de la PR #95. Branche
`claude/serene-allen-loflan`, repartie de `main` = `78f76f6`.
Suite pytest : **121 → 122**. Le compte bouge peu parce que l'essentiel des
ajouts sont des points d'entree C++ a l'interieur du binaire comportemental,
pas des tests pytest : `test_fin_actuators.cpp`, `test_fin_storage.cpp`,
`test_fin_web.cpp`, `test_fin_midi.cpp`, `test_fin_gmb.cpp` et
`test_fin_serial.cpp` sont nouveaux.

La meme regle s'applique : reproduire d'abord, corriger ensuite, puis
reintroduire le defaut et constater que le test redevient rouge. Deux points du
brief ne se sont pas confirmes et sont rapportes comme tels.

## Tableau de validation

| ID | Gravite | Defaut | Reproduction | Correction | Test ajoute | Resultat |
|---|---|---|---|---|---|---|
| **F-1** | **P0** | Un ordre d'arret d'actionneur partait par l'anneau de commandes borne : `push()` rendait `false` sur anneau plein, la couche web ignorait cette valeur, et `endTestSession(false)` desarmait dans la foulee le filet `TEST_SESSION_MAX_MS`. Une pompe pouvait rester alimentee a sa consigne **sans aucune limite de temps** | `pump_stop_reaches_the_pump_even_when_the_ring_is_full` : le PWM reellement ecrit sur la broche reste non nul apres l'arret | Les ordres qui RETIRENT de l'energie quittent l'anneau (drapeau dedie, prise indivisible, hors borne par passe, purge de ce qui realimenterait la cible). `endTestSession()` demande la mise en securite AVANT d'effacer les drapeaux | `test_fin_actuators.cpp` — 12 des 16 tests rouges sur le code d'avant | **Corrige**, 11 mutations tuees |
| **F-2** | P1 | Tous les Note Off partaient dans le bitmap non perdable, applique APRES l'anneau : un `NOTE_OFF 60` suivi d'un `NOTE_ON 60` avant le tour suivant s'appliquait a l'envers et laissait la note **muette** | `a_note_off_followed_by_a_note_on_leaves_the_note_sounding` | Le Note Off emprunte l'anneau comme tout le monde ; le bitmap ne sert que de repli au REFUS — ce que le commentaire justifiant l'ordre d'application supposait deja, a tort | idem F-1 | **Corrige** |
| **F-3** | P1 | `configRecoverOnBoot()` promouvait le `.tmp` avant le `.bak`. Or `bak + tmp, pas de live` est exactement l'etat que laisse une sauvegarde ayant rendu **FALSE** : l'utilisateur avait recu une erreur, et le demarrage suivant appliquait quand meme la configuration refusee — qui peut decrire un autre cablage que celui monte | Faux systeme de fichiers en memoire place dans cet etat exact | Le `.bak` prime : il n'existe que parce qu'une configuration COMMITEE y a ete deplacee. Le `.tmp` n'est promu qu'en son absence | `test_fin_storage.cpp` | **Corrige** |
| **F-4** | P1 | `factoryReset()` supprimait la configuration vivante EN PREMIER. Un residu impossible a effacer faisait rendre `false` **alors qu'elle etait deja detruite** : erreur rendue ET configuration perdue | Injection de panne a chaque etape | Ordre inverse — residus d'abord, abandon au premier refus avec le live INTACT — et le succes n'est annonce qu'apres constat par `exists()`, jamais sur la valeur rendue par `remove()` | idem F-3 | **Corrige** |
| **F-5** | P1 | `configChangeRequiresRestart()` etait une liste ecrite a la main : **sept** champs manquaient, dont `endstopPin`, `endstopActiveHigh` et `hallPin`, tous trois passes a `pinMode()` dans `PressureController::begin()`. Changer la broche de fin de course a chaud laissait l'ANCIENNE configuree en entree et la NOUVELLE jamais initialisee, sans demande de redemarrage : le regulateur lisait une broche flottante | Un cas par champ manquant | Table centrale `ConfigTopology`, parcourue en boucle ; `configChangeRequiresRestart()` n'en est plus que le relais. Couverture **bidirectionnelle** : une entree ajoutee sans test echoue, une entree retiree aussi | idem F-3, + 29 champs eprouves comme NE devant PAS rebooter | **Corrige** |
| **F-6** | P1 | Le candidat de `POST /api/config` etait `new RuntimeConfig(cfg)` : 5132 octets recopies depuis la tache AsyncTCP pendant que `loop()` peut remplacer `cfg` | Lecture du chemin, puis extraction du module | Instantane PRIS SOUS VERROU (`ConfigSnapshot`, pur, verrou injecte) ; verrou refuse → `503 config_busy`, code deja existant | `test_fin_web.cpp` | **Corrige** |
| **F-7** | P1 | `/api/wifi/status` lisait `cfg.wifiSsid`, un `char[33]`, sans verrou. Une recopie partielle peut ne contenir **aucun `\0`**, et le serialiseur JSON lirait hors du tableau | idem | Copie bornee sous verrou qui pose le terminateur ; le pire cas devient un SSID tronque, signale par `ssid_busy` | idem F-6 | **Corrige** |
| **F-8** | P1 | `volatile` employe comme primitive de synchronisation dans `WebConfigurator`, et un hand-off AsyncTCP↔`loop()` ou l'abandon par l'appelant et la publication du resultat pouvaient se croiser — laissant l'emplacement occupe | idem | `volatile` disparait du fichier ; machine a etats explicite (IDLE/ARMED/RUNNING/DONE), abandon et publication tranches dans LA MEME section critique | idem F-6 | **Corrige** — contrat verrouille, **course non rejouee** |
| **F-9** | P1 | `releaseUploadLock()` etait appelee **inconditionnellement** apres le delai de 3 s, alors que `loop()` pouvait etre EN TRAIN d'executer la finalisation. AsyncTCP remettait a vide des `String` que `loop()` lisait : lecture apres liberation, declenchable par un simple timeout d'upload | idem | La liberation respecte l'etat de la finalisation | idem F-6 | **Corrige** |
| **F-10** | P1 | `_testNoteMidi` et `_testNoteOffTime` etaient lus puis effaces en deux temps : un `test_note` recu entre les deux eteignait la **mauvaise** note et effacait l'echeance de la nouvelle — note laissee a sonner, air ouvert, bornee seulement par les 30 s de session | idem | La note et son echeance sont posees et prises comme un COUPLE indivisible | idem F-6 | **Corrige** |
| **F-11** | P1 | Trouve en corrigeant F-1 : `pump_stop` ne transmettait **pas** l'index de pompe. Le routage par index recevait donc toujours 0 — demander l'arret de la pompe 2 arretait la pompe 0 | Lecture du handler apres integration de F-1 | L'index est transmis | idem F-6 | **Corrige** |
| **F-12** | P1 | Le remplacement d'un fichier MIDI existant n'etait pas transactionnel : un echec en cours de route pouvait laisser l'instrument sans aucun des deux fichiers | Injection de panne a chaque etape sur un faux systeme de fichiers | `FileTransaction` (pur, operations injectees) : `dest→bak`, `src→dest`, restauration si l'installation echoue, `bak` efface seulement apres succes confirme. Le `.bak` vit hors de `MIDI_DIR`, donc hors listing et hors quota. Un balayage au demarrage repare une installation coupee | `test_fin_midi.cpp` | **Corrige** |
| **F-13** | P2 | `MidiFilePlayer::update()` traitait tous les evenements echus d'un seul tour (jusqu'a 2000) | Rafale dense, observee de bout en bout sur un vrai `InstrumentManager` | Borne = `EVENT_QUEUE_SIZE` (16), parce que l'aval reel est la file d'evenements et qu'au-dela le travail supplementaire **detruit** des evenements deja emis dans la meme passe. `stop()`, `pause()` et les CC 120-127 restent hors borne | idem F-12 — aucune assertion n'utilise la constante, la mutation qui la porte a 32 les fait tomber | **Corrige**, 5 mutations tuees |
| **F-14** | P2 | `GmbSysExService::setSnapshot()` publiait l'instantane PUIS reconstruisait le document. Sur les 15 allocations d'une reconstruction, 15 faisaient annoncer une revision que le document servi ne portait pas — et GMB met alors le vieux document en cache sous le nouveau numero, sans jamais le redemander. Les 15 levaient aussi hors de `setSnapshot()`, donc **rebootaient la carte** pour un descripteur de decouverte | Balayage de CHACUNE des 15 allocations, avec contre-epreuve qu'au moins une panne s'est produite | Publication tout-ou-rien : construire d'abord, publier ensuite. Echec → l'ancien couple reste en place et un compteur s'incremente | `test_fin_gmb.cpp` | **Corrige**, 4 mutations tuees |
| **F-15** | P2 | Trouve en balayant les `while` des sources de production plutot qu'en suivant une liste : `while (_serial->available())` n'etait borne par rien. Au debit MIDI nominal l'UART ne peut pas alimenter la boucle plus vite qu'elle ne la vide — mais la broche RX est **configurable**, et flottante ou cablee sur un signal rapide elle produit des octets d'erreur de trame en continu : `loop()` ne revient plus | Alimentation continue du stub UART | Borne de 64 octets par passe. Le decodeur etant A ETAT, un message a cheval sur deux passes reste reconnu — verifie **avant** d'ecrire la borne | `test_fin_serial.cpp`. Le module entre du meme coup dans le build hote : il n'avait aucune couverture | **Corrige** |
| **F-16** | P1 | Residu de F-1, repris et corrige : `pump_target` / `fan_target` avec `v = 0` restaient perdables. Or les curseurs de l'interface ne passent **pas** par `pump_stop` / `fan_stop` pour ramener un actionneur a zero. Anneau plein = l'intention de couper est jetee. La borne par passe introduite par la passe precedente rapproche ce cas : 24 emplacements s'ecoulent par tranches de 6 | `a_zero_pump_target_is_not_lost_when_the_ring_is_full` : `postCommand()` rend `false` | Deux bits DISTINCTS de ceux des arrets durs, et le consommateur applique **la commande d'origine** : `stop()` n'est pas `setTargetPercent(0)` (il termine aussi le test mono-pompe et saute la rampe du ventilateur). Le comportement observable ne change pas, seule la perte disparait | 4 points d'entree dans `test_fin_actuators.cpp` | **Corrige**, 5 mutations tuees — la cinquieme seulement apres avoir RENFORCE le test |
| **F-17** | P2 | Trois reponses WebSocket emises par le firmware et **jetees en silence** par le navigateur : `stop_escalated` (un arret escalade en panic — securite atteinte, mais pas par le chemin demande, et tout le reste coupe avec), `noise` (succes ou refus d'une capture, avec sa raison) et `mic_reset`. Plus `descriptorRebuildFailures()`, incremente par le service GMB et lu par personne | Listes extraites des deux sources et confrontees | Les trois branchements ; le compteur expose dans `/api/status` et `/api/diagnostics`, ou il devient le controle `gmb_descriptor` | `test_every_websocket_reply_has_a_handler_in_the_ui` — les deux listes DERIVEES de leur source et confrontees **dans les deux sens**, donc rien qui puisse se perimer | **Corrige**, 3 mutations tuees |
| **F-18** | — | **Deux points du brief NON CONFIRMES**, sur `GmbSysExService` : `descriptorJson()`/`descriptorSize()` ne peuvent pas dereferencer un pointeur nul (le constructeur publie un document avant tout, et une garde de nullite y serait inatteignable — le reproche meme que ces passes font aux protections mortes) ; et reutiliser le tampon du document en place aurait transforme une fenetre benigne en **document dechire**, la route HTTP s'executant sur AsyncTCP | — | Aucune | — | **Pas des defauts** |
| **F-19** | — | **Residu de F-1 laisse ouvert, verifie puis ecarte** : `ACMD_SET_ACTUATOR_SESSION` avec `a == 0` reste perdable. Verification : cette commande n'a **aucun producteur** — les six sites appellent `setActuatorSessionActive()` en direct, tous sur `loop()`. Son seul effet materiel est de toute facon garde a la source (`powerOnServos()` refuse tant que `_hardwareInitStatus != HW_INIT_OK`) | Rien a reproduire | Aucune : ajouter un canal imperdable pour un chemin que personne n'emprunte serait du mecanisme sans defaut | — | **Latent, non vivant** |

## Ce que cette passe NE prouve PAS

Tout ce qui est ecrit dans la section homonyme de la passe precedente reste
vrai, et en particulier :

- **Rien n'a tourne sur un ESP32 avec des peripheriques physiques.** Les 80
  lignes de `HARDWARE_TEST_MATRIX.md` restent integralement
  `NOT TESTED — requires hardware` : cette passe en AJOUTE neuf au lieu d'en
  valider une seule.
- **Les courses inter-taches ne sont pas rejouees.** Un test hote est
  mono-tache et `portENTER_CRITICAL` y est un no-op. Ce qui est verrouille est
  le contrat qui rend la course impossible.
- **`WebConfigurator.cpp` et `web_content.h` ne sont compilables par aucun
  build hote.** Les corrections F-6 a F-11 et F-17 y ont ete **RELUES**, jamais
  compilees en local ; seul le build ESP32 de la CI les compile. C'est pourquoi
  les tests correspondants portent sur des modules EXTRAITS (`ConfigSnapshot`,
  `WebOpChannel`, `FileTransaction`) plutot que sur le fichier lui-meme, et
  pourquoi la distinction test comportemental / garde de source est ecrite dans
  `test_fin_web.cpp`.
- **F-14 est le seul changement qu'un build hote ne peut pas valider du tout** :
  son `try`/`catch` ne compile que parce que le builder Arduino ajoute
  `-fexceptions`. Si les exceptions venaient a etre coupees, il cesserait de
  compiler — bruyamment, et uniquement sur ESP32. Le build ESP32 de la CI est
  ce qui tient cette affirmation honnete.

---

# Passe de finalisation 2 — rapport de validation

Derniere passe logicielle avant le bring-up materiel. Repartie de `main` =
`330c429`. Suite pytest : **122 → 126**. Quatre defauts de jonction, tous
REPRODUITS avant correction, plus trois trouves en chemin.

Le perimetre est volontairement etroit : fermer les jonctions, prouver les
corrections, garder la CI verte. Aucune fonction ajoutee, aucun module qui
marche reecrit.

## Tableau de validation

| ID | Gravite | Defaut | Reproduction | Correction | Test ajoute | Resultat |
|---|---|---|---|---|---|---|
| **G-1** | P1 | `setActuatorSessionActive(true)` arretait le sequenceur et vidait la file d'EVENEMENTS MIDI, mais laissait intacte la file de COMMANDES d'actionneurs. Une commande web deposee par AsyncTCP et pas encore consommee par `update()` survivait a la prise de possession et s'appliquait PENDANT la mesure - angle de doigt, consigne de pompe, ouverture de valve - sur un instrument que le calibrateur croit posseder seul | `a_command_queued_before_the_session_never_reaches_the_actuators` : `ACMD_PUMP_TARGET 80` mis en file puis session prise → la consigne finit appliquee | Deux barrieres : **purge** a la transition (`_commands.clear()`, qui conserve arrets et panic) et **garde centrale** dans `applyCommand()`, sur la tache proprietaire des actionneurs | `test_fin2_session.cpp`, 14 points d'entree verifiant le PWM reellement ecrit | **Corrige**, 7 mutations tuees |
| **G-2** | P1 | Corollaire de G-1 : une garde ecrite sur `commandDrivesActuators()` aurait bloque `pump_target v=0`, qui "pilote un actionneur" mais est le canal IMPERDABLE par lequel l'interface COUPE une pompe. Bloquer une coupure pendant une calibration est l'inverse du but | Mutation M3 : la garde naive fait echouer `a_zero_setpoint_is_a_safety_order_and_is_never_blocked` | Predicat distinct `commandMayEnergizeActuator()`. Les 21 valeurs de `ActuatorCommandType` classees une par une, deux classements argumentes sur place (`PUMP_SINGLE_TEST` confisque la regulation meme a 0 % ; `RESET_CONTROLLERS` peut AUGMENTER le souffle) | idem G-1 | **Corrige** |
| **G-3** | P1 | `cancelActiveActuatorSession()`, appelee au demarrage d'une calibration, ne touche pas `_testActive` / `_testStartTime`. Une session de test manuel ouverte juste avant survivait, et son plafond `TEST_SESSION_MAX_MS` echoyait EN PLEINE MESURE : `endTestSession(true)` demandait alors un panic qui coupait la calibration | Chaine tracee de bout en bout dans le code ; le verdict est reproduit dans `CalibrationGate` | REFUSER, pas voler : `{"t":"acal_error","msg":"manual_test_active"}`. Effacer `_testActive` desarmerait le filet sans mise en securite - le defaut corrige a la passe precedente | `test_fin2_calgate.cpp`, 5 points d'entree + 2 gardes de source | **Corrige**, 7 mutations tuees |
| **G-4** | P2 | `serviceWsOps()` drainait la file ENTIEREMENT (`while (true)`). Six places, mais pas six operations equivalentes : `WEBOP_MIC_RESET` comporte un `delay(100)` et jusqu'a ~500 ms d'attente I2S. Six de cette famille retenaient `loop()` pres du plafond du chien de garde et repoussaient d'autant `InstrumentManager::update()` - l'endroit ou un ARRET ou un PANIC atteint les actionneurs | Six operations en file, une passe les executait toutes | UNE operation par passe. La latence ajoutee est bornee par la PLUS LONGUE operation, non par leur somme : ~600 ms au lieu de ~3,6 s | `test_fin2_wsops.cpp`, 8 points d'entree + garde de source | **Corrige**, 6 mutations tuees |
| **G-5** | P1 | `LittleFS.format()` efface ~1,9 Mo, bloquant : `loop()` ne tourne pas, `esp_task_wdt_reset()` n'est pas appele, et le chien de garde (4 s, `trigger_panic`) redemarrait la carte EN PLEIN EFFACEMENT. C'est le chemin de recuperation d'une carte vierge - celui du premier bring-up | Limitation deja documentee dans BRINGUP.md ; la sequence corrigee est reproduite avec primitives injectees | Sequence explicite : securite materiel (dont **/OE HIGH**, qu'`allSoundOff()` ne faisait pas) → suspension du chien de garde pour la TACHE COURANTE → formatage → restauration. Plafond NON allonge, chien de garde NON desinitialise | `test_fin2_format.cpp`, 8 points d'entree dont un balayage des 16 combinaisons d'echecs | **Corrige**, 7 mutations tuees |
| **G-6** | P3 | Trouve par la garde ajoutee pour G-3 : le chemin `acal_error` de l'interface affichait `d.msg` BRUT. La table `acalErrText()` existait mais n'etait pas consultee la, si bien que `no_microphone` - anterieur a cette passe - s'affichait tel quel a l'operateur. Meme famille que les trois codes du range finder trouves a l'audit precedent | La garde echoue sur `['no_microphone']` | Le chemin passe par la table (qui fait `M[e]||e`, donc un message en clair la traverse inchange) ; la garde verifie les DEUX bouts - chaque code a un libelle, ET ce chemin le consulte | garde `test_every_calibration_start_error_code_has_a_label_in_the_ui` | **Corrige** |
| **G-7** | — | **Defaut dans mon propre test**, corrige avant commit : les assertions de bornage etaient ecrites avec `WS_OP_MAX_PER_PASS`, donc elles auraient toutes passe si quelqu'un remontait la borne a 6 - c'est-a-dire s'il supprimait le bornage | Mutation M2 du lot 3 | Deux assertions reecrites SANS la constante (`first.size() < 6`, `passes >= 2`), et la garde de source exige une borne strictement inferieure a la capacite | idem G-4 | **Corrige** |

## Audit des courses de calibration

Demande explicitement : verifier que les decisions de securite finales ne
reposent jamais sur une lecture non synchronisee faite par AsyncTCP. Resultat,
apres inventaire des appelants de `_autoCalOwnerClientId`,
`_autoCal->isRunning()`, `isRangeFinderComplete()` et `isCalibrationActive()` :

| Lecture depuis AsyncTCP | Ce qu'elle decide | Barriere finale |
|---|---|---|
| `processWsMessage` / `actuatorCommandBlockedDuringCalibration` | refuser une commande d'actionneur (`calibration_active`) | **la garde centrale de `applyCommand()`** (G-1), sur la tache proprietaire. Une lecture perimee ne peut que laisser passer la commande jusqu'a la file, ou elle est refusee |
| `rejectIfCalibrationActive` | refuser une ecriture de configuration | le **verrou de configuration**, pris sur `loop()` par le commit transactionnel |
| `onWsEvent` (deconnexion du proprietaire) | annuler la calibration, mettre en securite | `requestCalibrationCancel()` + `requestPanic()`, deux **drapeaux imperdables** consommes par `loop()` |
| `processWsMessage` (`"stop"`) | annuler plutot qu'allSoundOff | idem. Une lecture perimee dans un sens n'arrete pas le lecteur MIDI, dans l'autre stoppe un lecteur deja en pause : aucune consequence materielle |
| `handleApiDiagnostics` | afficher | rien a decider |

**Aucune refonte.** Ce n'est pas que la course est impossible : c'est qu'aucune
de ces lectures ne porte plus la decision finale. C'est la correction G-1 qui
rend ce constat vrai - avant elle, le filtrage AsyncTCP etait bel et bien la
seule chose qui empechait une commande web de bouger un actionneur pendant une
mesure.

## Warnings de compilation — documente, non corrige

Les builds ESP32 sont verts mais emettent des avertissements ArduinoJson 7 sur
`containsKey()`, deprecie. **118 sites d'appel**, tous dans
`ConfigStorage.cpp` et `WebConfigurator.cpp` - c'est-a-dire dans les deux
fichiers qu'AUCUN build hote ne compile.

Le remplacement n'est pas mecanique : `containsKey("k")` est VRAI pour une cle
presente valant `null`, la ou `!doc["k"].isNull()` est faux. Le parseur de
configuration s'en sert justement pour distinguer "champ absent" de "champ
fourni", y compris sur des cles heritees. Reecrire 118 sites dans des fichiers
qu'on ne peut compiler qu'en CI, juste avant le bring-up, est exactement le
genre de modification que ce moment interdit.

**Laisse pour une PR separee**, avec la verification cle par cle que le cas
`null` ne change pas de sens.

## Ce que cette passe NE prouve PAS

- **Rien n'a tourne sur un ESP32 avec des peripheriques.** Les 84 lignes de
  `HARDWARE_TEST_MATRIX.md` restent integralement
  `NOT TESTED — requires hardware` ; cette passe en AJOUTE quatre.
- **G-5 n'est pas valide au banc.** Un test hote ne formate aucune flash et n'a
  pas de chien de garde. `TaskWatchdog.cpp` n'entre dans AUCUN build hote - il
  inclut `esp_task_wdt.h`, et un faux en-tete ne prouverait rien. Seuls les deux
  builds ESP32 attestent que ces appels existent et sont bien types. L'essai
  `FIN-FS-WDT` reste a faire.
- **Les courses inter-taches ne sont toujours pas rejouees** : un test hote est
  mono-tache et `portENTER_CRITICAL` y est un no-op. Ce qui est verrouille est
  le contrat.
- **`WebConfigurator.cpp` et `web_content.h` ne sont compilables par aucun build
  hote.** Les cablages de G-1, G-3, G-4 et G-5 y ont ete RELUS et sont
  verrouilles par des gardes de source ; seul le build ESP32 de la CI les
  compile.
