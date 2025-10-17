/*
  ESP32 DevKitC V4 (AZ-Delivery)
  Portal Wi-Fi+MQTT (sólo primera vez) + Reset por botón (10s) + Detector Apple 0x004C + MQTT + NVS
  - AP WPA2 con IP 192.168.1.1 para configurar SSID/Pass Wi-Fi y MQTT (host/puerto/usuario/clave)
  - Arranques normales: conecta a Wi-Fi guardada y usa MQTT guardado (no vuelve al portal automáticamente)
  - Reset de fábrica manteniendo botón (GPIO0/BOOT) 10 s: borra NVS (wifi+cfg+mqtt) y reinicia al portal
  - FIX: NO se inicia BLE en modo portal para evitar conflictos con SoftAP/DHCP
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <NimBLEDevice.h>

// =================== BOTÓN / RESET FÁBRICA ===================
#define BUTTON_PIN 0            // GPIO0 (BOOT)
#define LONG_PRESS_MS 10000     // 10 s
unsigned long pressStart = 0;
bool buttonWasPressed = false;

// =================== AP (PORTAL PRIMERA VEZ) =================
Preferences prefs;              // compartido (wifi, mqtt, cfg)
WebServer server(80);
DNSServer dns;
const byte DNS_PORT = 53;

// AP WPA2 y red 192.168.1.0/24
String   apSSID;
const char* apPASS = "ConfiguraESP";            // ≥ 8 chars
IPAddress apIP(192,168,1,1), apGW(192,168,1,1), apMASK(255,255,255,0);
const uint8_t AP_CHANNEL  = 6;                  // 1, 6 u 11 recomendado
const uint8_t AP_MAX_CONN = 4;

bool isPortalMode = false;      // <<< clave para decidir si arrancar BLE

// =================== MQTT CONFIG (persistente) ===============
String   MQTT_HOST = "192.168.1.100";
uint16_t MQTT_PORT = 1883;
String   MQTT_USER = "esp32";
String   MQTT_PASSWD = "clave";

void loadMqttFromNVS() {
  prefs.begin("mqtt", true);
  MQTT_HOST   = prefs.getString("host", MQTT_HOST);
  MQTT_PORT   = (uint16_t)prefs.getUInt("port", MQTT_PORT);
  MQTT_USER   = prefs.getString("user", MQTT_USER);
  MQTT_PASSWD = prefs.getString("pass", MQTT_PASSWD);
  prefs.end();
}
void saveMqttToNVS(const String& host, uint16_t port, const String& user, const String& pass) {
  prefs.begin("mqtt", false);
  prefs.putString("host", host);
  prefs.putUInt("port", port);
  prefs.putString("user", user);
  prefs.putString("pass", pass);
  prefs.end();
}

// =================== HTML portal =============================
String htmlPage(const String& body) {
  return String(F(
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'/>"
    "<title>ESP32 Setup</title><style>"
    "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;margin:2rem;}"
    "form{max-width:520px;margin:auto;padding:1rem;border:1px solid #ddd;border-radius:12px}"
    "h3{margin-top:1rem}"
    "label{display:block;margin:.5rem 0 .25rem}"
    "input{width:100%;padding:.6rem;border:1px solid #ccc;border-radius:8px}"
    "button{margin-top:1rem;padding:.7rem 1rem;border:0;border-radius:10px;background:#0b5;color:#fff;cursor:pointer;font-weight:600}"
    ".danger{background:#c33}.muted{color:#666;font-size:.9rem;margin-top:.5rem}"
    "</style></head><body>"
  )) + body + F("</body></html>");
}

void handleRoot() {
  prefs.begin("mqtt", true);
  String host = prefs.getString("host", "192.168.1.100");
  uint32_t port = prefs.getUInt("port", 1883);
  String user = prefs.getString("user", "");
  String pass = prefs.getString("pass", "");
  prefs.end();

  String body;
  body.reserve(2000);
  body += F("<h2>Configurar Wi-Fi + MQTT</h2>");
  body += F("<form method='POST' action='/save'>");
  body += F("<h3>Wi-Fi</h3>");
  body += F("<label>SSID</label><input name='ssid' required>");
  body += F("<label>Contraseña</label><input name='pass' type='password' required>");
  body += F("<h3>MQTT</h3>");
  body += F("<label>Host</label><input name='mqtt_host' required value='"); body += host; body += F("'>");
  body += F("<label>Puerto</label><input name='mqtt_port' type='number' min='1' max='65535' required value='"); body += String(port); body += F("'>");
  body += F("<label>Usuario (opcional)</label><input name='mqtt_user' value='"); body += user; body += F("'>");
  body += F("<label>Contraseña (opcional)</label><input name='mqtt_pass' type='password' value='"); body += pass; body += F("'>");
  body += F("<button type='submit'>Guardar y reiniciar</button>"
            "<p class='muted'>Se guardan en NVS (no cifrado). Para cambiar más tarde, usa el botón 10 s.</p>"
            "</form>"
            "<form method='POST' action='/factory'>"
            "<button class='danger' type='submit'>Reset de fábrica</button>"
            "</form>");
  server.send(200, "text/html", htmlPage(body));
}

bool haveSavedWiFi() {
  prefs.begin("wifi", true);
  bool configured = prefs.getBool("configured", false);
  String ssid = prefs.getString("ssid", "");
  prefs.end();
  return (configured && !ssid.isEmpty());
}

bool connectSavedWiFiNonBlocking() {
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();
  if (ssid.isEmpty()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("Conectando Wi-Fi a %s ...\n", ssid.c_str());
  return true;
}

void startAP() {
  uint64_t chipid = ESP.getEfuseMac();
  apSSID = "ESP32-Setup-" + String((uint32_t)(chipid & 0xFFFFFF), HEX);
  apSSID.toUpperCase();

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);

  WiFi.softAPConfig(apIP, apGW, apMASK);
  bool ok = WiFi.softAP(apSSID.c_str(), apPASS, AP_CHANNEL, /*hidden=*/false, AP_MAX_CONN);
  delay(200);

  dns.start(DNS_PORT, "*", apIP);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, [](){
    if (!server.hasArg("ssid") || !server.hasArg("pass")
        || !server.hasArg("mqtt_host") || !server.hasArg("mqtt_port")) {
      server.send(400, "text/plain", "Faltan parametros");
      return;
    }
    String ssid = server.arg("ssid");
    String wpass= server.arg("pass");
    String mhost= server.arg("mqtt_host");
    String mport= server.arg("mqtt_port");
    String muser= server.arg("mqtt_user");
    String mpass= server.arg("mqtt_pass");

    long portL = mport.toInt();
    if (portL <= 0 || portL > 65535) { server.send(400, "text/plain", "Puerto MQTT invalido"); return; }

    // Guardar Wi-Fi
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", wpass);
    prefs.putBool("configured", true);
    prefs.end();

    // Guardar MQTT
    saveMqttToNVS(mhost, (uint16_t)portL, muser, mpass);

    server.send(200, "text/html", htmlPage("<h2>Guardado ✅</h2><p>Reiniciando...</p>"));
    delay(1200);
    ESP.restart();
  });
  server.on("/factory", HTTP_POST, [](){
    prefs.begin("wifi", false); prefs.clear(); prefs.end();
    prefs.begin("cfg",  false); prefs.clear(); prefs.end();
    prefs.begin("mqtt", false); prefs.clear(); prefs.end();
    WiFi.disconnect(true, true);
    server.send(200, "text/html", htmlPage("<h2>Reset de fábrica</h2><p>Reiniciando…</p>"));
    delay(1200);
    ESP.restart();
  });
  server.onNotFound([](){ server.sendHeader("Location", "/", true); server.send(302); });
  server.begin();

  Serial.println();
  Serial.println("=== MODO CONFIGURACIÓN (AP) ===");
  Serial.printf("SSID: %s\nPASS: %s\nCANAL: %u\n", apSSID.c_str(), apPASS, AP_CHANNEL);
  Serial.printf("Portal: http://%s/\n", apIP.toString().c_str());
  Serial.printf("AP ok? %s | MAC AP: %s\n", ok ? "SI":"NO", WiFi.softAPmacAddress().c_str());
}

