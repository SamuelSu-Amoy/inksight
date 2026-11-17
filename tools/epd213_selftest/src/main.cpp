/*
 * ============================================================
 *  iDevice 墨水屏驱动板 · 2.13" 点屏测试（最小可运行示例）
 * ============================================================
 *
 *  适用硬件：
 *    - iDevice 墨水屏驱动板 ESP32-S3 版（N8 / N8R2 / N16R8 都行）
 *    - 板上 "epaper 2.13b" FPC 座直接插 2.13" 屏
 *
 *  引脚（iDevice 官方定义，见原厂 MicroPython 驱动 EPD213R）：
 *    ESP32-S3 :  SCK=12  MOSI=11  CS=10  DC=13  RST=14  BUSY=15
 *
 *  为什么用软件 SPI（bit-bang）？
 *    1) 这几根脚不是 S3 硬件 SPI 的默认脚，配外设反而容易踩坑
 *    2) 122x250 全屏才 4000 字节，软件 SPI 也就十几毫秒，完全够
 *    3) 避开 SPI.begin(sck, -1, ...) 在 S3 上回落到默认 MISO(GPIO13)
 *       并抢走 DC 脚的致命问题
 *
 *  烧录：Arduino IDE → 开发板选 "ESP32S3 Dev Module"
 *        按住 BOOT → 点 RST → 松开 BOOT → 上传
 *
 *  串口 115200 会打印每一步的 BUSY 状态，白屏时靠它定位。
 * ============================================================
 */

#include <Arduino.h>

// ── 引脚 ────────────────────────────────────────────────────
#define EPD_SCK   12
#define EPD_MOSI  11
#define EPD_CS    10
#define EPD_DC    13
#define EPD_RST   14
#define EPD_BUSY  15

// ── 面板几何 ────────────────────────────────────────────────
// 2.13" 面板原生是 122(宽) x 250(高) 竖屏
#define PANEL_W    122
#define PANEL_H    250
#define ROW_BYTES  ((PANEL_W + 7) / 8)     // 16 字节/行
#define BUF_LEN    (ROW_BYTES * PANEL_H)   // 4000 字节

static uint8_t bufBW[BUF_LEN];    // 黑白平面：0 = 黑，1 = 白
static uint8_t bufRED[BUF_LEN];   // 红色平面：1 = 红，0 = 不显示红
static bool    g_ok = false;      // 初始化是否成功（失败后 loop 不再刷屏）

// ── 底层 SPI（软件模拟）─────────────────────────────────────
static inline void spiByte(uint8_t b) {
  for (int i = 0; i < 8; i++) {
    digitalWrite(EPD_MOSI, (b & 0x80) ? HIGH : LOW);
    b <<= 1;
    digitalWrite(EPD_SCK, HIGH);
    digitalWrite(EPD_SCK, LOW);
  }
}

static inline void cmd(uint8_t c) {
  digitalWrite(EPD_DC, LOW);       // DC=LOW → 命令
  digitalWrite(EPD_CS, LOW);
  spiByte(c);
  digitalWrite(EPD_CS, HIGH);
}

static inline void data(uint8_t d) {
  digitalWrite(EPD_DC, HIGH);      // DC=HIGH → 数据
  digitalWrite(EPD_CS, LOW);
  spiByte(d);
  digitalWrite(EPD_CS, HIGH);
}

// ── BUSY 极性 ───────────────────────────────────────────────
// SSD1680 手册写的是「高 = 忙」，但实测这块屏复位后一直保持高、
// 收到 0x12 后反而变低 —— 说明它的 BUSY 是反相的（低 = 忙）。
// 这里在启动时自动判定，也可以用 -DBUSY_ACTIVE_HIGH 强制。
#if defined(BUSY_ACTIVE_HIGH)
static bool g_busyHigh = true;
#elif defined(BUSY_ACTIVE_LOW)
static bool g_busyHigh = false;
#else
static bool g_busyHigh = true;      // autodetectPolarity() 会覆盖
#endif

// ── BUSY 等待（带超时和日志，白屏诊断全靠它）───────────────
static bool busyIsBusy() {
  return g_busyHigh ? (digitalRead(EPD_BUSY) == HIGH)
                    : (digitalRead(EPD_BUSY) == LOW);
}

static bool waitBusy(const char *tag, uint32_t timeoutMs = 15000) {
  uint32_t t0 = millis();
  uint32_t nextLog = 5000;
  while (busyIsBusy()) {
    uint32_t el = millis() - t0;
    if (el > timeoutMs) {
      Serial.printf("[BUSY] %s TIMEOUT after %lu ms: still busy (%s)\n",
                    tag, (unsigned long)el, g_busyHigh ? "HIGH" : "LOW");
      return false;
    }
    if (el >= nextLog) {
      Serial.printf("[BUSY] %s still busy at %lu ms\n", tag, (unsigned long)el);
      nextLog += 5000;
    }
    delay(5);
  }
  Serial.printf("[BUSY] %s done in %lu ms\n", tag, (unsigned long)(millis() - t0));
  return true;
}

