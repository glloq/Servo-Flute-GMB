// LOT E - La publication du descripteur General-Midi-Boop est TOUT OU RIEN, et
// son echec degrade la decouverte, jamais le jeu.
//
// CE QUI A ETE REPRODUIT, en executant GmbSysExService, pas en le lisant :
//
//   1. setSnapshot() affectait `_snapshot` AVANT de reconstruire `_descriptor` :
//
//          _snapshot = snapshot;
//          _descriptor = std::shared_ptr<const std::string>(
//              new std::string(GmbDescriptor::toJson(_snapshot)));
//
//      Or ces deux membres forment UNE paire du point de vue du controleur : la
//      poignee de main (bloc 0x01) annonce `_snapshot.revision` et la taille de
//      `_descriptor`, et le bloc 0x10 sert `_descriptor`. Si le rendu echoue
//      entre les deux, la carte annonce la NOUVELLE revision et sert l'ANCIEN
//      document. Ce n'est pas une incoherence passagere : General-Midi-Boop met
//      le vieux document en cache sous le nouveau numero de revision, et comme
//      la revision ne bougera plus d'elle-meme, il ne le redemandera jamais.
//      Mesure sur le code d'avant, panne d'allocation armee sur la 4e
//      allocation de setSnapshot() :
//          revision annoncee par la poignee de main : 22
//          revision ecrite dans le document servi    : 11
//      Les deux devaient etre egales.
//
//   2. Le meme echec REDEMARRAIT la carte. Le firmware est compile AVEC les
//      exceptions - framework-arduinoespressif32 2.0.17,
//      tools/platformio-build-esp32.py :
//          CXXFLAGS = ["-Wno-frame-address", "-std=gnu++11", "-fexceptions",
//                      "-fno-rtti"]
//      au-dessus d'un ESP-IDF configure avec CONFIG_COMPILER_CXX_EXCEPTIONS=y
//      (tools/sdk/esp32/sdkconfig). Un std::string qui ne trouve pas de bloc
//      contigu sur un tas fragmente LEVE donc, il ne rend pas null - et
//      personne n'attrapait. L'exception remontait hors de loop(), ne trouvait
//      pas de gestionnaire, et abandonnait : reboot en pleine execution d'un
//      morceau, a cause d'un document de DECOUVERTE.
//
// Ce que ce fichier verrouille : quelle que soit l'allocation qui manque
// pendant une reconstruction, la carte continue de servir le DERNIER couple
// (instantane, descripteur) coherent, elle ne leve pas, elle ne redemarre pas,
// un transfert en cours n'est pas derange, et la reconstruction suivante
// republie normalement.
//
// Ce que ce fichier NE pretend PAS : rien n'a tourne sur une carte. Tout ce qui
// suit est execute sur hote (g++ -std=c++17), sur les sources de production.
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <string>
#include <vector>

#include "Arduino.h"
#include "ConfigStorage.h"
#include "gmb/Capabilities.h"
#include "gmb/GmbDescriptor.h"
#include "gmb/GmbSysEx.h"
#include "gmb/GmbSysExService.h"

using namespace gmb;