// ================== Detector Apple + MQTT ==================
// Identidad del dispositivo
const char* DEVICE_ID   = "esp32-airtag-1";
const char* DEVICE_NAME = "ESP32 AirTag Detector";

#define VERBOSO 0

// Tópicos
String baseStat   = String("home/") + DEVICE_ID + "/presence";
String topicState = baseStat + "/state";
String topicAttr  = baseStat + "/attributes";
String topicAvail = String("home/") + DEVICE_ID + "/status";

String discBin    = String("homeassistant/binary_sensor/") + DEVICE_ID + "/presence/config";

String paramsBase = String("home/") + DEVICE_ID + "/params";
String discNum_rssiStrong     = String("homeassistant/number/") + DEVICE_ID + "/rssi_strong/config";
String discNum_rssiVeryStrong = String("homeassistant/number/") + DEVICE_ID + "/rssi_verystrong/config";
String discNum_hitsReq        = String("homeassistant/number/") + DEVICE_ID + "/hits_req/config";
String discNum_window         = String("homeassistant/number/") + DEVICE_ID + "/strong_window/config";
String discNum_vstrongAge     = String("homeassistant/number/") + DEVICE_ID + "/vstrong_age/config";
String discNum_offgap         = String("homeassistant/number/") + DEVICE_ID + "/off_gap/config";

