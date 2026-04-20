
#include <Arduino.h>
#include <ArduinoJson.h>   // instalar desde Library Manager: "ArduinoJson" by Benoit Blanchon

// ── Pines táctiles del ESP32 (número de GPIO, no alias T0/T3) ─
// T0 = GPIO4, T3 = GPIO15
#define TOUCH_PIN_1  4    // GPIO 4  (T0)
#define TOUCH_PIN_2  15   // GPIO 15 (T3)

// ── Umbrales de detección (ajustar según tu ESP32) ──────────
// Valor típico en reposo: ~60-80 | tocado: <40
#define TOUCH_THRESHOLD  700

// ── Parámetros de las colas ──────────────────────────────────
#define QUEUE_SIZE  10

// ── Período de muestreo de los sensores (ms) ─────────────────
#define SAMPLE_PERIOD_MS  200

// ── Estructura de dato que viaja por la cola ─────────────────
typedef struct {
  int     sensorId;      // 1 ó 2
  int     rawValue;      // lectura cruda del touch
  bool    touched;       // true si superó el umbral
  uint32_t timestamp;   // millis() en el momento de la lectura
} SensorData_t;

// ── Parámetros que recibe cada tarea de lectura ───────────────
typedef struct {
  int           sensorId;
  uint8_t       pin;        // GPIO number (ej: 4 para T0, 15 para T3)
  QueueHandle_t queue;
} ReaderParams_t;

// ── Parámetros que recibe cada tarea de escritura ────────────
typedef struct {
  int           sensorId;
  QueueHandle_t queue;
} WriterParams_t;

// ── Handles globales ─────────────────────────────────────────
QueueHandle_t queue1;
QueueHandle_t queue2;
SemaphoreHandle_t serialMutex;   // evita acceso concurrente al Serial


//  Función A: tarea de LECTURA del sensor

void taskSensorReader(void* pvParameters) {
  ReaderParams_t* p = (ReaderParams_t*) pvParameters;

  Serial.printf("[INIT] Reader tarea sensor %d lista (pin T%d)\n",
                p->sensorId, p->pin);

  SensorData_t data;
  data.sensorId = p->sensorId;

  for (;;) {
    // Leer valor capacitivo crudo
    data.rawValue  = touchRead(p->pin);
    data.touched   = (data.rawValue < TOUCH_THRESHOLD);
    data.timestamp = millis();

    // Insertar en cola (espera hasta 0 ms; si llena, descarta)
    BaseType_t sent = xQueueSend(p->queue, &data, 0);

    if (sent != pdTRUE) {
      // Cola llena: se puede loguear sin bloquear Serial
      // (aquí usamos un printf simple para no complicar el ejemplo)
      Serial.printf("[WARN] Cola sensor %d llena, dato descartado\n",
                    p->sensorId);
    }

    vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
  }
}


//  Función B: tarea de ESCRITURA serial en JSON

void taskSerialWriter(void* pvParameters) {
  WriterParams_t* p = (WriterParams_t*) pvParameters;

  Serial.printf("[INIT] Writer tarea sensor %d lista\n", p->sensorId);

  SensorData_t data;
  char jsonBuffer[128];

  for (;;) {
    // Esperar dato en la cola (bloqueante, sin timeout)
    if (xQueueReceive(p->queue, &data, portMAX_DELAY) == pdTRUE) {

      // Construir JSON
      StaticJsonDocument<128> doc;
      doc["sensor"]    = data.sensorId;
      doc["raw"]       = data.rawValue;
      doc["touched"]   = data.touched;
      doc["timestamp"] = data.timestamp;
      serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));

      // ── Sección crítica: acceso exclusivo al Serial ──────
      if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        Serial.println(jsonBuffer);
        xSemaphoreGive(serialMutex);
      }
      // ── Fin sección crítica ──────────────────────────────
    }
  }
}


void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== SensorFreeRTOS - ESP32 ===");

  // Crear colas
  queue1 = xQueueCreate(QUEUE_SIZE, sizeof(SensorData_t));
  queue2 = xQueueCreate(QUEUE_SIZE, sizeof(SensorData_t));

  // Crear mutex para el Serial
  serialMutex = xSemaphoreCreateMutex();

  if (!queue1 || !queue2 || !serialMutex) {
    Serial.println("[ERROR] No se pudo crear cola o mutex. Reiniciando...");
    ESP.restart();
  }

  // ── Parámetros para las tareas de lectura ────────────────
  // Se declaran static para que no se destruyan al salir de setup()
  static ReaderParams_t readerParams1 = { 1, TOUCH_PIN_1, queue1 };
  static ReaderParams_t readerParams2 = { 2, TOUCH_PIN_2, queue2 };

  // ── Parámetros para las tareas de escritura ──────────────
  static WriterParams_t writerParams1 = { 1, queue1 };
  static WriterParams_t writerParams2 = { 2, queue2 };

  // ── Crear tareas (misma función, distintos parámetros) ───
  xTaskCreatePinnedToCore(
    taskSensorReader,   // Función A
    "Reader1",
    4096,
    &readerParams1,     // parámetros sensor 1
    2,                  // prioridad
    NULL,
    1                   // núcleo 1
  );

  xTaskCreatePinnedToCore(
    taskSensorReader,   // Función A (reutilizada)
    "Reader2",
    4096,
    &readerParams2,     // parámetros sensor 2
    2,
    NULL,
    1
  );

  xTaskCreatePinnedToCore(
    taskSerialWriter,   // Función B
    "Writer1",
    4096,
    &writerParams1,     // parámetros writer 1
    1,                  // prioridad menor que readers
    NULL,
    0                   // núcleo 0
  );

  xTaskCreatePinnedToCore(
    taskSerialWriter,   // Función B (reutilizada)
    "Writer2",
    4096,
    &writerParams2,     // parámetros writer 2
    1,
    NULL,
    0
  );

  Serial.println("[OK] Todas las tareas creadas. Toca los pines T0 (GPIO4) y T3 (GPIO15).");
}

// ============================================================
//  loop() — vacío, FreeRTOS maneja todo
// ============================================================
void loop() {
  vTaskDelay(portMAX_DELAY);
}
