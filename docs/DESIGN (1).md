# DESIGN DOCUMENT — Automation Frice Ride (AFR)
## Implementasi Kontrol Kecepatan Motor Pada Pengadukan Blondo
> Proyek Akhir · Dery Adinur Saputro · 2241170126 · Politeknik Negeri Malang · 2026

---

## 1. Identitas Proyek

| Field | Nilai |
|---|---|
| Nama Alat | Automation Frice Ride |
| Judul | Implementasi Kontrol Kecepatan Motor Pada Pengadukan Blondo Dalam Pembuatan Minyak Goreng Kelapa Berbasis PID |
| Jenis | Proyek Akhir |
| Mahasiswa | Dery Adinur Saputro |
| NIM | 2241170126 |
| Program Studi | Sarjana Terapan Teknik Elektronika |
| Jurusan | Teknik Elektro |
| Institusi | Politeknik Negeri Malang |
| Tahun | 2026 |
| Pembimbing 1 | Dr. Beauty Anggraheny Ikawanty, S.T., M.T. — NIP 198110312009122001 |
| Pembimbing 2 | Irfin Sandra Asti, S.S.T., M.T — NIP 199506272024062003 |

---

## 2. Diagram Blok Sistem

```
┌─────────────────────────────────────────────────────────────────┐
│                        ESP32 (Master)                           │
│                                                                 │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐   ┌───────────┐  │
│  │ TaskRPM  │    │ TaskPID  │    │ TaskUI   │   │TaskWebSvr │  │
│  │ Core1 P5 │    │ Core1 P4 │    │ Core0 P3 │   │ Core0 P2  │  │
│  │  1ms     │    │  150ms   │    │  50ms    │   │  yield    │  │
│  └────┬─────┘    └────┬─────┘    └────┬─────┘   └─────┬─────┘  │
│       │               │               │                │        │
└───────┼───────────────┼───────────────┼────────────────┼────────┘
        │               │               │                │
        │          ┌────┴─────┐         │          ┌─────┴──────┐
        │          │ MAX485   │         │          │  WiFi STA  │
        │          │ RS-485   │         │          │ "Biznet"   │
        │          └────┬─────┘         │          └─────┬──────┘
        │               │               │                │
  ┌─────┴─────┐   ┌─────┴──────┐  ┌────┴──────┐  ┌──────┴──────┐
  │ Proximity │   │    VFD     │  │ 7-Segment │  │  Browser   │
  │ Encoder  │   │  Modbus    │  │ TM1637 x2 │  │ Dashboard  │
  │  5 PPR   │   │  Slave     │  │Button,LED │  │ afr.local  │
  └──────────┘   │  9600 8E1  │  │  Rotary   │  └────────────┘
                 └────────────┘  └───────────┘
```

---

## 3. Wiring Diagram (Deskriptif)

### 3.1 Encoder → ESP32
```
Proximity Encoder VCC → 3.3V / 5V (sesuai spec sensor)
Proximity Encoder GND → GND
Proximity Encoder OUT → GPIO 4 (dengan pull-up internal)
```

### 3.2 MAX485 → ESP32 → VFD
```
MAX485 VCC  → 3.3V
MAX485 GND  → GND
MAX485 DE   → GPIO 27
MAX485 RE   → GPIO 27 (DE dan RE disambung)
MAX485 DI   → GPIO 17 (TX ESP32)
MAX485 RO   → GPIO 16 (RX ESP32)
MAX485 A/B  → RS-485 A/B ke VFD
```

### 3.3 TM1637 Display Atas → ESP32
```
TM1637-ATAS VCC  → 3.3V atau 5V
TM1637-ATAS GND  → GND
TM1637-ATAS CLK  → GPIO 21
TM1637-ATAS DIO  → GPIO 22
```

### 3.4 TM1637 Display Bawah → ESP32
```
TM1637-BAWAH VCC  → 3.3V atau 5V
TM1637-BAWAH GND  → GND
TM1637-BAWAH CLK  → GPIO 33
TM1637-BAWAH DIO  → GPIO 32
```

### 3.5 Kontrol → ESP32
```
Push Button → GPIO 2  + 3.3V (INPUT_PULLUP, tekan = LOW)
Rotary SW-1 → GPIO 25 + 3.3V (INPUT_PULLUP, aktif = LOW) [SP=30]
Rotary SW-2 → GPIO 35 + EXT 10kΩ ke 3.3V (input-only, aktif = LOW) [SP=25]
Rotary SW-3 → GPIO 34 + EXT 10kΩ ke 3.3V (input-only, aktif = LOW) [SP=20]
LED Merah   → GPIO 23 → R (330Ω) → LED → GND
```

