// visit http://localhost:9080 for website

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <uri/UriBraces.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASSWORD ""
#define WIFI_CHANNEL 6

WebServer server(80);

const int LED_GREEN = 26; 
const int LED_RED   = 27; 
const int ADC_PIN   = 34; 
const int BTN_PIN   = 18; 

const int SENSOR_THRESHOLD = 3000; 

// FreeRTOS objects
SemaphoreHandle_t serial;
SemaphoreHandle_t sensorCountingSem;
SemaphoreHandle_t buttonBinarySem;   
SemaphoreHandle_t ledState;   
QueueHandle_t eventQueue;

bool greenLedState = false;
bool redLedState   = false;

// System mode
enum SystemMode { MODE_NORMAL = 0, MODE_ALERT };
volatile SystemMode currentMode = MODE_NORMAL;

// Event types for the queue
enum EventType { EVT_SPEED_ALERT = 1, EVT_ESTOP_PRESS = 2, EVT_WEB_TOGGLE = 3 };

// Helper: safe Serial print with mutex
void safePrint(const char *msg) {
  if (xSemaphoreTake(serial, (TickType_t)pdMS_TO_TICKS(100))) {
    Serial.print(msg);
    xSemaphoreGive(serial);
  }
}

void safePrintf(const char *fmt, ...) {
  if (xSemaphoreTake(serial, (TickType_t)pdMS_TO_TICKS(100))) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    xSemaphoreGive(serial);
  }
}

// Update web page HTML (reads led state, so lock)
void sendHtml() {
  bool g, r;
  if (xSemaphoreTake(ledState, (TickType_t)pdMS_TO_TICKS(50))) {
    g = greenLedState;
    r = redLedState;
    xSemaphoreGive(ledState);
  } else { g = greenLedState; r = redLedState; } // fallback

  String response = R"(
    <!DOCTYPE html><html>
      <head>
        <title>ESP32 Web Server Demo</title>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <style>
          html { font-family: sans-serif; text-align: center; }
          body { display: inline-flex; flex-direction: column; }
          h1 { margin-bottom: 1.2em; } 
          h2 { margin: 0; }
          div { display: grid; grid-template-columns: 1fr 1fr; grid-template-rows: auto auto; grid-auto-flow: column; grid-gap: 1em; }
          .btn { background-color: #5B5; border: none; color: #fff; padding: 0.5em 1em;
                 font-size: 2em; text-decoration: none }
          .btn.OFF { background-color: #333; }
        </style>
      </head>
            
      <body>
        <h1>Ride System Status</h1>
        
        </div>
        <h2>ESTOPBUTTON</h2>
          <a href="/toggle/1" class="btn LED1">ESTOP</a>
        </div>
      </body>
    </html>
 )";
  response.replace("RED_TEXT", r ? "ON" : "OFF");
  response.replace("RED_CLASS", r ? "" : "OFF");

  server.send(200, "text/html", response);
}

// ISR (runs on button press interrupt)
//Hard Real Time Task
void IRAM_ATTR button_isr_handler(void* arg) {
    static uint32_t lastPress = 0;
    uint32_t now = xTaskGetTickCountFromISR();
    if(now - lastPress > pdMS_TO_TICKS(50)) {
        BaseType_t xHigherWoken = pdFALSE;
        xSemaphoreGiveFromISR(buttonBinarySem, &xHigherWoken);
        // Request context switch to the unblocked task, if needed:
        portYIELD_FROM_ISR(xHigherWoken);
        lastPress = now;
    }
}

// Web toggle handler: changes the Status on the website
void handleToggle() {
  String led = server.pathArg(0);
  int n = led.toInt();

  // Toggle the requested LED (protect via ledStateMutex)
  if (xSemaphoreTake(ledState, (TickType_t)pdMS_TO_TICKS(50))) {
    if (n == 2) {
      redLedState = !redLedState;
      digitalWrite(LED_RED, redLedState ? HIGH : LOW);
      safePrintf("Web toggled ESTOP -> %d\n", redLedState);
    }
    xSemaphoreGive(ledState);
  }

  EventType ev = EVT_WEB_TOGGLE;
  xQueueSend(eventQueue, &ev, 0);

  sendHtml();
}

// ---------- Tasks ----------

