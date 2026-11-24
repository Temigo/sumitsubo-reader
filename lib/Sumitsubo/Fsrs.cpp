#include "Fsrs.h"

#include <algorithm>
#include <cmath>

namespace sumitsubo::fsrs {
namespace {
constexpr double W[21] = {0.212,  1.2931, 2.3065, 8.2956, 6.4133, 0.8334, 3.0194, 0.001,  1.8722, 0.1666, 0.796,
                          1.4835, 0.0614, 0.2629, 1.6483, 0.6014, 1.8729, 0.5425, 0.0912, 0.0658, 0.1542};
constexpr double STABILITY_MIN = 0.001;
constexpr double RETENTION = 0.9;
constexpr int32_t MAX_INTERVAL = 36500;
const double DECAY = -W[20];
const double FACTOR = std::pow(0.9, 1.0 / DECAY) - 1.0;

double clampD(double d) { return std::min(std::max(d, 1.0), 10.0); }
double clampS(double s) { return std::max(s, STABILITY_MIN); }
double initialDifficulty(int r, bool clamp) {
  const double d = W[4] - std::exp(W[5] * (r - 1)) + 1.0;
  return clamp ? clampD(d) : d;
}
double nextDifficulty(double d, int r) {
  const double delta = -(W[6] * (r - 3));
  const double damped = d + (10.0 - d) * delta / 9.0;
  return clampD(W[7] * initialDifficulty(4, false) + (1.0 - W[7]) * damped);
}
double shortTerm(double s, int r) {
  double inc = std::exp(W[17] * (r - 3 + W[18])) * std::pow(s, -W[19]);
  if (r >= 2) inc = std::max(inc, 1.0);
  return clampS(s * inc);
}
double retrievability(double s, int32_t elapsed) { return std::pow(1.0 + FACTOR * std::max<int32_t>(0, elapsed) / s, DECAY); }
double nextStability(double d, double s, double ret, int r) {
  if (r == 1) {
    const double longTerm = W[11] * std::pow(d, -W[12]) * (std::pow(s + 1.0, W[13]) - 1.0) * std::exp((1.0 - ret) * W[14]);
    return clampS(std::min(longTerm, s / std::exp(W[17] * W[18])));
  }
  const double hard = r == 2 ? W[15] : 1.0, easy = r == 4 ? W[16] : 1.0;
  return clampS(s * (1.0 + std::exp(W[8]) * (11.0 - d) * std::pow(s, -W[9]) * (std::exp((1.0 - ret) * W[10]) - 1.0) * hard * easy));
}
}  // namespace

int32_t interval(const float stability) {
  const double days = (stability / FACTOR) * (std::pow(RETENTION, 1.0 / DECAY) - 1.0);
  // Python's round() is to-even at .5; nearbyint under the default rounding mode is the same.
  return std::min<int32_t>(std::max<int32_t>(static_cast<int32_t>(std::nearbyint(days)), 1), MAX_INTERVAL);
}

float recall(const Memory& m, const int32_t today) {
  return m.isNew() ? 0.0f : static_cast<float>(retrievability(m.stability, today - m.lastDay));
}

Memory review(const Memory& before, const Rating rating, const int32_t today) {
  const int r = static_cast<int>(rating);
  Memory m = before;
  double s, d;
  if (before.isNew()) {
    s = clampS(W[r - 1]);
    d = initialDifficulty(r, true);
  } else {
    const int32_t elapsed = today - before.lastDay;
    s = elapsed < 1 ? shortTerm(before.stability, r)
                    : nextStability(before.difficulty, before.stability, retrievability(before.stability, elapsed), r);
    d = nextDifficulty(before.difficulty, r);
  }
  m.stability = static_cast<float>(s);
  m.difficulty = static_cast<float>(d);
  m.lastDay = today;
  m.dueDay = today + interval(m.stability);
  return m;
}
}  // namespace sumitsubo::fsrs
