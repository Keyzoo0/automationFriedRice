# BUILD PLAN — Automation Frice Ride (AFR)
## Implementasi Kontrol Kecepatan Motor Pada Pengadukan Blondo
> Proyek Akhir · Dery Adinur Saputro · 2241170126 · Politeknik Negeri Malang · 2026

---

## 1. Gambaran Umum Proyek

Sistem kontrol kecepatan motor berbasis PID untuk pengadukan blondo dalam pembuatan minyak goreng kelapa. ESP32 bertugas sebagai master yang membaca RPM via proximity encoder, menghitung output PID, mengirim perintah ke VFD (Variable Frequency Drive) via Modbus RTU RS-485, dan menyajikan monitoring real-time via web dashboard.

---

## 2. Hardware Inventory

| Komponen | Pin ESP32 | Keterangan |
|---|---|---|
| Push Button (START/STOP) | GPIO 2 | Active LOW, INPUT_PULLUP |
| Proximity Encoder | GPIO 4 | 5 PPR, FALLING ISR |
| Rotary Switch pos-1 (SP=30) | GPIO 25 | Active LOW, INPUT_PULLUP |
| Rotary Switch pos-2 (SP=25) | GPIO 35 | Active LOW, INPUT_PULLUP, input-only |
| Rotary Switch pos-3 (SP=20) | GPIO 34 | Active LOW, INPUT_PULLUP, input-only |
| MAX485 DE/RE | GPIO 27 | Direction control RS-485 |
| MAX485 RX | GPIO 16 | Serial2 RX |
| MAX485 TX | GPIO 17 | Serial2 TX |
| TM1637 ATAS CLK | GPIO 21 | Display 4-digit atas |
| TM1637 ATAS DIO | GPIO 22 | Display 4-digit atas |
| TM1637 BAWAH CLK | GPIO 33 | Display 4-digit bawah |
| TM1637 BAWAH DIO | GPIO 32 | Display 4-digit bawah |
| LED Merah | GPIO 23 | HIGH=RUN, LOW=STOP |
| VFD (Modbus Slave) | Serial2 | Address 1, 9600 baud, 8E1 |

> **Catatan GPIO 34 & 35:** Input-only, tidak mendukung OUTPUT dan tidak ada internal pull-up pada beberapa revisi ESP32. Gunakan resistor pull-up eksternal 10kΩ ke 3.3V jika tidak stabil.

---

## 3. Arsitektur Software

### 3.1 RTOS Task Map

```
Core 1 (Realtime)
├── TaskRPM     Prio 5  delay 1ms    — ISR data → hitung RPM (moving avg 5 sample)
├── TaskPID     Prio 4  delay 150ms  — PID compute → Modbus write VFD
└── TaskModbus  Prio 3  delay 150ms  — (digabung di TaskPID, bukan task terpisah)

Core 0 (IO / Network)
├── TaskUI      Prio 3  delay 50ms   — Button scan, rotary scan, 7-seg update, LED
└── TaskWebSvr  Prio 2  yield        — ESPAsyncWebServer handler (non-blocking)
```

> **Catatan:** ESPAsyncWebServer berjalan di Core 0 via callback internal. TaskWebSvr tidak perlu `handleClient()` loop — hanya untuk inisialisasi. Semua handler async berjalan di background thread yang di-manage library.

### 3.2 State Machine

```
         [POWER ON]
              │
              ▼
           ST_STOP ◄──────────────────────────────────────┐
              │                                            │
         [Tombol ditekan]                          [Tombol ditekan]
              │                                            │
              ▼                                            │
     display atas = "RUN" (2 detik)                        │
              │                                            │
              ▼                                            │
           ST_RUN ─────────────────────────────────────────┘
```

### 3.3 Display State Machine

