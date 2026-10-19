// Damped spring, parameterised the way Apple's SwiftUI/UIKit springs are:
//   response  - roughly the period of the oscillation in seconds (how "fast")
//   damping   - damping fraction: 1.0 = critically damped (no overshoot),
//               < 1 overshoots a little and settles (the "bouncy" Apple feel)
// Springs keep their velocity when the target changes mid-flight, which is
// what makes interrupted animations feel continuous instead of jerky.
#pragma once
#include <cmath>

struct Spring {
    double x = 0, v = 0, target = 0;
    double response = 0.35, damping = 0.9;
    double eps = 0.001;  // settle threshold in the spring's own units

    Spring() = default;
    Spring(double r, double d, double e) : response(r), damping(d), eps(e) {}

    void snap(double val) { x = target = val; v = 0; }
    void params(double r, double d) { response = r; damping = d; }
    bool settled() const { return std::fabs(x - target) < eps && std::fabs(v) < eps * 8; }

    // Advances the spring; returns true while it is still moving.
    bool step(double dt) {
        if (settled()) { x = target; v = 0; return false; }
        const double w = 2.0 * 3.14159265358979323846 / response;
        const double k = w * w;
        const double c = 2.0 * damping * w;
        int n = (int)std::ceil(dt / 0.002);
        if (n < 1) n = 1;
        if (n > 100) n = 100;
        const double h = dt / n;
        for (int i = 0; i < n; i++) {  // semi-implicit Euler, 2 ms substeps
            const double a = -k * (x - target) - c * v;
            v += a * h;
            x += v * h;
        }
        if (settled()) { x = target; v = 0; return false; }
        return true;
    }
};
