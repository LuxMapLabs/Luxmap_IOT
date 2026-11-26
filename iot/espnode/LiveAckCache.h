#pragma once
#include <stdint.h>
#include <stddef.h>

// Volatile optimization only: eviction/reboot causes replay, never data deletion.
class LiveAckCache {
 public:
  bool contains(uint64_t session, uint32_t sequence) const {
    for (const auto& item : items)
      if (item.valid && item.session == session && item.sequence == sequence) return true;
    return false;
  }
  void add(uint64_t session, uint32_t sequence) {
    if (contains(session, sequence)) return;
    items[next] = {session, sequence, true};
    next = (next + 1) % Capacity;
  }
  static constexpr size_t Capacity = 64;
 private:
  struct Item { uint64_t session = 0; uint32_t sequence = 0; bool valid = false; };
  Item items[Capacity];
  size_t next = 0;
};