namespace {

// --- Panne d'allocation a la demande -----------------------------------------
//
// On remplace la fonction d'allocation globale LEVANTE - remplacable par la
// norme - plutot que d'ouvrir une trappe `#ifdef UNIT_TEST` dans le code de
// production (meme procede que test_harden_autocal.cpp, et meme raison).
//
// C'est bien `operator new(size_t)` et non la variante `std::nothrow` : cette
// derniere est DEJA remplacee par test_harden_autocal.cpp, qui est lie dans le
// meme binaire, et une seconde definition casserait l'edition de liens. Ce
// n'est pas une gene : leur remplacement delegue a `::operator new(sz)` dans un
// try/catch, donc il passe par celui-ci et rend nullptr quand on arme la panne
// ci-dessous. Armer ici, c'est donc armer les deux formes.
//
// C'est aussi la forme JUSTE pour ce qu'on simule : GmbDescriptor::toJson()
// construit une std::string, et std::string alloue par `operator new` levant.
// Mettre std::nothrow du cote de GmbSysExService ne pourrait rien y changer -
// la panne survient avant qu'un pointeur a nous existe.
long g_allocSeen = 0;      // allocations depuis l'armement
long g_allocFailAt = -1;   // -1 = ne jamais echouer
bool g_allocWatching = false;

void allocCountFrom() {
  g_allocSeen = 0;
  g_allocFailAt = -1;
  g_allocWatching = true;
}

void allocFailFrom(long nth) {
  g_allocSeen = 0;
  g_allocFailAt = nth;
  g_allocWatching = true;
}

long allocStop() {
  g_allocWatching = false;
  g_allocFailAt = -1;
  return g_allocSeen;
}

}  // namespace

