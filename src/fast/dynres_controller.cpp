#include "fast/dynres_controller.h"
#include <algorithm>
#include <cmath>

namespace Fast {
void DynresController::Reset(float scale) {
    if (!std::isfinite(scale))
        scale = MaxScale;
    mScale = std::clamp(std::round(scale / Step) * Step, MinScale, MaxScale);
    mSamples = mCooldown = mOver = mUnder = 0;
    mFiltered = 0.0;
}

void DynresController::SetBudget(float milliseconds) {
    if (!std::isfinite(milliseconds) || milliseconds < 1.0f || milliseconds > 1000.0f)
        milliseconds = 16.6f;
    if (milliseconds != mBudget) {
        const float scale = mScale;
        Reset(scale);
        mBudget = milliseconds;
    }
}

void DynresController::Tick() {
    if (mCooldown)
        --mCooldown;
}

uint32_t DynresController::ScaledDimension(uint32_t output) const {
    if (output == 0)
        return 1;
    if (mScale == MaxScale)
        return output;
    const auto minimum = static_cast<uint32_t>(std::ceil(double(output) * MinScale));
    const auto aligned = static_cast<uint32_t>(std::round(double(output) * mScale / 8.0) * 8.0);
    return std::clamp(aligned, minimum, output);
}

bool DynresController::Observe(double ms, float measuredScale) {
    if (!std::isfinite(ms) || ms <= 0.0 || ms > 2000.0 || !std::isfinite(measuredScale) ||
        std::abs(measuredScale - mScale) > 0.001f)
        return false;
    mRecent[mSamples % 3] = ms;
    ++mSamples;
    if (mSamples < 3)
        return false;
    double sorted[3] = { mRecent[0], mRecent[1], mRecent[2] };
    std::sort(sorted, sorted + 3);
    mFiltered = mFiltered == 0.0 ? sorted[1] : mFiltered + 0.12 * (sorted[1] - mFiltered);
    const double target = mBudget * 0.94; // 6% for jitter / unmeasured presentation work
    mOver = mFiltered > target * 1.04 ? mOver + 1 : 0;
    mUnder = mFiltered < target * 0.90 ? mUnder + 1 : 0;
    if (mCooldown || (mOver < 3 && mUnder < 36))
        return false;
    const float estimate = mScale * static_cast<float>(std::sqrt(target / mFiltered));
    float next = mOver >= 3 ? std::max(mScale - 4 * Step, std::min(mScale - Step, estimate)) : mScale + Step;
    next = std::clamp(std::round(next / Step) * Step, MinScale, MaxScale);
    if (next == mScale)
        return false;
    Reset(next);
    mCooldown = 24;
    return true;
}
} // namespace Fast