```
KONDISI ST_STOP:
  Display ATAS  : "STOP"          (4 karakter 7-segment)
  Display BAWAH : "SP" + XX       (misal SP20, SP25, SP30)

TRANSISI STOP → RUN (saat tombol ditekan):
  Display ATAS  : "RUN " → 2000ms → setpoint numerik
  Display BAWAH : langsung actual RPM (XX.X format)

KONDISI ST_RUN (setelah 2 detik):
  Display ATAS  : setpoint RPM    (misal 0025 untuk 25 RPM)
  Display BAWAH : actual RPM      (format XX.X, max 99.9)

TRANSISI RUN → STOP (saat tombol ditekan):
  Display ATAS  : "STOP"
  Display BAWAH : "SP" + setpoint aktif
```

---

## 4. Struktur File Project

```
AFR/
├── AFR.ino                     — entry point, setup(), loop() kosong
├── config.h                    — semua konstanta, pin, parameter
├── shared_state.h              — struct SharedState, mutex, deklarasi global
├── shared_state.cpp            — definisi variabel global
├── task_rpm.h / .cpp           — TaskRPM: ISR + moving average RPM
├── task_pid.h / .cpp           — TaskPID: PID compute + Modbus write
├── task_ui.h / .cpp            — TaskUI: button, rotary, 7-seg, LED
├── task_websrv.h / .cpp        — TaskWebSrv: ESPAsyncWebServer setup
├── web_handlers.h / .cpp       — semua API endpoint handler
├── display_helper.h / .cpp     — semua fungsi tampil 7-segment
└── data/                       — LittleFS filesystem
    ├── index.html              — SPA dashboard (tab Dashboard + Info)
    ├── style.css               — styling dashboard
    └── app.js                  — JavaScript polling + chart
```

---

## 5. Dependencies (Platform: ESP32 Arduino Core v3.x)

```ini
; platformio.ini
[env:esp32]
platform = espressif32
board = esp32dev
framework = arduino
lib_deps =
    paulstoffregen/OneWire                  ; (tidak dipakai, skip)
    lennarthennigs/TM1637@^1.0.0            ; 7-segment TM1637
    4-20ma/ModbusMaster@^2.0.1             ; Modbus RTU Master
    me-no-dev/ESPAsyncWebServer@^1.2.3     ; Async Web Server
    me-no-dev/AsyncTCP@^1.1.1              ; dependency ESPAsyncWebServer
    bblanchon/ArduinoJson@^7.0.0           ; JSON API response
monitor_speed = 115200
board_build.filesystem = littlefs
```

> **Jika pakai Arduino IDE:** Install semua library di atas via Library Manager. Pastikan ESP32 Arduino Core versi 3.0.0+. Untuk upload LittleFS gunakan plugin "ESP32 LittleFS Data Upload".

---

## 6. Shared State — Struktur Data

```cpp
// shared_state.h
struct SharedState {
  // ── RPM ──────────────────────────────────────────
  float    current_rpm;        // hasil moving average, dibaca TaskUI & TaskPID
  float    raw_rpm;            // hasil ISR sebelum filter

  // ── Setpoint ─────────────────────────────────────
  float    setpoint;           // aktif saat ini (dari rotary atau NVS)
  float    sp_pos1;            // nilai RPM posisi rotary 1 (default 30)
  float    sp_pos2;            // nilai RPM posisi rotary 2 (default 25)
  float    sp_pos3;            // nilai RPM posisi rotary 3 (default 20)

  // ── PID ──────────────────────────────────────────
  float    kp, ki, kd;
  float    pid_output;         // output PID terakhir (untuk debug web)

  // ── Modbus ───────────────────────────────────────
  bool     modbus_ok;          // last write success
  uint16_t vfd_freq_raw;       // nilai raw yang dikirim ke VFD register 0x2001

  // ── System State ─────────────────────────────────
  bool     is_running;         // true = ST_RUN
  bool     transition_run;     // flag: tampil "RUN" 2 detik
  uint32_t transition_start_ms;// timestamp mulai transisi

  // ── Uptime ───────────────────────────────────────
  uint32_t uptime_ms;
};

extern SharedState    g_sys;
extern SemaphoreHandle_t xMutex;
```

---

## 7. ISR & RPM Calculation

### 7.1 ISR (IRAM_ATTR, Core 1)

