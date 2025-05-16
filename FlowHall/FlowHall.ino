#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ESP32Time.h>
#include <ESPmDNS.h>
#include <ESP32Ping.h>

WiFiManager wm;
HTTPClient http;
IPAddress ip;
ESP32Time rtc;

#define WIFI_RESET 13

int contador;
int tempoFluxo = 0;
int tempoAnterior = 0;        // Variável de controle de tempo para envio de dados do código.
int tempoDesejado = 2000;     // Valor incial do volume que será enviado ao banco de dados;
int tentativasReconexao = 0;  // Contador de tentativas de reconexão

float volume = 0;
float volumeFluxo = 0;
float porcentagem;
float fluxo = 0;

#define BAUD_RATE 115200

hw_timer_t *timer = NULL;
bool travar = false;
const int wdtTimeout = 3000;  //time in ms to trigger the watchdog

// Função chamada pelo watchdog (timer)
void ARDUINO_ISR_ATTR resetModule() {
  ets_printf("WatchDog\n");
  esp_restart();
}

void setupTimer() {
  timer = timerBegin(3000000);                //timer 1Mhz resolution
  timerAttachInterrupt(timer, &resetModule);  //attach callback
  timerAlarm(timer, 30000000, false, 0);       //set time in us
}

void printResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();

  Serial.print("Motivo do último reset: ");
  switch (reason) {
    case ESP_RST_POWERON: Serial.println("Power on"); break;
    case ESP_RST_EXT: Serial.println("Reset externo"); break;
    case ESP_RST_SW: Serial.println("Reset por software"); break;
    case ESP_RST_PANIC: Serial.println("Pânico (exceção)"); break;
    case ESP_RST_INT_WDT: Serial.println("Watchdog de interrupção"); break;
    case ESP_RST_TASK_WDT: Serial.println("Watchdog de tarefa"); break;
    case ESP_RST_WDT: Serial.println("Watchdog (timer)"); break;
    case ESP_RST_DEEPSLEEP: Serial.println("Wakeup de deep sleep"); break;
    default: Serial.println("Outro ou desconhecido"); break;
  }
}

void monitorarMemoria() {
  Serial.printf("Memória: %d bytes\n", ESP.getFreeHeap());
}

void passagem() {
  contador++;
}
void enviaDados(const char *id, float fluxo, float volume) {
  unsigned long startTime = millis();
  const char *url = "Minha Api";

  char dataHora[20];
  snprintf(dataHora, sizeof(dataHora), "%04d-%02d-%02d %02d:%02d:%02d", rtc.getYear(), rtc.getMonth() + 1, rtc.getDay(), rtc.getHour(true), rtc.getMinute(), rtc.getSecond());
  char jsonPayload[130];
  snprintf(jsonPayload, sizeof(jsonPayload), "{\"id\":\"%s\", \"fluxo\":%.2f, \"volume\":%.2f, \"data\":\"%s\"}", id, fluxo, volume, dataHora);

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);  // Também aqui

  int httpResponseCode = http.POST(jsonPayload);
  yield();  // alimenta o watchdog

  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println(response);
  } else {
    Serial.println(httpResponseCode);
  }

  http.end();

  unsigned long elapsedTime = millis() - startTime;
  Serial.printf("Tempo para enviar dados: %lu ms\n", elapsedTime);
}

bool transmiteID() {
  uint64_t chipMac = ESP.getEfuseMac();
  char macSuffix[7];  // 6 digits + null terminator
  sprintf(macSuffix, "%02X%02X%02X",
          (uint8_t)(chipMac >> 24),   // 0x40
          (uint8_t)(chipMac >> 32),   // 0x91
          (uint8_t)(chipMac >> 40));  // 0x51

  char hostname[22];
  char serviceName[42];
  sprintf(hostname, "ESP-%s", macSuffix);
  sprintf(serviceName, "_ESP-%s._http._tcp.", macSuffix);

  if (MDNS.begin(hostname)) {
    MDNS.addService((const char *)serviceName, "tcp", 80);  // Conversão explícita para const char*
    MDNS.addService("_http", "_tcp", 80);
    MDNS.addServiceTxt("_http", "_tcp", "mac", (const char *)macSuffix);
    MDNS.addServiceTxt("_http", "_tcp", "id", WiFi.macAddress());
    MDNS.addServiceTxt("_http", "_tcp", "device", "ESP - ");
    return true;
  }
  return false;
}

