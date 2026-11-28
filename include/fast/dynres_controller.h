#pragma once
#include <cstdint>

namespace Fast {
// Pure CPU policy. Time samples must describe the scale actually measured, not
// the scale requested today: timestamp queries arrive several frames late.
class DynresController {
  public:
    void Reset(float scale = 1.0f);
    void SetBudget(float milliseconds);
    void Tick();
    bool Observe(double gpuMilliseconds, float measuredScale);
    float Scale() const {
        return mScale;
    }
    double FilteredMs() const {
        return mFiltered;
    }
    uint32_t ScaledDimension(uint32_t output) const;
    static constexpr float MinScale = 0.5f;
    static constexpr float MaxScale = 1.0f;
    static constexpr float Step = 1.0f / 32.0f;

  private:
    float mScale = 1.0f;
    float mBudget = 16.6f;
    double mRecent[3]{};
    double mFiltered = 0.0;
    unsigned mSamples = 0, mCooldown = 0, mOver = 0, mUnder = 0;
};
} // namespace Fast
