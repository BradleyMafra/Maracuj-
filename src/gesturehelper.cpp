#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include <glib.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <pango/pangocairo.h>
#include <cairo.h>
#include "wheelappearance.h"

#include <QByteArray>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QUrl>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <utility>
#include <unistd.h>

namespace {

constexpr int WindowSize = 460;
// Reuse two bounded buffers across all sizes. Window geometry and input region
// cover only the requested extent; the unused pixels remain transparent.
constexpr int MaxWindowSize = WheelAppearance::sizeExtent(130);
constexpr int Stride = MaxWindowSize * 4;
constexpr size_t BufferBytes = size_t(Stride) * MaxWindowSize;
constexpr double Center = 230.0;
constexpr double OuterRadius = 198.0;
constexpr double InnerRadius = 82.0;
constexpr double Pi = 3.14159265358979323846;
constexpr int MaxMessageBytes = 1024 * 1024;
constexpr uint32_t Copy = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;

struct State;
struct Offer {
    State *state = nullptr;
    wl_data_offer *object = nullptr;
    bool hasUris = false;
    bool received = false;
    uint32_t sourceActions = 0;
    uint32_t selectedAction = 0;
};
struct Buffer {
    State *state = nullptr;
    wl_buffer *object = nullptr;
    void *pixels = MAP_FAILED;
    cairo_surface_t *canvas = nullptr;
    bool busy = false;
};
struct State {
    GMainLoop *loop = nullptr;
    GSource *waylandSource = nullptr;
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_compositor *compositor = nullptr;
    wl_shm *shm = nullptr;
    wl_seat *seat = nullptr;
    wl_data_device_manager *manager = nullptr;
    wl_data_device *device = nullptr;
    wl_surface *surface = nullptr;
    xdg_wm_base *wmBase = nullptr;
    xdg_surface *xdgSurface = nullptr;
    xdg_toplevel *toplevel = nullptr;
    uint32_t wmVersion = 0;
    QHash<wl_data_offer *, Offer *> offers;
    Offer *active = nullptr;
    Buffer buffers[2];
    guint dismissIdle = 0;
    guint probeTimer = 0;
    guint inputWatch = 0;
    guint uriWatch = 0;
    guint uriTimer = 0;
    guint signalInt = 0;
    guint signalTerm = 0;
    int uriFd = -1;
    guint token = 0;
    uint32_t enterSerial = 0;
    bool configured = false;
    bool mapped = false;
    bool suspended = false;
    bool busy = false;
    bool validDrag = false;
    bool dropFinished = false;
    bool pendingDraw = false;
    bool failed = false;
    bool tools = false;
    int wheelTransparency = 35;
    int wheelSize = 100;
    int requestedWheelSize = 100;
    WheelAppearance::Rgb wheelColor = WheelAppearance::DefaultColor;
    wl_callback *frameCallback = nullptr;
    gint64 openingStarted = 0;
    gint64 hoverStarted = 0;
    bool opening = false;
    bool hovering = false;
    double openingProgress = 1;
    std::array<double, 16> hoverLevels {};
    std::array<double, 16> hoverFrom {};
    int hovered = -1;
    double pointerX = Center;
    double pointerY = Center;
    QStringList paths;
    QJsonArray formats;
    QByteArray input;
    QByteArray uris;
};

void render(State *state);
void dismissLater(State *state);
void handleMessage(State *state, const QJsonObject &message);
void emitMessage(State *state, const QJsonObject &message);

double easeOut(double value)
{
    const double inverse = 1 - std::clamp(value, 0.0, 1.0);
    return 1 - inverse * inverse * inverse;
}

void updateAnimation(State *state, gint64 now)
{
    if (state->opening) {
        state->openingProgress = std::clamp(double(now - state->openingStarted) / 290000.0, 0.0, 1.0);
        state->opening = state->openingProgress < 1;
    }
    if (state->hovering) {
        const double progress = std::clamp(double(now - state->hoverStarted) / 155000.0, 0.0, 1.0);
        const double eased = easeOut(progress);
        for (size_t index = 0; index < state->hoverLevels.size(); ++index) {
            const double target = int(index) == state->hovered ? 1 : 0;
            state->hoverLevels[index] = state->hoverFrom[index] + (target - state->hoverFrom[index]) * eased;
        }
        state->hovering = progress < 1;
    }
}

void stopAnimation(State *state)
{
    if (state->frameCallback) {
        wl_callback_destroy(state->frameCallback);
        state->frameCallback = nullptr;
    }
    state->opening = false;
    state->hovering = false;
    state->openingProgress = 1;
    state->hoverLevels.fill(0);
    state->hoverFrom.fill(0);
}

void changeHover(State *state, int hovered)
{
    const bool enteredSector = hovered >= 0 && hovered < state->formats.size() && hovered != state->hovered
        && state->validDrag && !state->busy && !state->dropFinished;
    updateAnimation(state, g_get_monotonic_time());
    state->hoverFrom = state->hoverLevels;
    state->hovered = hovered;
    state->hoverStarted = g_get_monotonic_time();
    state->hovering = true;
    if (enteredSector)
        emitMessage(state, {{"type", "hover"}, {"sector", hovered}});
}

void quit(State *state, bool failed = false)
{
    state->failed = state->failed || failed;
    if (state->loop) g_main_loop_quit(state->loop);
}

void flush(State *state)
{
    if (state->display && wl_display_flush(state->display) < 0 && errno != EAGAIN)
        quit(state, true);
}

void updateWindowGeometry(State *state)
{
    if (!state->surface || !state->toplevel || !state->xdgSurface) return;
    const int extent = WheelAppearance::sizeExtent(state->wheelSize);
    wl_region *input = wl_compositor_create_region(state->compositor);
    wl_region_add(input, 0, 0, extent, extent);
    wl_surface_set_input_region(state->surface, input);
    wl_region_destroy(input);
    xdg_toplevel_set_min_size(state->toplevel, extent, extent);
    xdg_toplevel_set_max_size(state->toplevel, extent, extent);
    xdg_surface_set_window_geometry(state->xdgSurface, 0, 0, extent, extent);
}

bool applyPendingSize(State *state)
{
    // A saved size applies between gestures so the destination never moves or
    // changes its sectors underneath an active native drag.
    if (state->active || state->validDrag || state->wheelSize == state->requestedWheelSize) return false;
    state->wheelSize = state->requestedWheelSize;
    updateWindowGeometry(state);
    return true;
}

void emitMessage(State *state, const QJsonObject &message)
{
    QJsonObject payload = message;
    payload.insert(QStringLiteral("mode"), state->tools ? QStringLiteral("tools") : QStringLiteral("convert"));
    const QByteArray bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact) + '\n';
    qsizetype sent = 0;
    while (sent < bytes.size()) {
        const ssize_t count = ::write(STDOUT_FILENO, bytes.constData() + sent,
                                      size_t(bytes.size() - sent));
        if (count > 0) sent += count;
        else if (count < 0 && errno == EINTR) continue;
        else { quit(state); return; }
    }
}

