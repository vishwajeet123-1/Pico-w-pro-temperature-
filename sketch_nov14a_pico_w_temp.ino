/* =============================================================================
   Raspberry Pi Pico W — Professional Temperature Monitor System
   
   Features:
   - Built-in temperature sensor monitoring
   - Beautiful responsive web interface
   - Real-time data updates every 2 minutes
   - LED indicators for WiFi connection and sampling
   - Daily/Weekly temperature graphs
   - Data export functionality
   - NTP time synchronization
   
   Version: 3.0.0
   Author: Enhanced for production use
   ============================================================================= */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <hardware/adc.h>
#include <hardware/pwm.h>
#include <pico/stdlib.h>
#include <time.h>
#include <math.h>

#define PI 3.14159265358979323846f

// ========================== CONFIGURATION ==========================
// WiFi Settings - CHANGE THESE TO YOUR NETWORK
#define WIFI_SSID           "DUBE BROADBAND 9423351431"
#define WIFI_PASS           "78877532"

// Device Information
#define DEVICE_NAME         "PicoTemp Pro"
#define FIRMWARE_VERSION    "3.0.0"

// Server Settings
#define HTTP_PORT           80

// Hardware Pins
#define LED_PIN             25                // Built-in LED

// Timing Settings
#define SAMPLE_INTERVAL_MS  (2UL * 60UL * 1000UL)    // 2 minutes in milliseconds
#define MAX_SAMPLES         2000                      // Maximum stored samples
#define NTP_SYNC_INTERVAL_MS (6UL * 3600UL * 1000UL) // 6 hours

// File System Paths
#define CSV_PATH            "/data.csv"
#define SUMMARY_PATH        "/summary.json"

// NTP Settings
#define NTP_SERVER          "pool.ntp.org"
#define NTP_PACKET_SIZE     48

// Timezone offset in seconds (adjust to your timezone)
// Examples: IST = 5*3600 + 30*60, EST = -5*3600, PST = -8*3600
const long TZ_OFFSET_SECONDS = 5 * 3600 + 30 * 60;   // IST (UTC+5:30)

// Temperature calibration offset (if needed)
const float TEMP_CAL_OFFSET = 0.0f;

// ========================== GLOBAL VARIABLES ==========================
WiFiServer server(HTTP_PORT);
WiFiUDP ntpUDP;

// Temperature sample structure
struct Sample {
  uint32_t epoch;      // UTC epoch timestamp
  float    tempC;      // Temperature in Celsius
};

// Data storage
Sample samples[MAX_SAMPLES];
int    sampleCount = 0;

// Timing variables
unsigned long lastSampleMs   = 0;
unsigned long lastNtpSyncSec = 0;
unsigned long ntpSyncMillis  = 0;
unsigned long lastNtpCheckMs = 0;

// LED control
uint pwm_slice, pwm_chan;
bool  breathing   = false;
unsigned long breathStart = 0;
uint32_t breathDuration   = 15000;

// Summary tracking
uint32_t lastSummaryDay = 0;
String dailySummary, weeklySummary;

// ========================== TIME FUNCTIONS ==========================

/**
 * Get current time from NTP server
 * Returns: Unix epoch timestamp or 0 on failure
 */
unsigned long getNTPTime() {
  byte packet[NTP_PACKET_SIZE] = {0};
  
  // Initialize NTP request packet
  packet[0] = 0b11100011;   // LI, Version, Mode
  packet[12] = 49;
  packet[13] = 0x4E;
  packet[14] = 49;
  packet[15] = 52;

  // Send NTP request
  if (!ntpUDP.beginPacket(NTP_SERVER, 123)) return 0;
  ntpUDP.write(packet, NTP_PACKET_SIZE);
  ntpUDP.endPacket();

  // Wait for response (max 1.5 seconds)
  uint32_t started = millis();
  while (millis() - started < 1500) {
    if (ntpUDP.parsePacket() >= NTP_PACKET_SIZE) {
      ntpUDP.read(packet, NTP_PACKET_SIZE);
      
      // Extract timestamp from packet
      unsigned long high = word(packet[40], packet[41]);
      unsigned long low  = word(packet[42], packet[43]);
      unsigned long secs = (high << 16) | low;
      
      // Convert from 1900 epoch to 1970 epoch
      return secs - 2208988800UL;
    }
    delay(10);
  }
  return 0;
}

/**
 * Get current UTC epoch time
 */
uint32_t getEpochUTC() {
  if (lastNtpSyncSec == 0 || ntpSyncMillis == 0) {
    return millis() / 1000UL;  // Fallback to millis
  }
  return lastNtpSyncSec + ((millis() - ntpSyncMillis) / 1000UL);
}

/**
 * Get local epoch time (with timezone offset)
 */
uint32_t getLocalEpoch() {
  return getEpochUTC() + TZ_OFFSET_SECONDS;
}

/**
 * Format epoch timestamp as readable date-time string
 */
String formatDateTime(uint32_t epoch) {
  time_t t = epoch;
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return String(buf);
}

/**
 * Format epoch timestamp as date string
 */
