// Exercise the production native renderer and its appearance protocol without
// opening a Wayland connection, reserving a shortcut or inspecting user input.
#define main productionGestureMain
#include "../src/gesturehelper.cpp"
#undef main

#include <cstdint>
#include <stdexcept>
#include <vector>
#include <QTemporaryFile>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void checkHoverMessages(bool tools)
{
    QTemporaryFile capture;
    require(capture.open(), "Cannot capture private hover messages");
    const int savedOutput = dup(STDOUT_FILENO);
    require(savedOutput >= 0, "Cannot preserve test output");
    if (dup2(capture.handle(), STDOUT_FILENO) < 0) {
        close(savedOutput);
        throw std::runtime_error("Cannot redirect private hover messages");
    }
    State state;
    state.tools = tools;
    state.formats = QJsonArray{QJsonObject{}, QJsonObject{}, QJsonObject{}};
    changeHover(&state, 0); // The passive probe must be silent.
    state.validDrag = true;
    changeHover(&state, 1);
    changeHover(&state, 1); // Remaining over one option must be silent.
    changeHover(&state, -1);
    changeHover(&state, 0);
    state.busy = true;
    changeHover(&state, 1);
    state.busy = false;
    changeHover(&state, 2);
    state.dropFinished = true;
    changeHover(&state, 0);
    const int restored = dup2(savedOutput, STDOUT_FILENO);
    close(savedOutput);
    require(restored >= 0 && capture.seek(0), "Cannot restore test output");
    const auto messages = capture.readAll().trimmed().split('\n');
    require(messages.size() == 3, "Native hover repeated a sound request or sounded for an invalid interaction");
    const int sectors[]{1, 0, 2};
    for (int index = 0; index < 3; ++index) {
        const auto message = QJsonDocument::fromJson(messages[index]).object();
        require(message.value("type") == "hover" && message.value("sector").toInt(-1) == sectors[index]
                    && message.value("mode") == (tools ? "tools" : "convert"),
                "Native hover did not identify the newly entered sector and independent wheel");
    }
}

struct RenderedWheel {
    std::vector<uint32_t> pixels;

    explicit RenderedWheel(const State &state) : pixels(size_t(MaxWindowSize) * MaxWindowSize)
    {
        cairo_surface_t *surface = cairo_image_surface_create_for_data(
            reinterpret_cast<unsigned char *>(pixels.data()), CAIRO_FORMAT_ARGB32,
            MaxWindowSize, MaxWindowSize, MaxWindowSize * int(sizeof(uint32_t)));
        const auto initialStatus = cairo_surface_status(surface);
        cairo_t *cr = cairo_create(surface);
        drawWheel(&state, cr);
        const auto drawStatus = cairo_status(cr);
        cairo_destroy(cr);
        cairo_surface_flush(surface);
        const auto finalStatus = cairo_surface_status(surface);
        cairo_surface_destroy(surface);
        require(initialStatus == CAIRO_STATUS_SUCCESS && drawStatus == CAIRO_STATUS_SUCCESS
                    && finalStatus == CAIRO_STATUS_SUCCESS,
                "The production native wheel could not render into its private image surface");
    }

    uint32_t pixel(int x, int y) const { return pixels.at(size_t(y) * MaxWindowSize + x); }
    int alpha(int x, int y) const { return int(pixel(x, y) >> 24); }
};

QJsonObject appearance(const State &state, const QJsonValue &value)
{
    return {{QStringLiteral("type"), QStringLiteral("appearance")},
            {QStringLiteral("mode"), state.tools ? QStringLiteral("tools") : QStringLiteral("convert")},
            {QStringLiteral("transparency"), value}};
}

QJsonObject appearance(const State &state, const QJsonValue &transparency, const QJsonValue &size)
{
    auto message = appearance(state, transparency);
    message.insert(QStringLiteral("size"), size);
    return message;
}

