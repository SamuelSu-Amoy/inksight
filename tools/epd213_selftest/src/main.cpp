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

// ── BUSY 等待（带超时和日志，白屏诊断全靠它）───────────────
// SSD1680：BUSY 高 = 忙
static bool waitBusy(const char *tag, uint32_t timeoutMs = 15000) {
  uint32_t t0 = millis();
  while (digitalRead(EPD_BUSY) == HIGH) {
    if (millis() - t0 > timeoutMs) {
      Serial.printf("[BUSY] %s 超时！BUSY 一直为高\n", tag);
      return false;
    }
    delay(5);
  }
  Serial.printf("[BUSY] %s 完成，用时 %lu ms\n", tag, (unsigned long)(millis() - t0));
  return true;
}

// ── 复位 ────────────────────────────────────────────────────
static void epdReset() {
  digitalWrite(EPD_RST, HIGH); delay(20);
  digitalWrite(EPD_RST, LOW);  delay(20);
  digitalWrite(EPD_RST, HIGH); delay(50);
}

// ── 初始化（SSD1680 序列）──────────────────────────────────
static bool epdInit() {
  Serial.println("---- EPD init ----");
  Serial.printf("[PIN] BUSY 初始电平 = %d (空闲应为 0)\n", digitalRead(EPD_BUSY));

  epdReset();
  waitBusy("hw reset", 5000);

  cmd(0x12);                 // 软件复位
  delay(10);
  if (!waitBusy("sw reset", 5000)) return false;

  cmd(0x01);                 // 驱动输出控制：MUX = 250 - 1
  data(0xF9); data(0x00); data(0x00);

  cmd(0x11); data(0x03);     // 数据入口：X 递增，Y 递减

  cmd(0x44); data(0x00); data(0x0F);        // RAM X 起止：0 ~ 15
  cmd(0x45); data(0xF9); data(0x00);        // RAM Y 起止：249 ~ 0

  cmd(0x3C); data(0x05);     // 边框波形
  cmd(0x18); data(0x80);     // 内部温度传感器

  cmd(0x22); data(0xB1);     // 载入 OTP 波形 + 上电
  cmd(0x20);
  return waitBusy("power on", 10000);
}

// ── 写整帧并刷新 ────────────────────────────────────────────
static bool epdShow(const uint8_t *bw, const uint8_t *red,
                    const char *tag, bool fullRefresh = true) {
  Serial.printf("---- 刷新：%s ----\n", tag);

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

// ── setup ───────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("==========================================");
  Serial.println("  iDevice 2.13\" EPD 点屏测试");
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

  // 第一张图给纯白清屏，避免旧残影干扰判断
  fillBW(0xFF);
  fillRED(0x00);

  if (!epdInit()) {
    Serial.println("!! 初始化失败，屏没有响应。");
    Serial.println("   检查：FPC 插紧 / 3V3 供电 / BUSY 那根线");
    Serial.println("   （loop 已停止刷新，重新插好按 RST 即可）");
    return;
  }
  g_ok = true;
  Serial.println(">> 初始化成功");

  fillBW(0xFF); fillRED(0x00);
  epdShow(bufBW, bufRED, "全白", true);   // 首帧用全刷，清掉上电残影
  delay(2000);

  fillBW(0x00); fillRED(0x00);
  epdShow(bufBW, bufRED, "全黑", false);
  delay(2000);

  drawChecker();
  fillRED(0x00);
  epdShow(bufBW, bufRED, "8x8 棋盘", true);
  delay(2000);

  drawBars();
  fillRED(0x00);
  epdShow(bufBW, bufRED, "横向条纹", true);
  delay(2000);

  // 三色屏才会真的出红；黑白屏这一帧仍显示上一张
  fillBW(0xFF); fillRED(0xFF);
  epdShow(bufBW, bufRED, "全红（仅三色屏可见）", true);
  delay(3000);

  Serial.println(">> 测试结束，进入循环：白 → 黑 → 棋盘（每 5 秒）");
}

// ── loop：持续循环，方便观察 ────────────────────────────────
void loop() {
  if (!g_ok) {          // 初始化失败就别再刷了，省得串口刷屏
    delay(5000);
    return;
  }

  fillBW(0xFF); fillRED(0x00);
  epdShow(bufBW, bufRED, "loop 全白", false);
  delay(5000);

  fillBW(0x00); fillRED(0x00);
  epdShow(bufBW, bufRED, "loop 全黑", false);
  delay(5000);

  drawChecker(); fillRED(0x00);
  epdShow(bufBW, bufRED, "loop 棋盘", true);
  delay(5000);
}
