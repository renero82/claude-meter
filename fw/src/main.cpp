// Claude usage display per WT32-SC01 Plus (ESP32-S3, ST7796 480x320 8080 8 bit, touch FT6336U)
// Legge http://<server>:8790/usage.json (servizio sulla VM claude) e mostra il residuo
// del limite di sessione (5 ore) e settimanale del piano Claude.
//
// Wi-Fi: al primo avvio (o tenendo premuto lo schermo 6 s) apre la rete "ClaudeMeter-setup":
// collegarsi dal telefono e scegliere la rete 2,4 GHz inserendo la password (resta solo sull'ESP32).
// Tocco breve: aggiorna subito. http://<ip-scheda>/screen.bmp: copia dello schermo.
//
// Robustezza Wi-Fi: si collega sempre al nodo (Deco/AP) con il segnale piu' forte, riprova la rete
// salvata prima di aprire il portale, si riavvia se resta senza Wi-Fi per 5 minuti, cambia nodo se il
// segnale resta sotto -80 dBm, watchdog hardware sul loop. Diagnostica in http://<ip-scheda>/.

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#ifdef OTA_PASSWORD
#include <ArduinoOTA.h>  // aggiornamento via Wi-Fi, solo se la password e' definita in local.ini
#endif
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel;
  lgfx::Bus_Parallel8 _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_FT5x06 _touch;

 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.freq_write = 20000000;
      cfg.pin_wr = 47;
      cfg.pin_rd = -1;
      cfg.pin_rs = 0;
      cfg.pin_d0 = 9;
      cfg.pin_d1 = 46;
      cfg.pin_d2 = 3;
      cfg.pin_d3 = 8;
      cfg.pin_d4 = 18;
      cfg.pin_d5 = 17;
      cfg.pin_d6 = 16;
      cfg.pin_d7 = 15;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = -1;
      cfg.pin_rst = 4;
      cfg.pin_busy = -1;
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 45;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      cfg.x_min = 0;
      cfg.x_max = 319;
      cfg.y_min = 0;
      cfg.y_max = 479;
      cfg.pin_int = 7;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = 1;
      cfg.i2c_addr = 0x38;
      cfg.pin_sda = 6;
      cfg.pin_scl = 5;
      cfg.freq = 400000;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

static LGFX lcd;
static LGFX_Sprite fb(&lcd);
static WebServer web(80);
static Preferences prefs;
// URL predefinito del server (modificabile anche dal portale Wi-Fi).
// Per non scriverlo nel codice: fw/local.ini con  build_flags = ${env.build_flags} -DDEFAULT_SERVER_URL=...
#ifndef DEFAULT_SERVER_URL
#define DEFAULT_SERVER_URL "http://192.168.1.100:8790/usage.json"
#endif
static String serverUrl = DEFAULT_SERVER_URL;

// colori (RGB565 via color565)
static uint16_t C_BG, C_CARD, C_TEXT, C_DIM, C_TRACK, C_GREEN, C_AMBER, C_RED, C_ACCENT;

struct Win {
  bool valid = false;
  int used = 0, left = 0;
  long resetInS = 0;
  String resetLabel;
};
struct Data {
  bool have = false;       // almeno un dato ricevuto dal server
  bool srvOk = false;      // il server ha letto Anthropic con successo
  String srvError, plan, fetchedLabel;
  long ageS = 0;
  Win five, week;
  long tokOut = 0, tokIn = 0, tokCacheW = 0, tokCacheR = 0, tokMsgs = 0;
  uint32_t gotAtMs = 0;
} D;

static int failStreak = 0;

