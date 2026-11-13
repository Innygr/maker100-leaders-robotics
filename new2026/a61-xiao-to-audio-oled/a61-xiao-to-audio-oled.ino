#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <U8g2lib.h>

#include "AudioTools.h"
#include "AudioTools/Disk/AudioSourceSD.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"

using namespace audio_tools;

// XIAO ESP32-S3 Sense microSD pins (preserved from a60)
#define SD_CS   21
#define SD_SCK  7
#define SD_MISO 8
#define SD_MOSI 9

// PWM audio output pins (preserved from a60)
#define PWM_LEFT  D0
#define PWM_RIGHT D1

#define MUSIC_DIR "/music"
#define VIDEO_DIR "/video"
#define DEFAULT_VIDEO_FILE "/video/demo.gray"

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define VIDEO_FRAME_BYTES (OLED_WIDTH * OLED_HEIGHT)
#define VIDEO_FPS 3
#define VIDEO_FRAME_INTERVAL_MS (1000UL / VIDEO_FPS)
#define STATUS_REFRESH_MS 500

// SSD1306 128x64 I2C OLED.
// XIAO ESP32-S3 default I2C pins: D4/SDA and D5/SCL.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// a60 MP3 player
AudioSourceSD source(MUSIC_DIR, ".mp3", SD_CS);
PWMAudioOutput pwm;
MP3DecoderHelix decoder;
AudioPlayer player(source, pwm, decoder);

bool sdReady = false;
bool playerReady = false;
int trackCount = 0;
int currentTrack = 0;
int volumePercent = 10;

bool audioPaused = false;
bool audioStopped = true;

// Fresh 3FPS video uses raw 8-bit grayscale frames.
// Each frame is exactly 128 * 64 = 8192 bytes.
File videoFile;
uint8_t videoFrame[VIDEO_FRAME_BYTES];
bool videoPlaying = false;
bool videoPaused = false;
uint32_t videoFrameCount = 0;
uint32_t lastVideoFrameMs = 0;
uint32_t lastStatusRefreshMs = 0;

String commandBuffer;

// --------------------
// OLED status screen
// --------------------

void drawStatusScreen() {
  char line[32];

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "TV MAN PLAYER a61");
  u8g2.drawHLine(0, 12, 128);

  if (videoPlaying) {
    u8g2.drawStr(0, 25, videoPaused ? "VIDEO: PAUSED" : "VIDEO: PLAYING");
    snprintf(line, sizeof(line), "FRAME: %lu", (unsigned long)videoFrameCount);
    u8g2.drawStr(0, 38, line);
    u8g2.drawStr(0, 51, "MODE: FRESH 3FPS");
    u8g2.drawStr(0, 63, "video stop to exit");
  } else {
    if (audioStopped) {
      u8g2.drawStr(0, 25, "STATUS: STOPPED");
    } else if (audioPaused) {
      u8g2.drawStr(0, 25, "STATUS: PAUSED");
    } else {
      u8g2.drawStr(0, 25, "STATUS: PLAYING");
    }

    if (trackCount > 0) {
      snprintf(line, sizeof(line), "NOW:  %02d / %02d", currentTrack + 1, trackCount);
      u8g2.drawStr(0, 38, line);
      int nextTrack = (currentTrack + 1) % trackCount;
      snprintf(line, sizeof(line), "NEXT: %02d / %02d", nextTrack + 1, trackCount);
      u8g2.drawStr(0, 50, line);
    } else {
      u8g2.drawStr(0, 38, "NOW:  NO TRACKS");
      u8g2.drawStr(0, 50, "NEXT: --");
    }

    snprintf(line, sizeof(line), "VOL:%d%%  SD:%s", volumePercent, sdReady ? "OK" : "ERR");
    u8g2.drawStr(0, 63, line);
  }

  u8g2.sendBuffer();
}

// --------------------
// Fresh 3FPS video
// --------------------

void stopVideo() {
  if (videoFile) {
    videoFile.close();
  }
  videoPlaying = false;
  videoPaused = false;
  videoFrameCount = 0;
  drawStatusScreen();
  Serial.println("Video stopped.");
}

bool readAndDrawVideoFrame() {
  if (!videoFile) return false;

  int bytesRead = videoFile.read(videoFrame, VIDEO_FRAME_BYTES);

  // Loop the video when the end of the raw frame stream is reached.
  if (bytesRead == 0) {
    videoFile.seek(0);
    bytesRead = videoFile.read(videoFrame, VIDEO_FRAME_BYTES);
  }

  if (bytesRead != VIDEO_FRAME_BYTES) {
    Serial.println("Video frame read failed or file is incomplete.");
    stopVideo();
    return false;
  }

  u8g2.clearBuffer();

  // Convert grayscale to monochrome using a midpoint threshold.
  // The input is row-major: 128 pixels per row, 64 rows.
  for (int y = 0; y < OLED_HEIGHT; y++) {
    int rowOffset = y * OLED_WIDTH;
    for (int x = 0; x < OLED_WIDTH; x++) {
      if (videoFrame[rowOffset + x] >= 128) {
        u8g2.drawPixel(x, y);
      }
    }
  }

  u8g2.sendBuffer();
  videoFrameCount++;
  return true;
}