```
Referensi: program encoder lama (1 PPR) → diadaptasi ke 5 PPR

ISR trigger : FALLING edge pada GPIO 4
Debounce    : 300.000 µs (300ms) — sama persis dengan referensi
Formula RPM : 60.000.000 / interval_µs / 5.0   (5 PPR)
Timeout     : jika tidak ada pulse > 2.000.000 µs (2 detik) → RPM = 0
```

### 7.2 Moving Average Filter

```
Window size : 5 sample (numReadings = 5)
Metode      : ring buffer — sama persis dengan referensi
Input       : rawRPM dari ISR
Output      : displayRPM → g_sys.current_rpm
```

### 7.3 Cara Kerja (jiplak persis dari referensi, ubah hanya pulsePerRev)

```cpp
// Di ISR:
DEBOUNCE_TIME = 300000  // µs
intervalValid = now - lastPulseMicros
adaPulsa      = true

// Di TaskRPM (setiap 1ms):
if (adaPulsa) {
    rawRPM = (60000000.0 / intervalValid) / 5.0   // ← ubah dari 1.0 ke 5.0
    // moving average 5 sample (ring buffer)
    totalRPM -= rpmReadings[readIndex]
    rpmReadings[readIndex] = rawRPM
    totalRPM += rpmReadings[readIndex]
    readIndex = (readIndex + 1) % 5
    if (countReadings < 5) countReadings++
    g_sys.current_rpm = totalRPM / countReadings
    adaPulsa = false
}
// Timeout 2 detik
if (micros() - lastPulseMicros > 2000000) {
    g_sys.current_rpm = 0
    rawRPM = 0
    // reset ring buffer
}
```

---

## 8. PID + Modbus (TaskPID, Core 1, 150ms)

### 8.1 PID Formula

```
Sama persis dengan referensi program:

baseFreq   = (66.0 * setpoint) + 530.0
error      = setpoint - current_rpm
integral  += error * dt      (clamp ±200)
derivative = (error - lastError) / dt
outputPID  = baseFreq + (Kp*error) + (Ki*integral) + (Kd*derivative)
outputPID  = constrain(outputPID, 0, 5000)
```

### 8.2 Modbus Write

```
Register 0x2000 : command
  START = 0x0001
  STOP  = 0x0005 (retry 3x dengan delay 150ms jika gagal — sama dengan referensi)

Register 0x2001 : frekuensi output (uint16, range 0–5000)
  Ditulis setiap 150ms saat isRunning = true

Baud rate : 9600
Format    : 8E1 (8 data, Even parity, 1 stop)
Slave ID  : 1
```

### 8.3 Default PID Values

```cpp
float Kp = 0.23f;
float Ki = 0.30f;
float Kd = 21.4f;
// Sama dengan referensi — dapat di-tune via webserver
```

---

## 9. Button & Rotary Logic (TaskUI, Core 0, 50ms)

### 9.1 Push Button (GPIO 2)

```
Debounce software: cek LOW → delay 50ms → cek ulang LOW
Edge detection  : lastButtonState != currentButtonState

Aksi:
  ST_STOP → tekan → startSystem() → ST_RUN (+ tampil "RUN" 2 detik)
  ST_RUN  → tekan → stopSystem()  → ST_STOP
```

### 9.2 Rotary Switch

```
Hanya dibaca saat ST_STOP (isRunning = false)

if (pin25 == LOW) setpoint = sp_pos1  // default 30
if (pin35 == LOW) setpoint = sp_pos2  // default 25
if (pin34 == LOW) setpoint = sp_pos3  // default 20
```

---

## 10. Display 7-Segment (TaskUI, setiap 250ms)

### 10.1 Karakter Custom

```
"STOP" → S (SEG_A|C|D|F|G), T (SEG_A|D|E|F|G ← pakai lowercase t),
          O (SEG_A|B|C|D|E|F), P (SEG_A|B|E|F|G)

"RUN " → r (SEG_E|G), U (SEG_B|C|D|E|F), n (SEG_C|E|G), space

"SP"   → S (SEG_A|C|D|F|G), P (SEG_A|B|E|F|G)

Digit  → display.encodeDigit(n)
```