// --- stato Wi-Fi per diagnostica e riconnessione ---
static const char* HOSTNAME = "claude-meter";
static String wifiSsid, wifiPass;                 // rete salvata (NVS dell'ESP32)
static volatile uint8_t lastDiscReason = 0;       // codice wifi_err_reason_t dell'ultima disconnessione
static volatile uint32_t discCount = 0;
static uint32_t roamCount = 0, wifiDownSince = 0;
static const uint32_t WIFI_DOWN_RESTART_MS = 5UL * 60 * 1000;
static const int WEAK_RSSI = -80;
static const uint32_t WEAK_FOR_MS = 5UL * 60 * 1000, ROAM_EVERY_MS = 30UL * 60 * 1000;
static const uint32_t WDT_TIMEOUT_S = 120;
static uint32_t lastFetchMs = 0;
static const uint32_t FETCH_EVERY_MS = 60000;

static uint16_t levelColor(int left) {
  if (left > 50) return C_GREEN;
  if (left > 20) return C_AMBER;
  return C_RED;
}

static String fmtTok(long v) {
  char b[16];
  if (v >= 1000000) snprintf(b, sizeof b, "%.1fM", v / 1e6);
  else if (v >= 1000) snprintf(b, sizeof b, "%ldk", (v + 500) / 1000);
  else snprintf(b, sizeof b, "%ld", v);
  String s(b);
  s.replace('.', ',');
  return s;
}

static String fmtDur(long s) {
  if (s <= 0) return "ora";
  long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
  char b[24];
  if (d > 0) snprintf(b, sizeof b, "%ldg %ldh", d, h);
  else if (h > 0) snprintf(b, sizeof b, "%ldh %02ldm", h, m);
  else snprintf(b, sizeof b, "%ldm", m);
  return String(b);
}

static void drawGauge(int cx, int title_y, const char* title, const Win& w, bool stale) {
  const int cardW = 228, cardX = cx - cardW / 2;
  fb.fillRoundRect(cardX, 52, cardW, 214, 12, C_CARD);
  fb.setTextDatum(textdatum_t::top_center);
  fb.setFont(&fonts::FreeSansBold12pt7b);
  fb.setTextColor(C_TEXT);
  fb.drawString(title, cx, title_y);

  const int cy = 160, r1 = 72, r0 = 58;
  fb.fillArc(cx, cy, r0, r1, 135, 405, C_TRACK);
  if (w.valid) {
    long elapsed = (millis() - D.gotAtMs) / 1000;
    long remain = w.resetInS - elapsed;
    // se il reset e' passato senza un nuovo dato, il residuo torna al 100%
    int left = remain <= 0 ? 100 : w.left;
    uint16_t col = stale ? C_DIM : levelColor(left);
    if (left > 0) fb.fillArc(cx, cy, r0, r1, 135, 135 + 270.0f * left / 100.0f, col);
    fb.setTextDatum(textdatum_t::middle_center);
    fb.setFont(&fonts::FreeSansBold24pt7b);
    fb.setTextColor(col);
    fb.drawString(String(left) + "%", cx, cy - 6);
    fb.setFont(&fonts::FreeSans9pt7b);
    fb.setTextColor(C_DIM);
    fb.drawString("residuo", cx, cy + 26);
    fb.setTextDatum(textdatum_t::top_center);
    fb.setTextColor(C_TEXT);
    fb.drawString("usato " + String(100 - left) + "%", cx, 214);
    fb.setTextColor(C_DIM);
    if (remain <= 0) {
      fb.drawString("reset avvenuto", cx, 236);
    } else {
      fb.drawString("reset " + w.resetLabel, cx, 233);
      fb.drawString("tra " + fmtDur(remain), cx, 250);
    }
  } else {
    fb.setTextDatum(textdatum_t::middle_center);
    fb.setFont(&fonts::FreeSans12pt7b);
    fb.setTextColor(C_DIM);
    fb.drawString("n/d", cx, cy);
  }
}

