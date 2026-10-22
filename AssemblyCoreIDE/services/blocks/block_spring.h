#pragma once
// A damped spring: the physics behind the "jelly" effects of the canvas (a block popping out of its socket, the row that springs open when a block is
// dropped into a hole). No Qt: the canvas steps it from a timer, tests step it by hand. Under-damped on purpose -- it overshoots and wobbles before it settles,
// which is what makes a thing feel soft and bouncy; a fixed easing curve can only slide.
#include <algorithm>
#include <cmath>

namespace acide {
namespace blocks {

struct Spring {
    double x = 1, v = 0, target = 1;
    double stiffness = 380;                 // how hard it pulls back
    double damping = 13;                    // how fast the wobble dies (critical damping is 2*sqrt(stiffness) = ~39: well below that it rings)

    void kick(double from, double to = 1) { x = from; v = 0; target = to; }
    // Advances by dt seconds (in small steps so a long frame cannot blow it up).
    void step(double dt) {
        dt = std::min(dt, 0.1);
        const int n = std::max(1, (int)std::ceil(dt / (1.0 / 240.0)));
        const double h = dt / n;
        for (int i = 0; i < n; i++) {
            const double a = -stiffness * (x - target) - damping * v;
            v += a * h;                                        // semi-implicit Euler: stable for springs
            x += v * h;
        }
    }
    bool settled(double eps = 0.002) const { return std::fabs(x - target) < eps && std::fabs(v) < eps * 4; }
    void snap() { x = target; v = 0; }
};

}  // namespace blocks
}  // namespace acide