//Potentiometer mimics speed of the Ride System, Task measures this and triggers once the threshold is passed
//Hard Real-Time Task (Deadline ~1ms)
void SpeedMonitorTask(void *pvParameters) {
  const TickType_t period = pdMS_TO_TICKS(17);
  TickType_t lastWake = xTaskGetTickCount();
  int lastStateAbove = 0;

  for (;;) {
    vTaskDelayUntil(&lastWake, period);

    int val = analogRead(ADC_PIN);
    const int lowerThreshold = SENSOR_THRESHOLD - 100;

    if (!lastStateAbove && val >= SENSOR_THRESHOLD) {
      lastStateAbove = 1;
      if (sensorCountingSem) {
        xSemaphoreGive(sensorCountingSem);
      }
    
      EventType ev = EVT_SPEED_ALERT;
      xQueueSend(eventQueue, &ev, 0);

      safePrintf("SPEED SENSOR TOO FAST! adc=%d\n", val);
    } else if (lastStateAbove && val <= lowerThreshold) {
      lastStateAbove = 0;
      safePrintf("SPEED SENSOR WITHIN NORMAL LEVELS (adc=%d)\n", val);
    }
  }
}

//This task sees when the button is pressed it stops the system (Puts on ESTOP light)
// Hard Real-Time Task (Deadline ~1ms)
void EstopTask(void *pvParameters) {
  const TickType_t period = pdMS_TO_TICKS(10);
  TickType_t lastWake = xTaskGetTickCount();
  bool lastStableState = HIGH; 
  TickType_t debounceUntil = 0;

  TickType_t currentTime = pdTICKS_TO_MS( xTaskGetTickCount() );
  TickType_t previousTime = 0;

  for (;;) {
    vTaskDelayUntil(&lastWake, period);
    int raw = digitalRead(BTN_PIN);

    if (raw != lastStableState) {
      debounceUntil = xTaskGetTickCount() + pdMS_TO_TICKS(30);
      lastStableState = raw;
    } else {
      if (raw == LOW) {
        if (buttonBinarySem) {
          xSemaphoreGive(buttonBinarySem);
        }

        EventType ev = EVT_ESTOP_PRESS;
        xQueueSend(eventQueue, &ev, 0);

        safePrintf("Button pressed @ %lu \n", currentTime);
        previousTime = currentTime;
        currentTime = pdTICKS_TO_MS( xTaskGetTickCount() );

        while (digitalRead(BTN_PIN) == LOW) {
          vTaskDelay(pdMS_TO_TICKS(10));
        }
        safePrint("Button released\n");
      }
    }
  }
}

//Determines which task has occurred and adds it to the queue and gives the result of that Task.
//Hard Real-Time Task (Deadline ~1ms)
void EventTask(void *pvParameters) {
  TickType_t currentTime = pdTICKS_TO_MS( xTaskGetTickCount() );
  TickType_t previousTime = 0;
  EventType ev;
  for (;;) {

    if (xQueueReceive(eventQueue, &ev, portMAX_DELAY) == pdTRUE) {
      switch (ev) {
        case EVT_SPEED_ALERT:
        {
          safePrint("Event: SPEED_SENSOR_ALERT\n");
          if (xSemaphoreTake(ledState, (TickType_t)pdMS_TO_TICKS(100))) {
            if (currentMode == MODE_NORMAL) {
              currentMode = MODE_ALERT;
              greenLedState = false;
              redLedState = true;
            } else {
              currentMode = MODE_NORMAL;
              greenLedState = true;
              redLedState = false;
            }
            digitalWrite(LED_RED, redLedState ? HIGH : LOW);
            xSemaphoreGive(ledState);
          }
          
          safePrintf("Mode is now %s @ %lu \n", (currentMode==MODE_NORMAL)?"NORMAL":"ALERT" , currentTime);
          previousTime = currentTime;
          currentTime = pdTICKS_TO_MS( xTaskGetTickCount() );
        }
        break;

        case EVT_ESTOP_PRESS:
        {
          safePrint("Event: ESTOP_PRESS => toggling system mode \n ");
          if (xSemaphoreTake(ledState, (TickType_t)pdMS_TO_TICKS(100))) {
            if (currentMode == MODE_NORMAL) {
              currentMode = MODE_ALERT;
              greenLedState = false;
              redLedState = true;
            } else {
              currentMode = MODE_NORMAL;
              greenLedState = true;
              redLedState = false;
            }
            digitalWrite(LED_RED, redLedState ? HIGH : LOW);
            xSemaphoreGive(ledState);
          }
          
          safePrintf("Mode is now %s @ %lu \n", (currentMode==MODE_NORMAL)?"NORMAL":"ALERT" , currentTime);
          previousTime = currentTime;
          currentTime = pdTICKS_TO_MS( xTaskGetTickCount() );
        }
        break;

        case EVT_WEB_TOGGLE:
        {
          safePrintf("Event: Website ESTOP Pressed @ %lu\n", currentTime);
          if (xSemaphoreTake(ledState, (TickType_t)pdMS_TO_TICKS(100))) {
            if (currentMode == MODE_NORMAL) {
              currentMode = MODE_ALERT;
              greenLedState = false;
              redLedState = true;
            } else {
              currentMode = MODE_NORMAL;
              greenLedState = true;
              redLedState = false;
            }
            digitalWrite(LED_RED, redLedState ? HIGH : LOW);
            xSemaphoreGive(ledState);
          }
            safePrintf("Mode is now %s @ %lu \n", (currentMode==MODE_NORMAL)?"NORMAL":"ALERT" , currentTime);
            previousTime = currentTime;
            currentTime = pdTICKS_TO_MS( xTaskGetTickCount() );
        }
        break;

        default:
          safePrint("Event: UNKNOWN\n");
          break;
      }
    } 
  } 
}

