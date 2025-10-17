![Detector de presencia ESP32 BLE](assets/cover.png)

# ESP32 BLE Presence Detector for Home Assistant

Este proyecto consiste en un detector de presencia basado en ESP32 utilizando la biblioteca NimBLE para escanear dispositivos BLE como las AirTags de Apple. Permite publicar estados de presencia en MQTT/Home Assistant y ajustar parámetros por MQTT o almacenar datos en la NVS del ESP32.

## Características principales

* **Detección BLE**: Escanea anuncios Bluetooth Low Energy de dispositivos cercanos, filtrando por fabricante (Apple) o direcciones MAC configuradas.
* **Publicación MQTT**: Envía mensajes de presencia a un servidor MQTT para integrarse fácilmente con Home Assistant.
* **Configuración dinámica**: Permite actualizar valores de configuración (por ejemplo, umbral de señal o lista de dispositivos) mediante comandos MQTT o a través de la memoria NVS del microcontrolador.
* **Bajo consumo**: Utiliza el stack NimBLE de Espressif para reducir el uso de memoria y energía.

## Requisitos

* Una placa **ESP32** compatible (por ejemplo, DevKit v4).
* IDE Arduino o PlatformIO.
* Librerías recomendadas:
  - [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) para el escaneo BLE.
  - [PubSubClient](https://github.com/knolleary/pubsubclient) para la comunicación MQTT.
  - [Preferences](https://github.com/espressif/arduino-esp32/tree/master/libraries/Preferences) para almacenamiento NVS.

## Estructura del proyecto

```
esp32-ble-presence/
├── src/
│   └── main.cpp         # Código principal del firmware
├── assets/
│   └── cover.png        # Imagen de portada para la documentación
├── .gitignore
└── README.md
```

## Uso básico

1. Clona este repositorio y abre la carpeta `esp32-ble-presence` en tu IDE preferido.
2. Configura las credenciales Wi‑Fi y los parámetros MQTT en `src/main.cpp`.
3. Carga el código en tu ESP32.
4. Registra las direcciones MAC o UUIDs de los dispositivos que deseas detectar.
5. Conecta Home Assistant a tu broker MQTT para visualizar los cambios de presencia.

## Contribuciones

Las contribuciones son bienvenidas. Puedes abrir issues para reportar problemas o sugerir mejoras, y enviar pull requests con tu código. 

## Licencia

Este proyecto se distribuye bajo la licencia MIT (consulta el archivo `LICENSE`).