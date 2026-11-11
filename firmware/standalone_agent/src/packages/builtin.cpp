// Explicit, audited list of first-party packages.  Order matters only when
// two packages could claim the same call; keep scheme ownership disjoint.
// New capability = new file in this directory + one line here.
#include "../registry.h"

namespace vat::registry {

Package* make_memory_package();
Package* make_local_package();
Package* make_ssh_package();
Package* make_tmux_package();
Package* make_agents_package();
Package* make_policy_package();
Package* make_wifi_package();
Package* make_web_package();
Package* make_http_package();
Package* make_card_package();
Package* make_lora_package();
Package* make_gps_package();
Package* make_ble_package();
Package* make_place_package();
Package* make_display_package();
Package* make_hardware_package();
Package* make_files_package();
Package* make_timer_package();
Package* make_grove_package();
Package* make_ir_package();
Package* make_ui_package();
Package* make_program_package();
Package* make_debug_package();
Package* make_pkg_package();
Package* make_ask_package();
Package* make_lua_package();
Package* make_settings_package();
std::vector<Package*> make_catalog_packages();

void register_builtin() {
    add(make_memory_package());  // before local: owns memory://out|history|cases
    add(make_display_package());  // before local: owns device://local/display/{text,qr,image,banner,clear}
    add(make_hardware_package());  // before local: owns device://local/led and audio/beep
    add(make_local_package());
    add(make_settings_package());
    add(make_ssh_package());
    add(make_tmux_package());
    add(make_agents_package());
    add(make_policy_package());
    add(make_wifi_package());
    add(make_web_package());
    add(make_http_package());
    add(make_lora_package());
    add(make_gps_package());
    add(make_ble_package());
    add(make_place_package());
    add(make_files_package());
    add(make_timer_package());
    add(make_grove_package());
    add(make_ir_package());
    add(make_pkg_package());
    add(make_ask_package());
    add(make_lua_package());
    add(make_ui_package());
    // Programs and their run records after the other built-ins: their usage is the
    // longest and the one most worth being read last.
    add(make_program_package());
    add(make_debug_package());
    add(make_card_package());  // last: the TF card's packages never shadow a built-in target
    for (auto* catalog : make_catalog_packages()) add(catalog);
}

}  // namespace vat::registry
