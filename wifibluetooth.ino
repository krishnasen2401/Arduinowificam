#include <WiFi.h>
#include "BluetoothSerial.h"
#include <ArduinoJson.h>
#include "esp_camera.h"
// #define CAMERA_MODEL_M5STACK_V2_PSRAM
#define CAMERA_MODEL_M5STACK_WIDE
#include "camera_pins.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "fb_gfx.h"
#include "img_converters.h"
#define PART_BOUNDARY "123456789000000000000987654321"

// Check if Bluetooth is available
#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run `make menuconfig` to and enable it
#endif

// Check Serial Port Profile
#if !defined(CONFIG_BT_SPP_ENABLED)
#error Serial Port Profile for Bluetooth is not available or not enabled. It is only available for the ESP32 chip.
#endif

BluetoothSerial SerialBT;
bool stop_server_flag = false;

void sendStringOverBT(const String& data) {
  const char* charData = data.c_str();
  SerialBT.write((const uint8_t*)charData, strlen(charData)); // Sends the string over Bluetooth
  Serial.println("Data sent over Bluetooth:");
  Serial.println(data);
}

void endBluetooth() {
  Serial.println("Stopping Bluetooth...");
  SerialBT.flush();
  SerialBT.disconnect();
  SerialBT.end();
  Serial.println("Bluetooth stopped.");
}