void* operator new(std::size_t sz) {
  if (g_allocWatching) {
    g_allocSeen++;
    if (g_allocFailAt > 0 && g_allocSeen >= g_allocFailAt) throw std::bad_alloc();
  }
  if (sz == 0) sz = 1;
  void* p = std::malloc(sz);
  if (p == 0) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

// --- Lecteurs minimaux -------------------------------------------------------

// Vrai si le document ouvre et ferme exactement ce qu'il faut. Suffisant pour
// distinguer un document COMPLET d'un tronque, qui est tout ce qu'on cherche
// ici (le contenu, lui, est verrouille par test_gmb.cpp).
bool documentIsComplete(const std::string& j) {
  if (j.size() < 2 || j[0] != '{' || j[j.size() - 1] != '}') return false;
  if (j.compare(0, 20, "{\"gmb_descriptor\":2,") != 0) return false;
  int depth = 0;
  bool inStr = false;
  for (size_t i = 0; i < j.size(); i++) {
    const char c = j[i];
    if (inStr) {
      if (c == '\\') i++;
      else if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') inStr = true;
    else if (c == '{' || c == '[') depth++;
    else if (c == '}' || c == ']') depth--;
    if (depth < 0) return false;
  }
  return depth == 0 && !inStr;
}

// La revision ECRITE dans le document, telle que General-Midi-Boop la lira.
long documentRevision(const std::string& j) {
  const size_t p = j.find("\"revision\":");
  if (p == std::string::npos) return -1;
  return strtol(j.c_str() + p + 11, 0, 10);
}

// La revision ANNONCEE par la poignee de main, decodee comme le fait le
// controleur (5 octets a 7 bits, poids faible en tete).
uint32_t frameRevision(const std::vector<uint8_t>& m) {
  const uint8_t* b = &m[17];
  return ((uint32_t)(b[0] & 0x7F)) | ((uint32_t)(b[1] & 0x7F) << 7) |
         ((uint32_t)(b[2] & 0x7F) << 14) | ((uint32_t)(b[3] & 0x7F) << 21) |
         ((uint32_t)(b[4] & 0x0F) << 28);
}

uint32_t frameDescriptorSize(const std::vector<uint8_t>& m) {
  const uint8_t* b = &m[14];
  return ((uint32_t)(b[0] & 0x7F)) | ((uint32_t)(b[1] & 0x7F) << 7) |
         ((uint32_t)(b[2] & 0x7F) << 14);
}

std::vector<uint8_t> handshakeFrame() {
  return std::vector<uint8_t>{0xF0, 0x7D, 0x00, 0x01, 0x00, 0xF7};
}

std::vector<uint8_t> chunkFrame(uint16_t index) {
  return std::vector<uint8_t>{0xF0, 0x7D, 0x00, 0x10, 0x00,
                              (uint8_t)(index & 0x7F), (uint8_t)((index >> 7) & 0x7F), 0xF7};
}

// Les octets de document portes par une reponse 0x10 (en-tete de 9 octets, F7
// final).
std::string chunkText(const std::vector<uint8_t>& m) {
  std::string out;
  for (size_t k = 9; k + 1 < m.size(); k++) out.push_back((char)m[k]);
  return out;
}

// --- Fixture de configuration ------------------------------------------------
// Volontairement locale : `cfg` est global et partage avec les autres unites de
// test, donc chaque test part d'une configuration qu'il a pose lui-meme.
void finConfig(uint8_t count, uint8_t firstNote, uint8_t step) {
  memset(&cfg, 0, sizeof(cfg));
  cfg.numFingers = 6;
  cfg.numNotes = count;
  cfg.airflowPcaChannel = 10;
  cfg.fingerAngleOpen = 30;
  cfg.halfHolePercent = 50;
  strncpy(cfg.embouchure, "trav", sizeof(cfg.embouchure) - 1);
  for (int i = 0; i < 6; i++) {
    cfg.fingers[i].pcaChannel = (uint8_t)i;
    cfg.fingers[i].closedAngle = 90;
    cfg.fingers[i].direction = -1;
  }
  for (int i = 0; i < count; i++) {
    cfg.notes[i].midiNote = (uint8_t)(firstNote + i * step);
    cfg.notes[i].airflowMinPercent = 10;
    cfg.notes[i].airflowMaxPercent = 60;
    cfg.notes[i].airflowNominalPercent = 30;
    cfg.notes[i].anglePercent = 50;
  }
  cfg.midiChannel = 0;
  cfg.servoToSolenoidDelayMs = 105;
  cfg.minNoteIntervalForValveCloseMs = 50;
  cfg.minNoteDurationMs = 10;
  cfg.servoAirflowOff = 20;
  cfg.servoAirflowMin = 60;
  cfg.servoAirflowMax = 100;
  cfg.servoAngleOff = 90;
  cfg.servoAngleMin = 60;
  cfg.servoAngleMax = 120;
  cfg.vibratoFrequencyHz = 6.0f;
  cfg.vibratoMaxAmplitudeDeg = 8.0f;
  cfg.cc2Enabled = true;
  cfg.cc2SilenceThreshold = 10;
  cfg.cc2ResponseCurve = 1.4f;
  cfg.cc2TimeoutMs = 1000;
  cfg.solenoidPwmActivation = 255;
  cfg.solenoidPwmHolding = 128;
  cfg.solenoidActivationTimeMs = 50;
  cfg.airVelocityResponse = 60;
  cfg.airMode = AIR_MODE_SOLENOID_SERVO;
  cfg.valveType = 0;
  cfg.solenoidPin = SOLENOID_PIN;
  cfg.angleServoEnabled = false;
  cfg.numPumps = 1;
  strncpy(cfg.deviceName, "ServoFlute", sizeof(cfg.deviceName) - 1);
}

CapabilitySnapshot finSnapshot(uint32_t revision) {
  CapabilitySnapshot s = buildSnapshot(cfg, true);
  s.revision = revision;
  s.identity.instanceId = 0x5E11Au;
  return s;
}

// L'invariant que le controleur lit : ce qui est ANNONCE et ce qui est SERVI
// decrivent le meme document, et ce document est complet.
void assertPairIsCoherent(GmbSysExService& svc, uint32_t nowMs) {
  const std::string doc = svc.descriptorJson();
  assert(documentIsComplete(doc));
  assert(svc.descriptorSize() == (uint32_t)doc.size());

  std::vector<uint8_t> req = handshakeFrame();
  std::vector<uint8_t> hs = svc.handleMessage(req.data(), req.size(), nowMs);
  assert(hs.size() == 24);
  assert(frameDescriptorSize(hs) == (uint32_t)doc.size());
  // LE point : la revision annoncee est celle que porte le document servi.
  assert((long)frameRevision(hs) == documentRevision(doc));
  assert((long)svc.snapshot().revision == documentRevision(doc));
}

// ---------------------------------------------------------------------------
// 1. E-1.3 - une reconstruction qui echoue laisse le dernier couple valide
// ---------------------------------------------------------------------------
// On fait echouer, une par une, CHACUNE des allocations d'une reconstruction.
// Il n'y a donc pas d'etape privilegiee : ni la copie de l'instantane, ni le
// rendu JSON, ni la publication ne peuvent laisser le service a moitie migre.
void fin_gmb_failed_rebuild_keeps_the_last_valid_pair() {
  finConfig(30, 48, 3);
  const CapabilitySnapshot before = finSnapshot(11);
  finConfig(12, 60, 1);
  const CapabilitySnapshot after = finSnapshot(22);
  const std::string docBefore = GmbDescriptor::toJson(before);
  const std::string docAfter = GmbDescriptor::toJson(after);
  assert(docBefore != docAfter);
  assert(documentRevision(docBefore) == 11 && documentRevision(docAfter) == 22);

  // Combien d'allocations coute une reconstruction reussie ? C'est la borne du
  // balayage : au-dela, la panne ne tomberait plus dans setSnapshot().
  long total = 0;
  {
    GmbSysExService svc;
    svc.setSnapshot(before);
    allocCountFrom();
    svc.setSnapshot(after);
    total = allocStop();
    assert(svc.descriptorJson() == docAfter);   // le cas nominal, sans panne
  }
  assert(total > 0);

  int degraded = 0, escaped = 0;
  for (long k = 1; k <= total; k++) {
    GmbSysExService svc;
    svc.setSnapshot(before);
    assert(svc.descriptorJson() == docBefore);
    const uint32_t failuresBefore = svc.descriptorRebuildFailures();

    allocFailFrom(k);
    bool threw = false;
    try {
      svc.setSnapshot(after);
    } catch (...) {
      threw = true;
    }
    allocStop();
    if (threw) escaped++;

    // Quoi qu'il soit arrive, le service sert un document COMPLET, et ce
    // document est l'un des deux - jamais un melange, jamais un tronque.
    const std::string served = svc.descriptorJson();
    assert(served == docBefore || served == docAfter);
    assertPairIsCoherent(svc, 1000 + (uint32_t)k);

    if (served == docBefore) {
      degraded++;
      // Degrade veut dire : RIEN n'a bouge. L'ancien instantane est toujours la.
      assert(svc.snapshot().revision == 11u);
      assert(svc.descriptorRebuildFailures() == failuresBefore + 1u);
    } else {
      assert(svc.snapshot().revision == 22u);
      assert(svc.descriptorRebuildFailures() == failuresBefore);
    }
  }

  // Le balayage doit avoir REELLEMENT provoque des echecs, sinon il ne prouve
  // rien - c'est la contre-epreuve du dispositif de panne lui-meme.
  assert(degraded > 0);
  // Et aucun de ces echecs ne remonte au sequenceur : un descripteur qui ne se
  // construit pas ne doit pas faire abandonner la boucle principale.
  assert(escaped == 0);
}

// ---------------------------------------------------------------------------
// 2. Un transfert en cours n'est pas derange par une reconstruction qui echoue
// ---------------------------------------------------------------------------
// Verrou de non-regression : la propriete existait deja pour une reconstruction
// REUSSIE (test_gmb.cpp la couvre), il fallait qu'elle survive au nouveau
// chemin degrade - y compris a la poignee de main qui, elle, sait interrompre
// un transfert epingle sur un document perime.
void fin_gmb_transfer_in_flight_survives_a_failed_rebuild() {
  finConfig(30, 48, 3);
  const CapabilitySnapshot pinned = finSnapshot(7);
  finConfig(9, 70, 1);
  const CapabilitySnapshot other = finSnapshot(8);
  const std::string docPinned = GmbDescriptor::toJson(pinned);
  const std::string docOther = GmbDescriptor::toJson(other);
  assert(docPinned != docOther);

  GmbSysExService svc;
  svc.setSnapshot(pinned);
  const uint16_t total = GmbSysEx::chunkCount(docPinned.size());
  assert(total >= 3);

  uint32_t now = 2000;
  std::vector<uint8_t> req0 = chunkFrame(0);
  std::string rebuilt = chunkText(svc.handleMessage(req0.data(), req0.size(), now++));
  assert(!rebuilt.empty());
  assert(svc.transferInFlight());

  // L'utilisateur active une configuration pendant le transfert, et la
  // reconstruction manque de memoire au milieu.
  allocFailFrom(3);
  svc.setSnapshot(other);
  allocStop();
  assert(svc.descriptorRebuildFailures() == 1u);
  assert(svc.descriptorJson() == docPinned);
  assert(svc.transferInFlight());

  // Une poignee de main intercalee ne doit pas lacher l'epinglage : le document
  // epingle EST toujours le document courant.
  std::vector<uint8_t> hs = handshakeFrame();
  assert(svc.handleMessage(hs.data(), hs.size(), now++).size() == 24);
  assert(svc.transferInFlight());

  for (uint16_t i = 1; i < total; i++) {
    std::vector<uint8_t> req = chunkFrame(i);
    std::vector<uint8_t> m = svc.handleMessage(req.data(), req.size(), now++);
    assert(!m.empty());
    rebuilt += chunkText(m);
  }
  assert(rebuilt == docPinned);     // un seul document, reassemble exactement
  assert(!svc.transferInFlight());  // livre en entier : l'epinglage est rendu
}

// ---------------------------------------------------------------------------
// 3. descriptorJson() / descriptorSize() servent toujours un document
// ---------------------------------------------------------------------------
// Ces deux accesseurs dereferencent `_descriptor` sans le tester. Le defaut
// n'existe pas comme decrit - `_descriptor` n'est jamais nul - mais ce n'est
// une propriete que tant que personne ne casse les deux seuls endroits qui
// l'ecrivent. C'est cela qui est verrouille ici, dans tous les etats qu'un
// GmbSysExService peut prendre.
void fin_gmb_descriptor_is_always_servable() {
  finConfig(10, 60, 1);

  // (a) service neuf, aucun setSnapshot : le document de la section 5.1.
  GmbSysExService fresh;
  assert(documentIsComplete(fresh.descriptorJson()));
  assert(fresh.descriptorSize() == (uint32_t)fresh.descriptorJson().size());
  assert(fresh.descriptorSize() > 0);
  assert(fresh.descriptorJson().find("\"configured\":false") != std::string::npos);
  assertPairIsCoherent(fresh, 100);
  std::vector<uint8_t> req0 = chunkFrame(0);
  assert(!fresh.handleMessage(req0.data(), req0.size(), 101).empty());

  // (b) apres une reconstruction reussie, puis (c) apres une qui echoue.
  // Les deux instantanes sont construits AVANT d'armer la panne : les fabriquer
  // sous la panne ferait echouer buildSnapshot() au lieu de setSnapshot().
  const CapabilitySnapshot good = finSnapshot(5);
  const CapabilitySnapshot doomed = finSnapshot(6);
  fresh.setSnapshot(good);
  assertPairIsCoherent(fresh, 102);
  allocFailFrom(2);
  fresh.setSnapshot(doomed);
  allocStop();
  assert(fresh.descriptorRebuildFailures() == 1u);
  assertPairIsCoherent(fresh, 103);

  // (d) pendant un transfert epingle.
  assert(!fresh.handleMessage(req0.data(), req0.size(), 104).empty());
  assert(fresh.transferInFlight());
  assertPairIsCoherent(fresh, 105);

  // (e) un service dont le tout premier document ne peut pas etre construit
  //     n'existe pas : le constructeur leve au lieu de laisser derriere lui un
  //     GmbSysExService dont `_descriptor` serait nul. C'est ce qui rend
  //     l'invariant vrai pour de bon, et non seulement en pratique.
  bool constructed = false;
  allocFailFrom(1);
  try {
    GmbSysExService stillborn;
    constructed = true;
    (void)stillborn.descriptorSize();
  } catch (...) {
  }
  allocStop();
  assert(!constructed);
}

// ---------------------------------------------------------------------------
// 4. Le mode degrade n'est pas definitif
// ---------------------------------------------------------------------------
void fin_gmb_degraded_descriptor_is_not_permanent() {
  finConfig(20, 50, 2);
  const CapabilitySnapshot first = finSnapshot(30);
  finConfig(7, 64, 1);
  const CapabilitySnapshot second = finSnapshot(31);
  finConfig(25, 55, 2);
  const CapabilitySnapshot third = finSnapshot(32);
  const std::string docFirst = GmbDescriptor::toJson(first);
  const std::string docThird = GmbDescriptor::toJson(third);

  GmbSysExService svc;
  svc.setSnapshot(first);
  assert(svc.descriptorRebuildFailures() == 0u);

  allocFailFrom(2);
  svc.setSnapshot(second);
  allocStop();
  assert(svc.descriptorJson() == docFirst);
  assert(svc.snapshot().revision == 30u);
  assert(svc.descriptorRebuildFailures() == 1u);

  // La reconstruction SUIVANTE publie normalement : l'echec n'a rien laisse
  // derriere lui qui bloque la publication.
  svc.setSnapshot(third);
  assert(svc.descriptorJson() == docThird);
  assert(svc.snapshot().revision == 32u);
  assert(svc.descriptorRebuildFailures() == 1u);   // le compteur, lui, ne ment pas
  assertPairIsCoherent(svc, 4000);

  // Et le nouveau document est servable en entier sur le bloc 0x10.
  const uint16_t total = GmbSysEx::chunkCount(docThird.size());
  std::string rebuilt;
  uint32_t now = 4100;
  for (uint16_t i = 0; i < total; i++) {
    std::vector<uint8_t> req = chunkFrame(i);
    std::vector<uint8_t> m = svc.handleMessage(req.data(), req.size(), now++);
    assert(!m.empty());
    rebuilt += chunkText(m);
  }
  assert(rebuilt == docThird);
}

// ---------------------------------------------------------------------------
// 5. E-1.2 - une reconstruction identique ne republie pas
// ---------------------------------------------------------------------------
// GmbRuntime::onConfigurationActivated() republie l'instantane meme quand rien
// de ce qui est ANNONCE n'a change. Le rendu, lui, est inevitable (il faut
// comparer pour savoir), mais la publication ne l'est pas : le document reste
// le MEME objet, donc pas de seconde copie de ~800 octets, et l'epinglage d'un
// transfert en cours n'est pas lache pour un document qui n'a pas bouge.
void fin_gmb_identical_rebuild_reuses_the_published_document() {
  finConfig(30, 48, 3);
  const CapabilitySnapshot same = finSnapshot(77);
  finConfig(5, 62, 1);
  const CapabilitySnapshot changed = finSnapshot(78);

  GmbSysExService svc;
  svc.setSnapshot(changed);

  // Le MEME instantane est publie deux fois de suite : la seule difference
  // entre les deux mesures est la publication elle-meme, puisque le rendu et la
  // copie de l'instantane sont identiques mot pour mot.
  allocCountFrom();
  svc.setSnapshot(same);            // document different de `changed` : publie
  const long republishCost = allocStop();
  const std::string* published = &svc.descriptorJson();
  const std::string doc = svc.descriptorJson();

  allocCountFrom();
  svc.setSnapshot(same);            // document identique : rien a publier
  const long identicalCost = allocStop();
  assert(&svc.descriptorJson() == published);   // le meme objet, pas une copie
  assert(svc.descriptorJson() == doc);
  assert(svc.snapshot().revision == 77u);
  assert(identicalCost < republishCost);

  // Consequence observable : un transfert en cours traverse une activation qui
  // n'annonce rien de nouveau, poignee de main comprise.
  svc.setSnapshot(same);
  const std::string docSame = svc.descriptorJson();
  const uint16_t total = GmbSysEx::chunkCount(docSame.size());
  assert(total >= 3);
  uint32_t now = 6000;
  std::vector<uint8_t> req0 = chunkFrame(0);
  std::string rebuilt = chunkText(svc.handleMessage(req0.data(), req0.size(), now++));
  assert(svc.transferInFlight());
  svc.setSnapshot(same);
  std::vector<uint8_t> hs = handshakeFrame();
  assert(svc.handleMessage(hs.data(), hs.size(), now++).size() == 24);
  assert(svc.transferInFlight());
  for (uint16_t i = 1; i < total; i++) {
    std::vector<uint8_t> req = chunkFrame(i);
    std::vector<uint8_t> m = svc.handleMessage(req.data(), req.size(), now++);
    assert(!m.empty());
    rebuilt += chunkText(m);
  }
  assert(rebuilt == docSame);
}

// ---------------------------------------------------------------------------
// 6. Non-regression de contenu : meme entree -> meme JSON
// ---------------------------------------------------------------------------
// Le service publie EXACTEMENT ce que le serialiseur produit, octet pour octet,
// pour toute la variete de configurations que les tests de descripteur
// couvrent. Rien de ce qui precede (copie intermediaire, reutilisation du
// document identique, publication par deplacement) n'a le droit de modifier une
// seule virgule du document.
void fin_gmb_published_document_matches_the_renderer() {
  const int cases[][3] = {{4, 60, 1}, {13, 72, 1}, {30, 48, 3}, {1, 69, 1}, {60, 30, 1}};
  GmbSysExService svc;
  for (int i = 0; i < 5; i++) {
    finConfig((uint8_t)cases[i][0], (uint8_t)cases[i][1], (uint8_t)cases[i][2]);
    const CapabilitySnapshot s = finSnapshot((uint32_t)(100 + i));
    const std::string expected = GmbDescriptor::toJson(s);

    allocCountFrom();
    svc.setSnapshot(s);
    const long cost = allocStop();

    assert(svc.descriptorJson() == expected);
    assert(svc.descriptorSize() == (uint32_t)expected.size());
    assert(documentIsComplete(svc.descriptorJson()));
    assertPairIsCoherent(svc, (uint32_t)(7000 + i));
    // Borne de cout : une reconstruction mesuree ici coute 15 allocations
    // (11 pour GmbDescriptor::toJson, 3 pour la copie de l'instantane, 1 pour
    // la publication). La borne est large parce qu'elle depend de la
    // bibliotheque standard de l'hote ; elle sert a voir une derive d'ordre de
    // grandeur, pas a compter juste.
    assert(cost > 0 && cost <= 40);
  }

  // Un instantane non configure aussi : c'est le document que GMB recoit quand
  // la configuration active ne valide pas.
  finConfig(4, 60, 1);
  CapabilitySnapshot bare = buildSnapshot(cfg, false);
  bare.revision = 999;
  svc.setSnapshot(bare);
  assert(svc.descriptorJson() == GmbDescriptor::toJson(bare));
  assert(svc.descriptorJson().find("\"configured\":false") != std::string::npos);
  assertPairIsCoherent(svc, 7100);
}

}  // namespace

void fin_gmb_run_all_tests() {
  fin_gmb_failed_rebuild_keeps_the_last_valid_pair();
  fin_gmb_transfer_in_flight_survives_a_failed_rebuild();
  fin_gmb_descriptor_is_always_servable();
  fin_gmb_degraded_descriptor_is_not_permanent();
  fin_gmb_identical_rebuild_reuses_the_published_document();
  fin_gmb_published_document_matches_the_renderer();
}
