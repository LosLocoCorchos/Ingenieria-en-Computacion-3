/*
 * Node_Gas_CO2.ino  -  NODO FINAL: MQ2 (Keyes) + SCD40 + OLED 128x64 + Buzzer
 * ------------------------------------------------------------------
 * Placa: ESP32-C3 Super Mini
 *
 * Pines usados (ver diagramas de cableado):
 *   I2C   SDA -> GPIO4   |  I2C SCL -> GPIO5   (SCD40 + OLED, mismo bus)
 *   MQ2   AO  -> divisor R1(10k)/R2(15k) -> GPIO3 (ADC)
 *   Buzzer    -> GPIO10 -> R3(1k) -> Base transistor NPN -> buzzer a 5V
 *
 * IMPORTANTE (Arduino IDE, placa ESP32-C3 Super Mini):
 *   Herramientas -> USB CDC On Boot -> "Enabled"
 *   (si no activás esto, el Monitor Serie no va a mostrar nada, porque
 *   esta placa usa el USB nativo del chip, no un conversor USB-serie aparte)
 *
 * Librerías (Gestor de Librerías Arduino IDE):
 *   - painlessMesh
 *   - ArduinoJson
 *   - Sensirion I2C SCD4x   (by Sensirion) — usa la clase SensirionI2cScd4x
 *   - U8g2 (by oliver)
 *
 * Display: si usás U8G2_SSD1306_128X64_NONAME_F_HW_I2C y ves ruido/basura
 * en pantalla con el cableado ya verificado, tu módulo probablemente trae
 * el controlador SH1106 en vez de SSD1306 (pasa seguido con estos OLED
 * genéricos) — cambiá esa única línea por
 * U8G2_SH1106_128X64_NONAME_F_HW_I2C y recompilá, sin tocar nada más.
 *
 * Comportamiento:
 *   - Lee MQ2 (gas/humo) y SCD40 (CO2/temp/humedad) cada SENSOR_READ_MS.
 *   - Guarda historial de las últimas 128 lecturas de CO2 y de gas.
 *   - Dibuja en el OLED 128x64: valores numéricos arriba + dos gráficos
 *     de tendencia (CO2 y gas) abajo.
 *   - Si CO2 o gas superan su umbral, enciende el buzzer (vía transistor)
 *     y muestra "ALARMA" invertido en el encabezado del display.
 *   - Envía cada lectura por la mesh (painlessMesh, broadcast) para que
 *     el Gateway la reenvíe a AURA por MQTT.
 */

#include <painlessMesh.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <SensirionI2cScd4x.h>
#include <U8g2lib.h>

// ---------------- Debe coincidir EXACTO con el Gateway ----------------
#define MESH_PREFIX     "ESP32_MESH_AURA"
#define MESH_PASSWORD   "meshpass123"
#define MESH_PORT       5555

// ---------------- Pines (ESP32-C3 Super Mini) ----------------
#define I2C_SDA       4
#define I2C_SCL       5
#define MQ2_PIN       3      // GPIO3, ADC1
#define BUZZER_PIN    10     // controla la base del transistor vía R3

// ---------------- Display ----------------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define FONT_SMALL    u8g2_font_6x10_tf

// Constructor en modo "full buffer" + I2C por hardware (usa el Wire que
// ya inicializamos con los pines SDA/SCL elegidos). Si tu módulo resulta
// ser SH1106, cambiá SOLO esta línea (ver nota arriba en el encabezado).
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// ---------------- Umbrales de alarma (AJUSTAR tras calibrar) ----------------
#define GAS_ALARM_RAW   1800   // lectura ADC cruda (0-4095, ya pasada por el divisor)
#define CO2_ALARM_PPM   1500

// ---------------- Rangos para escalar los gráficos ----------------
#define CO2_GRAPH_MIN   400
#define CO2_GRAPH_MAX   3000
#define GAS_GRAPH_MIN   0
#define GAS_GRAPH_MAX   4095

#define SENSOR_READ_MS  5000   // SCD40 mide como mínimo cada 5s en modo periódico

// ---------------- Debug por Serial ----------------
#define DEBUG_SENSORS   true   // poné en false para silenciar el log detallado

// ---------------- Historial para los gráficos (uno por pixel de ancho) ----
#define GRAPH_LEN       SCREEN_WIDTH   // 128 muestras -> ~10.6 min de historial
uint16_t co2History[GRAPH_LEN];
uint16_t gasHistory[GRAPH_LEN];

