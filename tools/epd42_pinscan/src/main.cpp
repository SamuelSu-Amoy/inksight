// 4.2" HINK-E042A13-A0（SSD1619A，400x300）在 iDevice S3 板上的诊断固件
//
// 做的事：
//   阶段 1 · GPIO 指纹扫描
//     把所有安全 GPIO 设成 INPUT_PULLUP 读一遍，再翻转 RST，再读一遍。
//     找出「哪个脚会跟着 RST 变化」—— 那才是真正的 BUSY。
//     如果全都不变，说明屏根本没通电 / FPC 没接触。
//
//   阶段 2 · 双极性点屏
//     用标准 SSD1683 序列初始化，先按「高=忙」等 15 s，
//     再按「低=忙」等 15 s，两步都打印实际耗时。
//     然后刷全黑、全白各一次 —— 屏上看得见闪烁就说明通路是通的。
//
// 刷机：merged.bin 烧 0x0（DIO）
// 日志：USB CDC 115200

#include <Arduino.h>

#define PIN_RST   14
#define PIN_DC    13
#define PIN_CS    10
#define PIN_SCK   12
#define PIN_MOSI  11
#define PIN_BUSY  15

// ESP32-S3 上可安全操作的 GPIO（跳过 19/20 USB、26~37 flash/PSRAM）
static const int PINS[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
    10, 11, 12, 13, 14, 15, 16, 17, 18,
    21, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48
};
static const int NP = (int)(sizeof(PINS) / sizeof(PINS[0]));

static char snapA[NP + 1], snapB[NP + 1], snapC[NP + 1], snapD[NP + 1];

// ── 软件 SPI ───────────────────────────────────────────────
static void spiByte(uint8_t b) {
    for (int i = 0; i < 8; i++) {
        digitalWrite(PIN_MOSI, (b & 0x80) ? HIGH : LOW);
        b <<= 1;
        digitalWrite(PIN_SCK, HIGH);
        digitalWrite(PIN_SCK, LOW);
    }
}
static void cmd(uint8_t c) {
    digitalWrite(PIN_DC, LOW);
    digitalWrite(PIN_CS, LOW);
    spiByte(c);
    digitalWrite(PIN_CS, HIGH);
}
static void dat(uint8_t d) {
    digitalWrite(PIN_DC, HIGH);
    digitalWrite(PIN_CS, LOW);
    spiByte(d);
    digitalWrite(PIN_CS, HIGH);
}

// ── 输出脚恢复成驱动状态 ───────────────────────────────────
static void restoreOutputs() {
    pinMode(PIN_RST,  OUTPUT); digitalWrite(PIN_RST,  HIGH);
    pinMode(PIN_DC,   OUTPUT); digitalWrite(PIN_DC,   LOW);
    pinMode(PIN_CS,   OUTPUT); digitalWrite(PIN_CS,   HIGH);
    pinMode(PIN_SCK,  OUTPUT); digitalWrite(PIN_SCK,  LOW);
    pinMode(PIN_MOSI, OUTPUT); digitalWrite(PIN_MOSI, LOW);
}

// ── 把全部脚设成上拉输入采样一次 ───────────────────────────
static void snap(char *out) {
    for (int i = 0; i < NP; i++) pinMode(PINS[i], INPUT_PULLUP);
    delay(6);
    for (int i = 0; i < NP; i++) out[i] = digitalRead(PINS[i]) ? '1' : '0';
    out[NP] = 0;
}

static void printRaw(const char *tag, const char *s) {
    Serial.printf("  %s %s\n", tag, s);
}

static void printDiff(const char *tag, const char *base, const char *cur) {
    Serial.printf("  %s:", tag);
    bool any = false;
    for (int i = 0; i < NP; i++) {
        if (base[i] != cur[i]) {
            Serial.printf(" GPIO%d %c->%c", PINS[i], base[i], cur[i]);
            any = true;
        }
    }
    if (!any) Serial.print(" (no change)");
    Serial.println();
}

static void printLows(const char *tag, const char *s) {
    Serial.printf("  %s low-pins:", tag);
    bool any = false;
    for (int i = 0; i < NP; i++) {
        if (s[i] == '0') { Serial.printf(" GPIO%d", PINS[i]); any = true; }
    }
    if (!any) Serial.print(" (none)");
    Serial.println();
}

// ── BUSY 轮询（可按极性），返回耗时毫秒，超时返回 -1 ─────────
static long waitBusy(bool highIsBusy, unsigned long timeoutMs) {
    unsigned long t0 = millis();
    while (true) {
        int v = digitalRead(PIN_BUSY);
        bool busy = highIsBusy ? (v == HIGH) : (v == LOW);
        if (!busy) return (long)(millis() - t0);
        if (millis() - t0 > timeoutMs) return -1;
        delay(10);
    }
}

// ── SSD1683 / SSD1619 标准初始化 ───────────────────────────
static void ssdReset() {
    digitalWrite(PIN_RST, HIGH); delay(20);
    digitalWrite(PIN_RST, LOW);  delay(20);
    digitalWrite(PIN_RST, HIGH); delay(20);
}