void disconnectWifi(){
  Serial.println("[WiFi] Disconnecting from WiFi!");
    // This function will disconnect and turn off the WiFi (NVS WiFi data is kept)
    if (WiFi.disconnect(true, false)) {
      Serial.println("[WiFi] Disconnected from WiFi!");
      sendStringOverBT("[WiFi] Disconnected from WiFi!");
    }
}
void connectWifi(const char *ssid,const char *password){
  Serial.println();
  Serial.print("[WiFi] Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  int tryDelay = 500;
  int numberOfTries = 20;
  while (true) {
    Serial.println("Trying again attempts left"+String(numberOfTries)+"/20");
  switch (WiFi.status()) {
    case WL_NO_SSID_AVAIL: Serial.println("[WiFi] SSID not found"); break;
    case WL_CONNECT_FAILED:
      Serial.print("[WiFi] Failed - WiFi not connected! Reason: ");
      return;
      break;
    case WL_CONNECTION_LOST: Serial.println("[WiFi] Connection was lost"); break;
    case WL_CONNECTED:
      Serial.println("[WiFi] WiFi is connected! IP address"+WiFi.localIP().toString());
      sendStringOverBT("[WiFi] WiFi is connected! "+WiFi.localIP().toString());
      //  delay(1000);
      // startMinimalServer();
      return;
      break;
    case WL_DISCONNECTED: Serial.print("Wifi disconnected"); break;
    default:
      Serial.print("[WiFi] WiFi Status: ");
      Serial.println(WiFi.status());
      break; 
    } 
    delay(tryDelay);
    if (numberOfTries <= 0) {
      Serial.print("[WiFi] Failed to connect to WiFi!");
      sendStringOverBT("[WiFi] Failed to connect to WiFi!");
      // Use disconnect function to force stop trying to connect
      WiFi.disconnect();
      return;
    } else {
      numberOfTries--;
    }
  }    
  }

void setupCamera() {
    Serial.printf("Free heap before camera init: %d\n", heap_caps_get_free_size(MALLOC_CAP_8BIT));
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000; //20000000 10000000
  config.pixel_format = PIXFORMAT_JPEG;  // for streaming
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM; //
  config.jpeg_quality = 12;
  config.fb_count = 1;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA; // Use a smaller frame size
    config.jpeg_quality = 10;
    config.fb_count = 1; // Reduce frame buffer count
  } else {
    config.frame_size = FRAMESIZE_VGA; // Use an even smaller frame size
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

    if (!psramFound()) {
    Serial.println("PSRAM not found");
    return;
  }

  // Camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
Serial.printf("Free heap after camera init: %d\n", heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t stream_httpd = NULL;

static esp_err_t stream_handler(httpd_req_t *req){
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char * part_buf[64];
  Serial.println("Client connected");
  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if(res != ESP_OK){
    return res;
  }

  while(true){
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      if(fb->width > 400){
        if(fb->format != PIXFORMAT_JPEG){
          bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
          esp_camera_fb_return(fb);
          fb = NULL;
          if(!jpeg_converted){
            Serial.println("JPEG compression failed");
            res = ESP_FAIL;
          }
        } else {
          _jpg_buf_len = fb->len;
          _jpg_buf = fb->buf;
        }
      }
    }
    if(res == ESP_OK){
      size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if(fb){
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if(_jpg_buf){
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if(res != ESP_OK){
      break;
    }
    //Serial.printf("MJPG: %uB\n",(uint32_t)(_jpg_buf_len));
  }
  Serial.printf("Client disconnected");
  stop_server_flag = true;
  return res;
}

void startCameraServer(){
  endBluetooth();
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.lru_purge_enable=true;
  config.core_id=0;

  httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };
  
  //Serial.printf("Starting web server on port: '%d'\n", config.server_port);
esp_err_t err = httpd_start(&stream_httpd, &config);
if (err == ESP_OK) {
    Serial.println("Web server started successfully");
    httpd_register_uri_handler(stream_httpd, &index_uri);
} else {
    Serial.print("Failed to start web server, error code: ");
    Serial.println(err);
}
}

void stopCameraServer() {
    if (stream_httpd != NULL) {
        Serial.println("Stopping camera server...");
        esp_err_t err = httpd_stop(stream_httpd);
        Serial.println("After httpd_stop call");  // Debugging statement
        if (err == ESP_OK) {
            stream_httpd = NULL;
            Serial.println("Camera server stopped successfully");
            SerialBT.begin("m5stackesp32Cam");  // Bluetooth device name
            Serial.println("Restarted Bluetooth");
        } else {
            Serial.printf("Failed to stop camera server: %s\n", esp_err_to_name(err));
        }
    } else {
        Serial.println("Camera server handle is NULL, cannot stop server.");
    }
}

void setup() {
  Serial.begin(115200);
  delay(10);
  esp_log_level_set("*", ESP_LOG_DEBUG); // Set log level to DEBUG for all components
  if (psramFound()) {
    Serial.printf("Total PSRAM: %d bytes\n", ESP.getPsramSize());
    Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
  } else {
    Serial.println("PSRAM not found");
  }
  SerialBT.begin("m5stackesp32Cam");  //Bluetooth device name
  Serial.printf("The device with name m5stackesp32Cam is started.\nNow you can pair it with Bluetooth!\n");
  setupCamera();
  // WiFi.mode(WIFI_STA);

  // Register the SPP callback
    // esp_spp_register_callback(sppCallback);
    // esp_spp_init(ESP_SPP_MODE_CB);
}

// void sppCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
//     switch (event) {
//         case ESP_SPP_SRV_OPEN_EVT:  // Client connected
//             Serial.println("Client connected");
//             printDeviceAddress(param->srv_open.rem_bda);
//             break;
//         case ESP_SPP_CLOSE_EVT:  // Client disconnected
//             Serial.println("Client disconnected");
//             break;
//         default:
//             break;
//     }
// }

void printDeviceAddress(esp_bd_addr_t bda) {
    char bda_str[18];
    snprintf(bda_str, sizeof(bda_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    Serial.printf("Connected device address: %s\n", bda_str);
}

void loop() {
   if (stop_server_flag) {
        stopCameraServer();
        stop_server_flag = false;  // Reset the flag
    }

  if (SerialBT.available()) {
    String jsonString = SerialBT.readStringUntil('\n'); // Read until newline character
    jsonString.trim();
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, jsonString);

    if (error) {
      Serial.print("deserializeJson() failed: ");
      Serial.println(error.c_str());
      return;
    }
    const char *command=doc["command"];
    if (strcmp(command, "wificonnect") == 0) {
    const char *wifi = doc["wifi"];
    const char *password = doc["password"];
    connectWifi(wifi,password);
    } else if (strcmp(command, "wifidisconnect") == 0) {
    disconnectWifi();
    }else if (strcmp(command, "startcamera") == 0) {
      if (WiFi.status() == WL_CONNECTED) {
            startCameraServer();
      } else {
        Serial.println("WiFi not connected or server already running.");
      }
    } else {
    Serial.println("Command does not match");
  }
  }
}