String formatDate(uint32_t epoch) {
  time_t t = epoch;
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
  return String(buf);
}

// ========================== SENSOR FUNCTIONS ==========================

/**
 * Read temperature from Pico W's built-in sensor
 * Returns: Temperature in Celsius
 */
float readTempC() {
  adc_select_input(4);  // Channel 4 is internal temperature sensor
  uint16_t raw = adc_read();
  
  // Convert ADC reading to voltage
  float voltage = raw * 3.3f / 4095.0f;
  
  // Convert voltage to temperature using RP2040 formula
  float temp = 27.0f - (voltage - 0.706f) / 0.001721f;
  
  return temp + TEMP_CAL_OFFSET;
}

// ========================== LED CONTROL ==========================

/**
 * Initialize PWM for LED control
 */
void initPWM() {
  gpio_set_function(LED_PIN, GPIO_FUNC_PWM);
  pwm_slice = pwm_gpio_to_slice_num(LED_PIN);
  pwm_chan  = pwm_gpio_to_channel(LED_PIN);
  pwm_set_wrap(pwm_slice, 65535);  // 16-bit resolution
  pwm_set_chan_level(pwm_slice, pwm_chan, 0);
  pwm_set_enabled(pwm_slice, true);
}

/**
 * Set LED brightness (0-65535)
 */
void setBrightness(uint16_t level) {
  pwm_set_chan_level(pwm_slice, pwm_chan, level);
}

/**
 * Test LED on startup
 */
void testLED() {
  Serial.println("Testing LED...");
  setBrightness(32768);  // 50% brightness
  delay(1000);
  setBrightness(0);
  Serial.println("LED test complete.");
}

/**
 * Start breathing LED animation
 */
void startBreathing(uint32_t durationMs = 15000) {
  breathing      = true;
  breathStart    = millis();
  breathDuration = durationMs;
  Serial.println("LED breathing started for " + String(durationMs) + "ms");
}

/**
 * Update breathing LED animation (call in loop)
 */
void updateBreathing() {
  if (!breathing) return;
  
  unsigned long elapsed = millis() - breathStart;
  
  // Stop breathing after duration
  if (elapsed >= breathDuration) {
    breathing = false;
    setBrightness(0);
    Serial.println("LED breathing complete.");
    return;
  }
  
  // Calculate sine wave breathing effect
  float phase = (elapsed % 3000) / 3000.0f * 2.0f * PI;
  float intensity = (sin(phase) + 1.0f) / 2.0f;
  setBrightness((uint16_t)(intensity * 50000));
}

// ========================== FILE SYSTEM ==========================

/**
 * Append sample to CSV file
 */
void appendCSV(uint32_t epoch, float temp) {
  File f = LittleFS.open(CSV_PATH, "a");
  if (f) {
    f.printf("%u,%.2f\n", epoch, temp);
    f.close();
  }
}

/**
 * Save all samples to CSV file
 */
void saveAllToFS() {
  LittleFS.remove(CSV_PATH);
  File f = LittleFS.open(CSV_PATH, "w");
  if (!f) return;
  
  for (int i = 0; i < sampleCount; ++i) {
    f.printf("%u,%.2f\n", samples[i].epoch, samples[i].tempC);
  }
  f.close();
}

/**
 * Load samples from CSV file
 */
void loadCSV() {
  sampleCount = 0;
  if (!LittleFS.exists(CSV_PATH)) return;
  
  File f = LittleFS.open(CSV_PATH, "r");
  if (!f) return;
  
  while (f.available() && sampleCount < MAX_SAMPLES) {
    String line = f.readStringUntil('\n');
    line.trim();
    
    int comma = line.indexOf(',');
    if (comma <= 0) continue;
    
    samples[sampleCount].epoch  = line.substring(0, comma).toInt();
    samples[sampleCount].tempC  = line.substring(comma + 1).toFloat();
    ++sampleCount;
  }
  f.close();
}

/**
 * Save summary to file
 */
void saveSummary(const String &json) {
  File f = LittleFS.open(SUMMARY_PATH, "w");
  if (f) {
    f.print(json);
    f.close();
  }
}

/**
 * Load summary from file
 */
String loadSummary() {
  if (!LittleFS.exists(SUMMARY_PATH)) return "{}";
  
  File f = LittleFS.open(SUMMARY_PATH, "r");
  String s = f.readString();
  f.close();
  return s;
}

// ========================== DATA FILTERING ==========================

/**
 * Get filtered data for daily or weekly view
 */
void getFilteredData(bool isDaily, String &jsonOut) {
  if (sampleCount == 0) {
    jsonOut = "[]";
    return;
  }

  uint32_t now = getLocalEpoch();
  uint32_t cutoff = isDaily ? (now - (now % 86400)) : (now - 7 * 86400);
  bool first = true;

  jsonOut = "[";
  for (int i = 0; i < sampleCount; ++i) {
    uint32_t local = samples[i].epoch + TZ_OFFSET_SECONDS;
    bool inPeriod = (local >= cutoff);
    
    if (isDaily) {
      inPeriod = inPeriod && (local / 86400 == now / 86400);
    }
    
    if (inPeriod) {
      if (!first) jsonOut += ",";
      first = false;
      
      char buf[64];
      snprintf(buf, sizeof(buf), "{\"epoch\":%u,\"temp\":%.2f}", 
               samples[i].epoch, samples[i].tempC);
      jsonOut += buf;
    }
  }
  jsonOut += "]";
}