---

## 4. Flow Chart Lengkap

### 4.1 Setup Flow

```
START
  │
  ├─ Serial.begin(115200)
  ├─ xMutex = xSemaphoreCreateMutex()
  ├─ Display ATAS: "----" BAWAH: "----"  [DEBUG STEP 1]
  │
  ├─ Preferences.begin("afr")
  ├─ Load sp1, sp2, sp3, kp, ki, kd dari NVS
  ├─ Display ATAS: "    " BAWAH: "nUS " [DEBUG STEP 2: NVS OK]
  │
  ├─ pinMode semua GPIO
  ├─ attachInterrupt GPIO 4 → readRPM (FALLING)
  │
  ├─ Serial2.begin(9600, SERIAL_8E1, 16, 17)
  ├─ ModbusMaster.begin(1, Serial2)
  ├─ Display ATAS: "bUS " BAWAH: "----"  [DEBUG STEP 3: BUS OK]
  │
  ├─ LittleFS.begin(true)
  │
  ├─ WiFi.begin("Biznet", "12345678")
  ├─ Display ATAS: "UiFi" BAWAH: "----"  [DEBUG STEP 4: WiFi...]
  ├─ Tunggu max 10 detik
  │     ├─ Connected → Display ATAS: IP[hi] BAWAH: IP[lo]  [DEBUG STEP 5a]
  │     └─ Failed    → Display ATAS: "Err " BAWAH: "UiFi"  [DEBUG STEP 5b]
  │
  ├─ MDNS.begin("afr")
  ├─ Setup ESPAsyncWebServer routes
  ├─ server.begin()
  │
  ├─ Display ATAS: "----" BAWAH: "----"  [DEBUG STEP 6: siap]
  ├─ delay(500)
  ├─ Display normal: ATAS="STOP" BAWAH="SP"+setpoint
  │
  ├─ xTaskCreatePinnedToCore(TaskRPM,    Core1, Prio5)
  ├─ xTaskCreatePinnedToCore(TaskPID,    Core1, Prio4)
  ├─ xTaskCreatePinnedToCore(TaskUI,     Core0, Prio3)
  └─ xTaskCreatePinnedToCore(TaskWebSvr, Core0, Prio2)
```

### 4.2 TaskRPM Flow (Core 1, setiap 1ms)

```
LOOP:
  │
  ├─ Ambil snapshot: adaPulsa, intervalValid, lastPulseMicros
  │  (noInterrupts / interrupts)
  │
  ├─ Cek timeout: if (micros() - lastPulseMicros > 2.000.000)
  │     └─ Reset: current_rpm=0, rawRPM=0, reset ring buffer
  │
  ├─ if (adaPulsa && intervalValid > 0):
  │     ├─ rawRPM = 60.000.000 / intervalValid / 5.0
  │     ├─ totalRPM -= rpmReadings[readIndex]
  │     ├─ rpmReadings[readIndex] = rawRPM
  │     ├─ totalRPM += rawRPM
  │     ├─ readIndex = (readIndex+1) % 5
  │     ├─ if countReadings < 5: countReadings++
  │     └─ g_sys.current_rpm = totalRPM / countReadings  [MUTEX]
  │
  └─ vTaskDelay(1ms)
```

### 4.3 TaskPID Flow (Core 1, setiap 150ms)

```
LOOP:
  │
  ├─ Snap g_sys (mutex)
  │
  ├─ if (!snap.is_running): skip PID, pastikan VFD stop jika baru stop
  │
  ├─ Hitung dt = (millis - lastPIDTime) / 1000.0
  │
  ├─ error      = setpoint - current_rpm
  ├─ integral   = constrain(integral + error*dt, -200, 200)
  ├─ derivative = (error - lastError) / dt
  ├─ baseFreq   = (66.0 * setpoint) + 530.0
  ├─ outputPID  = baseFreq + Kp*error + Ki*integral + Kd*derivative
  ├─ outputPID  = constrain(outputPID, 0, 5000)
  │
  ├─ node.writeSingleRegister(0x2001, (uint16_t)outputPID)
  ├─ Update g_sys.modbus_ok, g_sys.vfd_freq_raw, g_sys.pid_output  [MUTEX]
  │
  ├─ lastError   = error
  ├─ lastPIDTime = millis()
  │
  └─ vTaskDelay(150ms)
```

### 4.4 TaskUI Flow (Core 0, setiap 50ms)

