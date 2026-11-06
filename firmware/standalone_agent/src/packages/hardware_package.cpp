// hardware: the Cardputer Adv's own RGB LED, speaker beep and IMU.  Battery
// stays with the local package (device://local/battery), now with charging.
//   write device://local/led          content: 红/绿/蓝…, #RRGGBB or off
//   read  device://local/led
//   write device://local/audio/beep   content: how many beeps (1-5)
//   read  sensor://imu/orientation|motion|acceleration   (any metric: full status)
#include "../common.h"
#include "../ops_hw.h"
#include "../registry.h"

namespace vat::registry {
namespace {

bool is_imu(String target) {
    target.toLowerCase();
    return target == "sensor://imu" || target.startsWith("sensor://imu/");
}

// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "本机 RGB 灯为 device://local/led，提示音为 device://local/audio/beep，姿态为 sensor://imu/orientation。";

class HardwarePackage : public Package {
public:
    const char* usage() const override { return kUsage; }
    bool reversible(const String& tool, const cJSON*) const override { return tool == "write"; }  // LED, beep

    const Manifest& manifest() const override {
        static const Manifest m{"hardware", "本机 RGB 灯、提示音、IMU 姿态（加速度/陀螺仪）",
                                "灯 RGB LED 指示灯 颜色 亮灯 关灯 红灯 绿灯 提示音 蜂鸣 响一下 哔 声音提醒 IMU 陀螺仪 加速度 "
                                "姿态 倾斜 角度 摇晃 晃动 水平 朝向 平放 移动",
                                {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        const String target = json_str(arguments, "target");
        if (tool == "write") return hw::is_led_target(target) || hw::is_beep_target(target);
        if (tool == "read") return hw::is_led_target(target) || is_imu(target);
        return false;
    }

    String execute(const String& tool, const cJSON* arguments, bool) override {
        const String target = json_str(arguments, "target");
        if (is_imu(target)) return hw::imu_text();
        if (hw::is_led_target(target)) return tool == "read" ? hw::led_text() : hw::set_led(json_str(arguments, "content"));
        const int count = json_str(arguments, "content").toInt();
        return hw::request_beep(count > 0 ? count : 1);
    }

    void hints(std::vector<Hint>& out) override {
        out.push_back({"RGB 灯", hw::led_text().substring(String("RGB 灯：").length()), "把灯调成绿色"});
        out.push_back({"姿态", "", "设备现在是平放的吗"});
    }

    void resources(cJSON* list, int& remaining) override {
        static const char* const items[][2] = {
            {"device://local/led", "hardware：本机 RGB 指示灯（本机、机身上的灯），write content 为颜色（红/绿/#RRGGBB）或 off"},
            {"device://local/audio/beep", "hardware：提示音，write content 为次数 1-5"},
            {"sensor://imu/orientation", "hardware：IMU 姿态、倾斜角、是否在动"},
        };
        for (const auto& item : items) {
            add_resource(list, remaining, item[0], item[1]);
        }
    }
};

}  // namespace

Package* make_hardware_package() { return new HardwarePackage(); }

}  // namespace vat::registry
