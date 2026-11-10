#include "diag_mem.h"

#include "common.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "net.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#if CONFIG_HEAP_TRACING_STANDALONE
#include <esp_heap_trace.h>
#endif

namespace vat::diag {
namespace {

constexpr uint32_t kCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr std::size_t kMaxBlocks = 192;
constexpr std::size_t kListUsed = 512;   // used blocks at least this large are listed
constexpr std::size_t kListFree = 1024;  // and free ones at least this large
constexpr std::size_t kBuckets = 6;
constexpr std::size_t kBucketTop[kBuckets] = {64, 256, 1024, 4096, 16384, SIZE_MAX};

struct Block {
    uintptr_t addr;
    uint32_t size;
    bool used;
};

// Filled while the heap is locked: nothing in here may allocate.
struct Walk {
    Block* list;
    std::size_t count;
    std::size_t dropped;
    uint32_t used_n[kBuckets];
    uint32_t used_b[kBuckets];
    uint32_t free_n[kBuckets];
    uint32_t free_b[kBuckets];
};

bool visit(walker_heap_into_t, walker_block_info_t block, void* user) {
    Walk& walk = *static_cast<Walk*>(user);
    std::size_t i = 0;
    while (block.size > kBucketTop[i]) ++i;
    if (block.used) {
        ++walk.used_n[i];
        walk.used_b[i] += block.size;
    } else {
        ++walk.free_n[i];
        walk.free_b[i] += block.size;
    }
    if (block.size >= (block.used ? kListUsed : kListFree)) {
        if (walk.count < kMaxBlocks)
            walk.list[walk.count++] = {reinterpret_cast<uintptr_t>(block.ptr), static_cast<uint32_t>(block.size), block.used};
        else
            ++walk.dropped;
    }
    return true;
}

// The task whose stack is this heap block, if any.
const TaskStatus_t* stack_owner(const Block& block, const TaskStatus_t* tasks, UBaseType_t count) {
    for (UBaseType_t t = 0; t < count; ++t) {
        const auto base = reinterpret_cast<uintptr_t>(tasks[t].pxStackBase);
        if (base >= block.addr && base < block.addr + block.size) return &tasks[t];
    }
    return nullptr;
}

}  // namespace

String mem_json() {
    // Both buffers are taken before the walk so they show up in it.
    const UBaseType_t room = uxTaskGetNumberOfTasks() + 4;
    auto* tasks = static_cast<TaskStatus_t*>(malloc(room * sizeof(TaskStatus_t)));
    auto* list = static_cast<Block*>(malloc(kMaxBlocks * sizeof(Block)));
    if (!tasks || !list) {
        free(tasks);
        free(list);
        return String();
    }
    const UBaseType_t task_count = uxTaskGetSystemState(tasks, room, nullptr);
    Walk walk{};
    walk.list = list;
    heap_caps_walk(kCaps, visit, &walk);

    multi_heap_info_t info;
    heap_caps_get_info(&info, kCaps);
    String out;
    out.reserve(1024 + walk.count * 48 + task_count * 80);
    out += "{\"heap\":{\"total\":" + String(heap_caps_get_total_size(kCaps)) +
           ",\"free\":" + String(info.total_free_bytes) + ",\"allocated\":" + String(info.total_allocated_bytes) +
           ",\"largest\":" + String(info.largest_free_block) + ",\"min_free\":" + String(info.minimum_free_bytes) +
           ",\"used_blocks\":" + String(info.allocated_blocks) + ",\"free_blocks\":" + String(info.free_blocks) + "}";

    out += ",\"buckets\":[";
    for (std::size_t i = 0; i < kBuckets; ++i) {
        if (i) out += ',';
        out += "{\"le\":" + (kBucketTop[i] == SIZE_MAX ? String("null") : String(kBucketTop[i])) +
               ",\"used_n\":" + String(walk.used_n[i]) + ",\"used_b\":" + String(walk.used_b[i]) +
               ",\"free_n\":" + String(walk.free_n[i]) + ",\"free_b\":" + String(walk.free_b[i]) + "}";
    }

    out += "],\"tasks\":[";
    for (UBaseType_t t = 0; t < task_count; ++t) {
        uint32_t stack = 0;  // 0: static stack, not on the heap
        for (std::size_t b = 0; b < walk.count; ++b)
            if (list[b].used && stack_owner(list[b], &tasks[t], 1)) stack = list[b].size;
        if (t) out += ',';
        out += "{\"name\":\"" + String(tasks[t].pcTaskName) + "\",\"prio\":" + String(tasks[t].uxCurrentPriority) +
               ",\"core\":" + String(static_cast<int>(tasks[t].xCoreID)) + ",\"stack\":" + String(stack) +
               ",\"stack_free_min\":" + String(tasks[t].usStackHighWaterMark) + "}";
    }

    out += "],\"blocks\":[";
    for (std::size_t b = 0; b < walk.count; ++b) {
        if (b) out += ',';
        out += "{\"a\":\"" + String(list[b].addr, HEX) + "\",\"s\":" + String(list[b].size) +
               ",\"u\":" + (list[b].used ? "1" : "0");
        if (list[b].used)
            if (const TaskStatus_t* owner = stack_owner(list[b], tasks, task_count))
                out += ",\"stack_of\":\"" + String(owner->pcTaskName) + "\"";
        out += '}';
    }
    uint32_t hits, misses;
    net::reserve_stats(hits, misses);
    out += "],\"dropped\":" + String(walk.dropped) + ",\"tls_reserve\":{\"hits\":" + String(hits) +
           ",\"misses\":" + String(misses) + "}}";
    free(tasks);
    free(list);
    return out;
}

namespace {
volatile uint32_t failed_count = 0;
volatile uint32_t failed_size = 0;
volatile uint32_t failed_caps = 0;
const char* volatile failed_function = nullptr;
uint32_t failed_reported = 0;

void on_alloc_failed(size_t size, uint32_t caps, const char* function) {
    failed_size = size;
    failed_caps = caps;
    failed_function = function;
    failed_count = failed_count + 1;
}
}  // namespace

void begin() { heap_caps_register_failed_alloc_callback(on_alloc_failed); }

void report() {
    const uint32_t count = failed_count;
    if (count == failed_reported) return;
    const uint32_t missed = count - failed_reported - 1;
    failed_reported = count;
    runtime_event("alloc_failed", String(static_cast<unsigned>(failed_size)) + " B caps=" + String(failed_caps, HEX) + " in " +
                                      (failed_function ? failed_function : "?") + " largest=" +
                                      String(static_cast<unsigned>(heap_caps_get_largest_free_block(kCaps))) +
                                      (missed ? " (+" + String(missed) + " more)" : String()));
}

#if CONFIG_HEAP_TRACING_STANDALONE
namespace {
heap_trace_record_t* records_buffer = nullptr;
std::size_t records_capacity = 0;
bool tracing = false;
}  // namespace

void trace_from_boot() {
    static heap_trace_record_t boot_records[300];
    records_buffer = boot_records;
    records_capacity = 300;
    if (heap_trace_init_standalone(records_buffer, records_capacity) == ESP_OK && heap_trace_start(HEAP_TRACE_LEAKS) == ESP_OK) {
        tracing = true;
    }
}

String heap_trace(const String& action, std::size_t records) {
    if (action == "start") {
        if (tracing) return "{\"error\":\"already tracing\"}";
        records = records ? std::min<std::size_t>(records, 600) : 200;
        if (records != records_capacity) {
            if (records_capacity != 300) heap_caps_free(records_buffer);  // 300: the static boot buffer
            records_buffer = static_cast<heap_trace_record_t*>(heap_caps_malloc(records * sizeof(heap_trace_record_t), kCaps));
            records_capacity = records_buffer ? records : 0;
            if (!records_buffer) return "{\"error\":\"no memory for the records\"}";
            if (heap_trace_init_standalone(records_buffer, records_capacity) != ESP_OK) return "{\"error\":\"init failed\"}";
        }
        if (heap_trace_start(HEAP_TRACE_LEAKS) != ESP_OK) return "{\"error\":\"start failed\"}";
        tracing = true;
        return "{\"tracing\":true,\"records\":" + String(records_capacity) + ",\"record_bytes\":" +
               String(sizeof(heap_trace_record_t)) + "}";
    }
    if (action == "stop") {
        if (tracing) heap_trace_stop();
        tracing = false;
        return "{\"tracing\":false,\"count\":" + String(heap_trace_get_count()) + "}";
    }
    if (action != "dump") return "{\"error\":\"action: start, stop or dump\"}";
    if (tracing) return "{\"error\":\"stop first: the dump itself would be traced\"}";
    const std::size_t count = heap_trace_get_count();
    String out;
    out.reserve(64 + count * (24 + CONFIG_HEAP_TRACING_STACK_DEPTH * 11));
    out += "{\"count\":" + String(count) + ",\"capacity\":" + String(records_capacity) + ",\"records\":[";
    for (std::size_t i = 0; i < count; ++i) {
        heap_trace_record_t r;
        if (heap_trace_get(i, &r) != ESP_OK) break;
        if (i) out += ',';
        out += "{\"a\":\"" + String(reinterpret_cast<uintptr_t>(r.address), HEX) + "\",\"s\":" + String(r.size) + ",\"by\":[";
        for (int d = 0; d < CONFIG_HEAP_TRACING_STACK_DEPTH && r.alloced_by[d]; ++d) {
            if (d) out += ',';
            out += "\"" + String(reinterpret_cast<uintptr_t>(r.alloced_by[d]), HEX) + "\"";
        }
        out += "]}";
    }
    out += "]}";
    return out;
}
#else
String heap_trace(const String&, std::size_t) { return "{\"error\":\"this build has no heap tracing (tools/heaptrace.py)\"}"; }
void trace_from_boot() {}
#endif

}  // namespace vat::diag