static void ssdInitCommands() {
    cmd(0x12);                    // SW reset
    delay(10);
    cmd(0x21); dat(0x40); dat(0x00);
    cmd(0x3C); dat(0x05);         // border waveform
    cmd(0x11); dat(0x03);         // data entry
    cmd(0x44); dat(0x00); dat(0x31);            // RAM X 0..49 (400/8-1)
    cmd(0x45); dat(0x00); dat(0x00); dat(0x2B); dat(0x01);  // RAM Y 0..299
    cmd(0x4E); dat(0x00);
    cmd(0x4F); dat(0x00); dat(0x00);
}

// 刷一整屏单色（0xFF = 白, 0x00 = 黑）
static void fillScreen(uint8_t value) {
    cmd(0x24);
    digitalWrite(PIN_DC, HIGH);
    digitalWrite(PIN_CS, LOW);
    for (int i = 0; i < 15000; i++) spiByte(value);   // 400/8*300
    digitalWrite(PIN_CS, HIGH);

    cmd(0x22); dat(0xF7);
    cmd(0x20);
}

void setup() {
    Serial.begin(115200);
    delay(2500);

    Serial.println();
    Serial.println("=========================================");
    Serial.println(" 4.2\" HINK / SSD1619  iDevice-S3 诊断");
    Serial.println(" SCK=12 MOSI=11 CS=10 DC=13 RST=14 BUSY=15");
    Serial.println("=========================================");
    Serial.printf("boot GPIO levels: BUSY(15)=%d DC(13)=%d CS(10)=%d RST(14)=%d\n",
                  digitalRead(PIN_BUSY), digitalRead(PIN_DC),
                  digitalRead(PIN_CS),  digitalRead(PIN_RST));

    // ── 阶段 1：GPIO 指纹 ──────────────────────────────────
    Serial.println("\n--- STAGE 1: GPIO fingerprint ---");

    snap(snapA);
    printLows("A(idle)", snapA);
    printRaw("A(pins)", snapA);

    restoreOutputs();
    delay(200);
    snap(snapB);
    printLows("B(RST=H,driven)", snapB);
    printDiff("B vs A", snapA, snapB);

    restoreOutputs();
    digitalWrite(PIN_RST, LOW);  delay(50);
    digitalWrite(PIN_RST, HIGH); delay(50);
    snap(snapC);
    printLows("C(after RST pulse 50ms)", snapC);
    printDiff("C vs A", snapA, snapC);

    restoreOutputs();
    delay(2500);
    snap(snapD);
    printLows("D(+2.5s settle)", snapD);
    printDiff("D vs C", snapC, snapD);

    // 单独盯 BUSY 30 次
    Serial.print("  BUSY(15) x30:");
    pinMode(PIN_BUSY, INPUT_PULLUP);
    for (int i = 0; i < 30; i++) { Serial.print(digitalRead(PIN_BUSY)); delay(20); }
    Serial.println();

    // ── 阶段 2：双极性点屏 ─────────────────────────────────
    Serial.println("\n--- STAGE 2: drive test (both polarities) ---");

    restoreOutputs();
    ssdReset();
    ssdInitCommands();

    pinMode(PIN_BUSY, INPUT_PULLUP);
    long t1 = waitBusy(true, 15000);
    Serial.printf("  wait(HIGH=busy) after init : %ld ms  BUSY=%d\n", t1, digitalRead(PIN_BUSY));
    long t2 = waitBusy(false, 15000);
    Serial.printf("  wait(LOW =busy) after init : %ld ms  BUSY=%d\n", t2, digitalRead(PIN_BUSY));

    Serial.println("  -> fill BLACK then WHITE, watch the panel");

    for (int round = 0; round < 2; round++) {
        uint8_t v = (round == 0) ? 0x00 : 0xFF;
        Serial.printf("  [round %d] fill 0x%02X\n", round, v);
        fillScreen(v);
        long a = waitBusy(true, 20000);
        if (a < 0) a = waitBusy(false, 20000);
        Serial.printf("  [round %d] busy done: %ld ms\n", round, a);
        delay(1500);
    }

    Serial.println("\n--- DONE (loops RST every 5s) ---");
    Serial.println("If panel flashed black/white -> wiring OK, only polarity/param issue.");
    Serial.println("If NOTHING changed        -> FPC pinout mismatch or panel unpowered.");
}

void loop() {
    // 每 5 秒来一次复位脉冲 + 打印 BUSY，方便一边看屏一边看日志
    restoreOutputs();
    digitalWrite(PIN_RST, LOW);  delay(30);
    digitalWrite(PIN_RST, HIGH); delay(30);
    pinMode(PIN_BUSY, INPUT_PULLUP);
    delay(50);
    Serial.printf("[loop] t=%lus BUSY=%d\n", (unsigned long)(millis() / 1000), digitalRead(PIN_BUSY));
    delay(5000);
}
