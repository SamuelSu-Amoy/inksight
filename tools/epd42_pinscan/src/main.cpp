// 4.2" 屏 —— 按原厂固件 EPD420R（UC8176 三色）序列驱动
//
// 依据：gitee.com/jetmie/evernote  open/idisplay/ble/epd.py
//     class EPD420R(EPD3):
//         EPD_WIDTH = 400 ; EPD_HEIGHT = 300 ; BUSY = 0   (低=忙)
//         init(): reset -> 0x06 17 17 17 -> 0x04 -> wait -> 0x00 0F
//         display(): 0x10(黑) -> 0x13(红) -> 0x12 -> wait
//     utils.devices = {132:EPD213, 133:EPD213R, 134:EPD266R, 135:EPD290, 136:EPD420R}
//
// 之前一直用 SSD1683 的指令集（0x12 软复位 / 0x11 / 0x44 / 0x45 / 0x24 / 0x22 / 0x20），
// 那是完全另一套控制器。UC8176 里 0x12 是 DISPLAY_REFRESH，
// 所以我们发的"软复位"对这块屏来说只是"刷新一屏空数据"，当然什么都不会发生。
//
// 这版按原厂序列走一遍，每步打印 BUSY 波形。
//
// 刷机：merged.bin 烧 0x0（DIO）

#include <Arduino.h>

#define PIN_RST   14
#define PIN_DC    13
#define PIN_CS    10
#define PIN_SCK   12
#define PIN_MOSI  11
#define PIN_BUSY  15

#define FB_LEN 15000      // 400*300/8

// UC8176 指令
#define CMD_PANEL_SETTING   0x00
#define CMD_POWER_OFF       0x02
#define CMD_POWER_ON        0x04
#define CMD_BOOSTER         0x06
#define CMD_DATA_1          0x10
#define CMD_DISPLAY_REFRESH 0x12
#define CMD_DATA_2          0x13
#define CMD_VCOM_INTERVAL   0x50
#define CMD_RESOLUTION      0x61

static void spiB(uint8_t b) {
    for (int i = 0; i < 8; i++) {
        digitalWrite(PIN_MOSI, (b & 0x80) ? HIGH : LOW);
        b <<= 1;
        digitalWrite(PIN_SCK, HIGH);
        digitalWrite(PIN_SCK, LOW);
    }
}
static void c(uint8_t cmd) {
    digitalWrite(PIN_DC, LOW);
    digitalWrite(PIN_CS, LOW);
    spiB(cmd);
    digitalWrite(PIN_CS, HIGH);
}
static void d1(uint8_t v) {
    digitalWrite(PIN_DC, HIGH);
    digitalWrite(PIN_CS, LOW);
    spiB(v);
    digitalWrite(PIN_CS, HIGH);
}

// 低 = 忙（EPD3.BUSY = 0）
static bool waitIdle(const char *tag, unsigned long timeoutMs) {
    unsigned long t0 = millis();
    while (digitalRead(PIN_BUSY) == LOW) {
        if (millis() - t0 > timeoutMs) {
            Serial.printf("  [%s] TIMEOUT %lu ms, BUSY=%d\n", tag,
                          (unsigned long)(millis() - t0), digitalRead(PIN_BUSY));
            return false;
        }
        delay(5);
    }
    Serial.printf("  [%s] idle ok %lu ms\n", tag, (unsigned long)(millis() - t0));
    return true;
}

// BUSY 波形（游程编码）
static void trace(const char *tag, unsigned long ms) {
    Serial.printf("  TRACE %-18s [", tag);
    unsigned long t0 = millis();
    int last = digitalRead(PIN_BUSY);
    unsigned long seg = t0;
    while (millis() - t0 < ms) {
        int v = digitalRead(PIN_BUSY);
        if (v != last) {
            Serial.printf("%c+%lu ", last ? 'H' : 'L', millis() - seg);
            seg = millis();
            last = v;
        }
        delay(1);
    }
    Serial.printf("%c+%lu]\n", last ? 'H' : 'L', millis() - seg);
}

