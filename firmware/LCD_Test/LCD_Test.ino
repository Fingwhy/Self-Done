/*
  LCD颜色测试+背光+TF卡
  使用ESP32core4.0.0进行调试烧录
  屏幕:2.4寸ST7789P3(8080 8-bit)
  主控:ESP32-S3-WROOM-1-N4R2
  LCD引脚: D0~D7=GPIO4~11,WR=12,DC=13,RST=14,CS=21,BL=37
  SD引脚(SDMMC 4-bit):CLK=17,CMD=15,D0=40,D1=41,D2=42,D3=16
  Q1=AO3401 P-MOS高边,IO37拉低点亮(低电平有效)
  端口选ESP32familyDevice
  USB CDC On Boot选ENABLED
  PSRAM选QSPI
  USB Mode选USB-OTG(TinyUSB)
  记得进入下载模式，SW1为复位键，SW2为BOOT键
 */

#if !SOC_USB_OTG_SUPPORTED || ARDUINO_USB_MODE
#error "请把 Tools -> USB Mode 改成 USB-OTG (TinyUSB)"
#endif

#include <USB.h>
#include <USBMSC.h>
#include <SD_MMC.h>

//LCD 引脚
#define LCD_D0 4
#define LCD_D1 5
#define LCD_D2 6
#define LCD_D3 7
#define LCD_D4 8
#define LCD_D5 9
#define LCD_D6 10
#define LCD_D7 11
#define LCD_WR 12
#define LCD_DC 13
#define LCD_RST 14
#define LCD_CS 21
#define BL_PWM 37

//SD 引脚
#define SD_CLK 17
#define SD_CMD 15
#define SD_D0  40
#define SD_D1  41
#define SD_D2  42
#define SD_D3  16

#define BACKLIGHT_ON()  digitalWrite(BL_PWM, LOW)
#define BACKLIGHT_OFF() pinMode(BL_PWM, INPUT)

const int DATA_PINS[8] = {LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_D4, LCD_D5, LCD_D6, LCD_D7};

//颜色
#define RED    0xF800
#define GREEN  0x07E0
#define BLUE   0x001F
#define WHITE  0xFFFF

//USB MSC (U盘)
USBMSC msc;

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
  uint32_t secSize = SD_MMC.sectorSize();
  if (!secSize) return 0;
  for (uint32_t x = 0; x < bufsize / secSize; x++)
  {
    uint8_t blkbuffer[secSize];
    memcpy(blkbuffer, (uint8_t *)buffer + secSize * x, secSize);
    if (!SD_MMC.writeRAW(blkbuffer, lba + x)) return 0;
  }
  return bufsize;
}

static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
  uint32_t secSize = SD_MMC.sectorSize();
  if (!secSize) return 0;
  for (uint32_t x = 0; x < bufsize / secSize; x++)
  {
    if (!SD_MMC.readRAW((uint8_t *)buffer + (x * secSize), lba + x)) return 0;
  }
  return bufsize;
}

static bool onStartStop(uint8_t power_condition, bool start, bool load_eject)
{
  return true;
}

//LCD 驱动
void setDataBus(uint8_t d)
{
  for (int i = 0; i < 8; i++)
  {
    digitalWrite(DATA_PINS[i], (d >> i) & 0x01);
  }
}

void wrPulse()
{
  digitalWrite(LCD_WR, LOW);
  delayMicroseconds(1);
  digitalWrite(LCD_WR, HIGH);
}

void writeCmd(uint8_t cmd)
{
  digitalWrite(LCD_CS, LOW);
  digitalWrite(LCD_DC, LOW);
  setDataBus(cmd);
  wrPulse();
  digitalWrite(LCD_CS, HIGH);
}

void writeData(uint8_t d)
{
  digitalWrite(LCD_CS, LOW);
  digitalWrite(LCD_DC, HIGH);
  setDataBus(d);
  wrPulse();
  digitalWrite(LCD_CS, HIGH);
}

void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
  writeCmd(0x2A);
  writeData(x0 >> 8); writeData(x0 & 0xFF);
  writeData(x1 >> 8); writeData(x1 & 0xFF);
  writeCmd(0x2B);
  writeData(y0 >> 8); writeData(y0 & 0xFF);
  writeData(y1 >> 8); writeData(y1 & 0xFF);
  writeCmd(0x2C);
}