static void render() {
  fb.fillScreen(C_BG);
  // intestazione
  fb.setTextDatum(textdatum_t::middle_left);
  fb.setFont(&fonts::FreeSansBold12pt7b);
  fb.setTextColor(C_ACCENT);
  fb.drawString("Claude", 14, 24);
  fb.setFont(&fonts::FreeSans12pt7b);
  fb.setTextColor(C_TEXT);
  fb.drawString(D.plan.length() ? D.plan : String("..."), 106, 24);

  bool stale = false;
  String status;
  uint16_t stCol = C_DIM;
  if (WiFi.status() != WL_CONNECTED) {
    status = "Wi-Fi assente";
    if (wifiDownSince) status += " da " + fmtDur((millis() - wifiDownSince) / 1000 + 59);
    if (lastDiscReason) status += " (motivo " + String(lastDiscReason) + ")";
    stCol = C_RED;
    stale = true;
  } else if (!D.have) {
    status = failStreak ? "server non raggiungibile" : "in attesa dati...";
    stCol = failStreak ? C_RED : C_DIM;
  } else if (failStreak >= 3) {
    status = "server non raggiungibile";
    stCol = C_RED;
    stale = true;
  } else if (!D.srvOk) {
    status = "errore " + D.srvError + " - dato delle " + D.fetchedLabel;
    stCol = C_AMBER;
    stale = D.ageS > 1800;
  } else {
    status = "aggiornato " + D.fetchedLabel;
  }
  fb.setTextDatum(textdatum_t::middle_right);
  fb.setFont(&fonts::FreeSans9pt7b);
  fb.setTextColor(stCol);
  fb.drawString(status, 466, 24);

  drawGauge(122, 62, "Sessione 5 ore", D.five, stale);
  drawGauge(358, 62, "Settimana", D.week, stale);

  // piede: token di oggi da Claude Code sulla VM
  fb.setTextDatum(textdatum_t::middle_center);
  fb.setFont(&fonts::FreeSans9pt7b);
  fb.setTextColor(C_DIM);
  if (D.have) {
    String t = "Token oggi sulla VM:  " + fmtTok(D.tokOut) + " out  /  " + fmtTok(D.tokIn + D.tokCacheW) +
               " in  /  " + fmtTok(D.tokCacheR) + " cache";
    fb.drawString(t, 240, 286);
  } else {
    fb.drawString(serverUrl, 240, 286);
  }
  if (WiFi.status() == WL_CONNECTED && WiFi.RSSI() < -75) {
    fb.setTextColor(C_AMBER);
    fb.drawString("Wi-Fi debole: " + String(WiFi.RSSI()) + " dBm su " + WiFi.SSID() + " (canale " +
                      String(WiFi.channel()) + ")", 240, 308);
  }
  fb.pushSprite(0, 0);
}

static void showMessage(const String& l1, const String& l2, const String& l3 = "", const String& l4 = "") {
  fb.fillScreen(C_BG);
  fb.setTextDatum(textdatum_t::middle_center);
  fb.setFont(&fonts::FreeSansBold12pt7b);
  fb.setTextColor(C_ACCENT);
  fb.drawString(l1, 240, 90);
  fb.setFont(&fonts::FreeSans12pt7b);
  fb.setTextColor(C_TEXT);
  fb.drawString(l2, 240, 150);
  fb.setFont(&fonts::FreeSans9pt7b);
  fb.setTextColor(C_DIM);
  fb.drawString(l3, 240, 200);
  fb.drawString(l4, 240, 230);
  fb.pushSprite(0, 0);
}

static void parseWin(JsonVariantConst v, Win& w) {
  w.valid = !v.isNull();
  if (!w.valid) return;
  w.used = v["used"] | 0;
  w.left = v["left"] | 0;
  w.resetInS = v["reset_in_s"] | 0L;
  w.resetLabel = (const char*)(v["reset_label"] | "");
}