Scheduler        userScheduler;
painlessMesh      mesh;
SensirionI2cScd4x scd4x;

bool scd4xOk      = false;
bool alarmActive  = false;

// Cuántos ciclos de sendSensorData() ignoramos tras el arranque antes de
// confiar en la lectura del SCD40 (evita el valor basura de la primera
// medición, sin depender de getDataReadyStatus() que resultó poco fiable
// en esta versión de la librería).
uint8_t scd4xWarmupCycles = 2;

// ---------------- Contadores de diagnóstico ----------------
uint32_t cycleCount        = 0;  // ciclos de sendSensorData() totales
uint32_t co2ReadOkCount    = 0;  // lecturas de SCD40 exitosas
uint32_t co2ReadFailCount  = 0;  // lecturas de SCD40 fallidas (error o warm-up)
uint16_t lastValidCo2      = 0;  // último CO2 válido conocido (para log)

// -------------------------------------------------------------------------
// Escanea el bus I2C y lista qué direcciones responden. Útil para
// confirmar de entrada si el SCD40 (0x62) y el OLED (0x3C, o el que
// hayas configurado) están bien cableados antes de seguir debuggeando.
void scanI2C() {
  Serial.println("Escaneando bus I2C...");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  Dispositivo I2C encontrado en 0x%02X\n", addr);
      found++;
    }
  }
  if (found == 0) {
    Serial.println("  No se encontró ningún dispositivo I2C (revisar SDA/SCL/alimentación)");
  } else {
    Serial.printf("  Total: %d dispositivo(s) I2C detectado(s)\n", found);
  }
}

// -------------------------------------------------------------------------
// Log detallado de cada lectura: valores crudos y convertidos de MQ2 y
// SCD40, con timestamp. Se puede apagar con DEBUG_SENSORS en false.
void printDebug(bool co2Ready, uint16_t co2, float temperature, float humidity,
                 int gasRaw, bool alarm) {
#if DEBUG_SENSORS
  float gasVoltage = gasRaw * (3.3f / 4095.0f);
  Serial.println("---------- Lectura de sensores ----------");
  Serial.printf("t = %lu ms | ciclo #%lu\n", millis(), (unsigned long) cycleCount);
  Serial.printf("MQ2   -> raw: %4d  (%.2fV en GPIO%d, ya pasado por el divisor)\n",
                gasRaw, gasVoltage, MQ2_PIN);
  if (co2Ready) {
    Serial.printf("SCD40 -> CO2: %4u ppm | Temp: %.1f C | Hum: %.1f %%\n",
                  co2, temperature, humidity);
  } else {
    Serial.printf("SCD40 -> sin dato nuevo este ciclo (último válido: %u ppm)\n", lastValidCo2);
  }
  Serial.printf("SCD40 -> lecturas OK: %lu, fallidas: %lu\n",
                (unsigned long) co2ReadOkCount, (unsigned long) co2ReadFailCount);
  Serial.printf("Alarma: %s  (umbrales: gas>=%d, CO2>=%d ppm)\n",
                alarm ? "ACTIVA" : "OK", GAS_ALARM_RAW, CO2_ALARM_PPM);
  Serial.println("------------------------------------------");
#endif
}

// -------------------------------------------------------------------------
// Traduce un código de error del SCD40 a texto legible usando la función
// que trae la propia librería de Sensirion (más confiable que adivinar
// qué significa el número hexadecimal a ojo).
void printSCD4xError(const char *context, uint16_t err) {
  char errorMessage[64];
  errorToString(err, errorMessage, sizeof(errorMessage));
  Serial.printf("%s (0x%x): %s\n", context, err, errorMessage);
}

// -------------------------------------------------------------------------
void beepAlarm(bool on) {
  digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
}