String cmd_rssiStrong     = paramsBase + "/rssi_strong/set";
String cmd_rssiVeryStrong = paramsBase + "/rssi_verystrong/set";
String cmd_hitsReq        = paramsBase + "/hits_req/set";
String cmd_window         = paramsBase + "/strong_window_ms/set";
String cmd_vstrongAge     = paramsBase + "/vstrong_age_ms/set";
String cmd_offgap         = paramsBase + "/off_gap_ms/set";

String st_rssiStrong      = paramsBase + "/rssi_strong/state";
String st_rssiVeryStrong  = paramsBase + "/rssi_verystrong/state";
String st_hitsReq         = paramsBase + "/hits_req/state";
String st_window          = paramsBase + "/strong_window_ms/state";
String st_vstrongAge      = paramsBase + "/vstrong_age_ms/state";
String st_offgap          = paramsBase + "/off_gap_ms/state";

// Parámetros por defecto (modo reposo)
int      RSSI_VERY_STRONG        = -52;
int      RSSI_STRONG             = -56;
uint8_t  STRONG_HITS_REQ         = 2;
uint32_t STRONG_WINDOW_MS        = 20000;
uint32_t VERY_STRONG_MAX_AGE_MS  = 15000;
uint32_t OFF_GAP_MS              = 60000;

const uint32_t EVAL_MS            = 250;
const uint32_t STARTUP_SILENCE_MS = 4000;
const uint32_t STATUS_EVERY_MS    = 15000;

// BLE buffers
struct Hit { uint32_t ts; int rssi; };
#define MAX_HITS 160
Hit hits[MAX_HITS];
uint8_t hitCount = 0;

inline uint32_t nowMs() { return millis(); }

void addHit(int rssi) {
  const uint32_t t = nowMs();
  if (hitCount < MAX_HITS) { hits[hitCount++] = {t, rssi}; return; }
  for (uint8_t i = 0; i < MAX_HITS - 1; i++) hits[i] = hits[i + 1];
  hits[MAX_HITS - 1] = {t, rssi};
}

void pruneOld(uint32_t windowMs) {
  const uint32_t cutoff = nowMs() - windowMs;
  uint8_t w = 0;
  for (uint8_t i = 0; i < hitCount; i++)
    if (hits[i].ts >= cutoff) hits[w++] = hits[i];
  hitCount = w;
}

uint8_t countStrongInWindow() {
  pruneOld(STRONG_WINDOW_MS);
  uint8_t c = 0;
  for (uint8_t i = 0; i < hitCount; i++)
    if (hits[i].rssi >= RSSI_STRONG) c++;
  return c;
}

bool haveVeryStrongRecent() {
  const uint32_t cutoff = nowMs() - VERY_STRONG_MAX_AGE_MS;
  for (uint8_t i = 0; i < hitCount; i++)
    if (hits[i].rssi >= RSSI_VERY_STRONG && hits[i].ts >= cutoff) return true;
  return false;
}

uint32_t ageSinceLastStrong() {
  int best = -200;
  uint32_t tsBest = 0;
  for (uint8_t i = 0; i < hitCount; i++) {
    if (hits[i].rssi >= RSSI_STRONG && hits[i].rssi > best) {
      best = hits[i].rssi; tsBest = hits[i].ts;
    }
  }
  return (tsBest == 0) ? 0xFFFFFFFFUL : (nowMs() - tsBest);
}