static void fetchData() {
  lastFetchMs = millis();
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(serverUrl)) { failStreak++; return; }
  int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    if (!err) {
      D.have = true;
      D.srvOk = doc["ok"] | false;
      D.srvError = (const char*)(doc["error"] | "");
      D.plan = (const char*)(doc["plan"] | "");
      D.fetchedLabel = (const char*)(doc["fetched_label"] | "--:--");
      D.ageS = doc["age_s"] | 0L;
      parseWin(doc["five_hour"], D.five);
      parseWin(doc["seven_day"], D.week);
      JsonVariantConst t = doc["tokens_today"];
      D.tokOut = t["output"] | 0L;
      D.tokIn = t["input"] | 0L;
      D.tokCacheW = t["cache_write"] | 0L;
      D.tokCacheR = t["cache_read"] | 0L;
      D.tokMsgs = t["messages"] | 0L;
      D.gotAtMs = millis();
      failStreak = 0;
      Serial.printf("dati: 5h %d%% 7g %d%% (%s)\n", D.five.used, D.week.used, D.fetchedLabel.c_str());
    } else {
      failStreak++;
    }
  } else {
    failStreak++;
    Serial.printf("HTTP %d\n", code);
  }
  http.end();
}

// --- copia dello schermo in BMP 24 bit (per verifiche da remoto) ---
static void handleScreen() {
  const int W = fb.width(), H = fb.height();
  const uint32_t rowBytes = W * 3, imgBytes = rowBytes * H, fileBytes = 54 + imgBytes;
  uint8_t hdr[54] = {'B', 'M'};
  auto le32 = [&](int off, uint32_t v) { for (int i = 0; i < 4; i++) hdr[off + i] = (v >> (8 * i)) & 0xFF; };
  le32(2, fileBytes); le32(10, 54); le32(14, 40); le32(18, W); le32(22, (uint32_t)(-H));
  hdr[26] = 1; hdr[28] = 24; le32(34, imgBytes);
  web.setContentLength(fileBytes);
  web.send(200, "image/bmp", "");
  web.sendContent((const char*)hdr, 54);
  static uint8_t row[480 * 3];
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      auto p = fb.readPixelRGB(x, y);
      row[x * 3 + 0] = p.b; row[x * 3 + 1] = p.g; row[x * 3 + 2] = p.r;
    }
    web.sendContent((const char*)row, rowBytes);
    esp_task_wdt_reset();  // con segnale debole l'invio puo' durare decine di secondi
  }
}

static void handleRoot() {
  String s = "{\"server\":\"" + serverUrl + "\",\"have\":" + (D.have ? "true" : "false") +
             ",\"five_used\":" + D.five.used + ",\"week_used\":" + D.week.used +
             ",\"fail_streak\":" + failStreak + ",\"rssi\":" + WiFi.RSSI() +
             ",\"uptime_s\":" + (millis() / 1000) + ",\"ssid\":\"" + WiFi.SSID() + "\",\"bssid\":\"" +
             WiFi.BSSIDstr() + "\",\"channel\":" + WiFi.channel() + ",\"ip\":\"" + WiFi.localIP().toString() +
             "\",\"disconnects\":" + discCount + ",\"last_disc_reason\":" + lastDiscReason +
             ",\"roams\":" + roamCount + ",\"reset_reason\":" + (int)esp_reset_reason() +
             ",\"heap_free\":" + ESP.getFreeHeap() + "}";
  web.send(200, "application/json", s);
}

static bool shouldSaveConfig = false;

// Legge SSID e password salvati nella NVS (li scrive WiFiManager dal portale).
static bool readSavedWifi() {
  wifi_config_t c;
  if (esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK) return false;
  wifiSsid = String((const char*)c.sta.ssid).substring(0, 32);
  wifiPass = String((const char*)c.sta.password).substring(0, 64);
  return wifiSsid.length() > 0;
}

// WiFi.begin con SSID/password espliciti: cosi' vale setSortMethod e l'ESP32 sceglie il nodo
// con il segnale piu' forte tra tutti quelli con lo stesso nome (con WiFi.begin() senza argomenti
// e con la scansione veloce di serie prende il primo che trova, anche se lontano).
static void beginStrongest() {
  if (!wifiSsid.length()) { WiFi.reconnect(); return; }
  WiFi.disconnect(false, false);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
}