// ── 复位 ────────────────────────────────────────────────────
// 复位前后各采 1 秒 BUSY 电平，看屏有没有反应
static void traceBusy(const char *tag, int ms = 3000) {
  Serial.printf("[TRACE] %-12s: ", tag);
  int n = ms / 100;
  for (int i = 0; i < n; i++) {
    Serial.print(digitalRead(EPD_BUSY) == HIGH ? '1' : '0');
    delay(100);
  }
  Serial.println();
}

static void epdReset() {
  Serial.printf("[RST] before: BUSY=%d\n", digitalRead(EPD_BUSY));
  digitalWrite(EPD_RST, HIGH); delay(20);
  digitalWrite(EPD_RST, LOW);  delay(20);
  Serial.printf("[RST] while LOW: BUSY=%d\n", digitalRead(EPD_BUSY));
  digitalWrite(EPD_RST, HIGH); delay(50);
  Serial.printf("[RST] after HIGH: BUSY=%d\n", digitalRead(EPD_BUSY));
}

// ── BUSY 极性自动判定 ──────────────────────────────────────
// 复位后连续 3 秒观察：若一直保持高，说明「高」其实是空闲状态，
// 也就是这块屏的忙电平是低（与 SSD1680 手册相反）。
static void autodetectPolarity() {
#if defined(BUSY_ACTIVE_HIGH) || defined(BUSY_ACTIVE_LOW)
  Serial.printf("[POL] fixed by build flag: busy = %s\n", g_busyHigh ? "HIGH" : "LOW");
  return;
#else
  pinMode(EPD_BUSY, INPUT);
  epdReset();
  uint32_t t0 = millis();
  int hi = 0, n = 0;
  while (millis() - t0 < 3000) {
    if (digitalRead(EPD_BUSY) == HIGH) hi++;
    n++;
    delay(5);
  }
  g_busyHigh = (hi < n * 0.9);
  Serial.printf("[POL] after reset: HIGH %d/%d -> busy level = %s (auto)\n",
                hi, n, g_busyHigh ? "HIGH" : "LOW");
#endif
}

// ── 初始化（SSD1680 序列）──────────────────────────────────
static bool epdInit() {
  Serial.println("---- EPD init ----");
  Serial.printf("[PIN] BUSY at boot = %d (idle should be 0)\n", digitalRead(EPD_BUSY));

  epdReset();
  traceBusy("after reset");
  waitBusy("hw reset", 5000);

  cmd(0x12);                 // 软件复位
  traceBusy("after sw reset");
  delay(10);
  if (!waitBusy("sw reset", 30000)) return false;

  cmd(0x01);                 // 驱动输出控制：MUX = 250 - 1
  data(0xF9); data(0x00); data(0x00);

  cmd(0x11); data(0x03);     // 数据入口：X 递增，Y 递减

  cmd(0x44); data(0x00); data(0x0F);        // RAM X 起止：0 ~ 15
  cmd(0x45); data(0xF9); data(0x00);        // RAM Y 起止：249 ~ 0

  cmd(0x3C); data(0x05);     // 边框波形
  cmd(0x18); data(0x80);     // 内部温度传感器

  cmd(0x22); data(0xB1);     // 载入 OTP 波形 + 上电
  cmd(0x20);
  traceBusy("after power on");
  return waitBusy("power on", 10000);
}

// ── 写整帧并刷新 ────────────────────────────────────────────
static bool epdShow(const uint8_t *bw, const uint8_t *red,
                    const char *tag, bool fullRefresh = true) {
  Serial.printf("---- refresh: %s ----\n", tag);

  cmd(0x4E); data(0x00);                    // X 计数器 = 0
  cmd(0x4F); data(0xF9); data(0x00);        // Y 计数器 = 249

  cmd(0x24);                                // 写黑白 RAM
  for (int i = 0; i < BUF_LEN; i++) data(bw[i]);

  if (red) {                                // 写红色 RAM（三色屏才有意义）
    cmd(0x26);
    for (int i = 0; i < BUF_LEN; i++) data(red[i]);
  }

  cmd(0x22);                                // 显示更新
  data(fullRefresh ? 0xF7 : 0xFF);
  cmd(0x20);

  return waitBusy(tag, 30000);
}

// ── 图案填充 ────────────────────────────────────────────────
static void fillBW(uint8_t v) { memset(bufBW, v, BUF_LEN); }
static void fillRED(uint8_t v) { memset(bufRED, v, BUF_LEN); }