static void hardReset() {
    // 原厂 EPD.reset(): rst=0 -> 200ms -> rst=1 -> 200ms
    digitalWrite(PIN_RST, LOW);  delay(200);
    digitalWrite(PIN_RST, HIGH); delay(200);
}

static void writePlane(uint8_t cmd, uint8_t value) {
    c(cmd);
    digitalWrite(PIN_DC, HIGH);
    digitalWrite(PIN_CS, LOW);
    for (int i = 0; i < FB_LEN; i++) spiB(value);
    digitalWrite(PIN_CS, HIGH);
}

void setup() {
    Serial.begin(115200);
    delay(2500);

    Serial.println();
    Serial.println("=================================================");
    Serial.println(" 4.2\" UC8176 (yuan chang EPD420R) sequence");
    Serial.println(" SCK=12 MOSI=11 CS=10 DC=13 RST=14 BUSY=15");
    Serial.println("=================================================");

    pinMode(PIN_RST,  OUTPUT);
    pinMode(PIN_DC,   OUTPUT);
    pinMode(PIN_CS,   OUTPUT);
    pinMode(PIN_SCK,  OUTPUT);
    pinMode(PIN_MOSI, OUTPUT);
    pinMode(PIN_BUSY, INPUT_PULLUP);
    digitalWrite(PIN_CS, HIGH);
    digitalWrite(PIN_DC, LOW);
    digitalWrite(PIN_SCK, LOW);

    Serial.printf("pre BUSY=%d (0=busy)\n", digitalRead(PIN_BUSY));

    // 1 硬复位
    Serial.println("\n[1] reset (RST L 200ms -> H 200ms)");
    hardReset();
    trace("after reset", 500);

    // 2 原厂 init
    Serial.println("\n[2] 0x06 booster 17 17 17");
    c(CMD_BOOSTER); d1(0x17); d1(0x17); d1(0x17);
    trace("after 0x06", 200);

    Serial.println("[3] 0x04 power on");
    c(CMD_POWER_ON);
    waitIdle("power on", 15000);
    trace("after 0x04", 500);

    Serial.println("[4] 0x00 panel setting 0x0F");
    c(CMD_PANEL_SETTING); d1(0x0F);
    delay(50);

    Serial.println("[5] 0x61 400x300 + 0x50 0x37");
    c(CMD_RESOLUTION); d1(0x01); d1(0x90); d1(0x01); d1(0x2C);
    c(CMD_VCOM_INTERVAL); d1(0x37);

    // 6 刷全黑
    Serial.println("\n[6] push BLACK: 0x10=00, 0x13=00, refresh");
    unsigned long t = millis();
    writePlane(CMD_DATA_1, 0x00);
    writePlane(CMD_DATA_2, 0x00);
    Serial.printf("  data done %lu ms\n", millis() - t);
    c(CMD_DISPLAY_REFRESH);
    delay(10);
    waitIdle("refresh black", 40000);
    trace("after refresh", 1000);

    // 7 刷全白
    Serial.println("\n[7] push WHITE: 0x10=FF, 0x13=00, refresh");
    writePlane(CMD_DATA_1, 0xFF);
    writePlane(CMD_DATA_2, 0x00);
    c(CMD_DISPLAY_REFRESH);
    delay(10);
    waitIdle("refresh white", 40000);
    trace("after refresh", 1000);

    // 8 断电对照
    Serial.println("\n[8] power off cycle");
    c(CMD_POWER_OFF);
    waitIdle("power off", 10000);
    trace("after 0x02", 1000);

    Serial.println("\n--- done ---");
}

void loop() {
    Serial.printf("[idle] t=%lus BUSY=%d\n", (unsigned long)(millis() / 1000), digitalRead(PIN_BUSY));
    delay(10000);
}