void fillScreen(uint16_t color)
{
  setWindow(0, 0, 239, 319);
  uint8_t hi = color >> 8, lo = color & 0xFF;
  digitalWrite(LCD_CS, LOW);
  digitalWrite(LCD_DC, HIGH);
  for (uint32_t i = 0; i < 240UL * 320UL; i++)
  {
    setDataBus(hi); wrPulse();
    setDataBus(lo); wrPulse();
  }
  digitalWrite(LCD_CS, HIGH);
}

void lcdInit()
{
  pinMode(LCD_RST, OUTPUT);
  pinMode(LCD_CS, OUTPUT);
  pinMode(LCD_DC, OUTPUT);
  pinMode(LCD_WR, OUTPUT);
  for (int i = 0; i < 8; i++) pinMode(DATA_PINS[i], OUTPUT);
  pinMode(BL_PWM, OUTPUT);

  digitalWrite(LCD_CS, HIGH);
  digitalWrite(LCD_WR, HIGH);

  // 硬件复位
  digitalWrite(LCD_RST, HIGH); delay(10);
  digitalWrite(LCD_RST, LOW);  delay(10);
  digitalWrite(LCD_RST, HIGH); delay(120);

  // ST7789 初始化序列
  writeCmd(0x11); delay(120);          // SLPOUT 退出睡眠
  writeCmd(0x3A); writeData(0x55);     // COLMOD: 16bit/pixel RGB565
  writeCmd(0x36); writeData(0x00);     // MADCTL: 竖屏 RGB
  writeCmd(0xB2); writeData(0x0C); writeData(0x0C); writeData(0x00); writeData(0x33); writeData(0x33);
  writeCmd(0xB7); writeData(0x35);     // GCTRL
  writeCmd(0xBB); writeData(0x19);     // VCOMS
  writeCmd(0xC0); writeData(0x2C);     // LCMCTRL
  writeCmd(0xC2); writeData(0x01);     // VDVVRHEN
  writeCmd(0xC3); writeData(0x12);     // VRHS
  writeCmd(0xC4); writeData(0x20);     // VDVS
  writeCmd(0xC6); writeData(0x0F);     // FRCTRL2
  writeCmd(0xD0); writeData(0xA4); writeData(0xA1);
  writeCmd(0xE0); writeData(0xD0); writeData(0x04); writeData(0x0D); writeData(0x11); writeData(0x13); writeData(0x2B); writeData(0x3F); writeData(0x54); writeData(0x4C); writeData(0x18); writeData(0x0D); writeData(0x0B); writeData(0x1F); writeData(0x23);
  writeCmd(0xE1); writeData(0xD0); writeData(0x04); writeData(0x0C); writeData(0x11); writeData(0x13); writeData(0x2C); writeData(0x3F); writeData(0x44); writeData(0x51); writeData(0x2F); writeData(0x1F); writeData(0x1F); writeData(0x20); writeData(0x23);
  writeCmd(0x21);                      // INVON 反转
  writeCmd(0x29);                      // DISPON 显示开

  Serial.println("LCD init done");
}

void setup()
{
  Serial.begin(115200);
  delay(500);

  //LCD 初始化 + 背光点亮
  lcdInit();
  digitalWrite(BL_PWM, LOW);
  Serial.println("Backlight ON");

  //TF卡挂载
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
  if (!SD_MMC.begin("/sdcard", false))
  {   // false = 4-bit 模式
    Serial.println("SD 挂载失败 -> U盘功能不可用");
  } 
  else
  {
    Serial.printf("SD 卡: %llu MB\n", (uint64_t)(SD_MMC.totalBytes() / 1024 / 1024));

    //把TF卡暴露成U盘
    msc.vendorID("ESP32-S3");
    msc.productID("LCD-Drive");
    msc.productRevision("1.0");
    msc.onRead(onRead);
    msc.onWrite(onWrite);
    msc.onStartStop(onStartStop);
    msc.mediaPresent(true);
    msc.begin(SD_MMC.numSectors(), SD_MMC.sectorSize());
    USB.begin();
    Serial.println("U盘模式已启动: 电脑应识别到一个可移动磁盘");
  }
}

void loop()
{
  fillScreen(RED);   delay(1500);
  fillScreen(GREEN); delay(1500);
  fillScreen(BLUE);  delay(1500);
  fillScreen(WHITE); delay(1500);
}