volatile int lastStrongRSSI_forAttr = -127;
bool present = false;
bool firstScanDone = false;

// ============== BLE: iniciar SOLO fuera de portal ===========
bool bleStarted = false;
NimBLEScan* scan = nullptr;

class AdvCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    const std::string& md = dev->getManufacturerData();
    if (md.size() < 2) return;
    if ((uint8_t)md[0] != 0x4C || (uint8_t)md[1] != 0x00) return; // Apple 0x004C
    const int rssi = dev->getRSSI();
    if (rssi >= RSSI_STRONG) {
      addHit(rssi);
      lastStrongRSSI_forAttr = rssi;
#if VERBOSO
      Serial.printf("[APPLE strong] RSSI=%d dBm addr=%s\n",
                    rssi, dev->getAddress().toString().c_str());
#endif
    }
  }
};

void startBLE() {
  if (bleStarted) return;
  NimBLEDevice::init("");
  scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new AdvCB(), /*wantDuplicates=*/true);
  scan->setDuplicateFilter(false);
  scan->setActiveScan(true);
  scan->setInterval(80);
  scan->setWindow(80);
  scan->start(0, false, true); // continuo
  bleStarted = true;
  Serial.println("BLE iniciado (escaneo continuo).");
}

void stopBLE() {
  if (!bleStarted) return;
  if (scan) scan->stop();
  NimBLEDevice::deinit(true);
  bleStarted = false;
  Serial.println("BLE detenido.");
}

// Persistencia de parámetros
void loadParamsFromNVS() {
  if (!prefs.begin("cfg", true)) return;
  RSSI_VERY_STRONG       = prefs.getInt("rssi_vstrong", RSSI_VERY_STRONG);
  RSSI_STRONG            = prefs.getInt("rssi_strong",  RSSI_STRONG);
  STRONG_HITS_REQ        = (uint8_t)prefs.getUInt("hits_req", STRONG_HITS_REQ);
  STRONG_WINDOW_MS       = prefs.getUInt("win_ms",      STRONG_WINDOW_MS);
  VERY_STRONG_MAX_AGE_MS = prefs.getUInt("vstrong_age", VERY_STRONG_MAX_AGE_MS);
  OFF_GAP_MS             = prefs.getUInt("off_gap",     OFF_GAP_MS);
  prefs.end();
}
void saveParamsToNVS() {
  if (!prefs.begin("cfg", false)) return;
  prefs.putInt("rssi_vstrong", RSSI_VERY_STRONG);
  prefs.putInt("rssi_strong",  RSSI_STRONG);
  prefs.putUInt("hits_req",    STRONG_HITS_REQ);
  prefs.putUInt("win_ms",      STRONG_WINDOW_MS);
  prefs.putUInt("vstrong_age", VERY_STRONG_MAX_AGE_MS);
  prefs.putUInt("off_gap",     OFF_GAP_MS);
  prefs.end();
}

// ================== Wi-Fi / MQTT infra ==================
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

void mqttPublish(const String& topic, const String& payload, bool retain=false) {
  mqtt.publish(topic.c_str(), payload.c_str(), retain);
}

void publishBinaryDiscovery() {
  char buf[768];
  snprintf(buf, sizeof(buf),
    "{"
      "\"name\":\"AirTag Presencia\","    
      "\"uniq_id\":\"%s_presence\","    
      "\"stat_t\":\"%s\","    
      "\"pl_on\":\"ON\",\"pl_off\":\"OFF\","    
      "\"avty_t\":\"%s\","    
      "\"json_attr_t\":\"%s\","    
      "\"dev_cla\":\"occupancy\","    
      "\"device\":{"
         "\"ids\":[\"%s\"],"
         "\"name\":\"%s\","
         "\"mdl\":\"ESP32 DevKitC\","
         "\"mf\":\"DIY\","
         "\"sw\":\"airtag-detector 1.2\""
      "}"
    "}",
    DEVICE_ID, topicState.c_str(), topicAvail.c_str(), topicAttr.c_str(),
    DEVICE_ID, DEVICE_NAME
  );
  mqttPublish(discBin, buf, true);
}