bool checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long startAttemptTime = millis();
    Serial.println("Wi-Fi desconectado. Tentando reconectar...");

    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
      WiFi.reconnect();
      delay(500);
      Serial.println("Tentando reconectar ao Wi-Fi...");
    }

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Falha ao reconectar ao Wi-Fi.");
      return false;
    }
  }

  if (!temInternet()) {
    Serial.println("Sem acesso à internet.");
    return false;
  }

  Serial.println("Wi-Fi conectado e internet disponível.");
  return true;
}

bool temInternet() {
  if (!WiFi.hostByName("google.com", ip)) {
    return false;
  }
  bool status = Ping.ping(ip, 2);
  return status;
}

void setupWiFi() {
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  wm.setConfigPortalTimeout(80);
  if (!wm.autoConnect("Filtro Purific")) {
    delay(3000);
  }
  for (int i = 0; i < 5; i++) {
    if (!checkWiFi()) {
      if (i >= 4) {
        Serial.println("Reiniciando ESP");
        esp_restart();  // Reinicia o ESP após 10 falhas
      }
    } else
      break;
  }
}

void checkButton() {
  if (digitalRead(WIFI_RESET) == LOW) {
    Serial.println("Botão pressionado. Resetando configurações...");
    delay(5000);
    if (digitalRead(WIFI_RESET) == LOW) {
      wm.resetSettings();
      Serial.println("Configurações de Wi-Fi resetadas.");
    }
    wm.setConfigPortalTimeout(120);
    if (!wm.startConfigPortal("Filtro Purific")) {
      Serial.println("Portal de configuração expirou. Reiniciando ESP...");
      delay(3000);
      esp_restart();
    } else {
      if (WiFi.isConnected()) {
        Serial.println("Conectado ao Wi-Fi após portal de configuração.");
      } else {
        Serial.println("Falha ao conectar ao Wi-Fi após portal de configuração.");
      }
    }
  }
}

void setup() {
  Serial.begin(BAUD_RATE);
  pinMode(WIFI_RESET, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(26), passagem, RISING);
  setupWiFi();
  delay(1000);
  if (transmiteID()) {
    Serial.println("Publicando");
  }

  configTime(3600 * -3, 0, "time.nist.gov");
  struct tm timeinfo;
  if (WiFi.status() == WL_CONNECTED and getLocalTime(&timeinfo)) {
    Serial.println("Hora: " + String(rtc.getHour(true)) + ":" + String(rtc.getMinute()));
    Serial.println("Data: " + String(rtc.getDay()) + "/" + String(rtc.getMonth() + 1) + "/" + String(rtc.getYear()));
  } else {
    Serial.println("Falha ao definir hora e data...");
  }
  setupTimer();
}

void loop() {
  timerWrite(timer, 0);  //reseta o temporizador (alimenta o watchdog)
  long tme = millis();   //tempo inicial do loop
  checkButton();
  volume = (contador * 0.56 + 10.4) / 1000;

  Serial.printf("Volume: %.2f L\n", fluxo);
  printResetReason();
  monitorarMemoria();

  if (millis() - tempoFluxo > 1000) {
    fluxo = (volume - volumeFluxo) / ((millis() - tempoFluxo) / 1000);
    tempoFluxo = millis();
    volumeFluxo = volume;
    Serial.printf("Fluxo: %.2f L/s\n", fluxo);
  }

  Serial.println("Executando normalmente...");
  Serial.print("tempo passado dentro do loop (ms) = ");
  tme = millis() - tme;  //calcula o tempo (atual - inicial)
  Serial.println(tme);
  if (millis() - tempoAnterior > tempoDesejado) {
    if (!checkWiFi()) {
      tentativasReconexao++;
      Serial.printf("Tentativa de reconexão: %d\n", tentativasReconexao);
      if (tentativasReconexao >= 10) {
        Serial.println("Falha ao reconectar após 10 tentativas. Reiniciando ESP...");
        esp_restart();
      }
    } else {
      tentativasReconexao = 0;
      Serial.println("Wi-Fi reconectado com sucesso.");
    }
    enviaDados(WiFi.macAddress().c_str(), fluxo, volume);
    tempoAnterior = millis();
    monitorarMemoria();
  }
}