#include "WebAuth.h"

static const char kHexDigits[] = "0123456789abcdef";

WebAuth::WebAuth() : _ttlMs(WEB_AUTH_DEFAULT_TTL_MS), _rng(nullptr) {
  for (uint8_t i = 0; i < WEB_AUTH_MAX_SESSIONS; i++) {
    _sessions[i].used = false;
    _sessions[i].expiresAt = 0;
    _sessions[i].token[0] = '\0';
  }
}

void WebAuth::begin(WebAuthRandomFn rng, unsigned long ttlMs) {
  _rng = rng;
  _ttlMs = (ttlMs == 0) ? WEB_AUTH_DEFAULT_TTL_MS : ttlMs;
  revokeAll();
}

void WebAuth::expire(unsigned long now) {
  for (uint8_t i = 0; i < WEB_AUTH_MAX_SESSIONS; i++) {
    // Comparaison signee sur 32 bits explicites : reste correcte au rollover de
    // millis(), et identique sur l'hote des tests (ou `long` fait 64 bits).
    if (_sessions[i].used && (int32_t)(now - _sessions[i].expiresAt) >= 0) {
      _sessions[i].used = false;
      _sessions[i].token[0] = '\0';
    }
  }
}

String WebAuth::createSession(unsigned long now) {
  expire(now);

  int slot = -1;
  for (uint8_t i = 0; i < WEB_AUTH_MAX_SESSIONS; i++) {
    if (!_sessions[i].used) { slot = i; break; }
  }
  if (slot < 0) {
    // Toutes les places prises : recycler celle qui expire le plus tot.
    slot = 0;
    for (uint8_t i = 1; i < WEB_AUTH_MAX_SESSIONS; i++) {
      if ((int32_t)(_sessions[i].expiresAt - _sessions[slot].expiresAt) < 0) slot = i;
    }
  }

  Session& s = _sessions[slot];
  for (uint8_t i = 0; i < WEB_AUTH_TOKEN_LEN; i += 8) {
    uint32_t r = _rng ? _rng() : 0;
    for (uint8_t j = 0; j < 8 && (i + j) < WEB_AUTH_TOKEN_LEN; j++) {
      s.token[i + j] = kHexDigits[(r >> (4 * j)) & 0x0F];
    }
  }
  s.token[WEB_AUTH_TOKEN_LEN] = '\0';
  s.expiresAt = now + _ttlMs;
  s.used = true;
  return String(s.token);
}

bool WebAuth::constantTimeEquals(const char* a, const char* b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) {
    diff |= (uint8_t)(a[i] ^ b[i]);
  }
  return diff == 0;
}

bool WebAuth::validate(const String& token, unsigned long now) {
  if (token.length() != WEB_AUTH_TOKEN_LEN) return false;
  expire(now);
  bool ok = false;
  for (uint8_t i = 0; i < WEB_AUTH_MAX_SESSIONS; i++) {
    if (!_sessions[i].used) continue;
    if (constantTimeEquals(_sessions[i].token, token.c_str(), WEB_AUTH_TOKEN_LEN)) {
      _sessions[i].expiresAt = now + _ttlMs;   // expiration glissante
      ok = true;
      // Pas de break : parcourir toutes les places garde le temps d'execution
      // independant de la position du jeton.
    }
  }
  return ok;
}

void WebAuth::revoke(const String& token) {
  if (token.length() != WEB_AUTH_TOKEN_LEN) return;
  for (uint8_t i = 0; i < WEB_AUTH_MAX_SESSIONS; i++) {
    if (_sessions[i].used && constantTimeEquals(_sessions[i].token, token.c_str(), WEB_AUTH_TOKEN_LEN)) {
      _sessions[i].used = false;
      _sessions[i].token[0] = '\0';
    }
  }
}

void WebAuth::revokeAll() {
  for (uint8_t i = 0; i < WEB_AUTH_MAX_SESSIONS; i++) {
    _sessions[i].used = false;
    _sessions[i].expiresAt = 0;
    _sessions[i].token[0] = '\0';
  }
}

uint8_t WebAuth::activeSessions(unsigned long now) const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < WEB_AUTH_MAX_SESSIONS; i++) {
    if (_sessions[i].used && (int32_t)(now - _sessions[i].expiresAt) < 0) n++;
  }
  return n;
}

bool webAuthSecretEquals(const String& a, const String& b) {
  // Longueurs differentes : on compare quand meme jusqu'au bout de la plus
  // longue pour ne pas transformer la longueur en oracle exploitable.
  size_t la = a.length(), lb = b.length();
  size_t n = (la > lb) ? la : lb;
  uint8_t diff = (uint8_t)((la ^ lb) & 0xFF);
  diff |= (uint8_t)(((la ^ lb) >> 8) & 0xFF);
  for (size_t i = 0; i < n; i++) {
    uint8_t ca = (i < la) ? (uint8_t)a[i] : 0;
    uint8_t cb = (i < lb) ? (uint8_t)b[i] : 0;
    diff |= (uint8_t)(ca ^ cb);
  }
  return diff == 0 && la == lb;
}
