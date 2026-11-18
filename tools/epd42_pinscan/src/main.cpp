// 4.2" HINK-E042A13-A0（SSD1619A） BUSY 波形追踪
//
// 上一版扫描的结论：GPIO15 在 idle 时被屏拉低（0），RST 驱动后变高（1）。
// 说明屏是通电的、BUSY 这根线是通的。但发完初始化命令 BUSY 没有任何动作。
//
// 这一版只干一件事：把 BUSY 的完整波形打出来（游程编码，只在跳变时输出）。
// 看波形就能区分：
//   · RST 释放后 有 L→H→L 的脉冲  → 屏在正常初始化，只是参数不对
//   · 全程一条直线                → 命令根本没被屏接收（SPI 线序 / BS 模式不对）
//   · 一直停在 H 不回来            → 屏在等升压，但这块板没有 4.2 需要的升压电路
//
// 刷机：merged.bin 烧 0x0（DIO）

#include <Arduino.h>

#define PIN_RST   14
#define PIN_DC    13
#define PIN_CS    10
#define PIN_SCK   12
#define PIN_MOSI  11
#define PIN_BUSY  15

// ── 软件 SPI（4 线）────────────────────────────────────────
static void spiByte(uint8_t b) {
    for (int i = 0; i < 8; i++) {
        digitalWrite(PIN_MOSI, (b & 0x80) ? HIGH : LOW);
        b <<= 1;
        digitalWrite(PIN_SCK, HIGH);
        digitalWrite(PIN_SCK, LOW);
    }
}
static void cmd4(uint8_t c) {
    digitalWrite(PIN_DC, LOW);
    digitalWrite(PIN_CS, LOW);
    spiByte(c);
    digitalWrite(PIN_CS, HIGH);
}
static void dat4(uint8_t d) {
    digitalWrite(PIN_DC, HIGH);
    digitalWrite(PIN_CS, LOW);
    spiByte(d);
    digitalWrite(PIN_CS, HIGH);
}

// ── 软件 SPI（3 线 / 9-bit：第 9 位 = D/C）─────────────────
static void spi3(uint8_t b, bool isData) {
    digitalWrite(PIN_CS, LOW);
    // 第 1 位是 D/C
    digitalWrite(PIN_MOSI, isData ? HIGH : LOW);
    digitalWrite(PIN_SCK, HIGH);
    digitalWrite(PIN_SCK, LOW);
    for (int i = 0; i < 8; i++) {
        digitalWrite(PIN_MOSI, (b & 0x80) ? HIGH : LOW);
        b <<= 1;
        digitalWrite(PIN_SCK, HIGH);
        digitalWrite(PIN_SCK, LOW);
    }
    digitalWrite(PIN_CS, HIGH);
}

// ── BUSY 波形追踪（游程编码）───────────────────────────────
static void trace(const char *tag, unsigned long ms) {
    Serial.printf("  TRACE %-22s [", tag);
    unsigned long t0 = millis();
    int last = digitalRead(PIN_BUSY);
    unsigned long tSeg = t0;
    while (millis() - t0 < ms) {
        int v = digitalRead(PIN_BUSY);
        if (v != last) {
            Serial.printf("%c+%lu ", last ? 'H' : 'L', millis() - tSeg);
            tSeg = millis();
            last = v;
        }
        delay(1);
    }
    Serial.printf("%c+%lu]\n", last ? 'H' : 'L', millis() - tSeg);
}

static void pinsOutput() {
    pinMode(PIN_RST,  OUTPUT);
    pinMode(PIN_DC,   OUTPUT);
    pinMode(PIN_CS,   OUTPUT);
    pinMode(PIN_SCK,  OUTPUT);
    pinMode(PIN_MOSI, OUTPUT);
    digitalWrite(PIN_CS, HIGH);
    digitalWrite(PIN_SCK, LOW);
    digitalWrite(PIN_MOSI, LOW);
    digitalWrite(PIN_DC, LOW);
    pinMode(PIN_BUSY, INPUT_PULLUP);
}