void publishNumberDiscovery(
  const String& discTopic, const char* name, const char* uniq_suffix,
  const String& cmd_t, const String& stat_t,
  int minV, int maxV, int step, const char* unit=nullptr
) {
  char buf[1000];
  snprintf(buf, sizeof(buf),
    "{"
      "\"name\":\"%s\","
      "\"uniq_id\":\"%s_%s\","
      "\"cmd_t\":\"%s\","
      "\"stat_t\":\"%s\","
      "\"avty_t\":\"%s\","
      "\"min\":%d,\"max\":%d,\"step\":%d,"
      "\"mode\":\"box\","
      "%s"
      "\"entity_category\":\"config\","
      "\"device\":{"
         "\"ids\":[\"%s\"],"
         "\"name\":\"%s\","
         "\"mdl\":\"ESP32 DevKitC\","
         "\"mf\":\"DIY\","
         "\"sw\":\"airtag-detector 1.2\""
      "}"
    "}",
    name, DEVICE_ID, uniq_suffix,
    cmd_t.c_str(), stat_t.c_str(), topicAvail.c_str(),
    minV, maxV, step,
    (unit ? (String("\"unit_of_meas\":\"") + unit + "\",": "").c_str() : ""),
    DEVICE_ID, DEVICE_NAME
  );
  mqttPublish(discTopic, buf, true);
}

void publishAllDiscovery() {
  publishBinaryDiscovery();
  publishNumberDiscovery(discNum_rssiStrong,     "RSSI_STRONG (dBm)",        "rssi_strong",     cmd_rssiStrong,     st_rssiStrong,     -95,  -40,   1, "dBm");
  publishNumberDiscovery(discNum_rssiVeryStrong, "RSSI_VERY_STRONG (dBm)",   "rssi_verystrong", cmd_rssiVeryStrong, st_rssiVeryStrong, -95,  -40,   1, "dBm");
  publishNumberDiscovery(discNum_hitsReq,        "STRONG_HITS_REQ",          "hits_req",        cmd_hitsReq,        st_hitsReq,          1,    6,   1, nullptr);
  publishNumberDiscovery(discNum_window,         "STRONG_WINDOW_MS",         "window_ms",       cmd_window,         st_window,       1000, 60000, 250, "ms");
  publishNumberDiscovery(discNum_vstrongAge,     "VERY_STRONG_MAX_AGE_MS",   "vstrong_age",     cmd_vstrongAge,     st_vstrongAge,    500,  60000, 250, "ms");
  publishNumberDiscovery(discNum_offgap,         "OFF_GAP_MS",               "offgap_ms",       cmd_offgap,         st_offgap,       1000,180000, 250, "ms");
}

void publishAvailability(bool online) { mqttPublish(topicAvail, online ? "online" : "offline", true); }
void publishState(bool isOn)         { mqttPublish(topicState, isOn ? "ON" : "OFF", true); }

void publishAttributes(uint8_t strongCnt, bool veryRecent, uint32_t gapStrongMs) {
  char buf[700];
  snprintf(buf, sizeof(buf),
    "{"
      "\"strongInWin\":%u,\"veryRecentStrong\":\"%s\",\"gapStrongMs\":%lu,"
      "\"lastStrongRSSI\":%d,"
      "\"RSSI_STRONG\":%d,\"RSSI_VERY_STRONG\":%d,\"STRONG_HITS_REQ\":%u,"
      "\"STRONG_WINDOW_MS\":%lu,\"VERY_STRONG_MAX_AGE_MS\":%lu,\"OFF_GAP_MS\":%lu"
    "}",
    strongCnt, (veryRecent ? "YES" : "NO"),
    (unsigned long)((gapStrongMs==0xFFFFFFFFUL)?999999:gapStrongMs),
    lastStrongRSSI_forAttr,
    RSSI_STRONG, RSSI_VERY_STRONG, STRONG_HITS_REQ,
    (unsigned long)STRONG_WINDOW_MS, (unsigned long)VERY_STRONG_MAX_AGE_MS, (unsigned long)OFF_GAP_MS
  );
  mqttPublish(topicAttr, buf, false);
}