QJsonObject appearance(const State &state, const QJsonValue &transparency,
                       const QJsonValue &size, const QJsonValue &color)
{
    auto message = appearance(state, transparency, size);
    message.insert(QStringLiteral("color"), color);
    return message;
}

bool sameColor(WheelAppearance::Rgb left, WheelAppearance::Rgb right)
{
    return left.red == right.red && left.green == right.green && left.blue == right.blue;
}

void checkColorTransfer()
{
    using namespace WheelAppearance;
    Rgb color{0.17, 0.29, 0.41};
    const auto original = color;
    const std::vector<std::string> invalid = {
        "", "red", "transparent", "#abc", "#12345", "#1234567", "#12345678", "#12G456",
        "123456", " #123456", "#123456 ", "#123456\n", "#１２３４５６", "#аbcdef",
        std::string("#123\0ab", 7)
    };
    for (const auto &value : invalid)
        require(!parseColor(value, &color) && sameColor(color, original),
                "The shared RGB parser accepted a malformed color or partially changed its output");
    require(!parseColor("#123456", nullptr)
                && parseColor("#B99cDE", &color) && sameColor(color, DefaultColor)
                && parseColor("#000000", &color) && sameColor(color, {0, 0, 0})
                && parseColor("#FFFFFF", &color) && sameColor(color, {1, 1, 1}),
            "The shared RGB parser lost its default, case normalization, RGB bounds or null guard");

    const std::vector<Rgb> references = {
        {1, 0.994, 0.98}, {0.95, 0.97, 1}, {0.94, 0.90, 0.99},
        {0.91, 0.84, 0.99}, {0.72, 0.59, 0.82}, {0.55, 0.35, 0.71},
        {1, 1, 1}, {0.5, 0.5, 0.5}, {0, 0, 0}
    };
    const std::vector<Rgb> selected = {
        DefaultColor, {0.1, 0.4, 1}, {1, 0.15, 0.1}, {0, 0, 0}, {1, 1, 1}
    };
    for (const auto reference : references) {
        require(sameColor(tint(reference, DefaultColor), reference),
                "The default RGB tint no longer preserves the existing palette exactly");
        for (const auto choice : selected) {
            const auto result = tint(reference, choice);
            require(result.red >= 0 && result.red <= 1 && result.green >= 0 && result.green <= 1
                        && result.blue >= 0 && result.blue <= 1,
                    "The shared RGB tint generated an unbounded channel");
        }
    }
    require(sameColor(tint({1, 1, 1}, {0, 0, 0}), {1, 1, 1})
                && !sameColor(tint({0.72, 0.59, 0.82}, {0.1, 0.4, 1}), {0.72, 0.59, 0.82})
                && colorLabelHalo(DefaultColor) == 0 && colorLabelHalo({1, 1, 1}) == 0
                && colorLabelHalo({0, 0, 0}) > 0 && colorLabelHalo({1, 0.15, 0.1}) > 0,
            "The RGB transfer tinted white reflections or lost dark-color label protection");
}

QJsonArray formatsFor(bool tools)
{
    QJsonArray formats;
    const QStringList ids = tools
        ? QStringList{QStringLiteral("tool:compress"), QStringLiteral("tool:metadata"), QStringLiteral("tool:photo")}
        : QStringList{QStringLiteral("docx"), QStringLiteral("png"), QStringLiteral("jpg"),
                      QStringLiteral("txt"), QStringLiteral("md"), QStringLiteral("html"),
                      QStringLiteral("epub"), QStringLiteral("pptx"), QStringLiteral("xlsx")};
    for (const QString &id : ids)
        formats.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("label"), id.toUpper()},
                                   {QStringLiteral("detail"), QStringLiteral("Fixture")}});
    return formats;
}

