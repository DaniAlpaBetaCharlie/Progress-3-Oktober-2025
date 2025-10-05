// ESP8266 + MFRC522 + Web UI + BasicAuth + Badge Register (dengan Nama) + Export CSV (LittleFS)
//
// Library yang dibutuhkan:
// - MFRC522 by Miguel Balboa
// - ESP8266WiFi, ESP8266WebServer (bawaan core ESP8266)
// - LittleFS (aktifkan di Tools > Flash File System: LittleFS)
//
// Wiring RC522 (umum ESP8266 NodeMCU): 
// SDA(SS)->D8, RST->D0, SCK->D5, MOSI->D7, MISO->D6, 3V3->3V3, GND->G
//
// Catatan: proteksi HTTP Basic Auth akan memunculkan pop-up username/password di browser.
// Setelah login sekali, fetch() AJAX akan ikut terautentikasi.

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <SPI.h>
#include <MFRC522.h>

#define SS_PIN  D8
#define RST_PIN D0

// ====== WiFi ======
const char* WIFI_SSID = "m";
const char* WIFI_PASS = "12345678";

// ====== Basic Auth (ubah sesuai kebutuhan) ======
const char* WWW_USER = "admin";
const char* WWW_PASS = "admin123";

// ====== File CSV ======
const char* BADGES_FILE = "/badges.csv"; // format: UID,Name (satu entri per baris)

// ====== Global ======
MFRC522 rfid(SS_PIN, RST_PIN);
ESP8266WebServer server(80);

volatile bool expectingRegister = false;
volatile bool expectingLogin = false;
volatile unsigned long modeTimeout = 0;
const unsigned long MODE_WAIT_MS = 20000;

String pendingName = "";       // nama yang dimasukkan saat pendaftaran
String lastActionResult = "Ready.";

// ---------- Helpers ----------
void ensureFS() {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed. Formatting...");
    LittleFS.format();
    LittleFS.begin();
  }
}

String uidToHexString(byte *uid, byte size) {
  String s;
  for (byte i = 0; i < size; i++) {
    if (uid[i] < 0x10) s += "0";
    s += String(uid[i], HEX);
  }
  s.toUpperCase();
  return s;
}

bool checkAuth() {
  if (!server.authenticate(WWW_USER, WWW_PASS)) {
    server.requestAuthentication(); // 401
    return false;
  }
  return true;
}

bool badgeExists(String uid, String &currentName) {
  ensureFS();
  if (!LittleFS.exists(BADGES_FILE)) return false;
  File f = LittleFS.open(BADGES_FILE, "r");
  if (!f) return false;
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim();
    if (line.length() == 0) continue;
    int comma = line.indexOf(',');
    if (comma < 0) continue;
    String fUID = line.substring(0, comma); fUID.trim();
    String fName = line.substring(comma + 1); fName.trim();
    if (fUID.equalsIgnoreCase(uid)) {
      currentName = fName;
      f.close();
      return true;
    }
  }
  f.close();
  return false;
}

// Tambah atau update nama untuk UID
bool upsertBadge(const String &uid, const String &name) {
  ensureFS();
  // kalau file belum ada → langsung append
  if (!LittleFS.exists(BADGES_FILE)) {
    File nf = LittleFS.open(BADGES_FILE, "w");
    if (!nf) return false;
    nf.println(uid + "," + name);
    nf.close();
    return true;
  }

  // baca semua, tulis ulang (update jika ada UID sama)
  File in = LittleFS.open(BADGES_FILE, "r");
  if (!in) return false;
  String outContent;
  bool updated = false;

  while (in.available()) {
    String line = in.readStringUntil('\n'); line.trim();
    if (line.length() == 0) continue;
    int comma = line.indexOf(',');
    if (comma < 0) continue;
    String fUID = line.substring(0, comma); fUID.trim();
    if (fUID.equalsIgnoreCase(uid)) {
      // ganti nama
      outContent += uid + "," + name + "\n";
      updated = true;
    } else {
      outContent += line + "\n";
    }
  }
  in.close();

  if (!updated) {
    outContent += uid + "," + name + "\n";
  }

  File out = LittleFS.open(BADGES_FILE, "w");
  if (!out) return false;
  out.print(outContent);
  out.close();
  return true;
}

