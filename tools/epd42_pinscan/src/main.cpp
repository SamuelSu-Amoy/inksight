// 4.2" 屏 —— SPI 通路矩阵测试
//
// 上一轮的关键线索：
//     pre BUSY=0        → 上电时屏在拉低 BUSY（屏有电、BUSY 线通）
//     after reset H     → RST 脉冲后屏释放（RST 线也通）
//     after 0x04 H+500  → power on 之后 BUSY 完全没动（命令没进屏）
//
// 结论：RST 和 BUSY 两条线是通的，但 SPI 写入没有到达芯片。
// 可能只有三种：CS 不使能 / DC 反了 / 屏被 BS 脚切到 3 线模式。
//
// 这一版把三种可能排列组合成 6 个组合，每个组合都做一遍
// "复位 → booster → power on"，然后看 BUSY 会不会被拉低。
// 只要有一种让 BUSY 变低，就说明命令进去了，问题就定位了。
//
// 判据：0x04 power on 会让任何 EPD 控制器把 BUSY 拉低几百毫秒。

#include <Arduino.h>

#define PIN_RST   14
#define PIN_DC    13
#define PIN_CS    10
#define PIN_SCK   12
#define PIN_MOSI  11
#define PIN_BUSY  15

static bool g_csFixed = false;   // true = CS 常低，不用 GPIO 控制
static bool g_dcFixedLow = false; // true = DC 常低（所有字节都当命令）

// ── 4 线 SPI ───────────────────────────────────────────────
static void emit(uint8_t b, bool isData) {
    if (!g_csFixed) {
        digitalWrite(PIN_CS, LOW);
    }
    if (g_dcFixedLow) {
        digitalWrite(PIN_DC, LOW);
    } else {
        digitalWrite(PIN_DC, isData ? HIGH : LOW);
    }
    for (int i = 0; i < 8; i++) {
        digitalWrite(PIN_MOSI, (b & 0x80) ? HIGH : LOW);
        b <<= 1;
        digitalWrite(PIN_SCK, HIGH);
        digitalWrite(PIN_SCK, LOW);
    }
    if (!g_csFixed) {
        digitalWrite(PIN_CS, HIGH);
    }
}

// ── 3 线 / 9-bit（第 1 位 = D/C）──────────────────────────
static void emit3(uint8_t b, bool isData) {
    digitalWrite(PIN_CS, LOW);
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

static void hardReset() {
    digitalWrite(PIN_RST, LOW);  delay(200);
    digitalWrite(PIN_RST, HIGH); delay(200);
}

// 观察 BUSY 一段时间，返回「出现低电平」的累计毫秒
static unsigned long watchLow(unsigned long ms) {
    unsigned long total = 0, t0 = millis();
    while (millis() - t0 < ms) {
        if (digitalRead(PIN_BUSY) == LOW) total++;
        delay(1);
    }
    return total;
}

static void resetPins() {
    pinMode(PIN_RST,  OUTPUT);
    pinMode(PIN_DC,   OUTPUT);
    pinMode(PIN_CS,   OUTPUT);
    pinMode(PIN_SCK,  OUTPUT);
    pinMode(PIN_MOSI, OUTPUT);
    pinMode(PIN_BUSY, INPUT_PULLUP);
    digitalWrite(PIN_RST, HIGH);
    digitalWrite(PIN_CS, g_csFixed ? LOW : HIGH);
    digitalWrite(PIN_DC, LOW);
    digitalWrite(PIN_SCK, LOW);
    digitalWrite(PIN_MOSI, LOW);
}

// 一个组合：复位 → 0x06 17 17 17 → 0x04，返回低电平累计
static unsigned long probe4(const char *name, bool csFixed, bool dcLow) {
    g_csFixed = csFixed;
    g_dcFixedLow = dcLow;
    resetPins();
    hardReset();
    unsigned long base = watchLow(200);      // 复位后的基线

    emit(0x06, false); emit(0x17, true); emit(0x17, true); emit(0x17, true);
    emit(0x04, false);                        // POWER ON
    unsigned long busy = watchLow(3000);

    Serial.printf("  %-34s base=%lums  afterPwrOn=%lums  %s\n",
                  name, base, busy,
                  (busy > base + 50) ? "<<< BUSY 动了！" : "");
    return busy;
}

static unsigned long probe3(const char *name) {
    resetPins();
    hardReset();
    unsigned long base = watchLow(200);

    emit3(0x06, false); emit3(0x17, true); emit3(0x17, true); emit3(0x17, true);
    emit3(0x04, false);
    unsigned long busy = watchLow(3000);

    Serial.printf("  %-34s base=%lums  afterPwrOn=%lums  %s\n",
                  name, base, busy,
                  (busy > base + 50) ? "<<< BUSY 动了！" : "");
    return busy;
}

void setup() {
    Serial.begin(115200);
    delay(2500);

    Serial.println();
    Serial.println("==========================================================");
    Serial.println(" 4.2\" SPI path matrix  (0x04 power-on must pull BUSY low)");
    Serial.println(" SCK=12 MOSI=11 CS=10 DC=13 RST=14 BUSY=15");
    Serial.println("==========================================================");
    Serial.printf("pre BUSY=%d (0=busy)\n\n", digitalRead(PIN_BUSY));

    Serial.println("[A] 4-wire combinations");
    probe4("A1 CS=gpio  DC=gpio   (normal)",     false, false);
    probe4("A2 CS=fixed DC=gpio",                true,  false);
    probe4("A3 CS=gpio  DC=fixed-low",           false, true);
    probe4("A4 CS=fixed DC=fixed-low",           true,  true);

    Serial.println("\n[B] 3-wire (9-bit) mode");
    probe3("B1 9-bit  DC in first bit");

    Serial.println("\n==========================================================");
    Serial.println(" 判读：");
    Serial.println("  任一组合出现 <<< BUSY 动了  -> 命令通了，按那组配置继续");
    Serial.println("  全部 base 接近 afterPwrOn   -> 屏侧没收到 SPI，或板子缺升压电路");
    Serial.println("==========================================================");

    resetPins();
    hardReset();
}

void loop() {
    Serial.printf("[idle] t=%lus BUSY=%d\n", (unsigned long)(millis() / 1000), digitalRead(PIN_BUSY));
    delay(10000);
}