void stopProbeTimer(State *state)
{
    if (state->probeTimer) {
        g_source_remove(state->probeTimer);
        state->probeTimer = 0;
    }
}

void cancelDismissal(State *state)
{
    if (state->dismissIdle) {
        g_source_remove(state->dismissIdle);
        state->dismissIdle = 0;
    }
}

void cancelUriRead(State *state)
{
    if (state->uriWatch) {
        g_source_remove(state->uriWatch);
        state->uriWatch = 0;
    }
    if (state->uriTimer) {
        g_source_remove(state->uriTimer);
        state->uriTimer = 0;
    }
    if (state->uriFd >= 0) {
        ::close(state->uriFd);
        state->uriFd = -1;
    }
    state->uris.clear();
}

void destroyOffer(State *state, Offer *offer)
{
    if (!offer) return;
    if (state->active == offer) state->active = nullptr;
    state->offers.remove(offer->object);
    wl_data_offer_destroy(offer->object);
    delete offer;
}

void clearDrag(State *state)
{
    stopAnimation(state);
    cancelUriRead(state);
    destroyOffer(state, state->active);
    state->validDrag = false;
    state->dropFinished = false;
    state->hovered = -1;
    state->paths.clear();
    state->formats = {};
    render(state);
}

void dismiss(State *state)
{
    stopProbeTimer(state);
    cancelDismissal(state);
    clearDrag(state);
    if (state->toplevel && state->mapped) {
        xdg_toplevel_set_minimized(state->toplevel);
        flush(state);
    }
    if (applyPendingSize(state)) render(state);
    emitMessage(state, {{QStringLiteral("type"), QStringLiteral("dismiss")},
                        {QStringLiteral("token"), int(state->token)}});
}

gboolean dismissIdle(gpointer data)
{
    auto *state = static_cast<State *>(data);
    state->dismissIdle = 0;
    dismiss(state);
    return G_SOURCE_REMOVE;
}

void dismissLater(State *state)
{
    if (!state->dismissIdle)
        state->dismissIdle = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, dismissIdle, state, nullptr);
}

gboolean probeTimeout(gpointer data)
{
    auto *state = static_cast<State *>(data);
    state->probeTimer = 0;
    if (!state->validDrag) dismissLater(state);
    return G_SOURCE_REMOVE;
}

void arm(State *state)
{
    cancelDismissal(state);
    stopProbeTimer(state);
    if (state->busy) dismissLater(state);
    else if (!state->validDrag) state->probeTimer = g_timeout_add(300, probeTimeout, state);
}

int sectorAt(const State *state, double x, double y)
{
    if (!state->validDrag || state->formats.isEmpty()) return -1;
    const double scale = WheelAppearance::sizeExtent(state->wheelSize) / double(WindowSize);
    const double dx = x / scale - Center;
    const double dy = y / scale - Center;
    const double distance = std::hypot(dx, dy);
    if (distance < InnerRadius || distance > OuterRadius) return -1;
    const double step = 2 * Pi / state->formats.size();
    const double angle = std::fmod(std::atan2(dy, dx) + Pi / 2 + step / 2 + 2 * Pi, 2 * Pi);
    return int(std::floor(angle / step)) % state->formats.size();
}

void setColor(cairo_t *cr, const char *hex, double alpha = 1)
{
    unsigned red = 0, green = 0, blue = 0;
    std::sscanf(hex, "#%02x%02x%02x", &red, &green, &blue);
    cairo_set_source_rgba(cr, red / 255.0, green / 255.0, blue / 255.0, alpha);
}

void drawText(cairo_t *cr, const QString &text, double x, double y, double width,
              const char *font, const char *color, double halo = 0)
{
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *description = pango_font_description_from_string(font);
    pango_layout_set_font_description(layout, description);
    pango_layout_set_width(layout, int(width * PANGO_SCALE));
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    const QByteArray utf8 = text.toUtf8();
    pango_layout_set_text(layout, utf8.constData(), utf8.size());
    cairo_move_to(cr, x, y);
    if (halo > 0) {
        // A fine glyph outline keeps labels readable over a busy desktop at
        // high transparency, without adding an opaque plate behind them.
        pango_cairo_layout_path(cr, layout);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_set_line_width(cr, 2.5);
        setColor(cr, "#fffdfb", halo);
        cairo_stroke(cr);
        cairo_move_to(cr, x, y);
    }
    setColor(cr, color);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(description);
    g_object_unref(layout);
}

void roundedSector(cairo_t *cr, double inner, double outer, double start, double end)
{
    const double corner = std::min(10.0, (end - start) * inner / 4);
    const double outerInset = corner / outer;
    const double innerInset = corner / inner;
    const auto point = [](double radius, double angle) {
        return std::pair<double, double>(Center + radius * std::cos(angle), Center + radius * std::sin(angle));
    };
    const auto move = [&](double radius, double angle) { const auto p = point(radius, angle); cairo_move_to(cr, p.first, p.second); };
    const auto line = [&](double radius, double angle) { const auto p = point(radius, angle); cairo_line_to(cr, p.first, p.second); };
    const auto curve = [&](double r1, double a1, double r2, double a2, double r3, double a3) {
        const auto p1 = point(r1, a1), p2 = point(r2, a2), p3 = point(r3, a3);
        cairo_curve_to(cr, p1.first, p1.second, p2.first, p2.second, p3.first, p3.second);
    };
    cairo_new_path(cr);
    move(inner + corner, start);
    line(outer - corner, start);
    curve(outer, start, outer, start, outer, start + outerInset);
    cairo_arc(cr, Center, Center, outer, start + outerInset, end - outerInset);
    curve(outer, end, outer, end, outer - corner, end);
    line(inner + corner, end);
    curve(inner, end, inner, end, inner, end - innerInset);
    cairo_arc_negative(cr, Center, Center, inner, end - innerInset, start + innerInset);
    curve(inner, start, inner, start, inner + corner, start);
    cairo_close_path(cr);
}