// ========================== ANALYTICS ==========================

/**
 * Generate temperature summary statistics
 */
String generateSummary(bool daily) {
  if (sampleCount == 0) {
    return daily ? "{\"error\":\"No data\"}" : "{\"error\":\"No weekly data\"}";
  }

  uint32_t now     = getLocalEpoch();
  uint32_t cutoff  = daily ? now - (now % 86400) : now - 7 * 86400;
  
  float sum = 0, min = 200, max = -200;
  int   cnt = 0;

  // Calculate statistics
  for (int i = 0; i < sampleCount; ++i) {
    uint32_t local = samples[i].epoch + TZ_OFFSET_SECONDS;
    bool inPeriod = local >= cutoff;
    
    if (daily) {
      inPeriod = inPeriod && (local / 86400 == now / 86400);
    }
    
    if (inPeriod) {
      float t = samples[i].tempC;
      sum += t;
      ++cnt;
      if (t < min) min = t;
      if (t > max) max = t;
    }
  }
  
  if (cnt == 0) {
    return daily ? "{\"error\":\"No samples today\"}" : "{\"error\":\"No weekly data\"}";
  }

  float avg = sum / cnt;
  
  // Determine trend
  String trend = (max - min < 1.0f) ? "Stable"
                 : (max - min < 3.0f) ? "Mild" : "Variable";

  // Format JSON response
  char buf[350];
  if (daily) {
    snprintf(buf, sizeof(buf),
      "{\"date\":\"%s\",\"samples\":%d,\"avg\":%.2f,\"min\":%.2f,\"max\":%.2f,\"trend\":\"%s\"}",
      formatDate(now).c_str(), cnt, avg, min, max, trend.c_str());
  } else {
    snprintf(buf, sizeof(buf),
      "{\"period\":\"Last 7 days\",\"samples\":%d,\"avg\":%.2f,\"min\":%.2f,\"max\":%.2f}",
      cnt, avg, min, max);
  }
  
  return String(buf);
}

// ========================== HTTP HELPERS ==========================

/**
 * Read HTTP headers from client
 */
String readHeaders(WiFiClient &client, String &method, String &path, int &clen) {
  String line = client.readStringUntil('\n');
  line.trim();
  
  // Parse request line
  int s1 = line.indexOf(' ');
  int s2 = line.indexOf(' ', s1 + 1);
  if (s1 > 0 && s2 > s1) {
    method = line.substring(0, s1);
    path   = line.substring(s1 + 1, s2);
  }
  
  // Read headers
  while (client.available()) {
    String h = client.readStringUntil('\n');
    h.trim();
    if (h.startsWith("Content-Length:")) {
      clen = h.substring(15).toInt();
    }
    if (h.length() == 0) break;
  }
  
  return line;
}

/**
 * Send HTTP response to client
 */
void send(WiFiClient &c, const String &status, const String &type, const String &body) {
  c.printf("HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
           status.c_str(), type.c_str(), body.length());
  c.print(body);
}

// ========================== HTTP HANDLER ==========================

/**
 * Handle incoming HTTP requests
 */
