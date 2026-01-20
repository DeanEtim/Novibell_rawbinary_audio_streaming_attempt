#include <Arduino.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <WiFiProv.h>
#include "MyWebPage.h"  // Test web page for streaming

// I2S configuration
#define I2S_SCK_PIN 13
#define I2S_WS_PIN 14
#define I2S_SD_PIN 15
#define I2S_LR_IO 26
#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE 44100
#define BUFFER_COUNT 10
#define BUFFER_SIZE 1024

// Pin definitions
const int buttonPin = 4;
const int led_indicator = 5;
const int resetPin = 19;

// WiFi and system state variables
bool isI2SInitialized = false;
bool isServerInitialized = false;
unsigned long previousMillis = 0;
bool ledState = LOW;

// Web server on port 80
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Buffer for audio sample
int16_t audioBuffer[BUFFER_SIZE];

// Control variables
bool wsClientConnected = false;
bool audioStreamingActive = false;

// Task handle for audio processing
TaskHandle_t audioTaskHandle = NULL;

//pushbutton parameters
unsigned long DEBOUNCE_DELAY = 50;            // milliseconds
volatile bool buttonState = HIGH;             // Current buttonstate
volatile bool lastButtonState = HIGH;         // Previous buttonstate
volatile unsigned long lastDebounceTime = 0;  // Last time buttonstate changed

// WiFi Provisioning
const char *service_name = "PROV_NoviBell";
const char *pop = "abcd1234";  // Proof of Possession
char ssid[32] = "";
char password[64] = "";

// Function prototypes
void setupI2S();
void setupWebServer();
void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
void handleWiFiEvent(WiFiEvent_t event);
void blinkLED();
void startWiFiProvisioning();
void audioTask(void *parameter);

void setup() {
  // Initialize serial
  Serial.begin(115200);

  // Initialize GPIO pins
  pinMode(I2S_SCK_PIN, OUTPUT);
  pinMode(I2S_WS_PIN, OUTPUT);
  pinMode(I2S_SD_PIN, INPUT);
  pinMode(I2S_LR_IO, OUTPUT);
  pinMode(led_indicator, OUTPUT);
  pinMode(resetPin, INPUT_PULLUP);
  pinMode(buttonPin, INPUT_PULLUP);

  digitalWrite(I2S_LR_IO, LOW);
  digitalWrite(led_indicator, LOW);

  // Check WiFi credentials
  WiFi.onEvent(SysProvEvent);

  if (WiFi.SSID().length() > 0) {
    WiFi.begin();
  } else {
    Serial.println("Start Provisioning");
    startWiFiProvisioning();
  }
}  // end setup

void loop() {
  // Handle LED indicator
  blinkLED();

  if (WiFi.status() == WL_CONNECTED) {
    checkButton();
  }

  // Factory reset
  if (digitalRead(resetPin) == LOW) {
    delay(3000);  // Wait to confirm long press
    if (digitalRead(resetPin) == LOW) {
      Serial.println("Factory reset");
      wifi_prov_mgr_reset_provisioning();
      ESP.restart();
    }
  }
  delay(1);  // Short delay to prevent CPU hogging
}  // end mainloop

void startWiFiProvisioning() {
  // Start provisioning with BLE transport
  WiFiProv.beginProvision(WIFI_PROV_SCHEME_BLE, WIFI_PROV_SCHEME_HANDLER_FREE_BLE,
                          WIFI_PROV_SECURITY_1, pop, service_name);
}  // end startWiFiProvisioning()

void setupI2S() {
  // I2S configuration
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,  // Changed to 16-bit
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,   // Using the LEFT channel
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = BUFFER_COUNT,
    .dma_buf_len = BUFFER_SIZE,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  // I2S pin configuration
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK_PIN,
    .ws_io_num = I2S_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD_PIN
  };

  // Install and configure I2S
  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("Failed installing I2S driver: %d\n", err);
    return;
  }

  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("Failed setting I2S pins: %d\n", err);
    return;
  }

  i2s_start(I2S_PORT);
  Serial.println("I2S driver installed and started");
}  // end setupI2S()