```
LOOP:
  │
  ├─ ── Button Scan ──
  │  currentBtn = digitalRead(2)
  │  if (lastBtn==HIGH && currentBtn==LOW):
  │      delay(50ms debounce — menggunakan millis bukan delay())
  │      if (digitalRead(2)==LOW):
  │          if (!is_running): CALL startSystem()
  │          else:             CALL stopSystem()
  │  lastBtn = currentBtn
  │
  ├─ ── Rotary Scan (hanya saat STOP) ──
  │  if (!is_running):
  │      if (pin25==LOW): setpoint = sp_pos1 (default 30)
  │      elif (pin35==LOW): setpoint = sp_pos2 (default 25)
  │      elif (pin34==LOW): setpoint = sp_pos3 (default 20)
  │      Update g_sys.setpoint  [MUTEX]
  │
  ├─ ── LED Update ──
  │  digitalWrite(23, is_running ? HIGH : LOW)
  │
  ├─ ── Display Update (setiap 250ms) ──
  │  if (transition_run && millis()-transition_start < 2000):
  │      ATAS: "RUN "
  │      BAWAH: current_rpm (XX.X)
  │  elif (is_running):
  │      ATAS: setpoint (misal "  25" atau "  30")
  │      BAWAH: current_rpm (XX.X)
  │  else (STOP):
  │      ATAS: "STOP"
  │      BAWAH: "SP" + setpoint (misal SP20, SP25, SP30)
  │
  └─ vTaskDelay(50ms)
```

### 4.5 startSystem() / stopSystem()

```
startSystem():
  ├─ node.writeSingleRegister(0x2000, 0x0001)  [Modbus: VFD RUN]
  ├─ digitalWrite(23, HIGH)                    [LED ON]
  ├─ g_sys.is_running = true              [MUTEX]
  ├─ g_sys.transition_run = true          [MUTEX]
  ├─ g_sys.transition_start_ms = millis() [MUTEX]
  ├─ Reset PID state (integral=0, lastError=0, lastPIDTime=millis())
  └─ Reset RPM ring buffer

stopSystem():
  ├─ for 3x retry: node.writeSingleRegister(0x2000, 0x0005) + delay(150)
  ├─ node.writeSingleRegister(0x2001, 0)       [Modbus: freq=0]
  ├─ digitalWrite(23, LOW)                     [LED OFF]
  └─ g_sys.is_running = false             [MUTEX]
```

---

## 5. 7-Segment Character Map Lengkap

```
Segment layout TM1637:
     _
    |_|
    |_|

Bit : a=0x01, b=0x02, c=0x04, d=0x08, e=0x10, f=0x20, g=0x40, dp=0x80

Karakter yang dibutuhkan:
  'S' = a|c|d|f|g     = 0x6D  (SEG_A|SEG_C|SEG_D|SEG_F|SEG_G)
  'T' = a|d|e|f|g     = 0x78  (lowercase t: d|e|f|g = 0x58, pakai 't')
  't' = d|e|f|g       = 0x78  (lebih mirip t pada 7-seg)
  'O' = a|b|c|d|e|f   = 0x3F  (= digit 0)
  'P' = a|b|e|f|g     = 0x73
  'R' = e|g           = 0x50  (lowercase r)
  'r' = e|g           = 0x50
  'U' = b|c|d|e|f     = 0x3E
  'N' = a|b|c|e|f     = 0x37  (atau n = c|e|g = 0x54)
  'n' = c|e|g         = 0x54
  '-' = g             = 0x40
  ' ' = 0             = 0x00
  'E' = a|d|e|f|g     = 0x79
  'F' = a|e|f|g       = 0x71
  'i' = c             = 0x04  (lebih baik I = b|c = 0x06)

Kata yang dipakai:
  "STOP" = {0x6D, 0x78, 0x3F, 0x73}   → S-t-O-P
  "RUN " = {0x50, 0x3E, 0x54, 0x00}   → r-U-n-space
  "SP"   = {0x6D, 0x73}               → S-P (2 digit pertama, 2 digit = nilai)
  "----" = {0x40, 0x40, 0x40, 0x40}   → debug: minus semua
  "nUS " = {0x54, 0x3E, 0x6D, 0x00}   → debug: n-U-S-space (NVS)
  "bUS " = {0x7C, 0x3E, 0x6D, 0x00}   → debug: b-U-S-space (BUS/Modbus)
  "UiFi" = {0x3E, 0x06, 0x71, 0x06}   → debug: U-i-F-i (WiFi)
  "Err " = {0x79, 0x50, 0x50, 0x00}   → debug: E-r-r-space
```

---

