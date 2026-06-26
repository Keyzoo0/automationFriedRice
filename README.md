# Automation Frice Ride (AFR)

**Implementasi Kontrol Kecepatan Motor Pada Pengadukan Blondo Dalam Pembuatan Minyak Goreng Kelapa Berbasis PID**

> Proyek Akhir · Dery Adinur Saputro · 2241170126  
> Program Studi Sarjana Terapan Teknik Elektronika  
> Jurusan Teknik Elektro · Politeknik Negeri Malang · 2026

---

## 📋 Daftar Isi

- [Tentang Proyek](#tentang-proyek)
- [Diagram Sistem](#diagram-sistem)
- [Hardware](#hardware)
- [Arsitektur Software](#arsitektur-software)
- [Pin Mapping](#pin-mapping)
- [Cara Upload](#cara-upload)
- [API Endpoints](#api-endpoints)
- [Web Dashboard](#web-dashboard)
- [Serial Commands](#serial-commands)
- [Debug Boot Sequence](#debug-boot-sequence)
- [Dependencies](#dependencies)
- [Struktur File](#struktur-file)
- [Lisensi](#lisensi)

---

## 🎯 Tentang Proyek

Sistem kontrol kecepatan motor berbasis **PID** untuk pengadukan blondo (ampas kelapa) dalam proses pembuatan minyak goreng kelapa. ESP32 bertugas sebagai **master** yang:

1. Membaca RPM dari **proximity encoder** (5 PPR) via interrupt
2. Menghitung output **PID** secara real-time
3. Mengirim perintah ke **VFD** (Variable Frequency Drive) via **Modbus RTU RS-485**
4. Menyajikan monitoring real-time via **web dashboard** (WiFi)
5. Menampilkan informasi via **dual 7-segment TM1637**

### ✨ Fitur

| Fitur | Detail |
|---|---|
| Kontrol PID | Kp=0.23, Ki=0.30, Kd=21.4 (tunable via web) |
| Anti-windup | Integral clamp ±200 |
| Base frequency | `(66 × SP) + 530` sebagai feedforward |
| 3 Preset Speed | Rotary switch: 20 / 25 / 30 RPM (configurable) |
| Dual Display | ATAS: setpoint/status, BAWAH: RPM aktual |
| Web Dashboard | SPA 2 tab (Dashboard + Info), Chart.js grafik real-time |
| REST API | 6 endpoint untuk kontrol & monitoring |
| NVS Storage | PID & setpoint tersimpan di flash, survive restart |
| Debug Sequence | 6-step boot diagnostic di 7-segment |

---

## 🧱 Diagram Sistem

```
                         ┌────────────────────────────────────────────────────┐
                         │                      ESP32                         │
                         │                                                    │
                         │  ┌──────────┐  ┌──────────┐  ┌────────┐ ┌───────┐ │
                         │  │ TaskRPM  │  │ TaskPID  │  │ TaskUI │ │ Web   │ │
                         │  │ Core1 P5 │  │ Core1 P4 │  │Core0 P3│ │Core0  │ │
                         │  │   1ms    │  │  150ms   │  │  50ms  │ │ P2    │ │
                         │  └────┬─────┘  └────┬──────┘  └───┬────┘ └──┬────┘ │
                         └───────┼──────────────┼──────────────┼──────────┼─────┘
                                 │              │              │          │
                            ┌────┴─────┐   ┌────┴──────┐ ┌────┴─────┐ ┌──┴───────┐
                            │Proximity │   │ MAX485    │ │ TM1637   │ │ WiFi STA │
                            │Encoder   │   │ RS-485    │ │ Display  │ │ "Biznet" │
                            │ 5 PPR    │   └────┬──────┘ │ Button   │ └──┬───────┘
                            └──────────┘        │        │ Rotary    │    │
                                           ┌────┴──────┐ │ LED       │    │
                                           │    VFD    │ └───────────┘ ┌──┴───────┐
                                           │  Modbus   │               │ Browser  │
                                           │  9600 8E1 │               │Dashboard │
                                           └───────────┘               │afr.local │
                                                                       └──────────┘
```

---

## 🔧 Hardware

### Komponen

| No | Komponen | Spesifikasi | Fungsi |
|---|---|---|---|
| 1 | ESP32 | DOIT ESP32 DEVKIT V1 | Master kontroler |
| 2 | Proximity Encoder | 5 PPR, NPN NO | Sensor RPM |
| 3 | VFD + Motor | 3 phase, Modbus RTU | Penggerak motor |
| 4 | MAX485 | RS-485 transceiver | Konverter UART ↔ RS-485 |
| 5 | TM1637 (Atas) | 4-digit 7-segment | Display status/setpoint |
| 6 | TM1637 (Bawah) | 4-digit 7-segment | Display RPM aktual |
| 7 | Push Button | NO, momentary | Start/Stop motor |
| 8 | Rotary Switch | 3 posisi | Pemilih setpoint |
| 9 | LED Merah | 5mm | Indikator RUN |

### Wiring

```
Encoder:          MAX485:            TM1637 ATAS:       TM1637 BAWAH:
VCC → 3.3V        VCC → 3.3V         VCC → 3.3V         VCC → 3.3V
GND → GND         GND → GND          GND → GND          GND → GND
OUT → GPIO 4      DE  → GPIO 27      CLK → GPIO 21      CLK → GPIO 33
                  RE  → GPIO 27      DIO → GPIO 22      DIO → GPIO 32
                  DI  → GPIO 17
Push Button:      RO  → GPIO 16      Rotary:            LED:
GPIO 2 → GND      A/B → VFD A/B     GPIO 25/34/35       GPIO 23 → R 330Ω → LED → GND
(INPUT_PULLUP)                      (INPUT_PULLUP/ext)
```

---

## 🧠 Arsitektur Software

### FreeRTOS Task Map

```
Core 1 (Realtime)
├── TaskRPM    Prio 5  delay 1ms    — ISR data → hitung RPM (moving avg 5 sample)
├── TaskPID    Prio 4  delay 150ms  — PID compute → Modbus write VFD

Core 0 (IO / Network)
├── TaskUI     Prio 3  delay 50ms   — Button scan, rotary scan, 7-seg update, LED
└── TaskWebSvr Prio 2  yield        — Inisialisasi ESPAsyncWebServer
```

### State Machine Display

```
POWER ON
    │
    ▼
┌─────────┐                    ┌─────────┐
│  STOP   │ ←─── Tombol ───→  │  RUN    │
│ ATAS:SP │    ditekan        │ ATAS:SP │
│ BWH:SP+X│                   │ BWH:RPM │
└─────────┘                    └─────────┘
     │                              │
     │ [Transisi 2 detik]           │
     │ ATAS: "RUN "                 │
     │ BWH: RPM aktual              │
     └──────────────────────────────┘
```

---

## 📍 Pin Mapping

| Pin ESP32 | Terhubung ke | Mode | Keterangan |
|-----------|-------------|------|------------|
| GPIO 2 | Push Button | INPUT_PULLUP | START/STOP, Active LOW |
| GPIO 4 | Proximity Encoder | INPUT_PULLUP | FALLING ISR, 5 PPR |
| GPIO 16 | MAX485 RO | RX2 | Modbus receive |
| GPIO 17 | MAX485 DI | TX2 | Modbus transmit |
| GPIO 21 | TM1637 ATAS CLK | OUTPUT | Bit-bang |
| GPIO 22 | TM1637 ATAS DIO | OUTPUT | Bit-bang |
| GPIO 23 | LED Merah | OUTPUT | HIGH=RUN, LOW=STOP |
| GPIO 25 | Rotary pos-1 (SP=30) | INPUT_PULLUP | Active LOW |
| GPIO 27 | MAX485 DE/RE | OUTPUT | Direction RS-485 |
| GPIO 32 | TM1637 BAWAH DIO | OUTPUT | Bit-bang |
| GPIO 33 | TM1637 BAWAH CLK | OUTPUT | Bit-bang |
| GPIO 34 | Rotary pos-3 (SP=20) | INPUT | ⚠️ Input-only, ext pull-up |
| GPIO 35 | Rotary pos-2 (SP=25) | INPUT | ⚠️ Input-only, ext pull-up |

> **⚠️ Catatan GPIO 34 & 35:** Input-only, tidak support OUTPUT dan tidak ada internal pull-up. Wajib pasang resistor pull-up eksternal 10kΩ ke 3.3V.

---

## 📥 Cara Upload

### 1. Persyaratan

**PlatformIO** (recommended):
```ini
; platformio.ini
[env:esp32]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
board_build.filesystem = littlefs

lib_deps =
    lennarthennigs/TM1637@^1.0.0
    4-20ma/ModbusMaster@^2.0.1
    me-no-dev/ESPAsyncWebServer@^1.2.3
    me-no-dev/AsyncTCP@^1.1.1
    bblanchon/ArduinoJson@^7.0.0
```

**Arduino IDE** — Install library via Library Manager:
- `TM1637` by Lennart Hennigs
- `ModbusMaster` by 4-20ma
- `ESPAsyncWebServer` by me-no-dev
- `AsyncTCP` by me-no-dev
- `ArduinoJson` by Benoit Blanchon

### 2. Upload Firmware

```bash
# PlatformIO
pio run --target upload

# Arduino IDE
# Sketch → Upload (Ctrl+U)
```

### 3. Upload LittleFS (data/)

```bash
# PlatformIO
pio run --target uploadfs

# Arduino IDE — install ESP32 LittleFS Data Upload plugin
# Tools → ESP32 LittleFS Data Upload
```

> ⚠️ Tutup Serial Monitor sebelum upload LittleFS!

### 4. Akses Web Dashboard

```
WiFi SSID : "Biznet"
Password  : "12345678"
URL       : http://afr.local
atau       http://<IP_ESP32>
```

---

## 🌐 API Endpoints

| Method | Endpoint | Parameter | Deskripsi |
|--------|----------|-----------|-----------|
| GET | `/api/status` | — | JSON status lengkap (polling 500ms) |
| GET | `/api/start` | — | Jalankan motor |
| GET | `/api/stop` | — | Hentikan motor |
| GET | `/api/setpoint` | `?pos=1&value=30` | Set nilai RPM posisi rotary |
| GET | `/api/pid` | `?kp=0.23&ki=0.3&kd=21.4` | Update PID gains |
| GET | `/api/restart` | — | Restart ESP32 |

### Response `/api/status`

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
  "uptime_ms": 12345,
  "heap": 182736
}
```

---

## 🖥️ Web Dashboard

Dashboard SPA 2 tab dengan auto-polling setiap 500ms.

### Tab Dashboard
- Status RUN/STOP dengan LED indikator
- RPM aktual (angka besar) + setpoint
- Grafik RPM real-time (Chart.js, 60 titik terakhir)
- Kontrol START / STOP
- Input PID Kp/Ki/Kd + tombol Apply
- Input setpoint 3 posisi rotary + tombol Save
- Modbus status indicator
- System info (uptime, PID output, VFD freq, heap)

### Tab Info
- Judul proyek
- Identitas mahasiswa & pembimbing
- Program studi & institusi

---

## 💻 Serial Commands

Output debug dengan prefix tag:

| Prefix | Sumber | Keterangan |
|--------|--------|------------|
| `[INIT]` | setup() | Boot sequence |
| `[PID]` | TaskPID | Setiap siklus PID (SP, RPM, error, output) |
| `[MODBUS]` | TaskPID | Hasil write register (OK/FAIL) |
| `[SYS]` | start/stopSystem | Event start/stop |
| `[NVS]` | saveNVS() | Simpan preferences |
| `[WIFI]` | setup() | Status koneksi |

---

## 🚦 Debug Boot Sequence

6-step diagnostic di 7-segment saat startup:

```
Step 1: ATAS="----" BAWAH="----"  → GPIO init
Step 2: ATAS="    " BAWAH="nUS "  → NVS load
Step 3: ATAS="bUS " BAWAH="----"  → Modbus init
Step 4: ATAS="UiFi" BAWAH="----"  → WiFi connecting
Step 5: ATAS=IP[hi] BAWAH=IP[lo]  → WiFi connected (atau "Err "/"UiFi")
Step 6: ATAS="----" BAWAH="----"  → System ready
```

---

## 📦 Dependencies

| Library | Version | Fungsi |
|---------|---------|--------|
| `TM1637` | ≥1.0.0 | Driver 7-segment display |
| `ModbusMaster` | ≥2.0.1 | Modbus RTU master |
| `ESPAsyncWebServer` | ≥1.2.3 | Async HTTP server |
| `AsyncTCP` | ≥1.1.1 | TCP async support |
| `ArduinoJson` | ≥7.0.0 | JSON serialization |

---

## 📁 Struktur File

```
dery_AFR/
├── dery_AFR.ino           # Entry point — semua kode firmware
├── data/
│   ├── index.html         # SPA Dashboard (Tailwind + Chart.js)
│   ├── style.css          # Custom styles
│   └── app.js             # Polling + chart + event handlers
├── docs/
│   ├── BUILD_PLAN.md      # Rencana implementasi detail
│   └── DESIGN.md          # Dokumen desain lengkap
└── README.md              # Dokumentasi ini
```

---

## 📄 Lisensi

Proyek ini dikembangkan untuk keperluan **Proyek Akhir** mahasiswa.
© 2026 Dery Adinur Saputro — Politeknik Negeri Malang.