static bool connectStrongest(uint32_t timeoutMs) {
  beginStrongest();
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(100);
  return WiFi.status() == WL_CONNECTED;
}

static void startWifi(bool forcePortal) {
  WiFi.setHostname(HOSTNAME);  // prima di mode(), altrimenti il DHCP vede esp32s3-xxxx
  WiFi.mode(WIFI_STA);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.setAutoReconnect(true);
  // alimentato da USB: niente risparmio energetico. Con il modem sleep e segnale debole l'ESP32
  // perde le richieste ARP in broadcast e da alcuni host della LAN risulta irraggiungibile.
  WiFi.setSleep(false);
  bool haveSaved = readSavedWifi();
  if (!forcePortal && haveSaved) {
    // la rete salvata potrebbe essere solo momentaneamente giu' (Deco che si riavvia):
    // meglio riprovare qualche volta prima di aprire il portale
    for (int i = 1; i <= 3; i++) {
      showMessage("Connessione Wi-Fi", wifiSsid, "tentativo " + String(i) + " di 3",
                  lastDiscReason ? "ultimo errore: motivo " + String(lastDiscReason) : String(""));
      if (connectStrongest(20000)) return;
    }
  }
  WiFiManager wm;
  WiFiManagerParameter pUrl("server", "URL dati (server sulla VM)", serverUrl.c_str(), 120);
  wm.addParameter(&pUrl);
  wm.setSaveConfigCallback([]() { shouldSaveConfig = true; });
  wm.setHostname(HOSTNAME);
  // con una rete gia' salvata il portale resta aperto meno, poi si riavvia e riprova quella
  wm.setConfigPortalTimeout(haveSaved && !forcePortal ? 300 : 600);
  wm.setConnectTimeout(30);
  wm.setTitle("Claude Meter");
  wm.setAPCallback([](WiFiManager*) {
    showMessage("Configurazione Wi-Fi", "Collegati alla rete  ClaudeMeter-setup",
                "poi apri http://192.168.4.1 e scegli la rete 2,4 GHz",
                "(la password resta solo su questa scheda)");
  });
  bool ok = forcePortal ? wm.startConfigPortal("ClaudeMeter-setup") : wm.autoConnect("ClaudeMeter-setup");
  if (shouldSaveConfig) {
    String u = pUrl.getValue();
    u.trim();
    if (u.startsWith("http")) {
      serverUrl = u;
      prefs.putString("url", serverUrl);
    }
  }
  if (!ok) {
    showMessage("Wi-Fi non collegato", "Riavvio tra 5 secondi...",
                haveSaved ? "rete salvata: " + wifiSsid : String(""),
                lastDiscReason ? "ultimo errore: motivo " + String(lastDiscReason) : String(""));
    delay(5000);
    ESP.restart();
  }
  readSavedWifi();  // eventuale rete nuova scelta dal portale
}

// Il portale WiFiManager blocca per minuti: il watchdog va sospeso intorno a lui.
static void wdtOn() {
  esp_task_wdt_init(WDT_TIMEOUT_S, true);  // riconfigura il TWDT gia' avviato da ESP-IDF
  esp_task_wdt_add(NULL);
}
static void wdtOff() { esp_task_wdt_delete(NULL); }