String badgesTableHTML() {
  ensureFS();
  String html = "<table border='1' cellpadding='6' cellspacing='0'><tr><th>#</th><th>UID</th><th>Nama</th></tr>";
  if (LittleFS.exists(BADGES_FILE)) {
    File f = LittleFS.open(BADGES_FILE, "r");
    int idx = 1;
    while (f.available()) {
      String line = f.readStringUntil('\n'); line.trim();
      if (!line.length()) continue;
      int comma = line.indexOf(',');
      if (comma < 0) continue;
      String uid = line.substring(0, comma); uid.trim();
      String name = line.substring(comma + 1); name.trim();
      html += "<tr><td>" + String(idx++) + "</td><td><code>" + uid + "</code></td><td>" + name + "</td></tr>";
    }
    f.close();
  }
  html += "</table>";
  return html;
}

// ---------- Web UI ----------
const char* INDEX_PAGE = R"rawliteral(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>ESP8266 RFID Badge</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
  body { font-family: Arial, Helvetica, sans-serif; padding: 14px; max-width: 760px; margin: auto; }
  h2 { margin-top: 0; }
  label { display:block; margin: 10px 0 6px; }
  input[type=text], input[type=password] { width:100%; padding:10px; box-sizing:border-box; }
  button { padding: 10px 16px; margin: 8px 6px 8px 0; font-size: 15px; cursor: pointer; }
  #status { margin-top: 10px; font-weight: bold; min-height: 1.2em; }
  .row { margin: 10px 0; }
  .card { border:1px solid #ddd; padding:12px; border-radius:8px; margin-bottom:14px; }
  .muted { color:#666; font-size: 13px; }
</style>
</head>
<body>
  <h2>ESP8266 RFID Badge</h2>

  <div class="card">
    <h3>1) Daftar Badge (UID + Nama)</h3>
    <div class="row">
      <label>Nama pengguna</label>
      <input type="text" id="name" placeholder="mis. Dani Dani">
    </div>
    <button onclick="startRegister()">Daftar Badge</button>
    <div class="muted">Setelah menekan tombol, tempelkan badge dalam 20 detik.</div>
  </div>

  <div class="card">
    <h3>2) Login dengan Badge</h3>
    <button onclick="startLogin()">Login</button>
    <div class="muted">Tempelkan badge terdaftar dalam 20 detik.</div>
  </div>

  <div class="card">
    <h3>3) Daftar Badge Terdaftar</h3>
    <button onclick="refreshList()">Refresh Daftar</button>
    <a href="/export_csv" target="_blank"><button>Export CSV</button></a>
    <div id="listArea" style="margin-top:10px;"></div>
  </div>

  <div id="status"></div>

<script>
  var pollTimer = null;

  function startRegister() {
    const name = document.getElementById('name').value.trim();
    if (!name) { 
      alert('Nama tidak boleh kosong'); 
      return; 
    }
    fetch('/start_register?name=' + encodeURIComponent(name), {method:'GET', credentials:'include'})
      .then(_ => {
        document.getElementById('status').innerText = 'Mode: Menunggu badge untuk didaftar (20s)...';
        startPolling();
      });
  }

  function startLogin() {
    fetch('/start_login', {method:'GET', credentials:'include'})
      .then(_ => {
        document.getElementById('status').innerText = 'Mode: Menunggu badge untuk login (20s)...';
        startPolling();
      });
  }

  function refreshList() {
    fetch('/badges', {credentials:'include'})
      .then(r => r.text())
      .then(html => {
        document.getElementById('listArea').innerHTML = html;
      });
  }

  function startPolling() {
    if (pollTimer) clearInterval(pollTimer);
    pollTimer = setInterval(() => {
      fetch('/status', {credentials:'include'})
        .then(r => r.json())
        .then(j => {
          document.getElementById('status').innerText = j.message;
          if (!j.waiting) {
            clearInterval(pollTimer);
            pollTimer = null;
            refreshList();
          }
        });
    }, 1000);
  }

  // Load daftar saat halaman dibuka
  refreshList();
</script>
</body>
</html>
)rawliteral";

// ---------- Handlers ----------
void handleRoot() {
  if (!checkAuth()) return;
  server.send(200, "text/html", INDEX_PAGE);
}

void handleStartRegister() {
  if (!checkAuth()) return;
  if (expectingLogin) { server.send(200, "text/plain", "busy"); return; }

  String name = server.arg("name");
  name.trim();
  if (name.length() == 0) {
    server.send(400, "text/plain", "Nama wajib diisi");
    return;
  }

  pendingName = name; // simpan sementara
  expectingRegister = true;
  expectingLogin = false;
  modeTimeout = millis() + MODE_WAIT_MS;
  lastActionResult = "Menunggu badge untuk didaftar...";
  server.send(200, "text/plain", "ok");
}

