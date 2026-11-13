// grove: what is plugged into the Grove port (G2 SDA / G1 SCL, Ex_I2C).
// Only I2C devices answer a scan; UART or analog units cannot be detected.
//   read sensor://grove        scan + best guess of the unit by address
#include <M5Cardputer.h>

#include "../common.h"
#include "../registry.h"

namespace vat::registry {
namespace {

const char* guess(uint8_t address) {
    switch (address) {
        case 0x23: return "BH1750 光照";
        case 0x29: return "VL53L0X/VL53L1X 激光测距（ToF）";
        case 0x38: return "AHT20 温湿度（ENV IV 等）";
        case 0x3C: case 0x3D: return "SSD1306 OLED 屏";
        case 0x40: return "INA219/INA226 电流电压";
        case 0x44: case 0x45: return "SHT3x/SHT4x 温湿度（ENV III/IV）";
        case 0x48: case 0x49: return "ADS1115 模数转换 / TMP102 温度";
        case 0x51: return "BM8563 实时时钟";
        case 0x57: return "MAX30102 心率血氧";
        case 0x5A: return "MLX90614 红外测温";
        case 0x5C: return "QMP6988 气压（部分）/ DHT12";
        case 0x62: return "SCD40 二氧化碳";
        case 0x68: case 0x69: return "MPU6886/MPU6050 IMU 或 DS3231 时钟";
        case 0x70: return "QMP6988 气压（ENV III）/ PCA9548 I2C 分线器";
        case 0x76: case 0x77: return "BMP280/BME280/BMP388 气压温度";
        default: return "未知设备";
    }
}

String scan() {
    if (!M5.Ex_I2C.isEnabled()) return "ERROR: Grove 口 I2C 未初始化";
    M5.Ex_I2C.begin();
    String found;
    int count = 0;
    for (uint8_t address = 0x08; address < 0x78; ++address) {
        if (!M5.Ex_I2C.scanID(address, 100000)) continue;
        char hex[8];
        snprintf(hex, sizeof(hex), "0x%02X", address);
        found += String(++count) + ". " + hex + "  " + guess(address) + "\n";
    }
    if (!count) return "Grove 口（G2 SDA / G1 SCL）上没有 I2C 设备应答；UART、模拟量单元无法通过扫描发现";
    return "Grove 口发现 " + String(count) + " 个 I2C 设备（型号按地址推测）：\n" + found;
}

// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "Grove 口接了什么用 read sensor://grove。";

class GrovePackage : public Package {
public:
    const char* usage() const override { return kUsage; }
    const char* index() const override { return "sensor://grove"; }

    const Manifest& manifest() const override {
        static const Manifest m{"grove", "Grove 口外接模块探测（I2C 扫描，按地址推测型号）",
                                "Grove 外接 扩展口 模块 传感器 插了什么 接了什么 I2C 扫描 探测 ENV 温湿度单元", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        String target = json_str(arguments, "target");
        target.toLowerCase();
        return tool == "read" && (target == "sensor://grove" || target.startsWith("sensor://grove/"));
    }

    String execute(const String&, const cJSON*, bool) override { return scan(); }

    void resources(cJSON* list, int& remaining) override {
        add_resource(list, remaining, "sensor://grove", "grove：扫描 Grove 口接了哪些 I2C 模块");
    }
};

}  // namespace

Package* make_grove_package() { return new GrovePackage(); }

}  // namespace vat::registry
