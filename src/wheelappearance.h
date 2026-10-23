#pragma once

#include <algorithm>
#include <string>

namespace WheelAppearance {

struct Rgb {
    double red;
    double green;
    double blue;
};

constexpr Rgb DefaultColor{185 / 255.0, 156 / 255.0, 222 / 255.0};

inline bool parseColor(const std::string &hex, Rgb *color)
{
    if (hex.size() != 7 || hex.front() != '#' || !color) return false;
    unsigned channels[3]{};
    for (size_t index = 1; index < hex.size(); ++index) {
        const char digit = hex[index];
        const int value = digit >= '0' && digit <= '9' ? digit - '0'
            : digit >= 'a' && digit <= 'f' ? digit - 'a' + 10
            : digit >= 'A' && digit <= 'F' ? digit - 'A' + 10 : -1;
        if (value < 0) return false;
        auto &channel = channels[(index - 1) / 2];
        channel = channel * 16 + unsigned(value);
    }
    *color = {channels[0] / 255.0, channels[1] / 255.0, channels[2] / 255.0};
    return true;
}

inline Rgb tint(Rgb reference, Rgb selected)
{
    // Infer how much lilac the existing shade contains. Replacing that tint
    // keeps the default palette exact, preserves pale reflections, and makes
    // the new hue stronger in selected petals and outlines.
    const double range = std::max({reference.red, reference.green, reference.blue})
        - std::min({reference.red, reference.green, reference.blue});
    const double amount = std::clamp(range / (DefaultColor.blue - DefaultColor.green), 0.0, 1.0);
    return {
        std::clamp(reference.red + amount * (selected.red - DefaultColor.red), 0.0, 1.0),
        std::clamp(reference.green + amount * (selected.green - DefaultColor.green), 0.0, 1.0),
        std::clamp(reference.blue + amount * (selected.blue - DefaultColor.blue), 0.0, 1.0)
    };
}

inline double colorLabelHalo(Rgb selected)
{
    const auto surface = tint({0.94, 0.90, 0.99}, selected);
    return std::min({surface.red, surface.green, surface.blue}) < 0.80 ? 0.90 : 0;
}

constexpr int sizeExtent(int percent)
{
    return (460 * std::clamp(percent, 70, 130) + 50) / 100;
}

// Preserve the glass palette at the default 35% transparency. Only the
// surface changes; input geometry, text and the fruit keep their opacity.
inline double fillAlpha(double referenceAlpha, int transparency)
{
    const int percent = std::clamp(transparency, 0, 80);
    if (percent <= 35)
        return referenceAlpha + (1 - referenceAlpha) * (35 - percent) / 35.0;
    return referenceAlpha * (100 - percent) / 65.0;
}

inline double decorationAlpha(double referenceAlpha, int transparency)
{
    return referenceAlpha * std::min(1.0, (100 - std::clamp(transparency, 0, 80)) / 65.0);
}

inline double labelHaloAlpha(int transparency)
{
    return 0.95 * std::clamp((transparency - 35) / 30.0, 0.0, 1.0);
}

} // namespace WheelAppearance