## 6. Web Dashboard UI Design

### 6.1 Layout

```
┌────────────────────────────────────────────────────┐
│  🔴 AFR — Automation Frice Ride                    │
│  ─────────────────────────────────────────────────  │
│  [ Dashboard ]  [ Info ]                           │
├────────────────────────────────────────────────────┤
│                   DASHBOARD TAB                    │
│                                                    │
│  ┌─────────────────┐   ┌─────────────────────────┐ │
│  │   STATUS        │   │      RPM CHART          │ │
│  │   ● RUN / STOP  │   │  ___                    │ │
│  │                 │   │ /   \___/               │ │
│  │   Setpoint      │   │ 0    10s    20s    30s  │ │
│  │   [  25.0 ] RPM │   └─────────────────────────┘ │
│  │                 │                                │
│  │   Actual RPM    │   ┌─────────────────────────┐ │
│  │   24.7 RPM      │   │   PID PARAMETERS        │ │
│  │                 │   │   Kp: [0.23]            │ │
│  │  [▶ START]      │   │   Ki: [0.30]            │ │
│  │  [■ STOP ]      │   │   Kd: [21.4]            │ │
│  └─────────────────┘   │   [Apply PID]           │ │
│                        └─────────────────────────┘ │
│  ┌───────────────────────────────────────────────┐  │
│  │   SETPOINT POSITIONS                          │  │
│  │   Pos 1 (SP30): [30.0] RPM                    │  │
│  │   Pos 2 (SP25): [25.0] RPM                    │  │
│  │   Pos 3 (SP20): [20.0] RPM                    │  │
│  │   [Save Setpoints]                            │  │
│  └───────────────────────────────────────────────┘  │
│                                   Modbus: ● OK      │
├────────────────────────────────────────────────────┤
│                     INFO TAB                       │
│                                                    │
│  ┌───────────────────────────────────────────────┐  │
│  │  IMPLEMENTASI KONTROL KECEPATAN MOTOR         │  │
│  │  PADA PENGADUKAN BLONDO DALAM                 │  │
│  │  PEMBUATAN MINYAK GORENG KELAPA               │  │
│  │  BERBASIS PID                                 │  │
│  │                                               │  │
│  │  Proyek Akhir                                 │  │
│  │                                               │  │
│  │  Oleh:                                        │  │
│  │  Dery Adinur Saputro                          │  │
│  │  2241170126                                   │  │
│  │                                               │  │
│  │  Program Studi Sarjana Terapan                │  │
│  │  Teknik Elektronika                           │  │
│  │  Jurusan Teknik Elektro                       │  │
│  │  Politeknik Negeri Malang                     │  │
│  │  2026                                         │  │
│  │                                               │  │
│  │  Pembimbing 1:                                │  │
│  │  Dr. Beauty Anggraheny Ikawanty, S.T., M.T.   │  │
│  │  NIP 198110312009122001                       │  │
│  │                                               │  │
│  │  Pembimbing 2:                                │  │
│  │  Irfin Sandra Asti, S.S.T., M.T              │  │
│  │  NIP 199506272024062003                       │  │
│  └───────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────┘
```

### 6.2 Polling Logic (app.js)

```javascript
// Polling setiap 500ms
setInterval(fetchStatus, 500)

function fetchStatus() {
    fetch('/api/status')
        .then(r => r.json())
        .then(data => {
            updateStatusBadge(data.is_running)
            updateRPMDisplay(data.current_rpm)
            updateSetpointDisplay(data.setpoint)
            updateModbusIndicator(data.modbus_ok)
            updateChart(data.current_rpm)  // append ke chart
            updatePIDFields(data.kp, data.ki, data.kd)
        })
}

// Chart: simpan 60 titik terakhir (30 detik @ 500ms interval)
const rpmHistory = []
const MAX_POINTS = 60
```

---

## 7. Modbus RTU Protocol Detail

### 7.1 Physical Layer
```
Interface : RS-485 half-duplex via MAX485
Baud Rate : 9600
Format    : 8 data bits, Even parity, 1 stop bit (8E1)
Konektor  : Terminal A (+), B (-) ke VFD
```

### 7.2 Register Map VFD
```
Register  Mode  Nilai     Keterangan
0x2000    W     0x0001    Command: RUN
0x2000    W     0x0005    Command: STOP (retry 3x)
0x2001    W     0–5000    Frekuensi/speed setpoint
```