void startVideo(String path) {
  path.trim();

  if (!sdReady) {
    Serial.println("Cannot play video: SD card is unavailable.");
    return;
  }

  if (videoFile) videoFile.close();

  videoFile = SD.open(path.c_str(), FILE_READ);
  if (!videoFile) {
    Serial.print("Could not open video: ");
    Serial.println(path);
    return;
  }

  uint32_t fileBytes = videoFile.size();
  if (fileBytes < VIDEO_FRAME_BYTES || (fileBytes % VIDEO_FRAME_BYTES) != 0) {
    Serial.println("Invalid .gray file. Size must be a multiple of 8192 bytes.");
    videoFile.close();
    return;
  }

  videoFrameCount = 0;
  videoPlaying = true;
  videoPaused = false;
  lastVideoFrameMs = millis() - VIDEO_FRAME_INTERVAL_MS;

  Serial.print("Fresh 3FPS video started: ");
  Serial.println(path);
  Serial.print("Frames in file: ");
  Serial.println(fileBytes / VIDEO_FRAME_BYTES);
}

void updateVideo() {
  if (!videoPlaying || videoPaused) return;

  uint32_t now = millis();
  if ((uint32_t)(now - lastVideoFrameMs) >= VIDEO_FRAME_INTERVAL_MS) {
    lastVideoFrameMs = now;
    readAndDrawVideoFrame();
  }
}

// --------------------
// Serial command help
// --------------------

void printHelp() {
  Serial.println();
  Serial.println("===== TV MAN PLAYER a61 =====");
  Serial.println("help             - Show commands");
  Serial.println("list             - List MP3 files");
  Serial.println("play             - Play/resume MP3");
  Serial.println("play N           - Play MP3 track N");
  Serial.println("pause            - Pause MP3 playback");
  Serial.println("resume           - Resume MP3 playback");
  Serial.println("stop             - Stop MP3 playback");
  Serial.println("next             - Next MP3 track");
  Serial.println("prev             - Previous MP3 track");
  Serial.println("volume N         - Set volume (0-100)");
  Serial.println("status           - Show player status");
  Serial.println("video            - Play /video/demo.gray");
  Serial.println("video file PATH  - Play a .gray file");
  Serial.println("video pause      - Pause video");
  Serial.println("video resume     - Resume video");
  Serial.println("video stop       - Stop video");
  Serial.println("==============================");
  Serial.println("Video: 128x64 raw grayscale, 3 FPS, 8192 bytes/frame.");
  Serial.println();
}

// --------------------
// List MP3 files
// --------------------

void listMusic() {
  if (!sdReady) {
    Serial.println("SD card unavailable.");
    return;
  }

  File folder = SD.open(MUSIC_DIR);
  if (!folder || !folder.isDirectory()) {
    Serial.println("Could not open /music.");
    return;
  }

  Serial.println();
  Serial.println("===== MUSIC LIBRARY =====");
  int found = 0;
  File entry = folder.openNextFile();

  while (entry) {
    if (!entry.isDirectory()) {
      String name = entry.name();
      String lower = name;
      lower.toLowerCase();

      if (lower.endsWith(".mp3")) {
        found++;
        Serial.print(found);
        Serial.print(". ");
        Serial.print(name);
        Serial.print(" (");
        Serial.print(entry.size());
        Serial.println(" bytes)");
      }
    }
    entry.close();
    entry = folder.openNextFile();
  }

  folder.close();
  Serial.print("Total MP3 files: ");
  Serial.println(found);
  Serial.println("=========================");
}

// --------------------
// Player status
// --------------------

void printStatus() {
  Serial.println();
  Serial.println("===== PLAYER STATUS =====");
  Serial.print("SD card: ");
  Serial.println(sdReady ? "READY" : "NOT READY");
  Serial.print("Player: ");
  Serial.println(playerReady ? "READY" : "NOT READY");
  Serial.print("Tracks indexed: ");
  Serial.println(trackCount);
  Serial.print("Current track index: ");
  Serial.println(currentTrack);
  Serial.print("Video: ");
  Serial.println(videoPlaying ? (videoPaused ? "PAUSED" : "PLAYING") : "STOPPED");

  if (playerReady) {
    Serial.print("Playback active: ");
    Serial.println(player.isActive() ? "YES" : "NO");
    Serial.print("Volume: ");
    Serial.println(player.volume());
  }

  Serial.println("=========================");
}

// --------------------
// Commands
// --------------------