// -------------------------------------------------------------------------
int readMQ2Raw() {
  const int N = 10;
  int samples[N];
  long sum = 0;
  int minV = 4095, maxV = 0;

  for (int i = 0; i < N; i++) {
    samples[i] = analogRead(MQ2_PIN);
    sum += samples[i];
    if (samples[i] < minV) minV = samples[i];
    if (samples[i] > maxV) maxV = samples[i];
    delay(5);
  }
  int avg = (int)(sum / N);

#if DEBUG_SENSORS
  Serial.print("MQ2 muestras crudas: ");
  for (int i = 0; i < N; i++) {
    Serial.print(samples[i]);
    if (i < N - 1) Serial.print(", ");
  }
  Serial.printf("  | min=%d max=%d prom=%d (ruido=%d)\n", minV, maxV, avg, maxV - minV);
  if (minV == 0 || maxV >= 4090) {
    Serial.println("  Aviso: lectura pegada al piso/techo del ADC — revisar el divisor resistivo o el cableado del AO");
  }
#endif

  return avg;
}

// -------------------------------------------------------------------------
void pushHistory(uint16_t *buf, uint16_t newVal) {
  memmove(buf, buf + 1, (GRAPH_LEN - 1) * sizeof(uint16_t));
  buf[GRAPH_LEN - 1] = newVal;
}

// -------------------------------------------------------------------------
// Dibuja un gráfico de línea dentro de la franja [yTop, yTop+h) usando
// todo el ancho de la pantalla (1 muestra = 1 columna de pixel).
void drawGraph(uint16_t *buf, int yTop, int h, int minV, int maxV) {
  int prevX = -1, prevY = 0;
  for (int x = 0; x < GRAPH_LEN; x++) {
    int v = buf[x];
    if (v < minV) v = minV;
    if (v > maxV) v = maxV;
    int y = yTop + (h - 1) - (long)(v - minV) * (h - 1) / (maxV - minV);
    if (prevX >= 0) {
      u8g2.drawLine(prevX, prevY, x, y);
    }
    prevX = x;
    prevY = y;
  }
  for (int x = 0; x < GRAPH_LEN; x += 4) {
    u8g2.drawPixel(x, yTop + h - 1);
  }
}

// -------------------------------------------------------------------------
void updateDisplay(bool co2Ready, uint16_t co2, int gasRaw, bool alarm) {
  char buf[24];

  u8g2.clearBuffer();
  u8g2.setFont(FONT_SMALL);

  if (alarm) {
    u8g2.setDrawColor(1);
    u8g2.drawBox(0, 0, SCREEN_WIDTH, 9);        // franja blanca de fondo
    u8g2.setDrawColor(0);                       // texto en negro sobre blanco
    u8g2.setCursor(2, 8);
    u8g2.print("*** ALARMA - GAS/CO2 ***");
    u8g2.setDrawColor(1);                       // volver a blanco para el resto
  } else {
    u8g2.setDrawColor(1);
    u8g2.setCursor(0, 8);
    if (co2Ready) snprintf(buf, sizeof(buf), "CO2:%4u", co2);
    else          snprintf(buf, sizeof(buf), "CO2:----");
    u8g2.print(buf);

    u8g2.setCursor(70, 8);
    snprintf(buf, sizeof(buf), "Gas:%4d", gasRaw);
    u8g2.print(buf);
  }

  drawGraph(co2History, 11, 24, CO2_GRAPH_MIN, CO2_GRAPH_MAX);
  drawGraph(gasHistory, 38, 24, GAS_GRAPH_MIN, GAS_GRAPH_MAX);

  u8g2.sendBuffer();
}

// -------------------------------------------------------------------------
void sendSensorData() {
  uint16_t co2 = 0;
  float temperature = 0.0f, humidity = 0.0f;
  bool co2Ready = false;

  cycleCount++;

  if (scd4xOk) {
    if (scd4xWarmupCycles > 0) {
      scd4xWarmupCycles--;
      Serial.println("SCD40: estabilizando tras el arranque, se omite este ciclo");
    } else {
      uint16_t err = scd4x.readMeasurement(co2, temperature, humidity);

      // Reintento inmediato ante fallo puntual de la transacción I2C
      // (p.ej. un corte de clock-stretching aislado), antes de darlo
      // por fallido para este ciclo.
      if (err != 0) {
        delay(50);
        err = scd4x.readMeasurement(co2, temperature, humidity);
      }

      co2Ready = (err == 0 && co2 != 0);
      if (err != 0) {
        printSCD4xError("Error leyendo SCD40", err);
        co2ReadFailCount++;
      } else {
        co2ReadOkCount++;
        lastValidCo2 = co2;
      }
    }
  }

  int gasRaw = readMQ2Raw();

  bool alarm = (gasRaw >= GAS_ALARM_RAW) || (co2Ready && co2 >= CO2_ALARM_PPM);
  alarmActive = alarm;
  beepAlarm(alarm);

  pushHistory(co2History, co2Ready ? co2 : (co2History[GRAPH_LEN - 1]));
  pushHistory(gasHistory, (uint16_t) gasRaw);

  printDebug(co2Ready, co2, temperature, humidity, gasRaw, alarm);
  updateDisplay(co2Ready, co2, gasRaw, alarm);

  StaticJsonDocument<256> doc;
  doc["type"]    = "multi_sensor";
  doc["gas_raw"] = gasRaw;
  doc["alarm"]   = alarm;
  if (co2Ready) {
    doc["co2"]         = co2;
    doc["temperature"] = temperature;
    doc["humidity"]    = humidity;
  }

  String msg;
  serializeJson(doc, msg);
  mesh.sendBroadcast(msg);
  Serial.printf("Enviado: %s\n", msg.c_str());
}

