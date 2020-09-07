#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h> 
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <RtcDS3231.h> //https://github.com/Makuna/Rtc.git
#include "esp_task_wdt.h"
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <string.h>
#include <SPIFFS.h>

const char* ssid = "ssid";
const char* password = "password";

AsyncWebServer server(80);
// Set your Static IP address
IPAddress local_IP(192, 168, 15, 20);
IPAddress gateway(192, 168, 15, 11);
IPAddress subnet(255, 255, 0, 0);
IPAddress primaryDNS(8, 8, 8, 8); // optional
IPAddress secondaryDNS(8, 8, 4, 4); // optional
String header;

#define PINO_DIM    26
#define PINO_ZC     27 
#define ALARM_PIN     4
#define maxBrightness 800
#define minBrightness 7500
#define INTERVAL_TRIGGER_TRIAC 20
#define IDLE -1
#define countof(a) (sizeof(a) / sizeof(a[0]))

hw_timer_t * timerToPinHigh;
hw_timer_t * timerToPinLow;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void tempTask(void *pvParameters);
void clearWeekDays();
bool wasAlarmTriggered(const RtcDateTime& dt);
void startWakeUpCountDown();
int  getWakeUpDuration(byte minutes);
void uploadFirmwwareRoutine();
void printDateTime(const RtcDateTime& dt);
void setupRtc();
int matchWeekDays(byte dayOfWeek);

TaskHandle_t tempTaskHandle = NULL;

volatile bool isPinHighEnabled = false;
volatile long currentBrightness = minBrightness; 
byte counter = 0;
int interval = minBrightness - maxBrightness;
volatile int tempBrightness = minBrightness;
byte alarmHour;
byte alarmMinute;
int weekDays[7];
byte wakeUpDuration;
volatile bool isAlarmEnabled = false;
volatile bool shouldSetAlarm = false;

RtcDS3231<TwoWire> rtc2(Wire);

void IRAM_ATTR ISR_turnPinLow(){
  portENTER_CRITICAL_ISR(&mux); 
    digitalWrite(PINO_DIM, LOW);
    isPinHighEnabled = false;
  portEXIT_CRITICAL_ISR(&mux); 
}

void IRAM_ATTR setTimerPinLow(){
  timerToPinLow = timerBegin(1, 80, true);
  timerAttachInterrupt(timerToPinLow, &ISR_turnPinLow, true);
  timerAlarmWrite(timerToPinLow, INTERVAL_TRIGGER_TRIAC, false);
  timerAlarmEnable(timerToPinLow);
}

void IRAM_ATTR ISR_turnPinHigh(){
  portENTER_CRITICAL_ISR(&mux); 
    digitalWrite(PINO_DIM, HIGH);  
    setTimerPinLow(); 
  portEXIT_CRITICAL_ISR(&mux); 
}

void IRAM_ATTR setTimerPinHigh(long brightness){
  isPinHighEnabled = true; 
  timerToPinHigh = timerBegin(1, 80, true);
  timerAttachInterrupt(timerToPinHigh, &ISR_turnPinHigh, true);
  timerAlarmWrite(timerToPinHigh, brightness, false);
  timerAlarmEnable(timerToPinHigh);
}

void IRAM_ATTR ISR_zeroCross()  {
  if(currentBrightness == IDLE) return;
  portENTER_CRITICAL_ISR(&mux); 
    if(!isPinHighEnabled){
       setTimerPinHigh(currentBrightness);
    }
  portEXIT_CRITICAL_ISR(&mux); 
}

void start(){
  currentBrightness = IDLE;
  counter = 0;
  pinMode(PINO_ZC,  INPUT_PULLUP);
  pinMode(PINO_DIM, OUTPUT);
  digitalWrite(PINO_DIM, LOW);
}

void turnLightOn(){
  attachInterrupt(digitalPinToInterrupt(PINO_ZC), ISR_zeroCross, RISING);
  portENTER_CRITICAL(&mux);
    currentBrightness = maxBrightness;
    digitalWrite(PINO_DIM, HIGH);
  portEXIT_CRITICAL(&mux);
}