void processCommand(String command) {
  command.trim();
  command.toLowerCase();
  if (command.length() == 0) return;

  if (command == "help") {
    printHelp();
  } else if (command == "list") {
    listMusic();
  } else if (command == "status") {
    printStatus();
  } else if (command == "video") {
    startVideo(DEFAULT_VIDEO_FILE);
  } else if (command.startsWith("video file ")) {
    startVideo(originalCommand.substring(11));
  } else if (command == "video pause") {
    if (videoPlaying) {
      videoPaused = true;
      Serial.println("Video paused.");
    }
  } else if (command == "video resume") {
    if (videoPlaying) {
      videoPaused = false;
      lastVideoFrameMs = millis() - VIDEO_FRAME_INTERVAL_MS;
      Serial.println("Video resumed.");
    }
  } else if (command == "video stop") {
    stopVideo();
  } else if (!playerReady && command != "video pause" && command != "video resume" && command != "video stop") {
    Serial.println("MP3 player not ready. Video commands may still work if SD is ready.");
  } else if (command == "play" || command == "resume") {
    player.play();
    audioPaused = false;
    audioStopped = false;
    Serial.println("Playback started/resumed.");
  } else if (command.startsWith("play ")) {
    int number = command.substring(5).toInt();
    if (number < 1 || number > trackCount) {
      Serial.println("Invalid track number.");
    } else {
      currentTrack = number - 1;
      player.begin(currentTrack, true);
      audioPaused = false;
      audioStopped = false;
      Serial.print("Playing track ");
      Serial.println(number);
    }
  } else if (command == "pause") {
    player.stop();
    audioPaused = true;
    audioStopped = false;
    Serial.println("Playback paused.");
  } else if (command == "stop") {
    player.stop();
    player.begin(currentTrack, false);
    audioPaused = false;
    audioStopped = true;
    Serial.println("Playback stopped.");
  } else if (command == "next") {
    player.next();
    currentTrack = source.index();
    audioPaused = false;
    audioStopped = false;
    Serial.println("Next track.");
  } else if (command == "prev") {
    player.previous();
    currentTrack = source.index();
    audioPaused = false;
    audioStopped = false;
    Serial.println("Previous track.");
  } else if (command.startsWith("volume ")) {
    volumePercent = constrain(command.substring(7).toInt(), 0, 100);
    player.setVolume(volumePercent / 100.0f);
    Serial.print("Volume set to ");
    Serial.print(volumePercent);
    Serial.println("%");
  } else {
    Serial.println("Unknown command. Type 'help'.");
  }

  if (!videoPlaying) drawStatusScreen();
}

// --------------------
// Serial input
// --------------------

void serialInput() {
  while (Serial.available()) {
    char incoming = Serial.read();

    if (incoming == '\n' || incoming == '\r') {
      if (commandBuffer.length() > 0) {
        processCommand(commandBuffer);
        commandBuffer = "";
      }
    } else if (commandBuffer.length() < 120) {
      commandBuffer += incoming;
    }
  }
}

// --------------------
// Setup
// --------------------

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println("       TV MAN PLAYER a61");
  Serial.println("       FRESH 3FPS VIDEO");
  Serial.println("       XIAO ESP32-S3");
  Serial.println("================================");

  // Initialize OLED over the XIAO's default I2C bus (D4 SDA, D5 SCL).
  u8g2.begin();
  u8g2.setBusClock(400000);
  drawStatusScreen();

  // Initialize SD card (same SPI configuration as a60).
  Serial.println("Initializing microSD...");
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdReady = SD.begin(SD_CS, SPI, 4000000);

  if (!sdReady) {
    Serial.println("SD initialization FAILED.");
  } else {
    Serial.println("SD card initialized.");
  }

  // Initialize PWM audio (same configuration as a60).
  Serial.println("Initializing PWM audio...");
  auto config = pwm.defaultConfig(TX_MODE);
  config.sample_rate = 22050;
  config.channels = 2;
  config.bits_per_sample = 16;

  Pins pwmPins;
  pwmPins.push_back(PWM_LEFT);
  pwmPins.push_back(PWM_RIGHT);
  config.setPins(pwmPins);

  if (!pwm.begin(config)) {
    Serial.println("PWM initialization FAILED.");
  } else {
    Serial.println("PWM initialized.");
  }

  // Initialize MP3 source and player.
  if (sdReady) {
    Serial.println("Indexing MP3 files...");
    playerReady = player.begin(0, false);

    if (playerReady) {
      trackCount = source.size();
      currentTrack = 0;
      player.setVolume(volumePercent / 100.0f);
      Serial.print("Tracks found: ");
      Serial.println(trackCount);
    } else {
      trackCount = 0;
      Serial.println("No playable MP3 tracks found.");
    }
  }

  drawStatusScreen();
  printHelp();
  Serial.println("System initialization complete.");
}

// --------------------
// Main loop
// --------------------

void loop() {
  serialInput();

  // Keep the existing audio pipeline running.
  if (playerReady) {
    player.copy();
  }

  // Video frame updates are scheduled at 3 FPS without delay().
  updateVideo();

  // Refresh the OLED status screen periodically when not showing video.
  uint32_t now = millis();
  if (!videoPlaying && (uint32_t)(now - lastStatusRefreshMs) >= STATUS_REFRESH_MS) {
    lastStatusRefreshMs = now;
    drawStatusScreen();
  }
}
