# Nodo de Monitoreo de Gas, CO₂ y Clima — ESP32-C3 Super Mini

Este proyecto implementa un nodo final IoT basado en el microcontrolador **ESP32-C3 Super Mini**. El dispositivo mide concentraciones de gas/humo y CO₂ en tiempo real junto con temperatura y humedad, despliega la información e historial de tendencias en una pantalla OLED local, activa un sistema de alarma sonora/visual y retransmite las lecturas vía red **painlessMesh** hacia un Gateway central (para su posterior procesamiento en AURA mediante MQTT).

---

## 🛠️ Hardware Necesario

1. **Microcontrolador:** ESP32-C3 Super Mini.
2. **Sensor de CO₂ / Temperatura / Humedad:** Sensirion SCD40 (o SCD41) — Dirección I2C `0x62`.
3. **Sensor de Gas / Humo:** MQ2 (Módulo Keyes).
4. **Display OLED:** 128x64 píxeles (I2C, controlador SSD1306 o SH1106) — Dirección I2C habitual `0x3C`.
5. **Alarma Sonora:** Buzzer pasivo/activo alimentado a 5V.
6. **Componentes Electrónicos Auxiliares:**
   * Divisor de tensión para lectura analógica MQ2: $R_1 = 10\text{ k}\Omega$, $R_2 = 15\text{ k}\Omega$.
   * Transistor NPN (p. ej., 2N2222 o similar) para control del buzzer con $R_3 = 1\text{ k}\Omega$ en base.

---

## 🔌 Diagrama de Conexiones y Pines (Pinout)

| Componente | Pin del Componente | Pin ESP32-C3 | Descripción / Notas |
| :--- | :--- | :--- | :--- |
| **Bus I2C** (SCD40 + OLED) | SDA | **GPIO 4** | Bus I2C compartido para display y sensor de CO₂. |
| **Bus I2C** (SCD40 + OLED) | SCL | **GPIO 5** | Bus I2C compartido para display y sensor de CO₂. |
| **Sensor MQ2** | AO (Salida Analógica) | **GPIO 3** | Entrada ADC1. **Atención:** Requiere divisor $10\text{k}\Omega / 15\text{k}\Omega$. |
| **Buzzer** | Base Transistor NPN | **GPIO 10** | Control digital vía resistencia de $1\text{ k}\Omega$ en la base. |

> ⚠️️ **Nota de seguridad del ADC:** La salida analógica del sensor MQ2 entrega un rango de $0\text{ V} - 5\text{ V}$. El divisor resistivo ($R_1=10\text{k}\Omega$, $R_2=15\text{k}\Omega$) atenúa la señal para no exceder los $3.3\text{ V}$ soportados por el GPIO3 del ESP32-C3.

---

## ⚙️ Configuración en Arduino IDE

Al programar la placa **ESP32-C3 Super Mini**, asegúrate de aplicar la siguiente configuración obligatoria en el menú de la IDE:

* **Placa:** ESP32C3 Dev Module (o ESP32-C3 Super Mini si la versión de tu core lo incluye).
* **USB CDC On Boot:** **`Enabled`**  
  *(⚠️ **CRÍTICO:** Como esta placa utiliza el puerto USB nativo del SoC y carece de conversor USB-Serie dedicado, si no activas esta opción el Monitor Serie no mostrará ningún mensaje de salida).*
* **Velocidad del Monitor Serie:** `115200 baudios`.

---

## 📚 Librerías Requeridas

Instala las siguientes librerías desde el **Gestor de Librerías de Arduino IDE**:

1. **painlessMesh** (por Coarray / painlessMesh team) — Gestión de red Mesh.
2. **ArduinoJson** (por Benoit Blanchon) — Serialización de mensajes JSON (versión 6.x).
3. **Sensirion I2C SCD4x** (por Sensirion) — Control de sensores de CO₂ SCD40/SCD41.
4. **U8g2** (por Oliver Kraus) — Control del display OLED gráfico.

---

## ⚙️ Parámetros y Red Mesh

### Configuración de Red
Los credenciales de la red Mesh deben coincidir exactamente con la configuración asignada al Gateway:

```cpp
#define MESH_PREFIX     "ESP32_MESH_AURA"
#define MESH_PASSWORD   "meshpass123"
#define MESH_PORT       5555
```

### Umbrales de Alarma y Rangos
```cpp
#define GAS_ALARM_RAW   1800  // Valor de lectura ADC cruda (rango 0-4095)
#define CO2_ALARM_PPM   1500  // Umbral de CO2 en partes por millón (PPM)
```

---

## 🖥️ Interfaz Gráfica y Compatibilidad OLED

El firmware reserva un buffer de 128 muestras para mantener un historial dinámico que dibuja dos gráficos en la pantalla de $128\times64$ píxeles:

1. **Parte Superior:** Lecturas numéricas puntuales de CO₂ (PPM) y Gas (ADC crudo). Si se gatilla una alarma, la franja superior se invierte a color blanco mostrando `*** ALARMA - GAS/CO2 ***`.
2. **Sección Central (Gráfico 1):** Histórico de CO₂ ($400 - 3000\text{ PPM}$).
3. **Sección Inferior (Gráfico 2):** Histórico de Gas / Humo ($0 - 4095$ ADC).

### 🐛 Solución a problemas de visualización (Noise / Basura en Pantalla)
Muchos módulos OLED genéricos marcados como SSD1306 incluyen en realidad el controlador **SH1106**. Si observas distorsión gráfica:
* En el código, reemplaza la línea:
  ```cpp
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
  ```
* Por el constructor equivalente SH1106:
  ```cpp
  U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
  ```

---

## 📡 Protocolo de Datos (JSON)

### 1. Mensaje de Telemetría (Broadcast a la Mesh)
Cada `5000 ms` (5 segundos), el nodo emite una trama JSON con las mediciones actuales:

```json
{
  "type": "multi_sensor",
  "gas_raw": 850,
  "alarm": false,
  "co2": 650,
  "temperature": 22.5,
  "humidity": 45.2
}
```

### 2. Control de Alarma desde el Gateway / AURA
El nodo escucha comandos entrantes en la red Mesh. Para silenciar remotamente el buzzer activo, se debe enviar una trama JSON dirigida o en broadcast con el comando de silenciado:

```json
{
  "action": "silence"
}
```

---

## 🔍 Diagnóstico y Tolerancia a Fallos Incorporada

* **Optimización I2C para SCD40:** El bus I2C funciona configurado a `50 kHz` con un *timeout* extendido de `200 ms` para tolerar el *clock-stretching* prolongado del sensor Sensirion.
* **Ciclos de Warm-up:** Se descartan las dos primeras mediciones iniciales del SCD40 tras el arranque para garantizar estabilidad en las lecturas almacenadas.
* **Escaneo Automático de Bus:** Al iniciar el microcontrolador, la función `scanI2C()` verifica por consola Serie la presencia activa de los dispositivos conectados.