### 10.2 Format Setpoint (Display ATAS saat RUN)

```
Setpoint integer, max 2 digit (nilai 20/25/30)
Format: leading space → " " + " " + "2" + "0"  (misal untuk 20)
Atau dengan prefix SP saat STOP: S-P-2-0
```

### 10.3 Format RPM (Display BAWAH saat RUN)

```
Sama dengan referensi:
  max 99.9 RPM (4 digit: XX.X)
  data[0] = tens of intPart (atau blank jika < 10)
  data[1] = units of intPart + decimal point (| 0x80)
  data[2] = decimal digit
  data[3] = 0x00 (blank)
```

### 10.4 Startup Debug Display (Setup)

```
Tahap init yang ditampilkan di 7-segment:
  Step 1: Atas="    " Bawah="----"  → GPIO init
  Step 2: Atas="    " Bawah="nUS " → NVS/Preferences load
  Step 3: Atas="bUS " Bawah="----"  → Modbus Serial2 init
  Step 4: Atas="UiFi" Bawah="----"  → WiFi connecting
  Step 5: Atas=IP[0..1] Bawah=IP[2..3] → WiFi connected (4 digit IP terakhir)
          atau Atas="Err " Bawah="UiFi" → WiFi failed (tetap jalan)
  Step 6: Atas="----" Bawah="----"  → Semua siap, 500ms, lalu normal display

Catatan: semua text menggunakan karakter 7-segment yang tersedia.
"----" = SEG_G di semua 4 digit (tanda minus).
```

---

## 11. LED Merah (GPIO 23)

```
ST_STOP : LOW  (mati)
ST_RUN  : HIGH (nyala)
Transisi: nyala bersamaan dengan startSystem(), mati bersamaan dengan stopSystem()
```

---

## 12. Web Dashboard (ESPAsyncWebServer)

### 12.1 WiFi

```cpp
SSID     : "Biznet"
Password : "12345678"
Mode     : STA
Timeout  : 10 detik
mDNS     : "afr.local"
Port     : 80
```

### 12.2 API Endpoints

| Method | Endpoint | Parameter | Deskripsi |
|---|---|---|---|
| GET | `/api/status` | — | JSON status lengkap (polling 500ms) |
| GET | `/api/start` | — | Jalankan motor |
| GET | `/api/stop` | — | Hentikan motor |
| GET | `/api/setpoint` | `?pos=1&value=30` | Set nilai RPM posisi rotary |
| GET | `/api/pid` | `?kp=0.23&ki=0.3&kd=21.4` | Update PID gains |
| GET | `/api/restart` | — | Restart ESP32 |

### 12.3 Response /api/status

```json
{
  "is_running": true,
  "current_rpm": 24.7,
  "setpoint": 25.0,
  "sp_pos1": 30.0,
  "sp_pos2": 25.0,
  "sp_pos3": 20.0,
  "pid_output": 2178,
  "vfd_freq_raw": 2178,
  "modbus_ok": true,
  "kp": 0.23,
  "ki": 0.30,
  "kd": 21.4,
  "uptime_ms": 12345
}
```

### 12.4 Static Files via LittleFS

```
/data/index.html   — SPA 2 tab (Dashboard + Info)
/data/style.css    — styling
/data/app.js       — polling + Chart.js RPM graph

Tab Dashboard:
  - Status indicator (RUN/STOP)
  - RPM gauge atau angka besar
  - Grafik RPM real-time (Chart.js, 30 titik terakhir)
  - Setpoint display + input 3 posisi rotary
  - Kp/Ki/Kd input + tombol Apply
  - Tombol START / STOP

Tab Info:
  - Judul proyek
  - Nama mahasiswa + NIM
  - Program studi + institusi
  - Tahun
  - Nama + NIP pembimbing 1 & 2
```

---

## 13. NVS / Preferences

```cpp
namespace : "afr"
Key         Type    Default   Keterangan
"sp1"       float   30.0      RPM posisi rotary 1
"sp2"       float   25.0      RPM posisi rotary 2
"sp3"       float   20.0      RPM posisi rotary 3
"pidkp"     float   0.23      PID Kp
"pidki"     float   0.30      PID Ki
"pidkd"     float   21.4      PID Kd
```