void setup() {
  Serial.begin(115200);
  lcd.init();
  lcd.setRotation(1);  // orizzontale 480x320
  lcd.setBrightness(170);
  C_BG = lcd.color565(14, 14, 16);
  C_CARD = lcd.color565(30, 30, 34);
  C_TEXT = lcd.color565(236, 234, 228);
  C_DIM = lcd.color565(150, 148, 142);
  C_TRACK = lcd.color565(58, 58, 64);
  C_GREEN = lcd.color565(76, 196, 120);
  C_AMBER = lcd.color565(240, 170, 60);
  C_RED = lcd.color565(232, 84, 72);
  C_ACCENT = lcd.color565(217, 119, 87);

  fb.setColorDepth(16);
  fb.setPsram(true);
  fb.createSprite(lcd.width(), lcd.height());

  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    lastDiscReason = info.wifi_sta_disconnected.reason;
    discCount++;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  prefs.begin("cmeter", false);
  serverUrl = prefs.getString("url", serverUrl);

  showMessage("Claude Meter", "avvio...", "tieni premuto lo schermo per configurare il Wi-Fi");
  // tocco tenuto all'avvio -> portale di configurazione
  bool force = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 1500) {
    int32_t x, y;
    if (lcd.getTouch(&x, &y)) { force = true; break; }
    delay(20);
  }
  startWifi(force);

  web.on("/", handleRoot);
  web.on("/screen.bmp", handleScreen);
  web.begin();
  Serial.printf("IP %s su %s %s ch %d, %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.SSID().c_str(),
                WiFi.BSSIDstr().c_str(), WiFi.channel(), WiFi.RSSI());
  fetchData();
  render();
#ifdef OTA_PASSWORD
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    wdtOff();
    showMessage("Aggiornamento firmware", "non staccare la corrente");
  });
  ArduinoOTA.onError([](ota_error_t) { ESP.restart(); });
  ArduinoOTA.begin();
#endif
  wdtOn();
}

void loop() {
  web.handleClient();
#ifdef OTA_PASSWORD
  ArduinoOTA.handle();
#endif
  static uint32_t lastRender = 0, touchStart = 0;
  static bool touching = false;

  int32_t x, y;
  bool t = lcd.getTouch(&x, &y);
  if (t && !touching) { touching = true; touchStart = millis(); }
  if (!t && touching) {
    touching = false;
    if (millis() - touchStart < 2000) {  // tocco breve: aggiorna
      showMessage("Aggiornamento...", "");
      fetchData();
      render();
      lastRender = millis();
    }
  }
  if (touching && millis() - touchStart > 6000) {  // pressione lunga: nuovo Wi-Fi
    touching = false;
    wdtOff();
    startWifi(true);
    wdtOn();
    fetchData();
  }

  if (millis() - lastFetchMs > FETCH_EVERY_MS) fetchData();
  if (millis() - lastRender > 5000) { render(); lastRender = millis(); }
  // --- sorveglianza Wi-Fi ---
  static uint32_t lastRe = 0, weakSince = 0, lastRoam = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiDownSince) wifiDownSince = millis();
    weakSince = 0;
    if (millis() - lastRe > 30000) { beginStrongest(); lastRe = millis(); }
    if (millis() - wifiDownSince > WIFI_DOWN_RESTART_MS) {  // lo stack Wi-Fi a volte non si riprende
      Serial.println("Wi-Fi assente da 5 minuti: riavvio");
      showMessage("Wi-Fi assente da 5 minuti", "Riavvio...");
      delay(1000);
      ESP.restart();
    }
  } else {
    wifiDownSince = 0;
    if (WiFi.RSSI() < WEAK_RSSI) {
      if (!weakSince) weakSince = millis();
    } else {
      weakSince = 0;
    }
    // segnale debole da 5 minuti: riprova a collegarsi scegliendo il nodo piu' forte (max ogni 30 min)
    if (weakSince && millis() - weakSince > WEAK_FOR_MS && (!lastRoam || millis() - lastRoam > ROAM_EVERY_MS)) {
      Serial.printf("segnale debole (%d dBm): cerco un nodo migliore\n", WiFi.RSSI());
      lastRoam = millis();
      weakSince = 0;
      roamCount++;
      beginStrongest();
    }
  }
  esp_task_wdt_reset();
  delay(10);
}
