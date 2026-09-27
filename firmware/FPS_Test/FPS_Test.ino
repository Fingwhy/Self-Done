/*
 * LCD 帧率测试 - 使用 ESP32-S3 硬件 LCD_CAM (i80 8080 并口) + DMA
 * 屏: 240x320 ST7789P3, 8080 8-bit 并口
 * 主控: ESP32-S3-WROOM-1-N4R2
 *
 * 引脚: D0~D7 = IO4~11, WR = IO12, DC = IO13, RST = IO14, CS = IO21, BL = IO37
 *
 * 这个测试用 ESP-IDF 的 esp_lcd 组件(硬件 LCD_CAM 外设 + DMA)驱动屏,
 * 相比之前的位击(bit-bang)方式, 传输由 DMA 自动完成, 能测出接近硬件极限的帧率。
 *
 * 背光: Q1=AO3401 P-MOS 高边, IO37 拉低点亮。
 */

#include <Arduino.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_st7789.h>

#define LCD_DC  13
#define LCD_WR  12
#define LCD_CS  21
#define LCD_RST 14
#define BL_PWM  37

static esp_lcd_panel_handle_t    panel_handle = NULL;
static esp_lcd_panel_io_handle_t io_handle    = NULL;
static uint16_t *framebuffer = NULL;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n===== LCD 帧率测试 (硬件 i80 8080 + DMA) =====");

  // 背光开 (P-MOS 高边, 低电平点亮)
  pinMode(BL_PWM, OUTPUT);
  digitalWrite(BL_PWM, LOW);

  // ---- 1. i80 8080 总线 ----
  esp_lcd_i80_bus_config_t bus_config = {};
  bus_config.dc_gpio_num  = (gpio_num_t)LCD_DC;
  bus_config.wr_gpio_num  = (gpio_num_t)LCD_WR;
  bus_config.clk_src      = LCD_CLK_SRC_DEFAULT;
  bus_config.data_gpio_nums[0] = (gpio_num_t)4;
  bus_config.data_gpio_nums[1] = (gpio_num_t)5;
  bus_config.data_gpio_nums[2] = (gpio_num_t)6;
  bus_config.data_gpio_nums[3] = (gpio_num_t)7;
  bus_config.data_gpio_nums[4] = (gpio_num_t)8;
  bus_config.data_gpio_nums[5] = (gpio_num_t)9;
  bus_config.data_gpio_nums[6] = (gpio_num_t)10;
  bus_config.data_gpio_nums[7] = (gpio_num_t)11;
  bus_config.bus_width           = 8;
  bus_config.max_transfer_bytes  = 240 * 320 * 2;
  bus_config.dma_burst_size      = 64;

  esp_lcd_i80_bus_handle_t i80_bus = NULL;
  esp_err_t err = esp_lcd_new_i80_bus(&bus_config, &i80_bus);
  if (err != ESP_OK) { Serial.printf("i80 总线创建失败: 0x%x\n", err); return; }

  // ---- 2. i80 面板 IO ----
  esp_lcd_panel_io_i80_config_t io_config = {};
  io_config.cs_gpio_num       = (gpio_num_t)LCD_CS;
  io_config.pclk_hz           = 10 * 1000 * 1000;   // 10MHz (可上调测极限)
  io_config.trans_queue_depth = 10;
  io_config.lcd_cmd_bits      = 8;
  io_config.lcd_param_bits    = 8;
  io_config.dc_levels.dc_idle_level  = 0;
  io_config.dc_levels.dc_cmd_level   = 0;
  io_config.dc_levels.dc_dummy_level = 0;
  io_config.dc_levels.dc_data_level  = 1;

  err = esp_lcd_new_panel_io_i80(i80_bus, &io_config, &io_handle);
  if (err != ESP_OK) { Serial.printf("i80 IO 创建失败: 0x%x\n", err); return; }

  // ---- 3. ST7789 面板 ----
  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = (gpio_num_t)LCD_RST;
  panel_config.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_config.bits_per_pixel = 16;
  panel_config.data_endian    = LCD_RGB_DATA_ENDIAN_BIG;

  err = esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle);
  if (err != ESP_OK) { Serial.printf("ST7789 面板创建失败: 0x%x\n", err); return; }

  // ---- 4. 复位 + 初始化 + 显示开 ----
  esp_lcd_panel_reset(panel_handle);
  esp_lcd_panel_init(panel_handle);
  esp_lcd_panel_disp_on_off(panel_handle, true);

  // ---- 5. 帧缓冲 (优先 PSRAM, 2MB 足够) ----
  framebuffer = (uint16_t*)heap_caps_malloc(240 * 320 * 2, MALLOC_CAP_SPIRAM);
  if (!framebuffer) framebuffer = (uint16_t*)heap_caps_malloc(240 * 320 * 2, MALLOC_CAP_INTERNAL);
  if (!framebuffer) { Serial.println("帧缓冲分配失败"); return; }

  Serial.println("初始化完成, 开始测帧率...");
}

void loop() {
  static uint32_t round = 0;
  const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000};
  uint16_t color = colors[round % 5];

  // 填充一帧纯色
  for (uint32_t i = 0; i < 240UL * 320UL; i++) framebuffer[i] = color;

  // 连续刷 N 帧, 测纯 DMA 传输帧率
  const uint32_t N = 100;
  uint32_t start = micros();
  for (uint32_t i = 0; i < N; i++) {
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, 239, 319, framebuffer);
  }
  // 同步: 等待所有排队的传输完成
  esp_lcd_panel_io_tx_param(io_handle, -1, NULL, 0);
  uint32_t elapsed = micros() - start;

  float fps = (float)N * 1000000.0f / (float)elapsed;
  Serial.printf("[第%u轮] 颜色=0x%04X  刷%u帧耗时%u us  帧率=%.1f FPS\n",
                round, color, N, elapsed, fps);

  round++;
  delay(800);
}