void publishParamStates() {
  mqttPublish(st_rssiStrong,     String(RSSI_STRONG), true);
  mqttPublish(st_rssiVeryStrong, String(RSSI_VERY_STRONG), true);
  mqttPublish(st_hitsReq,        String(STRONG_HITS_REQ), true);
  mqttPublish(st_window,         String(STRONG_WINDOW_MS), true);
  mqttPublish(st_vstrongAge,     String(VERY_STRONG_MAX_AGE_MS), true);
  mqttPublish(st_offgap,         String(OFF_GAP_MS), true);
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String t(topic);
  String pl; pl.reserve(len);
  for (unsigned int i=0;i<len;i++) pl += (char)payload[i];
  pl.trim();

  auto toInt = [&](const String& s, long& out)->bool {
    char* endp=nullptr; long v = strtol(s.c_str(), &endp, 10);
    if (endp && *endp=='\0') { out=v; return true; }
    return false;
  };

  bool changed=false;
  long v;

  if (t == cmd_rssiStrong && toInt(pl, v)) {
    if (v >= -95 && v <= -40) { RSSI_STRONG = (int)v; changed=true; mqttPublish(st_rssiStrong, String(RSSI_STRONG), true); }
  } else if (t == cmd_rssiVeryStrong && toInt(pl, v)) {
    if (v >= -95 && v <= -40) { RSSI_VERY_STRONG = (int)v; changed=true; mqttPublish(st_rssiVeryStrong, String(RSSI_VERY_STRONG), true); }
  } else if (t == cmd_hitsReq && toInt(pl, v)) {
    if (v >= 1 && v <= 6) { STRONG_HITS_REQ = (uint8_t)v; changed=true; mqttPublish(st_hitsReq, String(STRONG_HITS_REQ), true); }
  } else if (t == cmd_window && toInt(pl, v)) {
    if (v >= 1000 && v <= 60000) { STRONG_WINDOW_MS = (uint32_t)v; changed=true; mqttPublish(st_window, String(STRONG_WINDOW_MS), true); }
  } else if (t == cmd_vstrongAge && toInt(pl, v)) {
    if (v >= 500 && v <= 60000) { VERY_STRONG_MAX_AGE_MS = (uint32_t)v; changed=true; mqttPublish(st_vstrongAge, String(VERY_STRONG_MAX_AGE_MS), true); }
  } else if (t == cmd_offgap && toInt(pl, v)) {
    if (v >= 1000 && v <= 180000) { OFF_GAP_MS = (uint32_t)v; changed=true; mqttPublish(st_offgap, String(OFF_GAP_MS), true); }
  }

  if (changed) {
    saveParamsToNVS();
    publishAttributes(countStrongInWindow(), haveVeryStrongRecent(), ageSinceLastStrong());
    Serial.printf("Parametro actualizado via MQTT: %s = %ld (persistido)\n", t.c_str(), v);
  } else {
    Serial.printf("Comando MQTT ignorado o fuera de rango: %s = %s\n", t.c_str(), pl.c_str());
  }
}

