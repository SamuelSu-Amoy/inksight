// 4.2" SSD1683/SSD1619 黑白 —— 硬件 SPI 版
//
// 前六组（软件 SPI：CS/DC 各种固定、9-bit 3线）全部 base=0 afterPwrOn=0，
// 说明命令一个都没进屏。软件 SPI 是唯一还没排除的变量：
// 4.2 的 FPC 比 2.13 长，走线电容大，digitalWrite 翻出来的边沿可能被吃掉。
//
// 这一版换成 ESP32 硬件 SPI（FSPI），并且分三档速率各试一遍：
//   1) 硬件 SPI 1MHz
//   2) 硬件 SPI 4MHz
//   3) 软件 SPI 100kHz（每 bit 加 10us 延时）
//
// 每档只发两条最能验证通路的命令：
//   0x12  Soft Reset   → SSD1683 会忙几十 ms（BUSY 翻转）
//   0x04  Power On     → 会忙几百 ms
// 只要有一档看到 BUSY 动了，就说明线是通的，只是速率/时序问题。
//
// 注意：GPIO13 是这块板的 DC，而它同时是 ESP32-S3 FSPI 的默认 MISO，
// 所以 SPI.begin 的 MISO 必须显式给 GPIO21，否则 SPI 会把 GPIO13 抢走。

#include <Arduino.h>
#include <SPI.h>

#define PIN_RST   14
#define PIN_DC    13
#define PIN_CS    10
#define PIN_SCK   12
#define PIN_MOSI  11
#define PIN_BUSY  15

static bool g_useHw = true;
static uint32_t g_hz = 1000000;

static void bitbang(uint8_t b, bool slow) {
    for (int i = 0; i < 8; i++) {
        digitalWrite(PIN_MOSI, (b & 0x80) ? HIGH : LOW);
        b <<= 1;
        digitalWrite(PIN_SCK, HIGH);
        if (slow) delayMicroseconds(5);
        digitalWrite(PIN_SCK, LOW);
        if (slow) delayMicroseconds(5);
    }
}

static void txCmd(uint8_t cmd) {
    digitalWrite(PIN_DC, LOW);
    digitalWrite(PIN_CS, LOW);
    if (g_useHw) {
        SPI.beginTransaction(SPISettings(g_hz, MSBFIRST, SPI_MODE0));
        SPI.transfer(cmd);
        SPI.endTransaction();
    } else {
        bitbang(cmd, false);
    }
    digitalWrite(PIN_CS, HIGH);
}

static void txData(uint8_t d) {
    digitalWrite(PIN_DC, HIGH);
    digitalWrite(PIN_CS, LOW);
    if (g_useHw) {
        SPI.beginTransaction(SPISettings(g_hz, MSBFIRST, SPI_MODE0));
        SPI.transfer(d);
        SPI.endTransaction();
    } else {
        bitbang(d, false);
    }
    digitalWrite(PIN_CS, HIGH);
}

static unsigned long watchLow(unsigned long ms) {
    unsigned long total = 0, t0 = millis();
    while (millis() - t0 < ms) {
        if (digitalRead(PIN_BUSY) == LOW) total++;
        delay(1);
    }
    return total;
}

static void hardReset() {
    digitalWrite(PIN_RST, HIGH); delay(10);
    digitalWrite(PIN_RST, LOW);  delay(10);
    digitalWrite(PIN_RST, HIGH); delay(10);
}

// 一档速率的完整探针
static void probe(const char *name, bool useHw, uint32_t hz) {
    g_useHw = useHw;
    g_hz = hz;

    pinMode(PIN_RST,  OUTPUT);
    pinMode(PIN_DC,   OUTPUT);
    pinMode(PIN_CS,   OUTPUT);
    pinMode(PIN_SCK,  OUTPUT);
    pinMode(PIN_MOSI, OUTPUT);
    pinMode(PIN_BUSY, INPUT_PULLUP);
    digitalWrite(PIN_CS, HIGH);
    digitalWrite(PIN_DC, LOW);
    digitalWrite(PIN_SCK, LOW);

    if (useHw) {
        SPI.end();
        SPI.begin(PIN_SCK, 21, PIN_MOSI, PIN_CS);   // MISO 用 21，避开 GPIO13=DC
    }

    hardReset();
    unsigned long base = watchLow(300);

    txCmd(0x12);                       // Soft Reset
    unsigned long afterResetCmd = watchLow(1500);

    txCmd(0x04);                       // Power On
    unsigned long afterPowerOn = watchLow(3000);

    Serial.printf("  %-22s base=%lums  0x12=%lums  0x04=%lums  %s\n",
                  name, base, afterResetCmd, afterPowerOn,
                  (afterResetCmd + afterPowerOn > 50) ? "<<< BUSY 动了！" : "");
}

void setup() {
    Serial.begin(115200);
    delay(2500);

    Serial.println();
    Serial.println("==========================================================");
    Serial.println(" 4.2\" SSD1683 BW  --  hardware SPI vs bit-bang");
    Serial.println("==========================================================");
    Serial.printf("pre BUSY=%d\n\n", digitalRead(PIN_BUSY));

    probe("HW SPI 1MHz",  true,  1000000);
    probe("HW SPI 4MHz",  true,  4000000);
    probe("HW SPI 100kHz", true, 100000);
    probe("bit-bang 100kHz", false, 0);

    Serial.println("\n==========================================================");
    Serial.println(" 任一行 0x12 / 0x04 有低电平 -> SPI 通了，只是速度/时序问题");
    Serial.println(" 四行全 0                     -> 信号线真的没通到屏");
    Serial.println("==========================================================");
}

void loop() {
    Serial.printf("[idle] t=%lus BUSY=%d\n", (unsigned long)(millis() / 1000), digitalRead(PIN_BUSY));
    delay(10000);
}