static void drawChecker() {
  for (int y = 0; y < PANEL_H; y++) {
    for (int xb = 0; xb < ROW_BYTES; xb++) {
      // 8x8 棋盘
      bufBW[y * ROW_BYTES + xb] = (((y / 8) + xb) & 1) ? 0xAA : 0x55;
    }
  }
}

static void drawBars() {
  for (int y = 0; y < PANEL_H; y++) {
    uint8_t v = ((y / 25) & 1) ? 0x00 : 0xFF;   // 每 25 行一条黑/白
    memset(&bufBW[y * ROW_BYTES], v, ROW_BYTES);
  }
}

// ── BUSY 引脚诊断：三态采样，判断引脚是否悬空 ──────────────
// 悬空引脚会跟着上下拉走：PULLUP 时全高、PULLDOWN 时全低。
// 屏真的接着的话，电平不会因为内部上下拉而翻转。
static void sampleBusy(const char *tag, uint8_t mode, int n = 50) {
  pinMode(EPD_BUSY, mode);
  delay(10);
  int hi = 0;
  for (int i = 0; i < n; i++) {
    if (digitalRead(EPD_BUSY) == HIGH) hi++;
    delay(1);
  }
  Serial.printf("[DIAG] %-16s high %2d/%d  %s\n", tag, hi, n,
                (hi == 0) ? "-> LOW" : (hi == n) ? "-> HIGH" : "-> FLOAT");
}

static void diagnoseBusy() {
  Serial.println("==== BUSY pin diagnostics (GPIO15) ====");
  sampleBusy("INPUT", INPUT);
  sampleBusy("INPUT_PULLUP", INPUT_PULLUP);
  sampleBusy("INPUT_PULLDOWN", INPUT_PULLDOWN);
  Serial.println("  PULLUP high + PULLDOWN low = pin floating (panel not connected)");
  Serial.println("  all LOW  = panel pulls low (normal idle)");
  Serial.println("  all HIGH = panel pulls high (abnormal)");
  Serial.println("================================");
  pinMode(EPD_BUSY, INPUT);
}

// ── setup ───────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("==========================================");
  Serial.println("  iDevice 2.13\" EPD bring-up test");
  Serial.println("==========================================");

  pinMode(EPD_SCK,  OUTPUT);
  pinMode(EPD_MOSI, OUTPUT);
  pinMode(EPD_CS,   OUTPUT);
  pinMode(EPD_DC,   OUTPUT);
  pinMode(EPD_RST,  OUTPUT);
  pinMode(EPD_BUSY, INPUT);

  digitalWrite(EPD_CS, HIGH);
  digitalWrite(EPD_SCK, LOW);
  digitalWrite(EPD_DC, HIGH);

  diagnoseBusy();        // 先判断 BUSY 这根线到底有没有接上
  autodetectPolarity();  // 再判定这块屏的 BUSY 极性（高忙还是低忙）

  // 第一张图给纯白清屏，避免旧残影干扰判断
  fillBW(0xFF);
  fillRED(0x00);

  if (!epdInit()) {
    Serial.println("!! init FAILED: panel not responding.");
    Serial.println("   check: FPC seated / 3V3 / BUSY wire");
    Serial.println("   (loop stopped, re-seat and press RST)");
    return;
  }
  g_ok = true;
  Serial.println(">> init OK");

  fillBW(0xFF); fillRED(0x00);
  epdShow(bufBW, bufRED, "white", true);   // 首帧用全刷，清掉上电残影
  delay(2000);

  fillBW(0x00); fillRED(0x00);
  epdShow(bufBW, bufRED, "black", false);
  delay(2000);

  drawChecker();
  fillRED(0x00);
  epdShow(bufBW, bufRED, "checker", true);
  delay(2000);

  drawBars();
  fillRED(0x00);
  epdShow(bufBW, bufRED, "bars", true);
  delay(2000);

  // 三色屏才会真的出红；黑白屏这一帧仍显示上一张
  fillBW(0xFF); fillRED(0xFF);
  epdShow(bufBW, bufRED, "red (3-colour only)", true);
  delay(3000);

  Serial.println(">> done, looping white/black/checker every 5s");
}

// ── loop：持续循环，方便观察 ────────────────────────────────
void loop() {
  if (!g_ok) {          // 初始化失败就别再刷了，省得串口刷屏
    delay(5000);
    return;
  }

  fillBW(0xFF); fillRED(0x00);
  epdShow(bufBW, bufRED, "loop white", false);
  delay(5000);

  fillBW(0x00); fillRED(0x00);
  epdShow(bufBW, bufRED, "loop black", false);
  delay(5000);

  drawChecker(); fillRED(0x00);
  epdShow(bufBW, bufRED, "loop checker", true);
  delay(5000);
}
