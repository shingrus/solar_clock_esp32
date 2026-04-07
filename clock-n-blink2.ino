#include <Wire.h>
#include <RTClib.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>

#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_bt.h"

RTC_DS3231 rtc;

GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT> display(
  GxEPD2_213_B74(5, 17, 16, 4)
); // CS, DC, RST, BUSY

constexpr int16_t SCREEN_W = GxEPD2_213_B74::HEIGHT;
constexpr int16_t SCREEN_H = GxEPD2_213_B74::WIDTH_VISIBLE;
constexpr uint16_t FRAMEBUFFER_BYTES = ((SCREEN_W + 7) / 8) * SCREEN_H;
constexpr int16_t TIME_X = 80;
constexpr int16_t TIME_Y = 50;
constexpr int16_t DATE_X = 75;
constexpr int16_t DATE_Y = 85;
constexpr int16_t DIRTY_PAD = 2;
constexpr bool FORCE_FULL_SCREEN_PARTIAL_TEST = true;

RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR bool retainedFrameValid = false;
RTC_DATA_ATTR uint8_t retainedFrame[FRAMEBUFFER_BYTES];

GFXcanvas1 frameCanvas(SCREEN_W, SCREEN_H);

struct Rect {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

void disableRadios() {
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();

  btStop();
  esp_bt_controller_disable();
}

void formatClockStrings(const DateTime& now, char* timeBuf, size_t timeLen,
                        char* dateBuf, size_t dateLen) {
  snprintf(timeBuf, timeLen, "%02d:%02d", now.hour(), now.minute());
  snprintf(dateBuf, dateLen, "%02d/%02d/%04d", now.day(), now.month(), now.year());
}

void renderClockFrame(GFXcanvas1& canvas, const DateTime& now) {
  char timeBuf[6];
  char dateBuf[11];
  formatClockStrings(now, timeBuf, sizeof(timeBuf), dateBuf, sizeof(dateBuf));

  canvas.fillScreen(GxEPD_WHITE);
  canvas.setTextColor(GxEPD_BLACK);

  canvas.setFont(&FreeMonoBold18pt7b);
  canvas.setCursor(TIME_X, TIME_Y);
  canvas.print(timeBuf);

  canvas.setFont(&FreeMonoBold9pt7b);
  canvas.setCursor(DATE_X, DATE_Y);
  canvas.print(dateBuf);
}

bool computeDirtyRect(const uint8_t* previousFrame, const uint8_t* nextFrame, Rect* dirty) {
  const int16_t bytesPerRow = (SCREEN_W + 7) / 8;
  int16_t minX = SCREEN_W;
  int16_t minY = SCREEN_H;
  int16_t maxX = -1;
  int16_t maxY = -1;

  for (int16_t y = 0; y < SCREEN_H; y++) {
    const int16_t rowOffset = y * bytesPerRow;
    for (int16_t byteIndex = 0; byteIndex < bytesPerRow; byteIndex++) {
      if (previousFrame[rowOffset + byteIndex] == nextFrame[rowOffset + byteIndex]) {
        continue;
      }

      const int16_t x0 = byteIndex * 8;
      const int16_t x1 = (x0 + 7 < SCREEN_W) ? (x0 + 7) : (SCREEN_W - 1);

      if (x0 < minX) minX = x0;
      if (y < minY) minY = y;
      if (x1 > maxX) maxX = x1;
      if (y > maxY) maxY = y;
    }
  }

  if (maxX < 0) {
    return false;
  }

  minX = (minX > DIRTY_PAD) ? (minX - DIRTY_PAD) : 0;
  minY = (minY > DIRTY_PAD) ? (minY - DIRTY_PAD) : 0;
  maxX = (maxX + DIRTY_PAD < SCREEN_W) ? (maxX + DIRTY_PAD) : (SCREEN_W - 1);
  maxY = (maxY + DIRTY_PAD < SCREEN_H) ? (maxY + DIRTY_PAD) : (SCREEN_H - 1);

  dirty->x = minX;
  dirty->y = minY;
  dirty->w = maxX - minX + 1;
  dirty->h = maxY - minY + 1;
  return true;
}

void drawFramebufferToDisplay(const uint8_t* framebuffer) {
  display.drawInvertedBitmap(0, 0, framebuffer, SCREEN_W, SCREEN_H, GxEPD_BLACK);
}

void writePreviousFramebuffer(const uint8_t* framebuffer) {
  display.firstPage();
  do {
    drawFramebufferToDisplay(framebuffer);
  } while (display.nextPageToPrevious());
}

void writeCurrentFramebuffer(const uint8_t* framebuffer) {
  display.firstPage();
  do {
    drawFramebufferToDisplay(framebuffer);
  } while (display.nextPage());
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nClock start");

  disableRadios();
  Wire.begin(21, 22);

  if (!rtc.begin()) {
    Serial.println("RTC not found");
    while (1) delay(10);
  }

  const esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
  const bool wokeFromTimer = (wakeReason == ESP_SLEEP_WAKEUP_TIMER);

  if (wokeFromTimer) {
    Serial.println("Wake from deep sleep");
    display.init(115200, false, 10, false);
  } else {
    Serial.println("Cold boot");
    display.init(115200, true, 10, false);
  }

  display.setRotation(1);

  const DateTime now = rtc.now();
  renderClockFrame(frameCanvas, now);

  bootCount++;
  const bool scheduledFullRefresh = (now.minute() == 0);
  const bool fullRefresh = !wokeFromTimer || !retainedFrameValid || scheduledFullRefresh;

  Serial.print("Boot count: ");
  Serial.println(bootCount);
  Serial.print("Framebuffer valid: ");
  Serial.println(retainedFrameValid ? "yes" : "no");

  if (fullRefresh) {
    Serial.println("FULL refresh");
    display.setFullWindow();
    writeCurrentFramebuffer(frameCanvas.getBuffer());
  } else {
    Rect dirty;
    if (computeDirtyRect(retainedFrame, frameCanvas.getBuffer(), &dirty)) {
      display.setFullWindow();
      writePreviousFramebuffer(retainedFrame);

      if (FORCE_FULL_SCREEN_PARTIAL_TEST) {
        Serial.println("PARTIAL refresh (full screen test)");
        display.setPartialWindow(0, 0, display.width(), display.height());
      } else {
        Serial.println("PARTIAL refresh");
        Serial.print("Dirty rect: ");
        Serial.print(dirty.x);
        Serial.print(", ");
        Serial.print(dirty.y);
        Serial.print(", ");
        Serial.print(dirty.w);
        Serial.print(", ");
        Serial.println(dirty.h);
        display.setPartialWindow(dirty.x, dirty.y, dirty.w, dirty.h);
      }

      writeCurrentFramebuffer(frameCanvas.getBuffer());
    } else {
      Serial.println("No display changes");
    }
  }

  memcpy(retainedFrame, frameCanvas.getBuffer(), FRAMEBUFFER_BYTES);
  retainedFrameValid = true;

  Serial.println("Clock drawn");
  display.hibernate();

  int secondsToNextMinute = 60 - now.second();
  if (secondsToNextMinute <= 0) {
    secondsToNextMinute = 60;
  }

  const uint64_t sleepTimeUs = uint64_t(secondsToNextMinute) * 1000000ULL;

  Serial.print("Sleeping for seconds: ");
  Serial.println(secondsToNextMinute);

  esp_sleep_enable_timer_wakeup(sleepTimeUs);
  esp_deep_sleep_start();
}

void loop() {}