void audioTask(void *parameter) {
  setupI2S();

  size_t bytesRead = 0;
  while (true) {
    // Only process audio if streaming is active and we have connected clients
    if (audioStreamingActive && wsClientConnected) {
      esp_err_t result = i2s_read(I2S_PORT, &audioBuffer, sizeof(audioBuffer), &bytesRead, portMAX_DELAY);

      if (result == ESP_OK && bytesRead > 0) {
        // Send binary data directly
        ws.binaryAll((const char *)audioBuffer, bytesRead);
      }

      else if (result != ESP_OK) {
        Serial.printf("I2S read error: %d\n", result);
      }
    }  // end if

    else {
      delay(100);
    }
  }  // end while loop
}  // end audio task()

void setupWebServer() {
  if (isServerInitialized) {
    Serial.println("Web server already initialized, skipping");
    return;
  }
  // Setup WebSocket
  ws.onEvent(onWebSocketEvent);
  server.addHandler(&ws);
  // Initialize web page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", test_page);
  });

  // Start server
  server.begin();
  Serial.println("Web server successfully started");
  isServerInitialized = true;

  // Create audio processing task on core 1
  xTaskCreatePinnedToCore(
    audioTask,         // Task function
    "audioTask",       // Task name
    10000,             // Stack size
    NULL,              // Parameters
    1,                 // Priority
    &audioTaskHandle,  // Task handle
    1                  // Core 1
  );
}  // end setupWebServer()

void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      wsClientConnected = true;
      break;

    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
      if (ws.count() == 0) {
        wsClientConnected = false;
        audioStreamingActive = false;  // Stop streaming if all clients disconnect
      }
      break;

    case WS_EVT_DATA:
      // Handle incoming WebSocket data (play/stop commands)
      if (len) {
        // Convert payload to a null-terminated string
        char *message = (char *)malloc(len + 1);
        memcpy(message, data, len);
        message[len] = '\0';

        // Process the command
        String command = String(message);
        if (command == "play") {
          audioStreamingActive = true;
          Serial.println("Audio streaming started");
        } else if (command == "stop") {
          audioStreamingActive = false;
          Serial.println("Audio streaming stopped");
        }
        free(message);
      }
      break;
  }
}  // end onWeSocketEvent()

void SysProvEvent(arduino_event_t *sys_event) {
  switch (sys_event->event_id) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("Connected to WiFi network: ");
      Serial.println(WiFi.SSID());
      Serial.print("IP Address: ");
      Serial.println(WiFi.localIP());

      // Initialize web server (which will also create the audio task)
      setupWebServer();
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("Disconnected from WiFi network");
      wsClientConnected = false;
      audioStreamingActive = false;
      break;

    case ARDUINO_EVENT_PROV_CRED_RECV:
      Serial.println("Received WiFi credentials");
      strncpy(ssid, (const char *)sys_event->event_info.prov_cred_recv.ssid, sizeof(ssid));
      strncpy(password, (const char *)sys_event->event_info.prov_cred_recv.password, sizeof(password));
      break;

    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
      Serial.println("Provisioning successful!");
      break;

    case ARDUINO_EVENT_PROV_CRED_FAIL:
      Serial.println("Provisioning failed!");
      break;

    case ARDUINO_EVENT_PROV_END:
      Serial.println(F("Provisioning complete"));
      break;
  }
}  // end SysProvEvent()

void blinkLED() {
  unsigned long currentMillis = millis();
  unsigned long interval;

  // Set interval based on WiFi connection status
  interval = (WiFi.status() == WL_CONNECTED) ? 4000 : 300;

  // Check if it's time to blink
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;

    // Blink pattern: ON for 300ms then OFF
    digitalWrite(led_indicator, HIGH);
    delay(100);
    digitalWrite(led_indicator, LOW);
  }
}  // end blinkLED()

void checkButton() {
  // Read Current button_state
  bool current_reading = digitalRead(buttonPin);

  // If the button state has changed
  if (current_reading != lastButtonState) {
    // Reset the debounce timer
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    // Button_state Changed?
    if (current_reading != buttonState) {
      buttonState = current_reading;

      if (buttonState == LOW) {
        Serial.println("Button pressed!");
        // doSomething();
      }
    }
  }
  lastButtonState = current_reading;
}  // end checkButton()
