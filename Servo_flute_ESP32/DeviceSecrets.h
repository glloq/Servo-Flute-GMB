/***********************************************************************************************
 * DeviceSecrets - Secrets persistants de l'appareil (NVS / Preferences)
 *
 * POURQUOI
 * --------
 * 1. Le mot de passe du hotspot etait derive des 24 bits bas du MAC du chip
 *    ("flute-%06X"). Le MAC n'est pas un secret : il est diffuse en clair dans
 *    chaque trame 802.11 et le BSSID de l'AP l'expose directement. La cle WPA2
 *    etait donc devinable par quiconque voyait le reseau, et l'espace de
 *    recherche se reduisait a rien.
 * 2. L'interface web n'avait aucune authentification.
 *
 * Ce module genere, au PREMIER demarrage, deux vrais secrets aleatoires tires du
 * generateur materiel de l'ESP32 (esp_random(), alimente par le bruit RF), puis
 * les conserve en NVS :
 *   - la cle WPA2 du hotspot (>= 12 caracteres, WPA2 exige >= 8),
 *   - le mot de passe administrateur de l'interface web.
 *
 * Les deux sont affichables sur le port serie (appareil headless) et
 * regenerables volontairement. Ni le MAC ni le BSSID n'entrent dans leur calcul.
 *
 * Le mot de passe administrateur choisi par l'utilisateur n'est PAS stocke en
 * clair : seule une empreinte salee (SHA-256, iteree) est conservee. Un mot de
 * passe auto-genere est en revanche conserve en clair tant qu'il n'a pas ete
 * remplace, afin de pouvoir etre reaffiche sur le port serie.
 ***********************************************************************************************/
#ifndef DEVICE_SECRETS_H
#define DEVICE_SECRETS_H

#include <Arduino.h>

// Longueur des secrets generes (caracteres d'un alphabet sans ambiguite).
#define DEVICE_SECRET_LENGTH 14
// Iterations de derivation du mot de passe administrateur.
#define DEVICE_SECRET_HASH_ROUNDS 2000

class DeviceSecrets {
public:
  // Charge les secrets depuis NVS et en genere si necessaire (premier boot).
  static void begin();

  // --- Hotspot ---
  static String apPassword();
  static bool apPasswordIsGenerated();
  static String regenerateApPassword();

  // --- Interface web ---
  static bool verifyAdminPassword(const String& candidate);
  // Remplace le mot de passe administrateur par celui de l'utilisateur (seule
  // l'empreinte salee est conservee). Retourne false si trop court.
  static bool setAdminPassword(const String& password);
  // Vrai tant que le mot de passe est celui genere automatiquement (et donc
  // encore affichable sur le port serie).
  static bool adminPasswordIsGenerated();
  static String generatedAdminPassword();
  static String regenerateAdminPassword();

  // Affiche les secrets utiles sur le port serie (appareil headless).
  static void printToSerial();

  // Source d'alea pour WebAuth (jetons de session).
  static uint32_t randomWord();

private:
  static String randomSecret(uint8_t length);
  static void load();
  static void persistAdmin(const String& plainOrEmpty, const String& salt, const String& hash);
  static String hashPassword(const String& password, const String& salt);
};

#endif
