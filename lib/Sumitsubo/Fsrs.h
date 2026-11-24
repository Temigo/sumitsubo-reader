#pragma once
// FSRS-6 with the default parameters, scheduled by whole days and without learning steps.
// A port of py-fsrs 6.3 (MIT), which the Sumitsubo desktop program uses, so that the two agree:
// the desktop replays the device's review log with Scheduler(learning_steps=(), relearning_steps=()).
#include <cstdint>

namespace sumitsubo {

enum class Rating : uint8_t { Again = 1, Hard = 2, Good = 3, Easy = 4 };

struct Memory {
  float stability = 0;   // days; 0 = never reviewed
  float difficulty = 0;  // 1..10
  int32_t lastDay = 0;   // day of the last review
  int32_t dueDay = 0;
  bool isNew() const { return stability <= 0; }
};

namespace fsrs {
// The state after one review on `today`.
Memory review(const Memory& before, Rating rating, int32_t today);
// Chance of recalling the item on `today`, 0..1 (0 for a new item).
float recall(const Memory& m, int32_t today);
// Days until the next review for a given stability, at 90 % desired retention.
int32_t interval(float stability);
}  // namespace fsrs
}  // namespace sumitsubo
