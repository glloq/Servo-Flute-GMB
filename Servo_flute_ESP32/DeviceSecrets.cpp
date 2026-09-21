#include "DeviceSecrets.h"
#include "settings.h"

#include <Preferences.h>
// esp_random() vit dans esp_system.h sur ESP-IDF 4.x et dans esp_random.h a
// partir de la 4.4 ; les deux plateformes de la CI (espressif32 6.10 et 6.11)
// doivent compiler sans modification, d'ou l'inclusion conditionnelle.
#include <esp_system.h>
#if defined(__has_include)
#  if __has_include(<esp_random.h>)
#    include <esp_random.h>
#  endif
#endif

#include "Sha256.h"

namespace {

const char* kNamespace = "flutesec";
const char* kKeyApPass = "ap_pass";
const char* kKeyApGen = "ap_gen";
const char* kKeyAdmPlain = "adm_plain";   // present uniquement si auto-genere
const char* kKeyAdmSalt = "adm_salt";
const char* kKeyAdmHash = "adm_hash";

// Alphabet sans caracteres ambigus (0/O, 1/l/I) : le secret doit pouvoir etre
// relu sur un terminal serie et retape sans erreur.
const char kSecretAlphabet[] = "abcdefghijkmnpqrstuvwxyzACDEFGHJKLMNPQRSTUVWXYZ23456789";

bool g_loaded = false;
String g_apPassword;
bool g_apGenerated = true;
String g_adminPlain;      // vide si l'utilisateur a choisi son propre secret
String g_adminSalt;
String g_adminHash;

String toHex(const uint8_t* data, size_t len) {
  static const char* digits = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += digits[(data[i] >> 4) & 0x0F];
    out += digits[data[i] & 0x0F];
  }
  return out;
}

}  // namespace

uint32_t DeviceSecrets::randomWord() {
  // esp_random() est alimente par le bruit du sous-systeme RF quand le Wi-Fi ou
  // le BLE est actif ; il ne derive jamais du MAC.
  return esp_random();
}

String DeviceSecrets::randomSecret(uint8_t length) {
  const size_t alphabetSize = sizeof(kSecretAlphabet) - 1;
  String out;
  out.reserve(length);
  for (uint8_t i = 0; i < length; i++) {
    out += kSecretAlphabet[esp_random() % alphabetSize];
  }
  return out;
}

String DeviceSecrets::hashPassword(const String& password, const String& salt) {
  uint8_t digest[SHA256_DIGEST_SIZE];
  Sha256 ctx;
  ctx.update((const uint8_t*)salt.c_str(), salt.length());
  ctx.update((const uint8_t*)password.c_str(), password.length());
  ctx.finish(digest);
  // Iteration : rend une attaque par dictionnaire hors ligne nettement plus
  // couteuse si la NVS est un jour extraite.
  for (uint32_t i = 1; i < DEVICE_SECRET_HASH_ROUNDS; i++) {
    ctx.update(digest, sizeof(digest));
    ctx.update((const uint8_t*)salt.c_str(), salt.length());
    ctx.finish(digest);
  }
  return toHex(digest, sizeof(digest));
}

void DeviceSecrets::load() {
  if (g_loaded) return;
  g_loaded = true;

  Preferences prefs;
  bool haveNvs = prefs.begin(kNamespace, false);
  if (!haveNvs) {
    // NVS indisponible : on genere des secrets en RAM pour cette session. Le
    // hotspot reste chiffre et l'interface reste protegee, mais les secrets
    // changeront au prochain demarrage (signale sur le port serie).
    g_apPassword = randomSecret(DEVICE_SECRET_LENGTH);
    g_adminPlain = randomSecret(DEVICE_SECRET_LENGTH);
    g_adminSalt = randomSecret(16);
    g_adminHash = hashPassword(g_adminPlain, g_adminSalt);
    g_apGenerated = true;
    if (DEBUG) Serial.println("ERREUR: DeviceSecrets - NVS indisponible, secrets volatils");
    return;
  }

  g_apPassword = prefs.getString(kKeyApPass, "");
  g_apGenerated = prefs.getBool(kKeyApGen, true);
  if (g_apPassword.length() < 8) {
    g_apPassword = randomSecret(DEVICE_SECRET_LENGTH);
    g_apGenerated = true;
    prefs.putString(kKeyApPass, g_apPassword);
    prefs.putBool(kKeyApGen, true);
    if (DEBUG) Serial.println("DEBUG: DeviceSecrets - cle hotspot generee (premier demarrage)");
  }

  g_adminSalt = prefs.getString(kKeyAdmSalt, "");
  g_adminHash = prefs.getString(kKeyAdmHash, "");
  g_adminPlain = prefs.getString(kKeyAdmPlain, "");
  if (g_adminSalt.length() == 0 || g_adminHash.length() == 0) {
    g_adminPlain = randomSecret(DEVICE_SECRET_LENGTH);
    g_adminSalt = randomSecret(16);
    g_adminHash = hashPassword(g_adminPlain, g_adminSalt);
    prefs.putString(kKeyAdmPlain, g_adminPlain);
    prefs.putString(kKeyAdmSalt, g_adminSalt);
    prefs.putString(kKeyAdmHash, g_adminHash);
    if (DEBUG) Serial.println("DEBUG: DeviceSecrets - mot de passe admin genere (premier demarrage)");
  }
  prefs.end();
}

