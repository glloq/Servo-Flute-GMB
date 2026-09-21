/***********************************************************************************************
 * WebAuth - Sessions applicatives legeres pour l'interface web
 *
 * L'interface web pilote des actionneurs, reecrit la configuration, redemarre
 * l'appareil et gere les fichiers MIDI. Sur un hotspot partage (ou un reseau
 * domestique), n'importe quel client pouvait le faire sans aucune preuve
 * d'autorisation. Ce module ajoute une couche minimale :
 *
 *   POST /api/auth/login {"password": "..."}   ->  { "token": "..." }
 *   en-tete X-Auth-Token (ou ?token=) sur les routes protegees
 *   WebSocket : premier message {"t":"auth","token":"..."}
 *
 * Volontairement simple : quelques jetons aleatoires en RAM, duree de vie
 * glissante, comparaison en temps constant. Aucune allocation dynamique, aucune
 * dependance crypto : la robustesse vient de l'entropie du jeton (128 bits) et
 * du fait qu'il ne survit pas a un redemarrage.
 *
 * Module pur (pas de reseau, pas de NVS) pour etre testable sur hote.
 ***********************************************************************************************/
#ifndef WEB_AUTH_H
#define WEB_AUTH_H

#include <Arduino.h>

#define WEB_AUTH_MAX_SESSIONS 4
#define WEB_AUTH_TOKEN_LEN 32            // caracteres hexadecimaux => 128 bits
#define WEB_AUTH_DEFAULT_TTL_MS 3600000UL  // 1 h glissante

// Source d'alea injectee (esp_random() en production, deterministe en test).
typedef uint32_t (*WebAuthRandomFn)();

class WebAuth {
public:
  WebAuth();

  void begin(WebAuthRandomFn rng, unsigned long ttlMs = WEB_AUTH_DEFAULT_TTL_MS);

  // Cree une session et retourne son jeton. Si toutes les places sont prises,
  // la session la plus proche de l'expiration est recyclee.
  String createSession(unsigned long now);

  // Verifie un jeton. Rafraichit sa duree de vie (expiration glissante).
  bool validate(const String& token, unsigned long now);

  void revoke(const String& token);
  void revokeAll();
  uint8_t activeSessions(unsigned long now) const;

private:
  struct Session {
    char token[WEB_AUTH_TOKEN_LEN + 1];
    unsigned long expiresAt;
    bool used;
  };

  Session _sessions[WEB_AUTH_MAX_SESSIONS];
  unsigned long _ttlMs;
  WebAuthRandomFn _rng;

  static bool constantTimeEquals(const char* a, const char* b, size_t len);
  void expire(unsigned long now);
};

// Comparaison de mot de passe en temps constant (pas d'oracle temporel sur la
// longueur du prefixe correct).
bool webAuthSecretEquals(const String& a, const String& b);

#endif