void turnLightOff(){
  detachInterrupt(digitalPinToInterrupt(PINO_ZC));
  portENTER_CRITICAL(&mux);
    currentBrightness = IDLE;
    digitalWrite(PINO_DIM, LOW);
  portEXIT_CRITICAL(&mux);
}

void setPowerEndpoint(){
  server.on("/power", HTTP_POST, [](AsyncWebServerRequest *request){},
  [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final){},
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
      {
          StaticJsonDocument<200> jsonBuffer;
          DeserializationError error = deserializeJson(jsonBuffer, data);
          if (error) {
            Serial.printf("Error ao deserializar");
            Serial.println(error.c_str());
            return;
          }
          const byte power = jsonBuffer["power"];
          Serial.println("power");
          if(power == 1){
            turnLightOn();
            delay(10);
            request->send(200, "text/plain", "{\"response\": \"eu\"}");
          }else{
            turnLightOff();
            delay(10);
            request->send(201, "text/plain", "{\"response\": \"eu\"}");
          }
      });
}

void setBrightness(){
  server.on("/brightness", HTTP_POST, [](AsyncWebServerRequest *request){},
  [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final){},
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
      {
          StaticJsonDocument<200> jsonBuffer;
          DeserializationError error = deserializeJson(jsonBuffer, data);
          if (error) {
            Serial.printf("Error ao deserializar");
            Serial.println(error.c_str());
            return;
          }
          int brightness = jsonBuffer["brightness"];
          brightness = map(brightness, 100, 0, maxBrightness, minBrightness);
          portENTER_CRITICAL(&mux);
            currentBrightness = brightness;
          portEXIT_CRITICAL(&mux);
          Serial.println("brilho ");
          Serial.print(currentBrightness);
          delay(10);
          request->send(200, "text/plain", "{\"response\": \"eu\"}");
          
      });
}

void setAlarm(){
  server.on("/alarm", HTTP_POST, [](AsyncWebServerRequest *request){},
  [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final){},
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
      {
          StaticJsonDocument<400> jsonBuffer;
          DeserializationError error = deserializeJson(jsonBuffer, data);
          if (error) {
            Serial.printf("Error ao deserializar");
            Serial.println(error.c_str());
            return;
          }
          JsonArray days = jsonBuffer["days"];
          clearWeekDays();
          for(byte i = 0; i < days.size(); i++){
            weekDays[i] = days[i];
          }
          wakeUpDuration = jsonBuffer["duration"]; 
          isAlarmEnabled = jsonBuffer["enable"]; 
          alarmHour      = jsonBuffer["startTime"]["hour"]; 
          alarmMinute    = jsonBuffer["startTime"]["minute"];
          shouldSetAlarm = true;

          char output[128];
          StaticJsonDocument<200> jsonResponse;
          jsonResponse["response"].set("Alarme programado para ");
          serializeJson(jsonResponse, output);
          delay(10);
          request->send(200, "text/plain", output);
          
      });
}

void clearWeekDays(){
  memset(weekDays, 0, sizeof(weekDays));
}

void setup() {
  Serial.begin(115200);
  xTaskCreatePinnedToCore(
			tempTask,           
			"temp ",           
			10000,              
			NULL,               
			5,                   
			&tempTaskHandle,    
			0); 
  start();
  
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("STA Failed to configure");
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid,password);
  while(WiFi.waitForConnectResult() != WL_CONNECTED){
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  } 
  //uploadFirmwwareRoutine(); 
  server.begin();
  setBrightness();
  setPowerEndpoint();
  setAlarm();
  
}

