#include "mitm/color_scheme.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace mitm {

int cubeToXterm256(int r, int g, int b) {
    r = std::clamp(r, 0, 5);
    g = std::clamp(g, 0, 5);
    b = std::clamp(b, 0, 5);
    return 16 + 36 * r + 6 * g + b;
}

std::vector<int> makeBrightnessRamp(const BaseColor& base, int levels) {
    std::vector<int> ramp;
    if (levels <= 0) return ramp;
    ramp.reserve(levels);

    auto scaleChannel = [](int c, float t) {
        if (c == 0) return 0; // preserve hue: a channel that's off stays off
        return std::max(1, static_cast<int>(std::round(c * t)));
    };

    for (int i = 0; i < levels; ++i) {
        float t = 1.0f - static_cast<float>(i) / static_cast<float>(levels);
        ramp.push_back(
            cubeToXterm256(scaleChannel(base.r, t), scaleChannel(base.g, t), scaleChannel(base.b, t)));
    }
    return ramp;
}

BaseColor parseColorName(const std::string& name) {
    if (name == "red") return {5, 0, 0};
    if (name == "green") return {0, 5, 0};
    if (name == "blue") return {0, 0, 5};
    if (name == "yellow") return {5, 5, 0};
    if (name == "cyan") return {0, 5, 5};
    if (name == "magenta" || name == "purple") return {5, 0, 5};
    if (name == "white") return {5, 5, 5};
    return {0, 5, 0}; // default: green
}

std::string colorNameForIndex(int index) {
    static const std::array<const char*, 6> kRotation = {
        "red", "blue", "yellow", "cyan", "magenta", "white",
    };
    if (index < 0) index = 0;
    return kRotation[static_cast<size_t>(index) % kRotation.size()];
}

int basicAnsiColorIndex(const BaseColor& base) {
    int idx = 0;
    if (base.r > 0) idx |= 1;
    if (base.g > 0) idx |= 2;
    if (base.b > 0) idx |= 4;
    return idx == 0 ? 7 : idx; // avoid black if base somehow has no channels set
}

} // namespace mitm