std::vector<int> hitGeometry(const State &state)
{
    std::vector<int> sectors;
    const double scale = WheelAppearance::sizeExtent(state.wheelSize) / double(WindowSize);
    for (const double radius : {0.0, InnerRadius - 1, InnerRadius + 2,
                                (InnerRadius + OuterRadius) / 2, OuterRadius - 2, OuterRadius + 1})
        for (int step = 0; step < 48; ++step) {
            const double angle = 2 * Pi * step / 48;
            sectors.push_back(sectorAt(&state, scale * (Center + radius * std::cos(angle)),
                                      scale * (Center + radius * std::sin(angle))));
        }
    return sectors;
}

void checkMode(bool tools)
{
    State state;
    state.tools = tools;
    state.formats = formatsFor(tools);
    state.paths = {QStringLiteral("/private-fixture/example.png")};
    state.token = 47;
    state.hovered = 1;
    const auto originalFormats = state.formats;
    const auto originalPaths = state.paths;

    require(state.wheelTransparency == 35 && state.wheelSize == 100 && state.requestedWheelSize == 100,
            "The native renderer's default transparency or size changed");
    const std::vector<QJsonValue> invalidValues = {
        QStringLiteral("50"), true, QJsonValue(), QJsonObject(), QJsonArray(), -1, 81, 35.5
    };
    for (const auto &value : invalidValues) {
        handleMessage(&state, appearance(state, value));
        require(state.wheelTransparency == 35 && state.wheelSize == 100 && state.requestedWheelSize == 100
                    && !state.validDrag,
                "An invalid appearance message changed transparency or armed a native drop target");
    }
    const std::vector<QJsonValue> invalidSizes = {
        QStringLiteral("100"), true, QJsonValue(), QJsonObject(), QJsonArray(), 69, 131, 100.5
    };
    for (const auto &size : invalidSizes) {
        handleMessage(&state, appearance(state, 60, size));
        require(state.wheelTransparency == 35 && state.wheelSize == 100 && state.requestedWheelSize == 100
                    && !state.validDrag,
                "An invalid native size partially applied the appearance pair or armed a drop target");
    }
    for (const auto &value : invalidValues) {
        handleMessage(&state, appearance(state, value, 120));
        require(state.wheelTransparency == 35 && state.wheelSize == 100 && state.requestedWheelSize == 100,
                "An invalid transparency partially applied a valid native size");
    }
    QJsonObject otherMode = appearance(state, 80, 130);
    otherMode.insert(QStringLiteral("mode"), tools ? QStringLiteral("convert") : QStringLiteral("tools"));
    handleMessage(&state, otherMode);
    require(state.wheelTransparency == 35 && state.wheelSize == 100 && state.requestedWheelSize == 100,
            "An appearance message intended for the other native wheel changed this destination");

    state.validDrag = true;
    const auto originalGeometry = hitGeometry(state);
    state.validDrag = false;
    std::vector<RenderedWheel> rendered;
    for (int transparency : {0, 35, 80}) {
        // Protocol updates are handled before a drag, exactly as at startup.
        // Rendering below uses only an image surface and cannot contact KWin.
        handleMessage(&state, appearance(state, transparency));
        require(state.wheelTransparency == transparency && !state.validDrag
                    && state.formats == originalFormats && state.paths == originalPaths
                    && state.token == 47 && state.hovered == 1,
                "A valid appearance update changed the native drag's choices, selection or ownership");
        state.validDrag = true;
        require(hitGeometry(state) == originalGeometry,
                "Transparency changed the native sector hit geometry");
        rendered.emplace_back(state);
        state.validDrag = false;
    }

    // These points are in plain surface areas, away from text, outlines,
    // corners and the opaque fruit. Compare actual production pixels.
    const int petalX = int(Center), petalY = 55;
    const int coreX = 180, coreY = 185;
    require(rendered[0].alpha(petalX, petalY) == 255
                && rendered[1].alpha(petalX, petalY) >= 150
                && rendered[1].alpha(petalX, petalY) <= 180
                && rendered[2].alpha(petalX, petalY) > 0
                && rendered[2].alpha(petalX, petalY) < rendered[1].alpha(petalX, petalY),
            "The native petal did not become opaque at 0% and progressively translucent at 35%/80%");
    require(rendered[0].alpha(coreX, coreY) == 255
                && rendered[2].alpha(coreX, coreY) > 0
                && rendered[2].alpha(coreX, coreY) < rendered[1].alpha(coreX, coreY)
                && rendered[1].alpha(coreX, coreY) < 255,
            "The wheel's central surface did not follow the transparency preference");
    for (const auto &wheel : rendered) {
        require(wheel.alpha(0, 0) == 0, "The native wheel painted an opaque rectangular background");
        require(wheel.alpha(210, 205) == 255
                    && wheel.pixel(210, 205) == rendered.front().pixel(210, 205),
                "Changing surface transparency faded or changed the opaque fruit");
    }

    rendered.clear();
    for (const int size : {70, 100, 130}) {
        handleMessage(&state, appearance(state, 35, size));
        require(state.wheelSize == size && state.requestedWheelSize == size && !state.validDrag
                    && state.wheelTransparency == 35 && state.formats == originalFormats
                    && state.paths == originalPaths && state.token == 47 && state.hovered == 1,
                "A native size update lost appearance, choices, selection or drag ownership");
        state.validDrag = true;
        require(hitGeometry(state) == originalGeometry,
                "Resizing changed native sectors instead of scaling their hit coordinates");
        rendered.emplace_back(state);
        const auto &wheel = rendered.back();
        const int extent = WheelAppearance::sizeExtent(size);
        const double scale = extent / double(WindowSize);
        auto scaled = [scale](int coordinate) { return int(std::lround(coordinate * scale)); };
        require(wheel.alpha(scaled(petalX), scaled(petalY)) >= 145
                    && wheel.alpha(scaled(petalX), scaled(petalY)) <= 185
                    && wheel.alpha(scaled(coreX), scaled(coreY)) > 0
                    && wheel.alpha(scaled(coreX), scaled(coreY)) < 255,
                "Resizing did not scale the petal and central surface to the requested extent");
        require(wheel.alpha(scaled(210), scaled(205)) >= 254,
                "Resizing faded or clipped the opaque fruit");
        require(wheel.alpha(0, 0) == 0 && wheel.alpha(extent - 1, extent - 1) == 0,
                "Resizing introduced a rectangular native background");
        if (extent < MaxWindowSize) {
            for (int coordinate = 0; coordinate < MaxWindowSize; ++coordinate)
                require(wheel.alpha(extent + 1, coordinate) == 0
                            && wheel.alpha(coordinate, extent + 1) == 0,
                        "The small wheel painted outside its requested window extent");
        }
        bool footerVisible = false;
        for (int y = scaled(441); y < scaled(458); ++y)
            for (int x = scaled(10); x < scaled(450); ++x)
                footerVisible = footerVisible || wheel.alpha(x, y) > 0;
        require(footerVisible, "The resized wheel clipped its cancellation hint");
        for (int coordinate = 0; coordinate < MaxWindowSize; ++coordinate)
            require(wheel.alpha(MaxWindowSize - 1, coordinate) == 0
                        && wheel.alpha(coordinate, MaxWindowSize - 1) == 0,
                    "The maximum native buffer clipped visible wheel or footer pixels");
        state.validDrag = false;
    }
    // Same absolute point: clear at the original size and in a petal of the
    // enlarged wheel. The former large footprint is clear again at 70%.
    const int enlargedX = int(std::lround((Center + OuterRadius - 25) * 1.3));
    const int enlargedY = int(std::lround((Center + 14) * 1.3));
    require(rendered[2].alpha(enlargedX, enlargedY) > 0
                && rendered[1].alpha(enlargedX, enlargedY) == 0
                && rendered[0].alpha(enlargedX, enlargedY) == 0,
            "The native size preference did not change the actual painted proportions");

    handleMessage(&state, appearance(state, 35, 100));
    state.validDrag = true;
    const auto pendingGeometry = hitGeometry(state);
    handleMessage(&state, appearance(state, 41, 130));
    require(state.wheelTransparency == 41 && state.wheelSize == 100 && state.requestedWheelSize == 130
                && hitGeometry(state) == pendingGeometry && state.formats == originalFormats
                && state.paths == originalPaths && state.token == 47 && state.hovered == 1,
            "Saving a size during a native drag moved its active sectors or changed its choices");
    for (const auto &size : invalidSizes) {
        handleMessage(&state, appearance(state, 20, size));
        require(state.wheelTransparency == 41 && state.wheelSize == 100 && state.requestedWheelSize == 130,
                "An invalid size changed a native appearance pair while a size was pending");
    }
    otherMode = appearance(state, 20, 70);
    otherMode.insert(QStringLiteral("mode"), tools ? QStringLiteral("convert") : QStringLiteral("tools"));
    handleMessage(&state, otherMode);
    require(state.wheelTransparency == 41 && state.wheelSize == 100 && state.requestedWheelSize == 130,
            "A wrong-mode appearance message replaced a pending native size");
    handleMessage(&state, appearance(state, 57));
    require(state.wheelTransparency == 57 && state.wheelSize == 100 && state.requestedWheelSize == 130
                && hitGeometry(state) == pendingGeometry,
            "A legacy transparency-only message lost the size or moved an active drag");
    applyPendingSize(&state);
    require(state.wheelSize == 100, "An active valid drag applied a pending size prematurely");
    Offer activeOffer{};
    state.validDrag = false;
    state.active = &activeOffer;
    applyPendingSize(&state);
    require(state.wheelSize == 100, "An active native offer applied a pending size prematurely");
    state.active = nullptr;
    applyPendingSize(&state);
    require(state.wheelSize == 130 && state.requestedWheelSize == 130
                && state.wheelTransparency == 57 && state.formats == originalFormats,
            "The pending native size did not apply between gestures");
    state.validDrag = true;
    require(hitGeometry(state) == originalGeometry,
            "A deferred native size changed sector identities on the next gesture");
    state.validDrag = false;
    handleMessage(&state, appearance(state, 45));
    require(state.wheelSize == 130 && state.requestedWheelSize == 130 && state.wheelTransparency == 45,
            "A legacy transparency-only update reset an already applied native size");
}

