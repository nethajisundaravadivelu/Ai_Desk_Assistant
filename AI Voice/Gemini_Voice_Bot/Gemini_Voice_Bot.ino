#include <WiFi.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP_I2S.h>
#include <AudioOutputI2S.h>
#include "wifi_config.h"

// PC Wi-Fi address; update this if the PC's LAN address changes.
const char *LOCAL_ASK_URL = "http://192.168.10.193:8000/ask";
const char *LOCAL_SPEAK_URL = "http://192.168.10.193:8000/speak";

// INMP441: SCK=GPIO5, WS=GPIO6, SD=GPIO4, L/R=GND
// MAX98357A: BCLK=GPIO5, LRC/WS=GPIO6, DIN=GPIO7, SD=3V3
// OLED SH1106 I2C: SDA=GPIO8, SCL=GPIO9, address=0x3C
constexpr int PIN_I2S_BCLK = 5;
constexpr int PIN_I2S_WS = 6;
constexpr int PIN_MIC_SD = 4;
constexpr int PIN_AMP_DIN = 7;
// Touch module: SIG -> GPIO10, VCC -> 3V3, GND -> common GND.
constexpr int PIN_TOUCH = 10;
constexpr int OLED_SDA = 8;
constexpr int OLED_SCL = 9;
constexpr uint8_t OLED_ADDRESS = 0x3C;

constexpr uint32_t MIC_RATE = 16000;
constexpr uint32_t RECORD_SECONDS = 2;
constexpr size_t PCM_BYTES = MIC_RATE * RECORD_SECONDS * sizeof(int16_t);
constexpr size_t WAV_BYTES = 44 + PCM_BYTES;
constexpr size_t FRAMES_PER_READ = 256;

U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
I2SClass I2S;

AudioOutputI2S *speakerOut = nullptr;
static int32_t i2sSamples[FRAMES_PER_READ * 2];
uint8_t *wavBuffer = nullptr;
bool stopRequestedDuringPlayback = false;
bool touchInterruptArmed = false;

void showLines(const String &a, const String &b = "", const String &c = "") {
  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tf);
  display.drawUTF8(0, 14, a.c_str());
  display.drawUTF8(0, 32, b.c_str());
  display.drawUTF8(0, 50, c.c_str());
  display.sendBuffer();
}

void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)(v >> 8);
}

void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)(v >> 24);
}

void makeWavHeader() {
  memcpy(wavBuffer, "RIFF", 4);
  put32(wavBuffer + 4, 36 + PCM_BYTES);
  memcpy(wavBuffer + 8, "WAVEfmt ", 8);
  put32(wavBuffer + 16, 16);
  put16(wavBuffer + 20, 1);
  put16(wavBuffer + 22, 1);
  put32(wavBuffer + 24, MIC_RATE);
  put32(wavBuffer + 28, MIC_RATE * 2);
  put16(wavBuffer + 32, 2);
  put16(wavBuffer + 34, 16);
  memcpy(wavBuffer + 36, "data", 4);
  put32(wavBuffer + 40, PCM_BYTES);
}

bool recordAudio() {
  makeWavHeader();
  int16_t *pcm = (int16_t *)(wavBuffer + 44);
  size_t captured = 0;
  showLines("Recording now", "Speak clearly", "2 seconds");
  delay(350);

  while (captured < MIC_RATE * RECORD_SECONDS) {
    const size_t bytes = I2S.readBytes((char *)i2sSamples, sizeof(i2sSamples));
    const size_t frames = bytes / (2 * sizeof(int32_t));
    if (frames == 0) continue;
    for (size_t i = 0; i < frames && captured < MIC_RATE * RECORD_SECONDS; ++i) {
      // INMP441 L/R is tied to GND; use the left I2S slot.
      pcm[captured++] = (int16_t)(i2sSamples[i * 2] >> 16);
    }
  }
  return captured == MIC_RATE * RECORD_SECONDS;
}

String askLocalAssistant(String &transcript) {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(180000);
  if (!http.begin(client, LOCAL_ASK_URL)) return "Local server setup failed.";
  http.addHeader("Content-Type", "audio/wav");
  const int status = http.POST(wavBuffer, WAV_BYTES);
  const String responseBody = http.getString();
  http.end();

  Serial.print("Local assistant HTTP status: ");
  Serial.println(status);
  if (status < 200 || status >= 300) {
    Serial.println(responseBody);
    return "Local assistant error.";
  }

  JsonDocument response;
  if (deserializeJson(response, responseBody)) return "Could not parse local reply.";
  transcript = response["transcript"] | "";
  const char *answer = response["answer"] | "No answer returned.";
  return String(answer);
}