void drawFruit(cairo_t *cr, double x, double y)
{
    // A purple passion fruit and its golden, seed-filled cross section. Vector
    // shapes keep the little fruit crisp on the translucent native surface.
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_save(cr);
    cairo_translate(cr, -14, -4);
    cairo_scale(cr, 0.88, 1);
    cairo_pattern_t *skin = cairo_pattern_create_radial(-9, -13, 1, 0, 0, 27);
    cairo_pattern_add_color_stop_rgb(skin, 0, 0.67, 0.40, 0.80);
    cairo_pattern_add_color_stop_rgb(skin, 0.65, 0.46, 0.22, 0.61);
    cairo_pattern_add_color_stop_rgb(skin, 1, 0.30, 0.12, 0.43);
    cairo_arc(cr, 0, 0, 25, 0, 2 * Pi);
    cairo_set_source(cr, skin);
    cairo_fill(cr);
    cairo_pattern_destroy(skin);
    cairo_arc(cr, -7, -9, 13, -2.8, -1.0);
    setColor(cr, "#e4c7f6", 0.38);
    cairo_set_line_width(cr, 2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cr);
    cairo_restore(cr);
    cairo_save(cr);
    cairo_translate(cr, 11, 5);
    cairo_rotate(cr, -0.19);
    cairo_scale(cr, 1, 0.89);
    cairo_arc(cr, 0, 0, 27, 0, 2 * Pi);
    setColor(cr, "#63358b");
    cairo_fill(cr);
    cairo_arc(cr, 0, 0, 23, 0, 2 * Pi);
    setColor(cr, "#fffcf2");
    cairo_fill(cr);
    cairo_pattern_t *pulp = cairo_pattern_create_radial(-5, -7, 0, 0, 0, 21);
    cairo_pattern_add_color_stop_rgb(pulp, 0, 1.0, 0.91, 0.48);
    cairo_pattern_add_color_stop_rgb(pulp, 1, 0.95, 0.69, 0.19);
    cairo_arc(cr, 0, 0, 20, 0, 2 * Pi);
    cairo_set_source(cr, pulp);
    cairo_fill(cr);
    cairo_pattern_destroy(pulp);
    for (int index = 0; index < 17; ++index) {
        const double angle = index * 2.3999632297;
        const double radius = 4 + 12.5 * std::sqrt(double(index) / 17);
        cairo_save(cr);
        cairo_translate(cr, radius * std::cos(angle), radius * std::sin(angle));
        cairo_rotate(cr, angle + 0.35);
        cairo_scale(cr, 1.25, 0.78);
        cairo_arc(cr, 0, 0, 2.45, 0, 2 * Pi);
        setColor(cr, "#ffed99", 0.95);
        cairo_fill(cr);
        cairo_arc(cr, 0, 0, 1.5, 0, 2 * Pi);
        setColor(cr, "#45304a");
        cairo_fill(cr);
        cairo_restore(cr);
    }
    cairo_restore(cr);
    cairo_new_path(cr);
    cairo_move_to(cr, -17, -28);
    cairo_curve_to(cr, -16, -40, -2, -36, 0, -30);
    cairo_curve_to(cr, -7, -25, -12, -24, -17, -28);
    setColor(cr, "#729855");
    cairo_fill(cr);
    cairo_restore(cr);
}