void checkColors(bool tools)
{
    State state;
    state.tools = tools;
    state.formats = formatsFor(tools);
    state.paths = {QStringLiteral("/private-fixture/example.png")};
    state.token = 71;
    state.hovered = 1;
    state.hoverLevels[1] = 1;
    const auto originalFormats = state.formats;
    const auto originalPaths = state.paths;
    require(sameColor(state.wheelColor, WheelAppearance::DefaultColor),
            "The native wheel did not preserve the existing lilac color by default");

    state.validDrag = true;
    const auto originalGeometry = hitGeometry(state);
    state.validDrag = false;
    const QStringList colors = {QStringLiteral("#B99CDE"), QStringLiteral("#2878ef"),
                                QStringLiteral("#ed3749"), QStringLiteral("#000000"),
                                QStringLiteral("#ffffff")};
    const int settings[][2] = {{35, 100}, {80, 70}, {0, 130}};
    for (const auto &setting : settings) {
        const int transparency = setting[0], size = setting[1];
        handleMessage(&state, appearance(state, transparency, size, QStringLiteral("#b99cde")));
        state.validDrag = true;
        const RenderedWheel baseline(state);
        state.validDrag = false;
        const double scale = WheelAppearance::sizeExtent(size) / double(WindowSize);
        auto scaled = [scale](int coordinate) { return int(std::lround(coordinate * scale)); };
        for (const auto &color : colors) {
            WheelAppearance::Rgb expected{};
            require(WheelAppearance::parseColor(color.toStdString(), &expected),
                    "Cannot parse a private color fixture");
            handleMessage(&state, appearance(state, transparency, size, color));
            require(sameColor(state.wheelColor, expected)
                        && state.wheelTransparency == transparency && state.wheelSize == size
                        && state.requestedWheelSize == size && !state.validDrag
                        && state.formats == originalFormats && state.paths == originalPaths
                        && state.token == 71 && state.hovered == 1 && state.hoverLevels[1] == 1,
                    "A color update changed choices, hover feedback, size or drag ownership");
            state.validDrag = true;
            require(hitGeometry(state) == originalGeometry,
                    "A color update changed a native sector's identity or hit coordinates");
            const RenderedWheel colored(state);
            require(colored.alpha(scaled(int(Center)), scaled(55))
                            == baseline.alpha(scaled(int(Center)), scaled(55))
                        && colored.alpha(scaled(180), scaled(185))
                            == baseline.alpha(scaled(180), scaled(185)),
                    "Color changed plain petal or core opacity instead of retaining transparency");
            require(colored.alpha(0, 0) == 0
                        && colored.alpha(scaled(210), scaled(205)) >= 254
                        && colored.pixel(scaled(210), scaled(205))
                            == baseline.pixel(scaled(210), scaled(205)),
                    "Color introduced a rectangular background or changed the opaque fruit");
            if (sameColor(expected, WheelAppearance::DefaultColor)) {
                require(colored.pixels == baseline.pixels,
                        "Selecting the default color did not exactly preserve the rendered wheel");
            } else {
                bool changedPetal = false;
                // This small plain patch is away from the top format label,
                // its protective halo, the petal's edges and hover outline.
                for (int y = scaled(45); y <= scaled(58); ++y)
                    for (int x = scaled(218); x <= scaled(242); ++x)
                        changedPetal = changedPetal
                            || (colored.pixel(x, y) & 0x00ffffffU)
                                != (baseline.pixel(x, y) & 0x00ffffffU);
                require(changedPetal,
                        "A supported custom color did not change the production petal's RGB pixels");
            }
            state.validDrag = false;
        }
    }

    WheelAppearance::Rgb blue{};
    require(WheelAppearance::parseColor("#30a4e8", &blue), "Cannot parse the protocol fixture color");
    handleMessage(&state, appearance(state, 55, 110, QStringLiteral("#30A4e8")));
    auto baselinePreserved = [&] {
        return state.wheelTransparency == 55 && state.wheelSize == 110 && state.requestedWheelSize == 110
            && sameColor(state.wheelColor, blue) && !state.validDrag
            && state.formats == originalFormats && state.paths == originalPaths
            && state.token == 71 && state.hovered == 1;
    };
    require(baselinePreserved(), "A valid mixed-case color message was not accepted");
    const std::vector<QJsonValue> invalidColors = {
        QJsonValue(), true, 123, QJsonObject(), QJsonArray(), QString(), QStringLiteral("red"),
        QStringLiteral("transparent"), QStringLiteral("#abc"), QStringLiteral("#12345"),
        QStringLiteral("#1234567"), QStringLiteral("#12345678"), QStringLiteral("#12G456"),
        QStringLiteral(" #123456"), QStringLiteral("#123456 "), QStringLiteral("#123456\n"),
        QStringLiteral("#１２３４５６"), QStringLiteral("#аbcdef"), QStringLiteral("#123\0ab")
    };
    for (const auto &value : invalidColors) {
        handleMessage(&state, appearance(state, 20, 75, value));
        require(baselinePreserved(),
                "An invalid color partially changed native color, transparency, size or ownership");
    }
    const std::vector<QJsonValue> invalidTransparency = {
        QStringLiteral("50"), true, QJsonValue(), QJsonObject(), QJsonArray(), -1, 81, 35.5
    };
    for (const auto &value : invalidTransparency) {
        handleMessage(&state, appearance(state, value, 75, QStringLiteral("#ff0000")));
        require(baselinePreserved(),
                "An invalid transparency partially applied a valid native size or color");
    }
    const std::vector<QJsonValue> invalidSizes = {
        QStringLiteral("100"), true, QJsonValue(), QJsonObject(), QJsonArray(), 69, 131, 100.5
    };
    for (const auto &value : invalidSizes) {
        handleMessage(&state, appearance(state, 20, value, QStringLiteral("#ff0000")));
        require(baselinePreserved(),
                "An invalid size partially applied a valid native transparency or color");
    }
    auto otherMode = appearance(state, 20, 75, QStringLiteral("#ff0000"));
    otherMode.insert(QStringLiteral("mode"), tools ? QStringLiteral("convert") : QStringLiteral("tools"));
    handleMessage(&state, otherMode);
    require(baselinePreserved(), "A color message for the other native wheel changed this destination");
    handleMessage(&state, appearance(state, 40, 90));
    handleMessage(&state, appearance(state, 35));
    require(state.wheelTransparency == 35 && state.wheelSize == 90 && state.requestedWheelSize == 90
                && sameColor(state.wheelColor, blue) && !state.validDrag,
            "A legacy appearance message reset the selected native color");

    state.validDrag = true;
    const auto activeGeometry = hitGeometry(state);
    handleMessage(&state, appearance(state, 45, 130, QStringLiteral("#000000")));
    require(state.wheelTransparency == 45 && state.wheelSize == 90 && state.requestedWheelSize == 130
                && sameColor(state.wheelColor, {0, 0, 0}) && hitGeometry(state) == activeGeometry,
            "Saving a color with a pending size moved the active native sectors");
    handleMessage(&state, appearance(state, 20, 70, QStringLiteral("#12345678")));
    require(state.wheelTransparency == 45 && state.wheelSize == 90 && state.requestedWheelSize == 130
                && sameColor(state.wheelColor, {0, 0, 0}) && hitGeometry(state) == activeGeometry,
            "An invalid color replaced live appearance or a pending size during a drag");
    state.validDrag = false;
    applyPendingSize(&state);
    require(state.wheelSize == 130 && sameColor(state.wheelColor, {0, 0, 0}),
            "Applying a deferred size discarded the native color preference");
}

} // namespace

