#include "diag_mem.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <cstdlib>

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
    out += "],\"dropped\":" + String(walk.dropped) + "}";
    free(tasks);
    free(list);
    return out;
}

}  // namespace vat::diag