void drawWheel(const State *state, cairo_t *cr)
{
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    // Grayscale text keeps transparent surfaces free of RGB fringes.
    cairo_font_options_t *fontOptions = cairo_font_options_create();
    cairo_font_options_set_antialias(fontOptions, CAIRO_ANTIALIAS_GRAY);
    cairo_set_font_options(cr, fontOptions);
    cairo_font_options_destroy(fontOptions);
    if (!state->validDrag || state->formats.isEmpty()) {
        cairo_restore(cr);
        return;
    }
    const double wheelScale = WheelAppearance::sizeExtent(state->wheelSize) / double(WindowSize);
    cairo_scale(cr, wheelScale, wheelScale);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    const double opening = state->openingProgress;
    const double reveal = easeOut(std::min(1.0, opening * 1.8));
    const auto surfaceAlpha = [state](double alpha) {
        return WheelAppearance::fillAlpha(alpha, state->wheelTransparency);
    };
    const auto decorationAlpha = [state](double alpha) {
        return WheelAppearance::decorationAlpha(alpha, state->wheelTransparency);
    };
    const double labelHalo = std::max(WheelAppearance::labelHaloAlpha(state->wheelTransparency),
                                     WheelAppearance::colorLabelHalo(state->wheelColor));
    const auto tintedColor = [state, cr](const char *hex, double alpha) {
        WheelAppearance::Rgb reference{};
        WheelAppearance::parseColor(hex, &reference);
        const auto color = WheelAppearance::tint(reference, state->wheelColor);
        cairo_set_source_rgba(cr, color.red, color.green, color.blue, alpha);
    };
    const auto tintedStop = [state](cairo_pattern_t *pattern, double stop,
                                   double red, double green, double blue, double alpha) {
        const auto color = WheelAppearance::tint({red, green, blue}, state->wheelColor);
        cairo_pattern_add_color_stop_rgba(pattern, stop, color.red, color.green, color.blue, alpha);
    };
    cairo_push_group(cr);

    // Keep each overlapping layer light: an opaque underlay would hide the
    // desktop even with translucent petals. Text and fruit stay fully opaque.
    // No backdrop or screen capture is used.
    for (int ring = 14; ring >= 1; --ring) {
        cairo_new_path(cr);
        cairo_arc(cr, Center, Center + 6, OuterRadius + ring, 0, 2 * Pi);
        tintedColor("#63358b", decorationAlpha(0.002) * reveal);
        cairo_fill(cr);
    }
    cairo_pattern_t *glass = cairo_pattern_create_linear(80, 30, 350, 432);
    tintedStop(glass, 0, 1.0, 0.988, 0.96, decorationAlpha(0.08));
    tintedStop(glass, 0.53, 0.91, 0.96, 1.0, decorationAlpha(0.06));
    tintedStop(glass, 1, 0.88, 0.81, 0.96, decorationAlpha(0.10));
    cairo_new_path(cr);
    cairo_arc(cr, Center, Center, OuterRadius + 4, 0, 2 * Pi);
    cairo_set_source(cr, glass);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(glass);
    setColor(cr, "#ffffff", 0.52);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
    cairo_arc(cr, Center, Center, OuterRadius + 7, -Pi * 0.89, -Pi * 0.13);
    setColor(cr, "#f3c64d", 0.44);
    cairo_set_line_width(cr, 1.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cr);

    const int count = state->formats.size();
    const double step = 2 * Pi / count;
    for (int index = 0; index < count; ++index) {
        const double hover = state->hoverLevels[size_t(index)];
        const double delay = count > 1 ? 0.17 * index / (count - 1) : 0;
        const double progress = easeOut(std::clamp((opening - delay) / (1 - delay), 0.0, 1.0));
        const double middle = -Pi / 2 + index * step;
        const double start = middle - step / 2 + 0.026;
        const double end = middle + step / 2 - 0.026;
        cairo_save(cr);
        // Only the painted petals move; native drop geometry is fixed and
        // remains available throughout the short reveal and hover transitions.
        const double lift = 3.5 * hover - (1 - progress) * 7;
        cairo_translate(cr, std::cos(middle) * lift, std::sin(middle) * lift);
        cairo_translate(cr, Center, Center);
        const double scale = 0.91 + 0.09 * progress;
        cairo_scale(cr, scale, scale);
        cairo_translate(cr, -Center, -Center);
        cairo_push_group(cr);
        if (hover > 0.001) {
            for (int glow = 7; glow >= 1; --glow) {
                roundedSector(cr, InnerRadius - 1, OuterRadius + 1, start, end);
                tintedColor("#9458c4", 0.009 * hover);
                cairo_set_line_width(cr, glow * 1.8);
                cairo_stroke(cr);
            }
        }
        roundedSector(cr, InnerRadius + 1, OuterRadius - 1, start, end);
        cairo_pattern_t *gradient = cairo_pattern_create_linear(110, 35, 350, 420);
        tintedStop(gradient, 0,
            1 - 0.06 * hover, 0.994 - 0.12 * hover, 0.98 + 0.02 * hover, surfaceAlpha(0.64 + 0.13 * hover));
        tintedStop(gradient, 0.46,
            0.95 - 0.04 * hover, 0.97 - 0.13 * hover, 1 - 0.01 * hover, surfaceAlpha(0.60 + 0.14 * hover));
        tintedStop(gradient, 1,
            0.94 - 0.04 * hover, 0.90 - 0.11 * hover, 0.99, surfaceAlpha(0.58 + 0.15 * hover));
        cairo_set_source(cr, gradient);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(gradient);
        cairo_set_line_width(cr, 1 + 0.7 * hover);
        const auto outline = WheelAppearance::tint(
            {0.72 - 0.17 * hover, 0.59 - 0.24 * hover, 0.82 - 0.11 * hover}, state->wheelColor);
        cairo_set_source_rgba(cr, outline.red, outline.green, outline.blue, 0.28 + 0.36 * hover);
        cairo_stroke(cr);
        cairo_new_path(cr);
        cairo_arc(cr, Center, Center, OuterRadius - 7, start + 0.048, end - 0.048);
        cairo_set_line_width(cr, 1);
        setColor(cr, "#ffffff", 0.50);
        cairo_stroke(cr);
        if (hover > 0.001) {
            cairo_new_path(cr);
            cairo_arc(cr, Center, Center, OuterRadius - 3, middle - step * 0.22, middle + step * 0.22);
            setColor(cr, "#f3c64d", 0.95 * hover);
            cairo_set_line_width(cr, 3);
            cairo_stroke(cr);
        }
        const QJsonObject format = state->formats.at(index).toObject();
        const double radius = (OuterRadius + InnerRadius) / 2;
        const bool dense = count >= 8;
        const double labelWidth = dense ? 76 : 116;
        const double x = Center + radius * std::cos(middle) - labelWidth / 2;
        const double y = Center + radius * std::sin(middle) - (dense ? 10 : 20);
        drawText(cr, format.value(QStringLiteral("label")).toString(), x, y, labelWidth,
                 dense ? "Segoe UI Semibold 11" : "Segoe UI Semibold 13", hover > 0.5 ? "#422051" : "#33213f", labelHalo);
        if (!dense) drawText(cr, format.value(QStringLiteral("detail")).toString(), x, y + 25, labelWidth,
                            "Segoe UI 7.5", "#44314f", labelHalo);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, progress);
        cairo_restore(cr);
    }

    cairo_save(cr);
    cairo_translate(cr, Center, Center);
    const double coreScale = 0.88 + 0.12 * easeOut(std::min(1.0, opening * 1.5));
    cairo_scale(cr, coreScale, coreScale);
    cairo_translate(cr, -Center, -Center);
    cairo_new_path(cr);
    cairo_arc(cr, Center, Center + 3, InnerRadius - 5, 0, 2 * Pi);
    tintedColor("#63358b", decorationAlpha(0.035));
    cairo_fill(cr);
    cairo_pattern_t *core = cairo_pattern_create_linear(Center, 155, Center, 305);
    tintedStop(core, 0, 1.0, 0.992, 0.96, surfaceAlpha(0.61));
    tintedStop(core, 0.45, 0.94, 0.97, 1.0, surfaceAlpha(0.57));
    tintedStop(core, 1, 0.974, 0.92, 0.998, surfaceAlpha(0.56));
    cairo_arc(cr, Center, Center, InnerRadius - 7, 0, 2 * Pi);
    cairo_set_source(cr, core);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(core);
    setColor(cr, "#ffffff", 0.64);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    drawFruit(cr, Center, Center - 27);
    drawText(cr, QStringLiteral("Maracujá"), Center - 66, Center + 8, 132,
             "Segoe UI Semibold 10.5", "#422051", labelHalo);
    drawText(cr, state->hovered >= 0 ? state->formats.at(state->hovered).toObject().value(QStringLiteral("detail")).toString()
                                   : state->tools ? QStringLiteral("Escolha uma ferramenta") : QStringLiteral("Escolha um formato"),
             Center - 72, Center + 34, 144, "Segoe UI 7.5", "#44314f", labelHalo);
    cairo_restore(cr);

    // A brief golden glint follows the initial reveal, then all animation
    // stops. Compositor frame callbacks, rather than an idle timer, drive it.
    if (opening > 0 && opening < 1) {
        const double glint = std::sin(opening * Pi);
        const double angle = -Pi * 0.95 + opening * Pi * 1.5;
        cairo_new_path(cr);
        cairo_arc(cr, Center, Center, OuterRadius + 7, angle, angle + 0.26);
        setColor(cr, "#fff4bf", 0.95 * glint);
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
        for (int spark = 0; spark < 3; ++spark) {
            const double a = angle - spark * 0.15;
            const double radius = OuterRadius + 10 + spark * 2;
            cairo_new_path(cr);
            cairo_arc(cr, Center + radius * std::cos(a), Center + radius * std::sin(a),
                      1.9 - spark * 0.4, 0, 2 * Pi);
            setColor(cr, "#f3c64d", glint * (0.85 - spark * 0.18));
            cairo_fill(cr);
        }
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, reveal);
    drawText(cr, QStringLiteral("Solte no formato · Centro ou fora da roda cancela"),
             10, 441, 440, "Segoe UI 7.5", "#81708f");
    cairo_restore(cr);
}


void bufferReleased(void *data, wl_buffer *)
{
    auto *buffer = static_cast<Buffer *>(data);
    buffer->busy = false;
    if (buffer->state->pendingDraw) render(buffer->state);
}

const wl_buffer_listener BufferListener {bufferReleased};

