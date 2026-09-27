/*
 * TF 卡(SDMMC 4-bit)读取测试
 * 主控: ESP32-S3-WROOM-1-N4R2
 *
 * SDMMC 4-bit 引脚(依据 V2.2 原理图, 已对照 WROOM-1 官方引脚表核实):
 *   SD_CLK = GPIO17 (经 R8 22Ω)
 *   SD_CMD = GPIO15
 *   SD_D0  = GPIO40
 *   SD_D1  = GPIO41
 *   SD_D2  = GPIO42
 *   SD_D3  = GPIO16
 *   SD_CD  = GPIO38 (卡检测, 有 R11 10k 上拉到 3.3V)
 *
 * 注意: ESP32-S3 的 SDMMC 主机走 GPIO 矩阵, 可任意映射引脚,
 *       所以这里必须用 setPins() 显式指定上面的自定义引脚。
 *
 * 使用前准备:
 *   1. 把 TF 卡格式化成 FAT32(不要用 exFAT/NTFS)
 *   2. 卡里可放一个 /test.txt 文本文件用于读取验证
 */

#include "SD_MMC.h"
#include "FS.h"

#define SD_CLK 17
#define SD_CMD 15
#define SD_D0  40
#define SD_D1  41
#define SD_D2  42
#define SD_D3  16
#define SD_CD  38

// 递归列出目录
void listDir(fs::FS &fs, const char *dirname, uint8_t levels) {
  Serial.printf("目录: %s\n", dirname);
  File root = fs.open(dirname);
  if (!root) {
    Serial.println("  - 打开目录失败");
    return;
  }
  if (!root.isDirectory()) {
    Serial.println("  - 不是目录");
    return;
  }
  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      Serial.print("  [DIR ] ");
      Serial.println(file.name());
      if (levels) listDir(fs, file.path(), levels - 1);
    } else {
      Serial.print("  [FILE] ");
      Serial.print(file.name());
      Serial.print("  (");
      Serial.print(file.size());
      Serial.println(" 字节)");
    }
    file = root.openNextFile();
  }
}

// 读取并打印文本文件内容
void readFile(fs::FS &fs, const char *path) {
  Serial.printf("读取文件: %s\n", path);
  File file = fs.open(path);
  if (!file) {
    Serial.println("  - 打开文件失败");
    return;
  }
  Serial.println("  --- 内容开始 ---");
  while (file.available()) {
    Serial.write(file.read());
  }
  file.close();
  Serial.println("\n  --- 内容结束 ---");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n========== TF 卡读取测试 ==========");

  // 卡检测(可选): R11 10k 上拉, 卡插入时 CD 脚被拉到 GND
  pinMode(SD_CD, INPUT);
  if (digitalRead(SD_CD) == HIGH) {
    Serial.println("[提示] 未检测到 TF 卡插入 (SD_CD=HIGH)");
  } else {
    Serial.println("[提示] 检测到 TF 卡插入 (SD_CD=LOW)");
  }

  // 1) 指定 SDMMC 4-bit 引脚(必须在 begin 之前调用)
  if (!SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3)) {
    Serial.println("setPins 失败");
    return;
  }
  Serial.println("引脚配置: OK (CLK=17 CMD=15 D0=40 D1=41 D2=42 D3=16)");

  // 2) 挂载 SD 卡(4-bit 模式, 失败不格式化)
  if (!SD_MMC.begin("/sdcard", false, false)) {
    Serial.println("挂载失败! 请检查:");
    Serial.println("  1) TF 卡是否插入且接触良好");
    Serial.println("  2) 卡是否 FAT32 格式(不是 exFAT/NTFS)");
    Serial.println("  3) SD 卡座焊接 / 3.3V 供电 / 上拉电阻(R5~R11)");
    return;
  }
  Serial.println("挂载成功!");

  // 3) 卡类型
  uint8_t cardType = SD_MMC.cardType();
  Serial.print("SD 卡类型: ");
  if (cardType == CARD_NONE)     Serial.println("无");
  else if (cardType == CARD_MMC) Serial.println("MMC");
  else if (cardType == CARD_SD)  Serial.println("SDSC");
  else if (cardType == CARD_SDHC) Serial.println("SDHC");
  else                           Serial.println("UNKNOWN");

  // 4) 容量
  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  Serial.printf("SD 卡容量: %llu MB\n", cardSize);
  Serial.printf("总空间: %llu MB, 已用: %llu MB\n",
                (uint64_t)(SD_MMC.totalBytes() / (1024 * 1024)),
                (uint64_t)(SD_MMC.usedBytes() / (1024 * 1024)));

  // 5) 列出根目录
  Serial.println("\n--- 根目录文件列表 ---");
  listDir(SD_MMC, "/", 1);

  // 6) 读取 test.txt(如果存在)
  Serial.println("\n--- 读取 /test.txt ---");
  if (SD_MMC.exists("/test.txt")) {
    readFile(SD_MMC, "/test.txt");
  } else {
    Serial.println("/test.txt 不存在(可在TF卡根目录放一个 test.txt 来测读取)");
  }

  Serial.println("\n========== 测试结束 ==========");
}

void loop() {
  delay(10000);
}