//Shows that the system is functioning by blinking a Green LED
//Soft Real-Time Task (Deadline ~1000ms)
void SystemOnTask(void *pvParameters) {
  const TickType_t onDelay = pdMS_TO_TICKS(1000);
  const TickType_t offDelay = pdMS_TO_TICKS(1000);
  for (;;) {
    if (xSemaphoreTake(ledState, (TickType_t)pdMS_TO_TICKS(50))) {
      bool shouldBeOn = (currentMode == MODE_NORMAL);
      if (shouldBeOn) {
        greenLedState = !greenLedState;
        digitalWrite(LED_GREEN, greenLedState ? HIGH : LOW);
      } else {
        greenLedState = false;
        digitalWrite(LED_GREEN, LOW);
      }
      xSemaphoreGive(ledState);
    }
    vTaskDelay(onDelay);
  }
}

// ---------- Setup and loop ----------

void setup(void) {
  Serial.begin(115200);
  delay(100);

  // Initialize pins
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, LOW);

  pinMode(BTN_PIN, INPUT_PULLUP); 
  analogReadResolution(12); // 0..4095

  gpio_install_isr_service(0);

  serial = xSemaphoreCreateMutex();
  sensorCountingSem = xSemaphoreCreateCounting(10, 0);
  buttonBinarySem = xSemaphoreCreateBinary();
  ledState = xSemaphoreCreateMutex();

  eventQueue = xQueueCreate(10, sizeof(EventType));

  if (!serial || !sensorCountingSem || !buttonBinarySem || !eventQueue || !ledState) {
    Serial.println("Failed to create FreeRTOS primitives!");
    while (1) { delay(10); }
  }

  if (xSemaphoreTake(ledState, (TickType_t)pdMS_TO_TICKS(100))) {
    greenLedState = true;
    redLedState = false;
    digitalWrite(LED_GREEN, greenLedState ? HIGH : LOW);
    digitalWrite(LED_RED, redLedState ? HIGH : LOW);
    xSemaphoreGive(ledState);
  }

  xTaskCreatePinnedToCore(SpeedMonitorTask, "Sensor", 3072, NULL, 3, NULL, tskNO_AFFINITY);
  xTaskCreatePinnedToCore(EstopTask, "Button", 2048, NULL, 4, NULL, tskNO_AFFINITY);
  xTaskCreatePinnedToCore(EventTask, "Event", 4096, NULL, 3, NULL, tskNO_AFFINITY);
  xTaskCreatePinnedToCore(SystemOnTask, "SystemOn", 2048, NULL, 1, NULL, tskNO_AFFINITY);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
  Serial.print("Connecting to WiFi ");
  Serial.print(WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
    Serial.print(".");
  }
  Serial.println(" Connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  server.on("/", sendHtml);
  server.on(UriBraces("/toggle/{}"), []() {
    handleToggle();
  });

  server.begin();
  Serial.println("HTTP server started");
}

void loop(void) {
  server.handleClient();

  static TickType_t lastPrint = 0;
  if (xTaskGetTickCount() - lastPrint > pdMS_TO_TICKS(10000)) {
    lastPrint = xTaskGetTickCount();
    if (sensorCountingSem) {
      UBaseType_t count = uxSemaphoreGetCount(sensorCountingSem);
      safePrintf("Sensor over speed count: %u\n", (unsigned)count);
    }
  }

  delay(2);
}