void handleClient() {
  WiFiClient client = server.accept();
  if (!client) return;
  
  client.setTimeout(3000);

  String method, path;
  int clen = 0;
  readHeaders(client, method, path, clen);

  // Read request body if present
  String body;
  if (clen > 0) {
    body.reserve(clen);
    uint8_t buf[256];
    int remaining = clen;
    
    while (remaining > 0) {
      size_t toRead = min((size_t)remaining, sizeof(buf));
      size_t bytesRead = client.readBytes((char*)buf, toRead);
      body += String((char*)buf, bytesRead);
      remaining -= bytesRead;
      if (bytesRead == 0) break;
    }
  }

  // ==================== ROUTE HANDLERS ====================
  
  // Main page
  if (path == "/" && method == "GET") {
    const char *html = R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Temperature Monitor</title>
    <script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.4.0/chart.umd.min.js"></script>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }

        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, Cantarell, sans-serif;
            background: linear-gradient(135deg, #0f0c29 0%, #302b63 50%, #24243e 100%);
            min-height: 100vh;
            color: white;
            touch-action: pan-y;
            -webkit-user-select: none;
            user-select: none;
            margin: 0;
            padding: 0;
        }

        .container {
            position: relative;
            width: 100%;
            min-height: 100vh;
            display: flex;
            transition: transform 0.5s cubic-bezier(0.4, 0, 0.2, 1);
        }

        .page {
            min-width: 100vw;
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 1rem;
        }

        .temp-card {
            background: rgba(255, 255, 255, 0.05);
            backdrop-filter: blur(20px);
            border-radius: 30px;
            padding: 2.5rem 2rem;
            border: 1px solid rgba(255, 255, 255, 0.1);
            box-shadow: 0 20px 60px rgba(0, 0, 0, 0.3);
            text-align: center;
            max-width: 450px;
            width: 90%;
            position: relative;
            overflow: hidden;
        }

        .temp-card::before {
            content: '';
            position: absolute;
            top: -50%;
            left: -50%;
            width: 200%;
            height: 200%;
            background: radial-gradient(circle, rgba(138, 43, 226, 0.3) 0%, transparent 70%);
            animation: breathe 4s ease-in-out infinite;
        }

        @keyframes breathe {
            0%, 100% {
                transform: scale(1);
                opacity: 0.5;
            }
            50% {
                transform: scale(1.2);
                opacity: 0.8;
            }
        }

        .icon-container {
            width: 80px;
            height: 80px;
            margin: 0 auto 1.5rem;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            border-radius: 50%;
            display: flex;
            align-items: center;
            justify-content: center;
            position: relative;
            z-index: 2;
            box-shadow: 0 10px 40px rgba(138, 43, 226, 0.5);
        }

        .icon-container svg {
            width: 40px;
            height: 40px;
            fill: white;
        }

        .temp-display {
            font-size: 3.5rem;
            font-weight: 700;
            margin: 1rem 0;
            background: linear-gradient(135deg, #ffffff 0%, #c7d2fe 100%);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            background-clip: text;
            position: relative;
            z-index: 2;
        }

        .temp-unit {
            font-size: 1.5rem;
            opacity: 0.6;
        }

        .temp-label {
            font-size: 1rem;
            opacity: 0.7;
            margin-top: 0.5rem;
            position: relative;
            z-index: 2;
        }

        .status-indicator {
            display: inline-flex;
            align-items: center;
            gap: 0.5rem;
            margin-top: 1rem;
            padding: 0.6rem 1.2rem;
            background: rgba(255, 255, 255, 0.1);
            border-radius: 20px;
            font-size: 0.85rem;
            position: relative;
            z-index: 2;
        }

        .status-dot {
            width: 10px;
            height: 10px;
            background: #34d399;
            border-radius: 50%;
            box-shadow: 0 0 10px rgba(52, 211, 153, 0.8);
            animation: pulse 2s ease-in-out infinite;
        }

        @keyframes pulse {
            0%, 100% {
                transform: scale(1);
                opacity: 1;
            }
            50% {
                transform: scale(1.2);
                opacity: 0.7;
            }
        }

        .graph-card {
            background: rgba(255, 255, 255, 0.05);
            backdrop-filter: blur(20px);
            border-radius: 30px;
            padding: 1.5rem;
            border: 1px solid rgba(255, 255, 255, 0.1);
            box-shadow: 0 20px 60px rgba(0, 0, 0, 0.3);
            max-width: 900px;
            width: 90%;
            max-height: 90vh;
            display: flex;
            flex-direction: column;
        }

        .graph-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 1.5rem;
            flex-wrap: wrap;
            gap: 1rem;
        }

        .graph-title {
            font-size: 1.5rem;
            font-weight: 600;
        }

        .time-selector {
            display: flex;
            gap: 0.5rem;
            background: rgba(255, 255, 255, 0.05);
            padding: 0.4rem;
            border-radius: 15px;
        }

        .time-btn {
            padding: 0.5rem 1rem;
            background: transparent;
            border: none;
            color: rgba(255, 255, 255, 0.6);
            cursor: pointer;
            border-radius: 12px;
            transition: all 0.3s ease;
            font-size: 0.85rem;
        }

        .time-btn.active {
            background: rgba(255, 255, 255, 0.1);
            color: white;
        }

        .chart-container {
            flex: 1;
            position: relative;
            min-height: 250px;
            max-height: 400px;
        }

        .swipe-indicator {
            position: absolute;
            bottom: 1rem;
            left: 50%;
            transform: translateX(-50%);
            display: flex;
            align-items: center;
            gap: 0.5rem;
            color: rgba(255, 255, 255, 0.5);
            font-size: 0.85rem;
            animation: bounce 2s ease-in-out infinite;
            z-index: 10;
        }

        @keyframes bounce {
            0%, 100% {
                transform: translateX(-50%) translateY(0);
            }
            50% {
                transform: translateX(-50%) translateY(-10px);
            }
        }

        .swipe-indicator svg {
            width: 20px;
            height: 20px;
            fill: rgba(255, 255, 255, 0.5);
        }

        .stats-grid {
            display: grid;
            grid-template-columns: repeat(3, 1fr);
            gap: 0.8rem;
            margin-bottom: 1.5rem;
        }

        .stat-card {
            background: rgba(255, 255, 255, 0.05);
            padding: 0.8rem;
            border-radius: 15px;
            text-align: center;
        }

        .stat-value {
            font-size: 1.2rem;
            font-weight: 600;
            margin: 0.3rem 0;
        }

        .stat-label {
            font-size: 0.75rem;
            opacity: 0.6;
        }

        .nav-dots {
            position: fixed;
            bottom: 1rem;
            left: 50%;
            transform: translateX(-50%);
            display: flex;
            gap: 0.5rem;
            z-index: 100;
        }

        .nav-dot {
            width: 8px;
            height: 8px;
            background: rgba(255, 255, 255, 0.3);
            border-radius: 50%;
            transition: all 0.3s ease;
            cursor: pointer;
        }

        .nav-dot.active {
            width: 24px;
            border-radius: 4px;
            background: white;
        }

        .update-time {
            text-align: center;
            font-size: 0.75rem;
            color: rgba(255, 255, 255, 0.5);
            margin-top: 1rem;
        }

        /* Responsive Design */
        @media (max-width: 768px) {
            .temp-display {
                font-size: 3rem;
            }
            
            .temp-card {
                padding: 2rem 1.5rem;
            }

            .graph-title {
                font-size: 1.2rem;
            }
            
            .graph-card {
                padding: 1rem;
            }

            .time-selector {
                width: 100%;
                justify-content: center;
            }
            
            .stats-grid {
                grid-template-columns: 1fr;
                gap: 0.6rem;
            }
            
            .chart-container {
                min-height: 200px;
                max-height: 300px;
            }
            
            .swipe-indicator {
                font-size: 0.75rem;
            }
        }
        
        @media (min-width: 769px) and (max-width: 1024px) {
            .temp-display {
                font-size: 4rem;
            }
            
            .graph-card {
                max-height: 85vh;
            }
            
            .chart-container {
                max-height: 450px;
            }
        }
        
        @media (min-width: 1025px) {
            .temp-display {
                font-size: 4.5rem;
            }
            
            .graph-card {
                max-height: 80vh;
            }
            
            .chart-container {
                max-height: 500px;
            }
            
            .stats-grid {
                grid-template-columns: repeat(3, 1fr);
            }
        }
    </style>
</head>
<body>
    <div class="container" id="container">
        <!-- Page 1: Current Temperature -->
        <div class="page">
            <div class="temp-card">
                <div class="icon-container">
                    <svg viewBox="0 0 24 24">
                        <path d="M15 13V5c0-1.66-1.34-3-3-3S9 3.34 9 5v8c-1.21.91-2 2.37-2 4 0 2.76 2.24 5 5 5s5-2.24 5-5c0-1.63-.79-3.09-2-4zm-4-8c0-.55.45-1 1-1s1 .45 1 1h-1v1h1v2h-1v1h1v2h-2V5z"/>
                    </svg>
                </div>
                <div class="temp-display">
                    <span id="currentTemp">--</span><span class="temp-unit">°C</span>
                </div>
                <div class="temp-label">Current Temperature</div>
                <div class="status-indicator">
                    <span class="status-dot"></span>
                    <span id="statusText">System Active</span>
                </div>
                <div class="update-time">Updates every 2 minutes</div>
            </div>
            <div class="swipe-indicator">
                <span>Swipe left for graphs</span>
                <svg viewBox="0 0 24 24">
                    <path d="M8.59 16.59L13.17 12 8.59 7.41 10 6l6 6-6 6-1.41-1.41z"/>
                </svg>
            </div>
        </div>

        <!-- Page 2: Temperature Graph -->
        <div class="page">
            <div class="graph-card">
                <div class="graph-header">
                    <div class="graph-title">Temperature History</div>
                    <div class="time-selector">
                        <button class="time-btn active" data-range="24h">Today</button>
                        <button class="time-btn" data-range="7d">Week</button>
                    </div>
                </div>
                
                <div class="stats-grid">
                    <div class="stat-card">
                        <div class="stat-label">Average</div>
                        <div class="stat-value" id="avgTemp">--°C</div>
                    </div>
                    <div class="stat-card">
                        <div class="stat-label">Maximum</div>
                        <div class="stat-value" id="maxTemp">--°C</div>
                    </div>
                    <div class="stat-card">
                        <div class="stat-label">Minimum</div>
                        <div class="stat-value" id="minTemp">--°C</div>
                    </div>
                </div>

                <div class="chart-container">
                    <canvas id="tempChart"></canvas>
                </div>
                <div class="update-time">Auto-refresh every 2 minutes</div>
            </div>
        </div>
    </div>

    <div class="nav-dots">
        <div class="nav-dot active" data-page="0"></div>
        <div class="nav-dot" data-page="1"></div>
    </div>

    <script>
        // Global variables
        let currentPage = 0;
        let startX = 0;
        let isDragging = false;
        let chart;
        let currentView = 'daily';
        let updateInterval;

        const container = document.getElementById('container');
        const navDots = document.querySelectorAll('.nav-dot');

        // ==================== PAGE NAVIGATION ====================
        
        function updatePage(page) {
            currentPage = page;
            container.style.transform = `translateX(-${currentPage * 100}vw)`;
            navDots.forEach((dot, i) => {
                dot.classList.toggle('active', i === currentPage);
            });
        }

        // Touch events for mobile
        container.addEventListener('touchstart', (e) => {
            startX = e.touches[0].clientX;
            isDragging = true;
        });

        container.addEventListener('touchmove', (e) => {
            if (!isDragging) return;
            const currentX = e.touches[0].clientX;
            const diff = startX - currentX;
            
            if (Math.abs(diff) > 50) {
                if (diff > 0 && currentPage < 1) {
                    updatePage(1);
                } else if (diff < 0 && currentPage > 0) {
                    updatePage(0);
                }
                isDragging = false;
            }
        });

        container.addEventListener('touchend', () => {
            isDragging = false;
        });

        // Mouse events for desktop
        container.addEventListener('mousedown', (e) => {
            startX = e.clientX;
            isDragging = true;
        });

        container.addEventListener('mousemove', (e) => {
            if (!isDragging) return;
            const diff = startX - e.clientX;
            
            if (Math.abs(diff) > 50) {
                if (diff > 0 && currentPage < 1) {
                    updatePage(1);
                } else if (diff < 0 && currentPage > 0) {
                    updatePage(0);
                }
                isDragging = false;
            }
        });

        container.addEventListener('mouseup', () => {
            isDragging = false;
        });

        // Navigation dots click
        navDots.forEach(dot => {
            dot.addEventListener('click', () => {
                updatePage(parseInt(dot.dataset.page));
            });
        });

        // ==================== DATA FETCHING ====================
        
        async function fetchData() {
            try {
                const endpoint = currentView === 'daily' ? '/data_daily.json' : '/data_weekly.json';
                const [dataRes, summaryRes] = await Promise.all([
                    fetch(endpoint),
                    fetch('/summary')
                ]);
                
                if (!dataRes.ok || !summaryRes.ok) {
                    throw new Error('Failed to fetch data');
                }
                
                const data = await dataRes.json();
                const summary = await summaryRes.json();
                
                updateChart(data);
                updateStats(summary);
                
                // Update current temperature
                if (data.length > 0) {
                    const latest = data[data.length - 1];
                    document.getElementById('currentTemp').textContent = latest.temp.toFixed(1);
                    
                    // Update status with last reading time
                    const lastTime = new Date(latest.epoch * 1000);
                    document.getElementById('statusText').textContent = 
                        'Last: ' + lastTime.toLocaleTimeString();
                }
                
                console.log('Data updated successfully');
            } catch (error) {
                console.error('Error fetching data:', error);
                document.getElementById('statusText').textContent = 'Update failed';
            }
        }

        // ==================== CHART RENDERING ====================
        
        function updateChart(data) {
            const ctx = document.getElementById('tempChart').getContext('2d');
            
            // Prepare chart data
            const labels = data.map(p => {
                const d = new Date(p.epoch * 1000);
                return d.toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'});
            });
            const temps = data.map(p => p.temp);

            // Destroy old chart if exists
            if (chart) {
                chart.destroy();
            }

            // Create gradient
            const gradient = ctx.createLinearGradient(0, 0, 0, 400);
            gradient.addColorStop(0, 'rgba(102, 126, 234, 0.5)');
            gradient.addColorStop(1, 'rgba(118, 75, 162, 0.1)');

            // Create new chart
            chart = new Chart(ctx, {
                type: 'line',
                data: {
                    labels: labels,
                    datasets: [{
                        label: 'Temperature (°C)',
                        data: temps,
                        borderColor: '#667eea',
                        backgroundColor: gradient,
                        borderWidth: 3,
                        fill: true,
                        tension: 0.4,
                        pointRadius: 2,
                        pointHoverRadius: 6,
                        pointHoverBackgroundColor: '#667eea',
                        pointHoverBorderColor: 'white',
                        pointHoverBorderWidth: 2
                    }]
                },
                options: {
                    responsive: true,
                    maintainAspectRatio: false,
                    plugins: {
                        legend: {
                            display: false
                        },
                        tooltip: {
                            backgroundColor: 'rgba(0, 0, 0, 0.8)',
                            padding: 12,
                            cornerRadius: 8,
                            displayColors: false,
                            callbacks: {
                                label: function(context) {
                                    return context.parsed.y.toFixed(2) + '°C';
                                }
                            }
                        }
                    },
                    scales: {
                        x: {
                            grid: {
                                color: 'rgba(255, 255, 255, 0.1)',
                                drawBorder: false
                            },
                            ticks: {
                                color: 'rgba(255, 255, 255, 0.6)',
                                maxTicksLimit: 8
                            }
                        },
                        y: {
                            grid: {
                                color: 'rgba(255, 255, 255, 0.1)',
                                drawBorder: false
                            },
                            ticks: {
                                color: 'rgba(255, 255, 255, 0.6)',
                                callback: function(value) {
                                    return value.toFixed(1) + '°C';
                                }
                            }
                        }
                    },
                    interaction: {
                        intersect: false,
                        mode: 'index'
                    }
                }
            });
        }

        // ==================== STATISTICS UPDATE ====================
        
        function updateStats(summary) {
            const s = currentView === 'daily' ? summary.daily : summary.weekly;
            
            if (s.error) {
                document.getElementById('avgTemp').textContent = '--°C';
                document.getElementById('maxTemp').textContent = '--°C';
                document.getElementById('minTemp').textContent = '--°C';
            } else {
                document.getElementById('avgTemp').textContent = s.avg.toFixed(1) + '°C';
                document.getElementById('maxTemp').textContent = s.max.toFixed(1) + '°C';
                document.getElementById('minTemp').textContent = s.min.toFixed(1) + '°C';
            }
        }

        // ==================== VIEW SWITCHING ====================
        
        document.querySelectorAll('.time-btn').forEach(btn => {
            btn.addEventListener('click', () => {
                document.querySelectorAll('.time-btn').forEach(b => b.classList.remove('active'));
                btn.classList.add('active');
                currentView = btn.dataset.range === '24h' ? 'daily' : 'weekly';
                fetchData();
            });
        });

        // ==================== AUTO-UPDATE ====================
        
        function startAutoUpdate() {
            // Clear existing interval if any
            if (updateInterval) {
                clearInterval(updateInterval);
            }
            
            // Update every 2 minutes (120000 milliseconds)
            updateInterval = setInterval(fetchData, 120000);
            console.log('Auto-update enabled: every 2 minutes');
        }

        // ==================== INITIALIZATION ====================
        
        // Initial data load
        fetchData();
        
        // Start auto-update
        startAutoUpdate();
        
        // Log startup
        console.log('Temperature Monitor v3.0.0 initialized');
        console.log('Updates: Every 2 minutes');
    </script>
</body>
</html>
)=====";
    send(client, "200 OK", "text/html", html);
  }

  // Get all data as JSON
  else if (path == "/data.json") {
    String json = "[";
    for (int i = 0; i < sampleCount; ++i) {
      char buf[64];
      snprintf(buf, sizeof(buf), "{\"epoch\":%u,\"temp\":%.2f}", 
               samples[i].epoch, samples[i].tempC);
      json += buf;
      if (i < sampleCount - 1) json += ",";
    }
    json += "]";
    send(client, "200 OK", "application/json", json);
  }

  // Get filtered data (daily or weekly)
  else if (path == "/data_daily.json" || path == "/data_weekly.json") {
    bool isDaily = (path == "/data_daily.json");
    String json;
    getFilteredData(isDaily, json);
    send(client, "200 OK", "application/json", json);
  }

  // Export data as CSV
  else if (path == "/data.csv") {
    String csv = "epoch,temp\n";
    for (int i = 0; i < sampleCount; ++i) {
      char buf[64];
      snprintf(buf, sizeof(buf), "%u,%.2f\n", samples[i].epoch, samples[i].tempC);
      csv += buf;
    }
    send(client, "200 OK", "text/csv", csv);
  }

  // Get summary statistics
  else if (path == "/summary") {
    String s = "{\"daily\":" + generateSummary(true) + 
               ",\"weekly\":" + generateSummary(false) + "}";
    send(client, "200 OK", "application/json", s);
  }

  // Clear all data
  else if (path == "/clear" && method == "POST") {
    LittleFS.remove(CSV_PATH);
    LittleFS.remove(SUMMARY_PATH);
    sampleCount = 0;
    Serial.println("All data cleared");
    send(client, "200 OK", "text/plain", "Data cleared successfully");
  }

  // Import CSV data
  else if (path == "/import" && method == "POST") {
    LittleFS.remove(CSV_PATH);
    sampleCount = 0;

    int imported = 0;
    int pos = 0;
    
    while (pos < (int)body.length()) {
      int nl = body.indexOf('\n', pos);
      if (nl == -1) nl = body.length();
      
      String line = body.substring(pos, nl);
      line.trim();
      pos = nl + 1;
      
      if (line.length() == 0) continue;
      
      int c = line.indexOf(',');
      if (c <= 0) continue;
      
      uint32_t e = line.substring(0, c).toInt();
      float    t = line.substring(c + 1).toFloat();

      if (sampleCount < MAX_SAMPLES) {
        samples[sampleCount++] = {e, t};
      } else {
        // Drop oldest sample
        memmove(samples, samples + 1, (MAX_SAMPLES - 1) * sizeof(Sample));
        samples[MAX_SAMPLES - 1] = {e, t};
      }
      
      appendCSV(e, t);
      ++imported;
    }
    
    Serial.printf("Imported %d samples\n", imported);
    send(client, "200 OK", "text/plain", "Imported: " + String(imported));
  }

  // Generate daily summary
  else if (path == "/gen_daily") {
    dailySummary = generateSummary(true);
    String full = "{\"daily\":" + dailySummary + 
                  ",\"weekly\":" + generateSummary(false) + "}";
    saveSummary(full);
    startBreathing(15000);
    Serial.println("Daily summary generated");
    send(client, "200 OK", "text/plain", dailySummary);
  }

  // 404 Not Found
  else {
    send(client, "404 Not Found", "text/plain", "Not found");
  }
  
  client.stop();
}