int makeMemoryFile()
{
    int fd = -1;
#ifdef SYS_memfd_create
    fd = int(::syscall(SYS_memfd_create, "mandarina-wheel", 0x0001U)); // MFD_CLOEXEC
#endif
    if (fd < 0) {
        gchar *name = nullptr;
        fd = g_file_open_tmp("mandarina-wheel-XXXXXX", &name, nullptr);
        if (name) { g_unlink(name); g_free(name); }
        if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    if (fd >= 0 && ftruncate(fd, off_t(BufferBytes)) < 0) {
        ::close(fd);
        fd = -1;
    }
    return fd;
}

bool createBuffer(State *state, Buffer &buffer)
{
    const int fd = makeMemoryFile();
    if (fd < 0) return false;
    void *pixels = mmap(nullptr, BufferBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) { ::close(fd); return false; }
    wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, int(BufferBytes));
    wl_buffer *object = wl_shm_pool_create_buffer(pool, 0, MaxWindowSize, MaxWindowSize, Stride,
                                                 WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    ::close(fd);
    if (!object) { munmap(pixels, BufferBytes); return false; }
    buffer.state = state;
    buffer.object = object;
    buffer.pixels = pixels;
    buffer.canvas = cairo_image_surface_create_for_data(static_cast<unsigned char *>(pixels),
        CAIRO_FORMAT_ARGB32, MaxWindowSize, MaxWindowSize, Stride);
    wl_buffer_add_listener(object, &BufferListener, &buffer);
    return cairo_surface_status(buffer.canvas) == CAIRO_STATUS_SUCCESS;
}

void frameDone(void *data, wl_callback *callback, uint32_t)
{
    auto *state = static_cast<State *>(data);
    if (state->frameCallback == callback) state->frameCallback = nullptr;
    wl_callback_destroy(callback);
    if (state->validDrag && (state->opening || state->hovering)) render(state);
}

const wl_callback_listener FrameListener {frameDone};

void render(State *state)
{
    if (!state->configured || !state->surface) return;
    updateAnimation(state, g_get_monotonic_time());
    Buffer *buffer = nullptr;
    for (auto &candidate : state->buffers) {
        if (candidate.object && !candidate.busy) { buffer = &candidate; break; }
    }
    if (!buffer) { state->pendingDraw = true; return; }
    state->pendingDraw = false;
    cairo_t *cr = cairo_create(buffer->canvas);
    drawWheel(state, cr);
    cairo_destroy(cr);
    cairo_surface_flush(buffer->canvas);
    buffer->busy = true;
    wl_surface_attach(state->surface, buffer->object, 0, 0);
    if (wl_surface_get_version(state->surface) >= 4)
        wl_surface_damage_buffer(state->surface, 0, 0, MaxWindowSize, MaxWindowSize);
    else wl_surface_damage(state->surface, 0, 0, MaxWindowSize, MaxWindowSize);
    // A callback belongs to this committed frame. Keep at most one pending;
    // buffer release resumes rendering if both shared buffers were in flight.
    if (state->validDrag && (state->opening || state->hovering) && !state->frameCallback) {
        state->frameCallback = wl_surface_frame(state->surface);
        wl_callback_add_listener(state->frameCallback, &FrameListener, state);
    }
    wl_surface_commit(state->surface);
    flush(state);
}

bool supportsCopy(const Offer *offer)
{
    return offer && offer->hasUris && (offer->sourceActions & Copy);
}

void acceptCopy(State *state)
{
    if (!state->active || state->dropFinished) return;
    // This is an allowed-action mask, not merely a preference. Shift cannot
    // negotiate MOVE: neither the source nor the compositor is offered MOVE.
    wl_data_offer_set_actions(state->active->object, Copy, Copy);
    wl_data_offer_accept(state->active->object, state->enterSerial, "text/uri-list");
    flush(state);
}

bool parseUris(State *state, const QByteArray &bytes)
{
    QStringList paths;
    const auto lines = bytes.split('\n');
    for (QByteArray line : lines) {
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (paths.size() >= 128 || line.contains('\0')) return false;
        const QUrl url = QUrl::fromEncoded(line, QUrl::StrictMode);
        if (!url.isValid() || !url.isLocalFile()
            || (!url.host().isEmpty() && url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive)))
            return false;
        const QString localPath = url.toLocalFile();
        const QFileInfo info(localPath);
        if (!info.isAbsolute() || !info.isFile() || !info.isReadable()) return false;
        const QString canonical = info.canonicalFilePath();
        if (canonical.isEmpty()) return false;
        if (!paths.contains(canonical)) paths.append(canonical);
    }
    if (paths.isEmpty()) return false;
    state->paths = paths;
    return true;
}

gboolean uriTimeout(gpointer data)
{
    auto *state = static_cast<State *>(data);
    state->uriTimer = 0;
    cancelUriRead(state);
    dismissLater(state);
    return G_SOURCE_REMOVE;
}

gboolean readUris(gint fd, GIOCondition condition, gpointer data)
{
    auto *state = static_cast<State *>(data);
    bool eof = false;
    bool failed = false;
    char bytes[8192];
    while (true) {
        const ssize_t count = ::read(fd, bytes, sizeof bytes);
        if (count > 0) {
            state->uris.append(bytes, count);
            if (state->uris.size() > MaxMessageBytes) { failed = true; break; }
        } else if (!count) { eof = true; break; }
        else if (errno == EINTR) continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        else { failed = true; break; }
    }
    if (condition & (G_IO_ERR | G_IO_NVAL)) failed = true;
    if (!eof && !failed && !(condition & G_IO_HUP)) return G_SOURCE_CONTINUE;
    const QByteArray uris = state->uris;
    state->uriWatch = 0;
    cancelUriRead(state);
    if (failed || !state->active || state->dropFinished || !parseUris(state, uris)) {
        dismissLater(state);
        return G_SOURCE_REMOVE;
    }
    // A real local URI drag may need a first capability probe for documents
    // or audio. Keep the short timer for false gestures, with a bounded grace
    // period only after the source has actually supplied valid local files.
    stopProbeTimer(state);
    state->probeTimer = g_timeout_add(4000, probeTimeout, state);
    QJsonArray paths;
    for (const QString &path : state->paths) paths.append(path);
    emitMessage(state, {{QStringLiteral("type"), QStringLiteral("preview")},
        {QStringLiteral("token"), int(state->token)}, {QStringLiteral("paths"), paths}});
    return G_SOURCE_REMOVE;
}

