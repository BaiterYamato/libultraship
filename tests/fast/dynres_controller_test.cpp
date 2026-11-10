#include "fast/dynres_controller.h"
#include <cassert>
#include <cmath>
#include <deque>
#include <iostream>
#include <limits>
#include <random>

using Fast::DynresController;
struct Sample { double ms; float scale; };
static void Feed(DynresController& c, double ms) { c.Tick(); c.Observe(ms, c.Scale()); }

int main() {
    DynresController c;
    std::deque<Sample> delayed;
    unsigned changes = 0;
    for (unsigned frame = 0; frame < 1800; ++frame) {
        const float before = c.Scale();
        delayed.push_back({ 3.0 + 27.0 * before * before, before });
        c.Tick();
        if (delayed.size() > 4) {
            const auto sample = delayed.front(); delayed.pop_front();
            if (c.Observe(sample.ms, sample.scale)) ++changes;
        }
        assert(c.Scale() >= 0.5f && c.Scale() <= 1.0f);
    }
    const double converged = 3.0 + 27.0 * c.Scale() * c.Scale();
    assert(converged >= 13.5 && converged <= 16.6);
    assert(changes < 12);
    std::cout << "convergencia atrasada: scale=" << c.Scale() << " GPU=" << converged << " ms changes=" << changes << '\n';

    // Noise around a settled budget cannot create a limit cycle.
    const float settled = c.Scale();
    std::mt19937 rng(69);
    std::uniform_real_distribution<double> noise(-0.35, 0.35);
    for (int i = 0; i < 3000; ++i) Feed(c, converged + noise(rng));
    assert(c.Scale() == settled);
    std::cout << "ruido: 3000 amostras sem oscilacao\n";

    c.Reset(0.75f);
    for (int i = 0; i < 100; ++i) Feed(c, 15.6);
    Feed(c, 120.0); // one outlier is rejected by the median
    for (int i = 0; i < 100; ++i) Feed(c, 15.6);
    assert(c.Scale() == 0.75f);
    for (int i = 0; i < 20; ++i) Feed(c, 35.0);
    assert(c.Scale() < 0.75f);
    std::cout << "pico isolado filtrado; sobrecarga sustentada reduz escala\n";

    c.Reset();
    for (int i = 0; i < 1500; ++i) Feed(c, 200.0);
    assert(c.Scale() == 0.5f);
    for (int i = 0; i < 2500; ++i) Feed(c, 2.0);
    assert(c.Scale() == 1.0f);
    for (unsigned dimension : {1u, 7u, 17u, 240u, 1080u, 1920u}) {
        c.Reset(0.5f);
        assert(c.ScaledDimension(dimension) >= (dimension + 1) / 2);
        assert(c.ScaledDimension(dimension) <= dimension);
        c.Reset();
        assert(c.ScaledDimension(dimension) == dimension);
    }
    assert(c.ScaledDimension(0) == 1);
    c.Reset(0.5f); c.SetBudget(33.3f);
    for (int i = 0; i < 2000; ++i) Feed(c, 3.0 + 27.0 * c.Scale() * c.Scale());
    assert(c.Scale() >= 0.9375f);
    std::cout << "limites e orcamento configuravel: OK\n";

    c.Reset(0.75f);
    for (int i = 0; i < 100; ++i) { c.Tick(); assert(!c.Observe(50.0, 1.0f)); }
    assert(c.Scale() == 0.75f);
    for (double invalid : { 0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
        assert(!c.Observe(invalid, c.Scale()));
    }
    c.SetBudget(std::numeric_limits<float>::quiet_NaN());
    c.Reset(std::numeric_limits<float>::infinity());
    assert(c.Scale() == 1.0f);
    std::cout << "amostras antigas e invalidas: OK\n";
}