// ========================== SETUP ==========================

void setup() {
  // Initialize serial communication
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=================================");
  Serial.println("Temperature Monitor v3.0.0");
  Serial.println("=================================\n");

  // Initialize LED
  pinMode(LED_PIN, OUTPUT);
  initPWM();
  testLED();

  // Initialize ADC for temperature sensor
  adc_init();
  adc_set_temp_sensor_enabled(true);
  Serial.println("Temperature sensor initialized");

  // Initialize file system
  if (!LittleFS.begin()) {
    Serial.println("ERROR: LittleFS mount failed!");
  } else {
    Serial.println("File system mounted successfully");
  }

  // Load existing data
  loadCSV();
  Serial.printf("Loaded %d samples from storage\n", sampleCount);

  // Connect to WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi connected successfully!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Access at: http://");
    Serial.println(WiFi.localIP());
    
    // LED indication - 3 quick blinks for successful connection
    for (int i = 0; i < 3; ++i) {
      setBrightness(65535);
      delay(200);
      setBrightness(0);
      delay(200);
    }
    
    // Initialize NTP
    ntpUDP.begin(2390);
    uint32_t t = getNTPTime();
    if (t) {
      lastNtpSyncSec = t;
      ntpSyncMillis  = millis();
      lastNtpCheckMs = millis();
      Serial.println("✓ NTP time synchronized");
      Serial.println("Current time: " + formatDateTime(getLocalEpoch()));
    } else {
      Serial.println("⚠ NTP sync failed (will retry later)");
    }
    
    // Start breathing LED
    startBreathing(10000);
    
  } else {
    Serial.println("\n✗ WiFi connection failed!");
    Serial.println("Please check SSID and password");
    
    // Error indication - 5 rapid blinks
    for (int i = 0; i < 5; ++i) {
      setBrightness(32768);
      delay(200);
      setBrightness(0);
      delay(200);
    }
  }

  // Start HTTP server
  server.begin();
  Serial.println("✓ HTTP server started on port 80\n");
  
  Serial.println("=================================");
  Serial.println("System ready!");
  Serial.println("Sample interval: 2 minutes");
  Serial.println("=================================\n");

  // Set first sample to happen soon
  lastSampleMs = millis() - SAMPLE_INTERVAL_MS + 10000;
}