### 7.3 preTransmission / postTransmission
```cpp
void preTransmission() {
    digitalWrite(MAX485_DE_RE, HIGH);  // Driver Enable: siap transmit
}

void postTransmission() {
    Serial2.flush();
    delayMicroseconds(200);            // tunggu transmit selesai
    digitalWrite(MAX485_DE_RE, LOW);   // kembali ke receive mode
}
```

---

## 8. PID Design

### 8.1 Tuning Parameter Default
```
Kp = 0.23   (proportional — koreksi langsung thd error)
Ki = 0.30   (integral     — eliminasi steady-state error)
Kd = 21.4   (derivative   — redam overshoot/osilasi)

Anti-windup: clamp integral ±200
```

### 8.2 Base Frequency Formula
```
Tujuan: offset frekuensi VFD agar motor langsung berputar
di sekitar setpoint tanpa mengandalkan PID dari nol.

baseFreq = (66 × setpoint) + 530

Contoh:
  setpoint 20 RPM → baseFreq = (66×20)+530 = 1850
  setpoint 25 RPM → baseFreq = (66×25)+530 = 2180
  setpoint 30 RPM → baseFreq = (66×30)+530 = 2510
```

### 8.3 Output Range
```
outputPID = constrain(baseFreq + PID_correction, 0, 5000)
Dikirim ke register 0x2001 sebagai uint16_t
```

---

## 9. Timing Budget

```
Core 1:
  TaskRPM  : 1ms  × eksekusi ~0.1ms  = utilitas <10%
  TaskPID  : 150ms × eksekusi ~5ms   = utilitas  ~3%
  Headroom : ~87% untuk ISR handling

Core 0:
  TaskUI     : 50ms  × eksekusi ~2ms  = utilitas  ~4%
  TaskWebSvr : ESPAsync callback-based, ~10-20ms saat ada request
  Headroom   : ~76% normal, ~56% saat web request
```

---

## 10. Error Handling & Edge Cases

| Kondisi | Penanganan |
|---|---|
| WiFi gagal connect | Lanjut tanpa web, display "Err" "UiFi" di 7-seg, Serial debug |
| Modbus write gagal | Retry 3x (saat STOP), flag `modbus_ok=false`, tampil di web |
| RPM timeout >2 detik | Reset RPM=0, ring buffer reset |
| GPIO 34/35 floating | Wajib pull-up eksternal 10kΩ |
| LittleFS gagal mount | Lanjut tanpa static files, API tetap jalan |
| Rotary tidak ada yang LOW | Pertahankan setpoint terakhir dari NVS |
| Semua rotary LOW bersamaan | Prioritas: pin25 > pin35 > pin34 |

---

## 11. Memory Estimate

```
Task Stacks:
  TaskRPM    : 2048 bytes
  TaskPID    : 4096 bytes
  TaskUI     : 4096 bytes
  TaskWebSvr : 8192 bytes  ← ESPAsync butuh besar
  Total stack: ~18 KB dari DRAM (~320KB available)

Global:
  SharedState struct : ~100 bytes
  Modbus buffers     : ~256 bytes
  JSON doc (stack)   : ~384 bytes (StaticJsonDocument)
  
Flash:
  Firmware  : estimasi ~600KB
  LittleFS  : 1MB partition (index.html + css + js)
```

---

## 12. Checklist Testing

```
[ ] GPIO 34, 35 terbaca LOW saat rotary ditekan (pull-up external dipasang)
[ ] ISR terpicu saat encoder berputar, Serial debug RPM muncul
[ ] RPM timeout reset ke 0 setelah motor berhenti >2 detik
[ ] Moving average 5 sample bekerja, RPM stabil
[ ] Push button toggle start/stop dengan debounce 50ms
[ ] LED GPIO 23 nyala saat RUN, mati saat STOP
[ ] Display ATAS "STOP" saat berhenti, "RUN" 2 detik saat start, lalu setpoint
[ ] Display BAWAH SP+nilai saat STOP, RPM actual saat RUN
[ ] Modbus STOP command retry 3x
[ ] Modbus RUN command berhasil (Serial debug: [MODBUS] result=0)
[ ] PID output masuk range 0-5000
[ ] WiFi connect ke "Biznet" dan mDNS "afr.local" accessible
[ ] /api/status return JSON lengkap
[ ] Ubah Kp/Ki/Kd dari web → tersimpan di NVS → survive restart
[ ] Ubah sp_pos1/2/3 dari web → tersimpan di NVS → survive restart
[ ] Tab Info menampilkan data mahasiswa dan pembimbing lengkap
[ ] Grafik RPM real-time update setiap 500ms
[ ] LittleFS format auto saat pertama upload
[ ] Debug sequence 7-segment tampil urut saat boot
```