void ensureMQTT() {
  if (mqtt.connected()) return;
  mqtt.setServer(MQTT_HOST.c_str(), MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  const char* user = MQTT_USER.length() ? MQTT_USER.c_str() : NULL;
  const char* pass = MQTT_PASSWD.length() ? MQTT_PASSWD.c_str() : NULL;

  if (mqtt.connect(DEVICE_ID, user, pass, topicAvail.c_str(), 0, true, "offline")) {
    Serial.printf("MQTT conectado a %s:%u\n", MQTT_HOST.c_str(), MQTT_PORT);
    publishAvailability(true);
    publishAllDiscovery();
    publishParamStates();
    mqtt.publish(topicState.c_str(), present ? "ON":"OFF", true);
    // Suscripciones
    mqtt.subscribe(cmd_rssiStrong.c_str());
    mqtt.subscribe(cmd_rssiVeryStrong.c_str());
    mqtt.subscribe(cmd_hitsReq.c_str());
    mqtt.subscribe(cmd_window.c_str());
    mqtt.subscribe(cmd_vstrongAge.c_str());
    mqtt.subscribe(cmd_offgap.c_str());
  } else {
    Serial.printf("MQTT fallo (%d). Reintentaremos.\n", mqtt.state());
  }
}

// ================== Botón largo ========================
void setupButton() { pinMode(BUTTON_PIN, INPUT_PULLUP); }
void factoryResetAndReboot() {
  Serial.println("\n*** RESET DE FÁBRICA ***");
  prefs.begin("wifi", false); prefs.clear(); prefs.end();
  prefs.begin("cfg",  false); prefs.clear(); prefs.end();
  prefs.begin("mqtt", false); prefs.clear(); prefs.end();
  WiFi.disconnect(true, true);
  delay(200);
  ESP.restart();
}
void checkLongPress() {
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  if (pressed && !buttonWasPressed) { buttonWasPressed = true; pressStart = millis(); }
  else if (!pressed && buttonWasPressed) { buttonWasPressed = false; }
  if (buttonWasPressed && (millis() - pressStart) >= LONG_PRESS_MS) {
    factoryResetAndReboot();
  }
}

// ================== setup / loop =======================
void setup() {
  Serial.begin(115200);
  delay(150);
  setupButton();

  // Cargar parámetros y config MQTT persistidos
  loadParamsFromNVS();
  loadMqttFromNVS();

  // Selección de modo
  if (!haveSavedWiFi()) {
    isPortalMode = true;
    startAP();           // <<< Sólo AP y portal; SIN BLE
    stopBLE();           // por si acaso
  } else {
    isPortalMode = false;
    connectSavedWiFiNonBlocking();
  }

  // MQTT buffer
  mqtt.setBufferSize(1024);

  Serial.println("\n=== ESP32 Apple 0x004C — MQTT (HA) + Portal 1ª vez + Reset botón ===");
  Serial.printf("AP SSID: %s  PASS: %s  IP: %s\n", apSSID.c_str(), apPASS, apIP.toString().c_str());
  Serial.printf("MQTT destino actual: %s:%u (user='%s')\n", MQTT_HOST.c_str(), MQTT_PORT, MQTT_USER.c_str());
}

void loop() {
  const uint32_t t = nowMs();
  static uint32_t lastEval = 0, lastStatus = 0, lastConnTry = 0;

  // Botón largo
  checkLongPress();

  // Si estamos en AP (portal), atender DNS/HTTP y salir (sin BLE/MQTT)
  if (isPortalMode) {
    dns.processNextRequest();
    server.handleClient();
    delay(5);
    return;
  }

  // En modo STA: mantener Wi-Fi/MQTT y arrancar BLE si aún no está
  if ((t - lastConnTry) > 3000) {
    lastConnTry = t;
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
      Serial.print(".");
    } else {
      if (!bleStarted) startBLE();  // <<< BLE sólo cuando hay STA
      ensureMQTT();
    }
  }
  if (mqtt.connected()) mqtt.loop();

  // Cadencia de evaluación
  if (t - lastEval < EVAL_MS) return;
  lastEval = t;

  // Silencio inicial
  if (!firstScanDone) {
    if (t < STARTUP_SILENCE_MS) return;
    firstScanDone = true;
    present = false;
    Serial.println(">>> Estado: OFF (arranque estable)");
    if (mqtt.connected()) {
      publishState(false);
      publishAttributes(0, false, 0xFFFFFFFFUL);
    }
  }

  // Lógica de presencia
  const uint8_t strongCnt = countStrongInWindow();
  const bool veryRecent   = haveVeryStrongRecent();
  bool wantOn = false;

  if (strongCnt >= STRONG_HITS_REQ && veryRecent) wantOn = true; // entrada
  if (present) { // mantenimiento
    const uint32_t ageStrong = ageSinceLastStrong();
    if (ageStrong != 0xFFFFFFFFUL && ageStrong <= OFF_GAP_MS) wantOn = true;
  }

  if (wantOn != present) {
    present = wantOn;
    Serial.printf(">>> Estado: %s (strongInWin=%u, veryRecent=%s, gapStrong=%lums, lastRSSI=%d)\n",
                  present ? "ON" : "OFF",
                  strongCnt, veryRecent ? "YES":"NO",
                  (unsigned long)((ageSinceLastStrong()==0xFFFFFFFFUL)?999999:ageSinceLastStrong()),
                  lastStrongRSSI_forAttr);
    if (mqtt.connected()) {
      publishState(present);
      publishAttributes(strongCnt, veryRecent, ageSinceLastStrong());
    }
  }

  // Heartbeat / atributos periódicos
  if (t - lastStatus >= STATUS_EVERY_MS) {
    lastStatus = t;
    if (mqtt.connected()) {
      publishAttributes(countStrongInWindow(), haveVeryStrongRecent(), ageSinceLastStrong());
    }
  }
}