Task taskSendSensor(TASK_SECOND * (SENSOR_READ_MS / 1000.0), TASK_FOREVER, &sendSensorData);

// -------------------------------------------------------------------------
void receivedCallback(uint32_t from, String &msg) {
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, msg) != DeserializationError::Ok) return;

  const char* action = doc["action"];
  if (!action) return;

  if (strcmp(action, "silence") == 0) {
    beepAlarm(false);
    Serial.println("Buzzer silenciado manualmente desde AURA");
  }
}

// -------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  for (int i = 0; i < GRAPH_LEN; i++) {
    co2History[i] = CO2_GRAPH_MIN;
    gasHistory[i] = GAS_GRAPH_MIN;
  }

  analogReadResolution(12);
  Wire.begin(I2C_SDA, I2C_SCL);

  // El SCD40 usa clock stretching prolongado al armar la respuesta de
  // readMeasurement() (9 bytes). A 100kHz/timeout por defecto, el ESP32
  // puede cortar la lectura antes de tiempo ("Not enough data received").
  // Bajamos la velocidad del bus y extendemos el timeout para darle
  // margen de sobra al sensor.
  Wire.setClock(50000);     // 50kHz en vez de los 100kHz por defecto
  Wire.setTimeOut(200);     // 200ms de timeout por transacción (ms)

  scanI2C();

  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(FONT_SMALL);
  u8g2.setCursor(0, 10);
  u8g2.print("Iniciando sensores...");
  u8g2.sendBuffer();

  scd4x.begin(Wire, SCD41_I2C_ADDR_62);  // el SCD40 usa la misma dirección 0x62

  // Secuencia de arranque limpio recomendada por Sensirion. Cada comando
  // necesita un tiempo mínimo antes de que el sensor acepte el próximo:
  // si no se respeta, el comando siguiente devuelve error (NACK) aunque
  // el sensor esté perfectamente cableado. Logueamos cada paso por
  // separado para saber exactamente cuál falla si algo sigue mal.
  uint16_t e;

  e = scd4x.wakeUp();
  if (e != 0) printSCD4xError("  wakeUp", e);
  delay(30);

  e = scd4x.stopPeriodicMeasurement();
  if (e != 0) printSCD4xError("  stopPeriodicMeasurement", e);
  delay(500);

  e = scd4x.reinit();
  if (e != 0) printSCD4xError("  reinit", e);
  delay(30);

  // Leer el número de serie es una forma extra de confirmar que el chip
  // responde de verdad (no solo que contesta al ping de dirección I2C).
  uint64_t serialNumber = 0;
  e = scd4x.getSerialNumber(serialNumber);
  if (e != 0) {
    printSCD4xError("  getSerialNumber", e);
  } else {
    Serial.printf("  SCD40 serial: 0x%llX\n", serialNumber);
  }

  uint16_t err = scd4x.startPeriodicMeasurement();
  scd4xOk = (err == 0);
  if (!scd4xOk) {
    printSCD4xError("Error iniciando SCD40", err);
  } else {
    Serial.println("SCD40 iniciado correctamente, midiendo cada 5s");
  }

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);
  mesh.onReceive(&receivedCallback);

  userScheduler.addTask(taskSendSensor);
  taskSendSensor.enable();

  Serial.printf("Nodo Gas/CO2 iniciado. ID: %u\n", (uint32_t) mesh.getNodeId());
}

void loop() {
  mesh.update();
}