void setupRtc(){
    Serial.print("compiled: ");
    Serial.print(__DATE__);
    Serial.println(__TIME__);

    //--------RTC SETUP ------------
    // if you are using ESP-01 then uncomment the line below to reset the pins to
    // the available pins for SDA, SCL
    // Wire.begin(0, 2); // due to limited pins, use pin 0 and 2 for SDA, SCL
    
    rtc2.Begin();

    RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);
    //printDateTime(compiled);
    Serial.println();

    if (!rtc2.IsDateTimeValid()) {
        if (rtc2.LastError() != 0){
            // we have a communications error
            // see https://www.arduino.cc/en/Reference/WireEndTransmission for 
            // what the number means
            Serial.print("RTC communications error = ");
            Serial.println(rtc2.LastError());
            ESP.restart();
        }
        else{
            Serial.println("RTC lost confidence in the DateTime!");
            rtc2.SetDateTime(compiled);
        }
  }
  if (!rtc2.GetIsRunning()){
        Serial.println("RTC was not actively running, starting now");
        rtc2.SetIsRunning(true);
    }

    RtcDateTime now = rtc2.GetDateTime();
    if (now < compiled) {
        Serial.println("RTC is older than compile time!  (Updating DateTime)");
        rtc2.SetDateTime(compiled);
    }
    // never assume the Rtc was last configured by you, so
    // just clear them to your needed state
    rtc2.Enable32kHzPin(false);
    rtc2.SetSquareWavePin(DS3231SquareWavePin_ModeNone); 
} 

void printDateTime(const RtcDateTime& dt)
{
    char datestring[20];

    snprintf_P(datestring, 
            countof(datestring),
            PSTR("%02u/%02u/%04u %02u:%02u:%02u"),
            dt.Month(),
            dt.Day(),
            dt.Year(),
            dt.Hour(),
            dt.Minute(),
            dt.Second() );
            Serial.print(dt.DayOfWeek());
    Serial.println(datestring);
}

void uploadFirmwwareRoutine(){
  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

  ArduinoOTA.begin();
}

bool wasAlarmTriggered(const RtcDateTime& dt){
  return alarmHour == dt.Hour() && alarmMinute == dt.Minute() && matchWeekDays(dt.DayOfWeek()) == 1; 
}

int matchWeekDays(byte dayOfWeek){
  int position = 0;
  while(weekDays[position] != 0){
    if(weekDays[position] == dayOfWeek){
      return 1;
    }
    position++;
  }
  return 0;
}

void tempTask(void *pvParameters) {  
     setupRtc();
     for(;;){
      if(wasAlarmTriggered(rtc2.GetDateTime())){
        Serial.println("Disparou");
        int timeDelay = getWakeUpDuration(alarmMinute);
        attachInterrupt(digitalPinToInterrupt(PINO_ZC), ISR_zeroCross, RISING);
        while(tempBrightness > maxBrightness){
          esp_task_wdt_reset();
          Serial.print(currentBrightness);
          Serial.println(" disparando ");
          startWakeUpCountDown();
          delay(timeDelay);
        }
      }else{
        Serial.println("Não disparou");
        Serial.println("");
        Serial.println("-- Hora para disparar --");
        Serial.print(alarmHour);
        Serial.print(":");
        Serial.print(alarmMinute);
        Serial.print(" dia da semana ");
        Serial.print(rtc2.GetDateTime().DayOfWeek());
        Serial.println("");
        Serial.println("-- Hora atual --");
        Serial.print(rtc2.GetDateTime().Hour());
        Serial.print(":");
        Serial.print(rtc2.GetDateTime().Minute());
        Serial.print(" dia da semana ");
        Serial.print(rtc2.GetDateTime().DayOfWeek());
        delay(2000);
      }
      esp_task_wdt_reset();
     } 
}

int getWakeUpDuration(byte minutes){
  return (minutes * 60000) / minBrightness;
}

void startWakeUpCountDown(){
  if(counter < interval){
    tempBrightness = tempBrightness - 1;
    portENTER_CRITICAL(&mux);
      currentBrightness = tempBrightness;
    portEXIT_CRITICAL(&mux);
  }
  counter++;
}

void loop() {
  
  //ArduinoOTA.handle();
  // time_t time = now();
  // Serial.print(hour(time));
  // Serial.print("");
  // delay(1000);
}