# Modes WiFi

## Vue d'ensemble

L'ESP32 supporte deux modes WiFi, plus le mode Bluetooth :

```
                    Switch GPIO4
                   /            \
                LOW              HIGH
                 |                |
            Bluetooth          WiFi
            (BLE-MIDI)           |
                          +------+------+
                          |             |
                     cfg.wifiSsid    (vide)
                      renseigne        |
                          |          Mode AP
                     Mode STA       (hotspot)
                          |
                    Connexion OK ?
                     /        \
                   Oui        Non (timeout 10s)
                    |           |
              STA connecte   Fallback AP
```

## Mode Bluetooth (BLE-MIDI)

- Active quand le switch GPIO4 est LOW
- NimBLE stack pour BLE-MIDI
- Compatible avec :
  - iOS (natif depuis iOS 8)
  - macOS (natif dans Audio MIDI Setup)
  - Windows (via BLE-MIDI apps)
  - Android (via MIDI apps supportant BLE)
- Nom du peripherique : `cfg.deviceName` (defaut: "ServoFlute")

### LED
- Clignotement rapide : advertising (en attente de connexion)
- Clignotement lent : connecte

### Bouton
- Appui court : restart advertising (si deconnecte)

## Mode WiFi STA (Station)

- Active quand le switch GPIO4 est HIGH ET `cfg.wifiSsid` est renseigne
- Se connecte au reseau WiFi sauvegarde
- Timeout de connexion : 10 secondes
- Si echec : fallback automatique vers AP

### Services disponibles
- rtpMIDI (port 5004) : compatible macOS MIDI Network, Windows rtpMIDI
- Serveur web (port 80) : interface de controle
- mDNS : accessible via `servo-flute.local`

### LED
- Double flash : STA connecte

## Mode WiFi AP (Access Point / Hotspot)

Active dans 3 cas :
1. Switch WiFi HIGH et aucun SSID configure
2. Echec de connexion STA (fallback automatique)
3. Appui long (3s) sur le bouton en mode WiFi

### Configuration AP
- SSID : `ServoFlute-Setup` (defini dans settings.h, non modifiable a chaud)
- Mot de passe : aucun (portail ouvert)
- Canal : 1
- Max connexions : 2
- IP : `192.168.4.1`

### LED
- Triple flash : AP actif

## Changer de reseau WiFi

### Depuis le hotspot (premiere utilisation ou changement)

1. L'ESP32 est en mode AP (pas de SSID configure ou appui long bouton)
2. Se connecter au WiFi `ServoFlute-Setup` depuis un telephone
3. Ouvrir `192.168.4.1`
4. Onglet **WiFi**
5. Cliquer **Scanner** pour voir les reseaux disponibles
6. Selectionner un reseau dans la liste
7. Entrer le mot de passe
8. Cliquer **Connecter**
9. L'ESP32 sauvegarde les credentials et tente la connexion STA
10. En cas de succes, le hotspot se ferme. L'ESP32 est accessible via la nouvelle IP ou `servo-flute.local`

### Forcer le retour en hotspot

Si le reseau WiFi change ou n'est plus accessible :
- **Appui long (3s)** sur le bouton physique (GPIO0)
- L'ESP32 passe immediatement en mode AP
- Les anciens credentials restent sauvegardes (reessayer au prochain reboot)

## Flux de connexion WiFi

```
boot
  |
  v
WirelessManager.begin()
  |
  +-- switch = BT ? --> BleMidiHandler.begin()
  |
  +-- switch = WiFi
        |
        +-- strlen(cfg.wifiSsid) > 0 ?
        |     |
        |     Oui --> WifiMidiHandler.startSTA(ssid, pass)
        |              |
        |              +-- WiFi.status() == WL_CONNECTED ? (polling dans update())
        |              |     |
        |              |     Oui --> setupMDNS() + setupRtpMidi()
        |              |     Non --> timeout 10s --> forceAP()
        |              |
        |     Non --> startAP() directement
        |
        +-- WebConfigurator.begin() (serveur web demarre dans tous les cas)
```

## Securite

- Le hotspot AP est ouvert (pas de mot de passe) pour faciliter la premiere connexion depuis un telephone
- Le mot de passe WiFi STA est stocke en clair dans `/config.json` sur LittleFS
- Pas de HTTPS (contrainte memoire ESP32)
- L'API REST n'a pas d'authentification (reseau local ou hotspot isole)