bool interruptionRequested() {
  static String serialCommand;
  while (Serial.available()) {
    const char ch = (char)Serial.read();
    if (ch == '\n') {
      serialCommand.trim();
      const bool stop = serialCommand.equalsIgnoreCase("r") ||
                        serialCommand.equalsIgnoreCase("stop");
      serialCommand = "";
      if (stop) {
        stopRequestedDuringPlayback = true;
        return true;
      }
    } else if (ch != '\r' && serialCommand.length() < 12) {
      serialCommand += ch;
    }
  }
  if (digitalRead(PIN_TOUCH) == LOW) {
    // Require a fresh touch after recording starts; the trigger touch may
    // still be held when the playback stream begins.
    touchInterruptArmed = true;
  } else if (touchInterruptArmed) {
    stopRequestedDuringPlayback = true;
    return true;
  }
  return false;
}

bool readChunkLine(WiFiClient *stream, String &line) {
  const unsigned long started = millis();
  line = "";
  while (millis() - started < 180000) {
    if (interruptionRequested()) return false;
    while (stream->available()) {
      const char ch = (char)stream->read();
      if (ch == '\n') {
        line.trim();
        return true;
      }
      if (ch != '\r') line += ch;
    }
    if (!stream->connected()) return false;
    delay(2);
  }
  return false;
}