// SSD1683 / SSD1619 全套初始化（4 线）
static void ssdFull4() {
    cmd4(0x12);                     // soft reset
    trace("after 0x12", 200);
    cmd4(0x21); dat4(0x40); dat4(0x00);
    cmd4(0x3C); dat4(0x05);
    cmd4(0x11); dat4(0x03);
    cmd4(0x44); dat4(0x00); dat4(0x31);
    cmd4(0x45); dat4(0x00); dat4(0x00); dat4(0x2B); dat4(0x01);
    cmd4(0x4E); dat4(0x00);
    cmd4(0x4F); dat4(0x00); dat4(0x00);
}

static void fill4(uint8_t v) {
    cmd4(0x24);
    digitalWrite(PIN_DC, HIGH);
    digitalWrite(PIN_CS, LOW);
    for (int i = 0; i < 15000; i++) spiByte(v);
    digitalWrite(PIN_CS, HIGH);
    cmd4(0x22); dat4(0xF7);
    cmd4(0x20);
}

void setup() {
    Serial.begin(115200);
    delay(2500);

    Serial.println();
    Serial.println("=================================================");
    Serial.println(" SSD1619 BUSY waveform tracer  (iDevice S3 4.2\")");
    Serial.println("=================================================");
    Serial.printf("pre: BUSY=%d\n", digitalRead(PIN_BUSY));

    pinsOutput();

    // ── ① 硬复位波形 ───────────────────────────────────────
    Serial.println("\n[1] hard reset waveform");
    digitalWrite(PIN_RST, HIGH); trace("RST=H before", 60);
    digitalWrite(PIN_RST, LOW);  trace("RST=L 20ms", 20 * 1);
    digitalWrite(PIN_RST, HIGH); trace("RST=H release", 400);

    // ── ② 4 线软复位 ───────────────────────────────────────
    Serial.println("\n[2] 4-wire: 0x12 soft reset");
    cmd4(0x12);
    trace("4w after 0x12", 400);

    // ── ③ 4 线 power on ───────────────────────────────────
    Serial.println("\n[3] 4-wire: full init + 0x04 power on");
    ssdFull4();
    cmd4(0x04);
    trace("4w after 0x04", 4000);

    // ── ④ 4 线刷全黑 ──────────────────────────────────────
    Serial.println("\n[4] 4-wire: fill BLACK (0x24=00, 0x22=F7, 0x20)");
    fill4(0x00);
    trace("4w fill BLACK", 20000);

    // ── ⑤ 4 线刷全白 ──────────────────────────────────────
    Serial.println("\n[5] 4-wire: fill WHITE (0x24=FF)");
    fill4(0xFF);
    trace("4w fill WHITE", 20000);

    // ── ⑥ 3 线（9-bit）再试一遍 ───────────────────────────
    Serial.println("\n[6] 3-wire 9-bit mode (in case BS pin selects 3-wire)");
    digitalWrite(PIN_RST, LOW);  delay(20);
    digitalWrite(PIN_RST, HIGH); delay(20);
    spi3(0x12, false);
    trace("3w after 0x12", 400);

    Serial.println("\n=================================================");
    Serial.println(" 判读：");
    Serial.println("  RST 释放后出现 L+xx / H+xx 跳变  -> 屏活着");
    Serial.println("  0x12 / 0x04 后有跳变           -> 命令收到了，是参数问题");
    Serial.println("  某一步之后一直是 H 不回来       -> 卡在升压（硬件缺电路）");
    Serial.println("  全程一条直线                    -> SPI 线序不对");
    Serial.println("=================================================");
}

void loop() {
    // 每 8 秒一次慢速 RST 脉冲，方便对照屏幕画面
    digitalWrite(PIN_RST, LOW);  delay(50);
    digitalWrite(PIN_RST, HIGH); delay(50);
    Serial.printf("[idle] t=%lus BUSY=%d\n", (unsigned long)(millis() / 1000), digitalRead(PIN_BUSY));
    delay(8000);
}