void requestUris(State *state)
{
    if (!state->active || state->active->received) return;
    state->active->received = true;
    int pipes[2] {-1, -1};
    if (pipe(pipes) < 0) { dismissLater(state); return; }
    fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
    const int flags = fcntl(pipes[0], F_GETFL);
    if (flags < 0 || fcntl(pipes[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(pipes[0]); ::close(pipes[1]); dismissLater(state); return;
    }
    state->uriFd = pipes[0];
    wl_data_offer_receive(state->active->object, "text/uri-list", pipes[1]);
    ::close(pipes[1]);
    state->uriWatch = g_unix_fd_add(state->uriFd,
        GIOCondition(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL), readUris, state);
    state->uriTimer = g_timeout_add(2000, uriTimeout, state);
    flush(state);
}

void offerMime(void *data, wl_data_offer *, const char *mime)
{
    auto *offer = static_cast<Offer *>(data);
    if (mime && !std::strcmp(mime, "text/uri-list")) offer->hasUris = true;
}

void offerSourceActions(void *data, wl_data_offer *, uint32_t actions)
{
    static_cast<Offer *>(data)->sourceActions = actions;
}

void offerAction(void *data, wl_data_offer *, uint32_t action)
{
    auto *offer = static_cast<Offer *>(data);
    offer->selectedAction = action;
    if (offer->state->active == offer && action != 0 && action != Copy)
        dismissLater(offer->state);
}

const wl_data_offer_listener OfferListener {offerMime, offerSourceActions, offerAction};

void newOffer(void *data, wl_data_device *, wl_data_offer *object)
{
    auto *state = static_cast<State *>(data);
    if (state->offers.size() >= 64) { wl_data_offer_destroy(object); return; }
    auto *offer = new Offer;
    offer->state = state;
    offer->object = object;
    state->offers.insert(object, offer);
    wl_data_offer_add_listener(object, &OfferListener, offer);
}

void dragEnter(void *data, wl_data_device *, uint32_t serial, wl_surface *surface,
               wl_fixed_t x, wl_fixed_t y, wl_data_offer *object)
{
    auto *state = static_cast<State *>(data);
    Offer *offer = state->offers.value(object);
    cancelDismissal(state);
    if (state->active != offer) clearDrag(state);
    ++state->token;
    if (!state->token) ++state->token;
    state->enterSerial = serial;
    state->pointerX = wl_fixed_to_double(x);
    state->pointerY = wl_fixed_to_double(y);
    state->dropFinished = false;
    state->active = offer;
    if (surface != state->surface || state->busy || !supportsCopy(offer)) {
        if (offer) {
            wl_data_offer_set_actions(offer->object, 0, 0);
            wl_data_offer_accept(offer->object, serial, nullptr);
        }
        flush(state);
        dismissLater(state);
        return;
    }
    arm(state);
    acceptCopy(state);
    requestUris(state);
}

void dragLeave(void *data, wl_data_device *)
{
    auto *state = static_cast<State *>(data);
    // Data has already been cached before an accepted drop. No late URI reads
    // or replies can finish a different drag after this offer is destroyed.
    clearDrag(state);
    dismissLater(state);
}

void dragMotion(void *data, wl_data_device *, uint32_t, wl_fixed_t x, wl_fixed_t y)
{
    auto *state = static_cast<State *>(data);
    state->pointerX = wl_fixed_to_double(x);
    state->pointerY = wl_fixed_to_double(y);
    if (!supportsCopy(state->active) || state->busy || state->dropFinished) return;
    acceptCopy(state);
    const int hovered = sectorAt(state, state->pointerX, state->pointerY);
    if (hovered != state->hovered) {
        changeHover(state, hovered);
        render(state);
    }
}

void dragDrop(void *data, wl_data_device *)
{
    auto *state = static_cast<State *>(data);
    const int sector = sectorAt(state, state->pointerX, state->pointerY);
    Offer *offer = state->active;
    if (!offer || state->busy || state->dropFinished || !supportsCopy(offer)
        || offer->selectedAction != Copy || sector < 0 || state->paths.isEmpty()) {
        // Destroying an unaccepted offer is a rejection. Never acknowledge a
        // MOVE, never finish before drop, and never ask a source to delete.
        dismissLater(state);
        return;
    }
    state->dropFinished = true;
    wl_data_offer_finish(offer->object);
    flush(state);
    QJsonArray paths;
    for (const QString &path : state->paths) paths.append(path);
    emitMessage(state, {{QStringLiteral("type"), QStringLiteral("drop")},
        {QStringLiteral("token"), int(state->token)}, {QStringLiteral("paths"), paths},
        {QStringLiteral("format"), state->formats.at(sector).toObject().value(QStringLiteral("id"))}});
    stopAnimation(state);
    state->validDrag = false;
    state->hovered = -1;
    render(state);
    dismissLater(state);
}

void selection(void *data, wl_data_device *, wl_data_offer *object)
{
    // The data-device object necessarily also reports selection offers. They
    // are destroyed immediately, without accept(), receive(), or inspecting
    // clipboard data. Only a drag-enter offer is ever read by this process.
    auto *state = static_cast<State *>(data);
    if (object) destroyOffer(state, state->offers.value(object));
}

const wl_data_device_listener DeviceListener {
    newOffer, dragEnter, dragLeave, dragMotion, dragDrop, selection
};

void handleMessage(State *state, const QJsonObject &message)
{
    // Each wheel is a separate native destination. Never let a delayed reply
    // from the other wheel arm this destination or populate its choices.
    const QString mode = message.value(QStringLiteral("mode")).toString();
    if (!mode.isEmpty() && mode != (state->tools ? QStringLiteral("tools") : QStringLiteral("convert"))) return;
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("appearance")) {
        const auto value = message.value(QStringLiteral("transparency"));
        const double percent = value.toDouble(-1);
        const auto sizeValue = message.value(QStringLiteral("size"));
        const double size = sizeValue.isUndefined() ? state->requestedWheelSize : sizeValue.toDouble(-1);
        const auto colorValue = message.value(QStringLiteral("color"));
        auto selectedColor = state->wheelColor;
        const bool validColor = colorValue.isUndefined() || (colorValue.isString()
            && WheelAppearance::parseColor(colorValue.toString().toStdString(), &selectedColor));
        if (value.isDouble() && percent >= 0 && percent <= 80 && std::floor(percent) == percent
                && (sizeValue.isUndefined() || sizeValue.isDouble())
                && size >= 70 && size <= 130 && std::floor(size) == size && validColor) {
            state->wheelTransparency = int(percent);
            state->requestedWheelSize = int(size);
            state->wheelColor = selectedColor;
            applyPendingSize(state);
            render(state);
        }
        return;
    }
    if (type == QStringLiteral("arm")) { arm(state); return; }
    if (type == QStringLiteral("busy")) {
        state->busy = message.value(QStringLiteral("value")).toBool();
        xdg_toplevel_set_title(state->toplevel,
            state->tools ? (state->busy ? "MaracujaToolsBusy" : "MaracujaToolsProbe")
                         : (state->busy ? "MaracujaShiftBusy" : "MaracujaShiftProbe"));
        flush(state);
        if (state->busy) dismissLater(state);
        return;
    }
    if (type != QStringLiteral("formats") || !state->active || state->busy || state->dropFinished
        || message.value(QStringLiteral("token")).toInt(-1) != int(state->token)
        || state->paths.isEmpty()) return;
    const QJsonArray incoming = message.value(QStringLiteral("formats")).toArray();
    QJsonArray formats;
    if (incoming.size() <= 16) {
        for (const auto &value : incoming) {
            const QJsonObject object = value.toObject();
            const QString id = object.value(QStringLiteral("id")).toString();
            bool validId = !id.isEmpty() && id.size() <= 16;
            for (QChar character : id)
                validId = validId && ((character >= QLatin1Char('a') && character <= QLatin1Char('z'))
                                     || (character >= QLatin1Char('0') && character <= QLatin1Char('9')));
            if (state->tools)
                validId = id == QStringLiteral("tool:compress") || id == QStringLiteral("tool:metadata")
                    || id == QStringLiteral("tool:photo");
            if (!validId) { formats = {}; break; }
            formats.append(QJsonObject{{QStringLiteral("id"), id},
                {QStringLiteral("label"), object.value(QStringLiteral("label")).toString().left(32)},
                {QStringLiteral("detail"), object.value(QStringLiteral("detail")).toString().left(80)}});
        }
    }
    if (formats.isEmpty() || !supportsCopy(state->active)) { dismissLater(state); return; }
    cancelDismissal(state);
    stopProbeTimer(state);
    state->formats = formats;
    state->validDrag = true;
    stopAnimation(state);
    state->openingStarted = g_get_monotonic_time();
    state->openingProgress = 0;
    state->opening = true;
    changeHover(state, sectorAt(state, state->pointerX, state->pointerY));
    acceptCopy(state);
    render(state);
}

gboolean readInput(gint fd, GIOCondition condition, gpointer data)
{
    auto *state = static_cast<State *>(data);
    char buffer[4096];
    bool ended = false;
    while (true) {
        const ssize_t count = ::read(fd, buffer, sizeof buffer);
        if (count > 0) {
            state->input.append(buffer, count);
            if (state->input.size() > MaxMessageBytes) { ended = true; break; }
            qsizetype newline;
            while ((newline = state->input.indexOf('\n')) >= 0) {
                const QByteArray line = state->input.left(newline);
                state->input.remove(0, newline + 1);
                QJsonParseError error;
                const QJsonDocument document = QJsonDocument::fromJson(line, &error);
                if (error.error == QJsonParseError::NoError && document.isObject())
                    handleMessage(state, document.object());
            }
        } else if (count == 0) { ended = true; break; }
        else if (errno == EINTR) continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        else { ended = true; break; }
    }
    if (ended || (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))) {
        state->inputWatch = 0;
        quit(state);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void wmPing(void *, xdg_wm_base *base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
const xdg_wm_base_listener WmListener {wmPing};

void surfaceConfigure(void *data, xdg_surface *surface, uint32_t serial)
{
    auto *state = static_cast<State *>(data);
    xdg_surface_ack_configure(surface, serial);
    state->configured = true;
    render(state);
    if (!state->mapped) {
        state->mapped = true;
        xdg_toplevel_set_minimized(state->toplevel);
        flush(state);
    }
}
const xdg_surface_listener SurfaceListener {surfaceConfigure};

void toplevelConfigure(void *data, xdg_toplevel *, int32_t, int32_t, wl_array *states)
{
    auto *state = static_cast<State *>(data);
    bool suspended = false;
    const auto *values = static_cast<const uint32_t *>(states->data);
    for (size_t index = 0; index < states->size / sizeof(uint32_t); ++index)
        if (values[index] == XDG_TOPLEVEL_STATE_SUSPENDED) suspended = true;
    const bool resumed = state->suspended && !suspended;
    state->suspended = suspended;
    if (resumed && state->mapped) arm(state);
}
void toplevelClose(void *data, xdg_toplevel *) { dismissLater(static_cast<State *>(data)); }
void toplevelBounds(void *, xdg_toplevel *, int32_t, int32_t) {}
void toplevelCapabilities(void *, xdg_toplevel *, wl_array *) {}
const xdg_toplevel_listener ToplevelListener {
    toplevelConfigure, toplevelClose, toplevelBounds, toplevelCapabilities
};
void seatCapabilities(void *, wl_seat *, uint32_t) {}
void seatName(void *, wl_seat *, const char *) {}
const wl_seat_listener SeatListener {seatCapabilities, seatName};

void global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    auto *state = static_cast<State *>(data);
    if (!std::strcmp(interface, wl_compositor_interface.name) && !state->compositor)
        state->compositor = static_cast<wl_compositor *>(wl_registry_bind(registry, name,
            &wl_compositor_interface, std::min(version, 4U)));
    else if (!std::strcmp(interface, wl_shm_interface.name) && !state->shm)
        state->shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    else if (!std::strcmp(interface, wl_seat_interface.name) && !state->seat) {
        state->seat = static_cast<wl_seat *>(wl_registry_bind(registry, name,
            &wl_seat_interface, std::min(version, 7U)));
        // Capabilities are acknowledged as metadata only. No keyboard, pointer,
        // touch, data-control, or input-capture object is ever requested.
        wl_seat_add_listener(state->seat, &SeatListener, state);
    } else if (!std::strcmp(interface, wl_data_device_manager_interface.name)
               && !state->manager && version >= 3)
        state->manager = static_cast<wl_data_device_manager *>(wl_registry_bind(registry, name,
            &wl_data_device_manager_interface, 3));
    else if (!std::strcmp(interface, xdg_wm_base_interface.name) && !state->wmBase) {
        state->wmVersion = std::min(version, 6U);
        state->wmBase = static_cast<xdg_wm_base *>(wl_registry_bind(registry, name,
            &xdg_wm_base_interface, state->wmVersion));
        xdg_wm_base_add_listener(state->wmBase, &WmListener, state);
    }
}
void globalRemoved(void *, wl_registry *, uint32_t) {}
const wl_registry_listener RegistryListener {global, globalRemoved};

// Integrate the display socket with GLib's poll cycle. prepare_read/read_events
// avoids a blocking round-trip and correctly cancels reads when stdin or timers
// wake the loop. There is only one display reader and one default event queue.
struct DisplaySource {
    GSource source;
    GPollFD poll;
    State *state;
    bool reading;
};
gboolean displayPrepare(GSource *source, gint *timeout)
{
    auto *display = reinterpret_cast<DisplaySource *>(source);
    State *state = display->state;
    *timeout = -1;
    if (display->reading) {
        wl_display_cancel_read(state->display);
        display->reading = false;
    }
    while (wl_display_prepare_read(state->display) != 0) {
        if (wl_display_dispatch_pending(state->display) < 0) { quit(state, true); return false; }
    }
    display->reading = true;
    display->poll.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
    if (wl_display_flush(state->display) < 0) {
        if (errno == EAGAIN) display->poll.events |= G_IO_OUT;
        else quit(state, true);
    }
    return false;
}
gboolean displayCheck(GSource *source)
{
    auto *display = reinterpret_cast<DisplaySource *>(source);
    if (display->reading) {
        if (display->poll.revents & G_IO_IN) {
            if (wl_display_read_events(display->state->display) < 0) quit(display->state, true);
        } else wl_display_cancel_read(display->state->display);
        display->reading = false;
    }
    return display->poll.revents != 0;
}
gboolean displayDispatch(GSource *source, GSourceFunc, gpointer)
{
    auto *display = reinterpret_cast<DisplaySource *>(source);
    State *state = display->state;
    if (display->poll.revents & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        quit(state, true);
        return G_SOURCE_CONTINUE;
    }
    if (wl_display_dispatch_pending(state->display) < 0) quit(state, true);
    flush(state);
    return G_SOURCE_CONTINUE;
}
void displayFinalize(GSource *source)
{
    auto *display = reinterpret_cast<DisplaySource *>(source);
    if (display->reading) wl_display_cancel_read(display->state->display);
}
GSourceFuncs DisplaySourceFunctions {
    displayPrepare, displayCheck, displayDispatch, displayFinalize, nullptr, nullptr
};

gboolean signalQuit(gpointer data)
{
    quit(static_cast<State *>(data));
    return G_SOURCE_CONTINUE;
}

bool initialize(State *state)
{
    state->display = wl_display_connect(nullptr);
    if (!state->display) return false;
    state->registry = wl_display_get_registry(state->display);
    wl_registry_add_listener(state->registry, &RegistryListener, state);
    if (wl_display_roundtrip(state->display) < 0 || !state->compositor || !state->shm
        || !state->seat || !state->manager || !state->wmBase) return false;
    for (auto &buffer : state->buffers) if (!createBuffer(state, buffer)) return false;
    state->device = wl_data_device_manager_get_data_device(state->manager, state->seat);
    wl_data_device_add_listener(state->device, &DeviceListener, state);
    state->surface = wl_compositor_create_surface(state->compositor);
    state->xdgSurface = xdg_wm_base_get_xdg_surface(state->wmBase, state->surface);
    xdg_surface_add_listener(state->xdgSurface, &SurfaceListener, state);
    state->toplevel = xdg_surface_get_toplevel(state->xdgSurface);
    xdg_toplevel_add_listener(state->toplevel, &ToplevelListener, state);
    xdg_toplevel_set_title(state->toplevel, state->tools ? "MaracujaToolsProbe" : "MaracujaShiftProbe");
    xdg_toplevel_set_app_id(state->toplevel, "org.winux.Mandarina");
    updateWindowGeometry(state);
    wl_surface_commit(state->surface);
    if (wl_display_roundtrip(state->display) < 0 || state->failed) return false;
    auto *source = reinterpret_cast<DisplaySource *>(g_source_new(&DisplaySourceFunctions, sizeof(DisplaySource)));
    source->state = state;
    source->reading = false;
    source->poll.fd = wl_display_get_fd(state->display);
    source->poll.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
    source->poll.revents = 0;
    g_source_add_poll(&source->source, &source->poll);
    state->waylandSource = &source->source;
    g_source_attach(state->waylandSource, nullptr);
    return true;
}

void cleanup(State *state)
{
    stopAnimation(state);
    if (state->waylandSource) {
        g_source_destroy(state->waylandSource);
        g_source_unref(state->waylandSource);
    }
    if (state->inputWatch) g_source_remove(state->inputWatch);
    if (state->signalInt) g_source_remove(state->signalInt);
    if (state->signalTerm) g_source_remove(state->signalTerm);
    stopProbeTimer(state);
    cancelDismissal(state);
    cancelUriRead(state);
    state->configured = false;
    while (!state->offers.isEmpty()) destroyOffer(state, state->offers.begin().value());
    for (auto &buffer : state->buffers) {
        if (buffer.object) wl_buffer_destroy(buffer.object);
        if (buffer.canvas) cairo_surface_destroy(buffer.canvas);
        if (buffer.pixels != MAP_FAILED) munmap(buffer.pixels, BufferBytes);
    }
    if (state->toplevel) xdg_toplevel_destroy(state->toplevel);
    if (state->xdgSurface) xdg_surface_destroy(state->xdgSurface);
    if (state->surface) wl_surface_destroy(state->surface);
    if (state->device) wl_data_device_release(state->device);
    if (state->manager) wl_data_device_manager_destroy(state->manager);
    if (state->seat) {
        if (wl_seat_get_version(state->seat) >= 5) wl_seat_release(state->seat);
        else wl_seat_destroy(state->seat);
    }
    if (state->shm) wl_shm_destroy(state->shm);
    if (state->compositor) wl_compositor_destroy(state->compositor);
    if (state->wmBase) xdg_wm_base_destroy(state->wmBase);
    if (state->registry) wl_registry_destroy(state->registry);
    if (state->display) { wl_display_flush(state->display); wl_display_disconnect(state->display); }
}

cairo_status_t writePreview(void *data, const unsigned char *bytes, unsigned int length)
{
    const int fd = *static_cast<int *>(data);
    size_t written = 0;
    while (written < length) {
        const ssize_t count = ::write(fd, bytes + written, length - written);
        if (count > 0) written += size_t(count);
        else if (count < 0 && errno == EINTR) continue;
        else return CAIRO_STATUS_WRITE_ERROR;
    }
    return CAIRO_STATUS_SUCCESS;
}

int preview(const char *path, const char *frame = "idle", bool tools = false)
{
    const QFileInfo output(QString::fromLocal8Bit(path));
    if (!output.isAbsolute() || output.exists()) return 2;
    if (std::strcmp(frame, "idle") && std::strcmp(frame, "hover") && std::strcmp(frame, "opening")) return 2;
    int fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return 2;
    State state;
    state.tools = tools;
    if (tools) {
        state.formats.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("tool:compress")},
            {QStringLiteral("label"), QStringLiteral("Comprimir")},
            {QStringLiteral("detail"), QStringLiteral("Reduzir tamanho")}});
        state.formats.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("tool:metadata")},
            {QStringLiteral("label"), QStringLiteral("Metadados")},
            {QStringLiteral("detail"), QStringLiteral("Inspecionar e editar")}});
        state.formats.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("tool:photo")},
            {QStringLiteral("label"), QStringLiteral("Editar foto")},
            {QStringLiteral("detail"), QStringLiteral("Luz, cor e detalhes")}});
    } else for (const auto &pair : {std::pair<const char *, const char *>("png", "Imagem"),
                             {"jpg", "Fotografia"}, {"webp", "Imagem compacta"}, {"pdf", "Documento"}})
        state.formats.append(QJsonObject{{QStringLiteral("id"), QLatin1String(pair.first)},
            {QStringLiteral("label"), QString::fromLatin1(pair.first).toUpper()},
            {QStringLiteral("detail"), QString::fromUtf8(pair.second)}});
    state.validDrag = true;
    state.openingProgress = !std::strcmp(frame, "opening") ? 0.48 : 1;
    if (!std::strcmp(frame, "hover")) {
        state.hovered = 0;
        state.hoverLevels[0] = 1;
    }
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, WindowSize, WindowSize);
    cairo_t *cr = cairo_create(surface);
    drawWheel(&state, cr);
    cairo_destroy(cr);
    const cairo_status_t result = cairo_surface_write_to_png_stream(surface, writePreview, &fd);
    cairo_surface_destroy(surface);
    const bool success = ::close(fd) == 0 && result == CAIRO_STATUS_SUCCESS;
    if (!success) ::unlink(path);
    return success ? 0 : 2;
}

} // namespace