int main()
{
    try {
        // This shared transfer function also drives the Windows renderer.
        // Native image checks below verify that the real Cairo paint uses it.
        double previous = 1;
        for (int percent = 0; percent <= 80; ++percent) {
            const double alpha = WheelAppearance::fillAlpha(0.6, percent);
            require(alpha > 0 && alpha <= previous && alpha <= 1,
                    "The shared surface-opacity mapping is not bounded and monotonic");
            previous = alpha;
        }
        require(WheelAppearance::fillAlpha(0.6, -1) == WheelAppearance::fillAlpha(0.6, 0)
                    && WheelAppearance::fillAlpha(0.6, 81) == WheelAppearance::fillAlpha(0.6, 80)
                    && WheelAppearance::fillAlpha(0.6, 35) == 0.6,
                "The shared appearance mapping lost its bounds or the existing glass default");
        checkColorTransfer();
        checkHoverMessages(false);
        checkHoverMessages(true);
        checkMode(false);
        checkMode(true);
        checkColors(false);
        checkColors(true);
        std::puts("{\"test\":\"native-wheel-appearance\",\"conversion_and_tools\":true,"
                  "\"protocol_validation\":true,\"mode_isolation\":true,\"rendered_alpha\":true,"
                  "\"fruit_opaque\":true,\"scaled_hit_geometry\":true,\"size_bounds\":true,"
                  "\"rendered_colors\":true,\"strict_rgb_parser\":true,\"color_atomic_protocol\":true,"
                  "\"deferred_drag_size\":true,\"legacy_protocol\":true,\"desktop_input_used\":false}");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Native wheel appearance check failed: %s\n", error.what());
        return 1;
    }
}