void handleStartLogin() {
  if (!checkAuth()) return;
  if (expectingRegister) { server.send(200, "text/plain", "busy"); return; }
  expectingLogin = true;
  expectingRegister = false;
  modeTimeout = millis() + MODE_WAIT_MS;
  lastActionResult = "Menunggu badge untuk login...";
  server.send(200, "text/plain", "ok");
}

void handleStatus() {
  if (!checkAuth()) return;
  bool busy = expectingRegister || expectingLogin;
  bool waiting = false;
  if (busy && millis() < modeTimeout) waiting = true;
  else if (busy && millis() >= modeTimeout) {
    expectingRegister = expectingLogin = false;
    pendingName = "";
    lastActionResult = "Waktu habis, tidak ada badge terdeteksi.";
  }

  String json = "{";
  json += "\"busy\":" + String(busy ? "true" : "false") + ",";
  json += "\"waiting\":" + String(waiting ? "true" : "false") + ",";
  json += "\"message\":\"" + lastActionResult + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleBadges() {
  if (!checkAuth()) return;
  String html = badgesTableHTML();
  server.send(200, "text/html", html);
}

void handleExportCSV() {
  if (!checkAuth()) return;
  ensureFS();
  if (!LittleFS.exists(BADGES_FILE)) {
    // kirim header CSV kosong
    server.sendHeader("Content-Disposition", "attachment; filename=badges.csv");
    server.send(200, "text/csv", "UID,Name\n");
    return;
  }
  File f = LittleFS.open(BADGES_FILE, "r");
  if (!f) {
    server.send(500, "text/plain", "Gagal membuka CSV");
    return;
  }
  // Stream file sebagai CSV download
  server.sendHeader("Content-Disposition", "attachment; filename=badges.csv");
  server.streamFile(f, "text/csv");
  f.close();
}

// ---------- Setup & Loop ----------
void setup() {
  Serial.begin(115200);
  delay(10);
  Serial.println();
  Serial.println("Booting...");

  ensureFS();

  SPI.begin();
  rfid.PCD_Init();
  Serial.println("RFID init done.");
  rfid.PCD_DumpVersionToSerial();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500); Serial.print(".");
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("Failed to connect. Starting AP mode...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP8266-RFID");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }

  // Routes (semua protected)
  server.on("/", HTTP_GET, handleRoot);
  server.on("/start_register", HTTP_GET, handleStartRegister);
  server.on("/start_login",    HTTP_GET, handleStartLogin);
  server.on("/status",         HTTP_GET, handleStatus);
  server.on("/badges",         HTTP_GET, handleBadges);
  server.on("/export_csv",     HTTP_GET, handleExportCSV);

  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
  Serial.println();
  Serial.print("Connected. IP: ");
  Serial.println(WiFi.localIP());
  // Scan RFID non-blocking
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String uidHex = uidToHexString(rfid.uid.uidByte, rfid.uid.size);
    Serial.print("Card detected: ");
    Serial.println(uidHex);

    if (expectingRegister && millis() < modeTimeout) {
      String oldName;
      if (upsertBadge(uidHex, pendingName)) {
        if (badgeExists(uidHex, oldName)) {
          lastActionResult = "Badge diperbarui: " + uidHex + " → " + pendingName;
        } else {
          lastActionResult = "Badge terdaftar: " + uidHex + " (" + pendingName + ")";
        }
      } else {
        lastActionResult = "Gagal menyimpan badge: " + uidHex;
      }
      expectingRegister = false;
      pendingName = "";
      modeTimeout = 0;
    } else if (expectingLogin && millis() < modeTimeout) {
      String foundName;
      if (badgeExists(uidHex, foundName)) {
        lastActionResult = "Login berhasil: " + foundName + " [" + uidHex + "]";
      } else {
        lastActionResult = "Login gagal. Badge belum terdaftar: " + uidHex;
      }
      expectingLogin = false;
      modeTimeout = 0;
    } else {
      lastActionResult = "Badge terdeteksi (di luar mode): " + uidHex;
    }

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }

  if ((expectingRegister || expectingLogin) && millis() >= modeTimeout) {
    expectingRegister = expectingLogin = false;
    pendingName = "";
    lastActionResult = "Waktu habis, tidak ada badge terdeteksi.";
    modeTimeout = 0;
  }
}