void DeviceSecrets::begin() { load(); }

String DeviceSecrets::apPassword() { load(); return g_apPassword; }
bool DeviceSecrets::apPasswordIsGenerated() { load(); return g_apGenerated; }

String DeviceSecrets::regenerateApPassword() {
  load();
  g_apPassword = randomSecret(DEVICE_SECRET_LENGTH);
  g_apGenerated = true;
  Preferences prefs;
  if (prefs.begin(kNamespace, false)) {
    prefs.putString(kKeyApPass, g_apPassword);
    prefs.putBool(kKeyApGen, true);
    prefs.end();
  }
  return g_apPassword;
}

void DeviceSecrets::persistAdmin(const String& plainOrEmpty, const String& salt, const String& hash) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  if (plainOrEmpty.length() > 0) {
    prefs.putString(kKeyAdmPlain, plainOrEmpty);
  } else {
    prefs.remove(kKeyAdmPlain);
  }
  prefs.putString(kKeyAdmSalt, salt);
  prefs.putString(kKeyAdmHash, hash);
  prefs.end();
}

bool DeviceSecrets::verifyAdminPassword(const String& candidate) {
  load();
  if (g_adminHash.length() == 0) return false;
  String computed = hashPassword(candidate, g_adminSalt);
  // Comparaison en temps constant sur l'empreinte hexadecimale.
  if (computed.length() != g_adminHash.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < computed.length(); i++) {
    diff |= (uint8_t)(computed[i] ^ g_adminHash[i]);
  }
  return diff == 0;
}

bool DeviceSecrets::setAdminPassword(const String& password) {
  load();
  if (password.length() < 8) return false;
  g_adminSalt = randomSecret(16);
  g_adminHash = hashPassword(password, g_adminSalt);
  g_adminPlain = "";   // secret choisi par l'utilisateur : jamais stocke en clair
  persistAdmin(g_adminPlain, g_adminSalt, g_adminHash);
  return true;
}

bool DeviceSecrets::adminPasswordIsGenerated() { load(); return g_adminPlain.length() > 0; }
String DeviceSecrets::generatedAdminPassword() { load(); return g_adminPlain; }

String DeviceSecrets::regenerateAdminPassword() {
  load();
  g_adminPlain = randomSecret(DEVICE_SECRET_LENGTH);
  g_adminSalt = randomSecret(16);
  g_adminHash = hashPassword(g_adminPlain, g_adminSalt);
  persistAdmin(g_adminPlain, g_adminSalt, g_adminHash);
  return g_adminPlain;
}

void DeviceSecrets::printToSerial() {
  load();
  // Affiche meme hors DEBUG : sur un appareil headless, le port serie est le
  // seul canal de premiere configuration.
  Serial.println("----------------------------------------");
  Serial.println(" SERVO FLUTE - ACCES");
  Serial.print(" Hotspot SSID      : ");
  Serial.println(AP_SSID);
  Serial.print(" Hotspot WPA2      : ");
  Serial.println(g_apPassword);
  if (g_adminPlain.length() > 0) {
    Serial.print(" Mot de passe web  : ");
    Serial.println(g_adminPlain);
    Serial.println(" (genere automatiquement - modifiable depuis l'interface)");
  } else {
    Serial.println(" Mot de passe web  : defini par l'utilisateur");
  }
  Serial.println(" Maintenir BOOT au demarrage regenere ces secrets.");
  Serial.println("----------------------------------------");
}