Semua nilai di atas tersimpan ke NVS setiap kali diubah dari webserver. Dibaca saat `setup()` sebelum task dimulai.

---

## 14. Build Order / Implementation Steps

```
Step 1 : Buat config.h — semua #define pin, konstanta, default value
Step 2 : Buat shared_state.h/.cpp — struct, mutex, extern
Step 3 : Buat display_helper.h/.cpp — semua fungsi 7-segment
Step 4 : Buat task_rpm.h/.cpp — ISR + moving average (jiplak referensi, ubah PPR)
Step 5 : Buat task_pid.h/.cpp — PID + Modbus write
Step 6 : Buat task_ui.h/.cpp — button, rotary, display update, LED
Step 7 : Buat web_handlers.h/.cpp — semua API handler
Step 8 : Buat task_websrv.h/.cpp — ESPAsyncWebServer setup + routes
Step 9 : Buat data/index.html + style.css + app.js
Step 10: Buat AFR.ino — setup() inisialisasi semua, xTaskCreatePinnedToCore, loop() idle
Step 11: Upload firmware, lalu upload LittleFS data folder
Step 12: Test serial monitor debug sequence
```

---

## 15. Serial Debug Output

Semua debug via `Serial.printf()` dengan prefix tag:

```
[INIT]   — setup sequence
[RPM]    — ISR + filter (verbose, bisa disable via #define DEBUG_RPM)
[PID]    — setiap compute cycle
[MODBUS] — hasil write register
[UI]     — button event, rotary change
[WEB]    — request masuk
[NVS]    — baca/tulis preferences
[WIFI]   — status koneksi
```

---

## 16. Catatan Penting untuk Claude Code

1. **ESPAsyncWebServer tidak thread-safe** — handler callback berjalan di Core 0 network thread. Semua akses ke `g_sys` di dalam handler **wajib** menggunakan `xSemaphoreTake(xMutex, portMAX_DELAY)`.

2. **GPIO 34 & 35 input-only** — jangan pernah panggil `pinMode(34/35, OUTPUT)`. Saat init, cukup `pinMode(34, INPUT)` (tanpa PULLUP karena tidak ada internal pull-up).

3. **TM1637 di I2C pins (21 & 22)** — TM1637 bukan I2C, ini bit-bang protocol. Tidak konflik dengan I2C karena tidak ada device I2C lain. Tapi perlu pastikan tidak ada `Wire.begin()` yang akan rebut pin tersebut.

4. **Serial2 untuk Modbus** — `Serial2.begin(9600, SERIAL_8E1, 16, 17)`. Format 8E1 wajib sesuai VFD.

5. **IRAM_ATTR untuk ISR** — fungsi `readRPM()` wajib pakai `IRAM_ATTR` agar berjalan dari IRAM, tidak kena cache miss saat flash sedang diakses web server.

6. **Mutex pattern** — gunakan macro helper untuk konsistensi:
   ```cpp
   #define SYS_READ(field)  ({ xSemaphoreTake(xMutex,portMAX_DELAY); auto v=g_sys.field; xSemaphoreGive(xMutex); v; })
   #define SYS_WRITE(f,v)   { xSemaphoreTake(xMutex,portMAX_DELAY); g_sys.f=(v); xSemaphoreGive(xMutex); }
   ```

7. **Stack size TaskWebSvr** — ESPAsyncWebServer butuh stack besar untuk handle JSON + LittleFS: minimal **8192 bytes**.

8. **Loop() kosong** — `loop()` hanya berisi `vTaskDelay(portMAX_DELAY)` agar task idle tidak memakan CPU.

9. **LittleFS format otomatis** — `LittleFS.begin(true)` dengan parameter `true` akan format otomatis jika filesystem kosong/corrupt.

10. **Debounce button software** — jangan pakai `delay()` di dalam task, gunakan `millis()` based debounce agar tidak block task lain.
