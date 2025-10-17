// ESP32 BLE Presence Detector
//
// Este código utiliza la pila NimBLE de Espressif para escanear anuncios BLE
// y detectar dispositivos fabricados por Apple (por ejemplo, AirTags o iBeacons).
// Cuando se detecta un dispositivo de Apple, se publica un mensaje JSON en un
// servidor MQTT especificado. Ajusta las credenciales Wi‑Fi y los parámetros MQTT
// antes de compilar y cargar en tu ESP32.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <PubSubClient.h>

// Configuración de la red Wi‑Fi
const char *ssid = "TU_SSID";
const char *password = "TU_CONTRASENA";

// Configuración del servidor MQTT
const char *mqtt_server = "192.168.1.100";
const uint16_t mqtt_port = 1883;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Callback para manejar dispositivos anunciados
class AdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
public:
  void onResult(NimBLEAdvertisedDevice *advertisedDevice) override {
    // Filtrar por ID de fabricante de Apple (0x004C)
    if (advertisedDevice->getManufacturerId() == 0x004C) {
      int rssi = advertisedDevice->getRSSI();
      std::string address = advertisedDevice->getAddress().toString();
      Serial.printf("Dispositivo Apple detectado: %s (RSSI: %d)\n", address.c_str(), rssi);

      // Construir carga útil JSON
      String payload = "{\"address\":\"";
      payload += address.c_str();
      payload += "\",\"rssi\":" + String(rssi) + "}";

      // Publicar el mensaje en MQTT
      mqttClient.publish("home/esp32/presence", payload.c_str(), true);
    }
  }
};

void connectToWiFi() {
  Serial.printf("Conectando a Wi‑Fi %s", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi‑Fi conectado");
}

void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Conectando a MQTT...");
    if (mqttClient.connect("ESP32PresenceDetector")) {
      Serial.println(" conectado");
    } else {
      Serial.print(" fallo, rc=");
      Serial.print(mqttClient.state());
      Serial.println("; reintentando en 5 segundos");
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);

  connectToWiFi();

  mqttClient.setServer(mqtt_server, mqtt_port);

  // Inicializar NimBLE
  NimBLEDevice::init("");
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks());
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(50);
  // Empezar a escanear de forma indefinida (0 indica sin tiempo límite)
  scan->start(0, nullptr, false);
}

void loop() {
  // Mantener la conexión MQTT
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  // Otros procesos pueden añadirse aquí (por ejemplo, gestión de comandos MQTT)
  delay(100);
}