int main(int argc, char **argv)
{
    std::signal(SIGPIPE, SIG_IGN);
    const bool tools = argc > 1 && !std::strcmp(argv[1], "--tools");
    const int first = tools ? 2 : 1;
    if (argc == first + 2 && !std::strcmp(argv[first], "--preview"))
        return preview(argv[first + 1], "idle", tools);
    if (argc == first + 4 && !std::strcmp(argv[first], "--preview") && !std::strcmp(argv[first + 2], "--preview-frame"))
        return preview(argv[first + 1], argv[first + 3], tools);
    if (argc != first) return 2;
    g_set_prgname("org.winux.Mandarina");
    g_set_application_name("Maracujá");
    const int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) return 2;
    State state;
    state.tools = tools;
    state.loop = g_main_loop_new(nullptr, false);
    if (!initialize(&state)) {
        std::fputs("Maracujá: não foi possível iniciar o alvo Wayland de arrasto.\n", stderr);
        cleanup(&state);
        g_main_loop_unref(state.loop);
        return 2;
    }
    state.inputWatch = g_unix_fd_add(STDIN_FILENO,
        GIOCondition(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL), readInput, &state);
    state.signalInt = g_unix_signal_add(SIGINT, signalQuit, &state);
    state.signalTerm = g_unix_signal_add(SIGTERM, signalQuit, &state);
    g_main_loop_run(state.loop);
    cleanup(&state);
    g_main_loop_unref(state.loop);
    return state.failed ? 2 : 0;
}