bool streamAskAndSpeak(const uint8_t *wav, size_t wavLength, bool &interrupted) {
  interrupted = false;
  stopRequestedDuringPlayback = false;
  touchInterruptArmed = false;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(180000);
  if (!http.begin(client, LOCAL_ASK_URL)) return false;
  http.addHeader("Content-Type", "audio/wav");
  const int status = http.POST((uint8_t *)wav, wavLength);
  Serial.print("Streaming assistant HTTP status: ");
  Serial.println(status);
  if (status < 200 || status >= 300) {
    Serial.println(http.getString());
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  stream->setTimeout(100);
  uint8_t pcm[512];
  bool completed = false;
  while (!completed) {
    if (interruptionRequested()) {
      interrupted = true;
      http.end();
      return true;
    }
    String sizeLine;
    if (!readChunkLine(stream, sizeLine)) {
      interruptionRequested();
      interrupted = stopRequestedDuringPlayback;
      http.end();
      return interrupted;
    }
    const size_t chunkLength = (size_t)strtoul(sizeLine.c_str(), nullptr, 16);
    if (chunkLength == 0) {
      while (readChunkLine(stream, sizeLine)) {
        if (sizeLine.startsWith("X-Interrupted:") && sizeLine.indexOf('1') >= 0) {
          interrupted = true;
        }
        if (sizeLine.length() == 0) break;
      }
      completed = true;
      break;
    }

    size_t remaining = chunkLength;
    while (remaining > 0) {
      if (interruptionRequested()) {
        interrupted = true;
        http.end();
        return true;
      }
      const int available = stream->available();
      if (available <= 0) {
        if (!stream->connected()) {
          http.end();
          return false;
        }
        delay(2);
        continue;
      }
      size_t count = (size_t)available;
      if (count > sizeof(pcm)) count = sizeof(pcm);
      if (count > remaining) count = remaining;
      count &= ~static_cast<size_t>(1);
      if (count == 0) { delay(1); continue; }
      const size_t got = stream->readBytes((char *)pcm, count);
      if (got == 0 || (got & 1)) {
        http.end();
        return false;
      }
      for (size_t i = 0; i < got; i += 2) {
        const int16_t value = (int16_t)((uint16_t)pcm[i] | ((uint16_t)pcm[i + 1] << 8));
        int16_t sample[2] = {value, value};
        while (!speakerOut->ConsumeSample(sample)) {
          if (interruptionRequested()) {
            interrupted = true;
            http.end();
            return true;
          }
          delay(1);
        }
      }
      remaining -= got;
    }
    // Every HTTP chunk is followed by CRLF before the next hexadecimal size.
    while (stream->available() < 2 && stream->connected()) {
      if (interruptionRequested()) {
        interrupted = true;
        http.end();
        return true;
      }
      delay(1);
    }
    if (stream->available() >= 2) {
      stream->read();
      stream->read();
    }
  }
  http.end();
  return completed;
}

bool speakWithOllama(const String &text) {
  JsonDocument request;
  request["text"] = text;
  String requestBody;
  serializeJson(request, requestBody);

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(180000);
  if (!http.begin(client, LOCAL_SPEAK_URL)) {
    Serial.println("Local speech server setup failed.");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  const int status = http.POST((uint8_t *)requestBody.c_str(), requestBody.length());
  requestBody = "";
  Serial.print("Ollama speech HTTP status: ");
  Serial.println(status);
  if (status < 200 || status >= 300) {
    Serial.println(http.getString());
    http.end();
    return false;
  }

  const int responseLength = http.getSize();
  if (responseLength <= 0 || (responseLength & 1)) {
    Serial.println("Ollama returned an invalid PCM length.");
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  uint8_t pcm[512];
  size_t remaining = (size_t)responseLength;
  const unsigned long started = millis();
  while (remaining > 0) {
    int available = stream->available();
    if (available <= 0) {
      if (!http.connected() || millis() - started > 180000) {
        Serial.println("Ollama PCM stream ended early.");
        http.end();
        return false;
      }
      delay(1);
      continue;
    }
    size_t count = (size_t)available;
    if (count > sizeof(pcm)) count = sizeof(pcm);
    if (count > remaining) count = remaining;
    count &= ~static_cast<size_t>(1);
    if (count == 0) { delay(1); continue; }
    const size_t got = stream->readBytes((char *)pcm, count);
    if (got == 0 || (got & 1)) {
      Serial.println("Could not read complete PCM samples.");
      http.end();
      return false;
    }

    for (size_t i = 0; i < got; i += 2) {
      const int16_t value = (int16_t)((uint16_t)pcm[i] | ((uint16_t)pcm[i + 1] << 8));
      int16_t sample[2] = {value, value};
      while (!speakerOut->ConsumeSample(sample)) delay(1);
    }
    remaining -= got;
  }
  http.end();
  return true;
}

bool restoreMicrophoneMode() {
  I2S.end();
  I2S.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_AMP_DIN, PIN_MIC_SD);
  if (!I2S.begin(I2S_MODE_STD, MIC_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                 I2S_SLOT_MODE_STEREO)) {
    Serial.println("Could not restart I2S microphone mode.");
    showLines("Audio restart failed", "Reset the board");
    return false;
  }
  Serial.println("I2S mic mode restored.");
  showLines("Ready to record", "Type r + Enter");
  Serial.println("Ready. Type r and press Enter for another question.");
  return true;
}

void handleRecording() {
  for (;;) {
    if (wavBuffer != nullptr) {
      free(wavBuffer);
      wavBuffer = nullptr;
    }
    wavBuffer = (uint8_t *)malloc(WAV_BYTES);
    if (wavBuffer == nullptr) {
      showLines("Not enough RAM", "to record audio");
      return;
    }
    if (!recordAudio()) {
      free(wavBuffer);
      wavBuffer = nullptr;
      showLines("Recording failed");
      return;
    }

    I2S.end();
    if (!speakerOut->begin()) {
      Serial.println("Could not start I2S speaker output.");
      restoreMicrophoneMode();
      free(wavBuffer);
      wavBuffer = nullptr;
      return;
    }
    Serial.println("Streaming local Ollama answer and speech.");
    showLines("Answer streaming", "Touch or R+Enter", "to interrupt");

    bool interrupted = false;
    const bool completed = streamAskAndSpeak(wavBuffer, WAV_BYTES, interrupted);
    free(wavBuffer);
    wavBuffer = nullptr;
    if (!completed && !interrupted) {
      Serial.println("Ollama streaming/playback failed.");
      showLines("Streaming failed");
    }

    if (interrupted) {
      speakerOut->stop();
    } else {
      speakerOut->flush();
      speakerOut->stop();
    }
    restoreMicrophoneMode();
    if (!interrupted) return;

    Serial.println("Interrupted. Listening for the next question now.");
    showLines("Interrupted", "Listening next...");
    while (digitalRead(PIN_TOUCH) == HIGH) delay(10);
    delay(120);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Local Ollama Voice Bot");
  pinMode(PIN_TOUCH, INPUT);
  Wire.begin(OLED_SDA, OLED_SCL);
  display.setI2CAddress(OLED_ADDRESS * 2);
  display.begin();

  showLines("Connecting Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 30000) delay(300);
  if (WiFi.status() != WL_CONNECTED) {
    showLines("Wi-Fi failed", "Check credentials");
    Serial.println("Wi-Fi connection failed.");
    return;
  }

  speakerOut = new AudioOutputI2S();
  if (speakerOut == nullptr) {
    showLines("Speaker allocation", "failed");
    Serial.println("Could not allocate I2S speaker output.");
    return;
  }
  speakerOut->SetPinout(PIN_I2S_BCLK, PIN_I2S_WS, PIN_AMP_DIN);
  speakerOut->SetOutputModeMono(true);
  speakerOut->SetRate(24000);
  speakerOut->SetGain(0.25f);

  I2S.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_AMP_DIN, PIN_MIC_SD);
  if (!I2S.begin(I2S_MODE_STD, MIC_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                 I2S_SLOT_MODE_STEREO)) {
    showLines("I2S audio", "could not start");
    Serial.println("I2S microphone start failed.");
    return;
  }
  showLines("Ready to record", "Type r + Enter");
  Serial.println("Ready. Type r and press Enter, then speak for 2 seconds.");
  Serial.print("PC bridge: ");
  Serial.println(LOCAL_ASK_URL);
}

void loop() {
  if (Serial.available()) {
    const char command = (char)Serial.read();
    if (command == 'r' || command == 'R') handleRecording();
  }

  static bool touchWasActive = false;
  const bool touchActive = digitalRead(PIN_TOUCH) == HIGH;
  if (touchActive && !touchWasActive) {
    delay(35);
    if (digitalRead(PIN_TOUCH) == HIGH) {
      touchWasActive = true;
      handleRecording();
    }
  } else if (!touchActive) {
    touchWasActive = false;
  }
}