// ========================== MAIN LOOP ==========================

void loop() {
  // Handle HTTP requests
  handleClient();
  
  // Update LED breathing animation
  updateBreathing();

  // ==================== NTP SYNC ====================
  if (WiFi.status() == WL_CONNECTED && lastNtpCheckMs &&
      millis() - lastNtpCheckMs > NTP_SYNC_INTERVAL_MS) {
    
    Serial.println("Performing NTP sync...");
    uint32_t t = getNTPTime();
    
    if (t) {
      lastNtpSyncSec = t;
      ntpSyncMillis  = millis();
      lastNtpCheckMs = millis();
      Serial.println("✓ NTP sync successful");
      Serial.println("Time: " + formatDateTime(getLocalEpoch()));
    } else {
      Serial.println("⚠ NTP sync failed");
    }
  }

  // ==================== WiFi RECONNECT ====================
  static uint32_t lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 10000) {
    lastWiFiCheck = millis();
    
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi connection lost - reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }

  // ==================== TEMPERATURE SAMPLING ====================
  if (millis() - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = millis();
    
    // Quick LED blink to indicate sampling
    setBrightness(65535);
    delay(100);
    setBrightness(0);

    // Read temperature
    float temp = readTempC();
    uint32_t epoch = getEpochUTC();

    // Store sample
    if (sampleCount < MAX_SAMPLES) {
      samples[sampleCount++] = {epoch, temp};
    } else {
      // Remove oldest sample
      memmove(samples, samples + 1, (MAX_SAMPLES - 1) * sizeof(Sample));
      samples[MAX_SAMPLES - 1] = {epoch, temp};
    }
    
    // Save to file system
    appendCSV(epoch, temp);

    // Log sample
    Serial.printf("Sample #%d: %.2f°C at %s\n", 
                  sampleCount, temp, formatDateTime(epoch + TZ_OFFSET_SECONDS).c_str());

    // ==================== DAILY SUMMARY ====================
    uint32_t localDay = (epoch + TZ_OFFSET_SECONDS) / 86400UL;
    
    if (lastSummaryDay && localDay != lastSummaryDay) {
      Serial.println("\n--- Generating daily summary ---");
      
      dailySummary = generateSummary(true);
      String full = "{\"daily\":" + dailySummary + 
                    ",\"weekly\":" + generateSummary(false) + "}";
      saveSummary(full);
      
      Serial.println("Daily summary: " + dailySummary);
      Serial.println("Summary saved to file system\n");
      
      // Extended breathing for daily summary
      startBreathing(20000);
    }
    
    lastSummaryDay = localDay;
  }

  // Small delay to prevent watchdog issues
  delay(10);
}

/* =============================================================================
   END OF CODE
   
   Usage Instructions:
   1. Update WIFI_SSID and WIFI_PASS with your WiFi credentials
   2. Adjust TZ_OFFSET_SECONDS to your timezone
   3. Upload to Raspberry Pi Pico W
   4. Open Serial Monitor to see IP address
   5. Access web interface from any device on your network
   
   Features:
   - Temperature readings every 2 minutes
   - Web interface auto-updates every 2 minutes
   - LED blinks 3 times on WiFi connection
   - LED pulses briefly when taking samples
   - Breathing LED effect for special events
   - Responsive design for all devices
   - Data export as CSV
   - Daily and weekly statistics
   
   ============================================================================= */