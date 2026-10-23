#define _POSIX_C_SOURCE 200809L
#include <wayland-client.h>
#include "fake-input-client-protocol.h"
#include <linux/input-event-codes.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static struct wl_display *display;
static struct org_kde_kwin_fake_input *fake;
static uint32_t fake_version;
static int button_down;
static unsigned char key_down[KEY_MAX + 1];

static int key_for_command(const char *command)
{
    // Explicit fixture chords only. This is not a general desktop typing tool.
    if (strcmp(command, "shift") == 0) return KEY_LEFTSHIFT;
    if (strcmp(command, "ctrl") == 0) return KEY_LEFTCTRL;
    if (strcmp(command, "alt") == 0) return KEY_LEFTALT;
    if (strcmp(command, "meta") == 0) return KEY_LEFTMETA;
    if (strcmp(command, "m") == 0) return KEY_M;
    if (strcmp(command, "f8") == 0) return KEY_F8;
    if (strcmp(command, "f9") == 0) return KEY_F9;
    return -1;
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    (void)data;
    if (!fake && strcmp(interface, "org_kde_kwin_fake_input") == 0) {
        fake_version = version < 6 ? version : 6;
        fake = wl_registry_bind(registry, name, &org_kde_kwin_fake_input_interface, fake_version);
    }
}
static void registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener listener = {registry_global, registry_remove};

static int synchronize(void)
{
    return wl_display_flush(display) < 0 || wl_display_roundtrip(display) < 0 ? -1 : 0;
}

static int step(const char *line)
{
    char command[32] = {0};
    char state[16] = {0};
    double x, y;
    unsigned ms;
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') return 0;
    if (sscanf(line, "%31s", command) != 1) return 0;
    if (strcmp(command, "move") == 0 && sscanf(line, "%*s %lf %lf", &x, &y) == 2
        && isfinite(x) && isfinite(y) && x >= 0 && y >= 0 && x <= 16384 && y <= 16384) {
        org_kde_kwin_fake_input_pointer_motion_absolute(fake, wl_fixed_from_double(x), wl_fixed_from_double(y));
    } else if ((strcmp(command, "button") == 0 || key_for_command(command) >= 0)
               && sscanf(line, "%*s %15s", state) == 1
               && (strcmp(state, "down") == 0 || strcmp(state, "up") == 0)) {
        const uint32_t pressed = strcmp(state, "down") == 0;
        if (strcmp(command, "button") == 0) {
            button_down = (int)pressed;
            org_kde_kwin_fake_input_button(fake, BTN_LEFT, pressed);
        } else {
            const int key = key_for_command(command);
            key_down[key] = (unsigned char)pressed;
            org_kde_kwin_fake_input_keyboard_key(fake, (uint32_t)key, pressed);
        }
    } else if (strcmp(command, "wait") == 0 && sscanf(line, "%*s %u", &ms) == 1 && ms <= 5000) {
        struct timespec remaining = {ms / 1000, (long)(ms % 1000) * 1000000L};
        if (synchronize() < 0) return -1;
        while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR) {}
    } else if (strcmp(command, "quit") == 0) {
        return 1;
    } else {
        fprintf(stderr, "invalid isolated input command: %s", line);
        return -1;
    }
    if (synchronize() < 0) return -1;
    printf("ok %s\n", command);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    const char *socket = getenv("WAYLAND_DISPLAY");
    const char *isolated = getenv("MANDARINA_ISOLATED_TEST");
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char prefix[] = "mandarina-test-";
    struct stat info;
    if (!isolated || strcmp(isolated, "1") != 0 || !socket
        || strncmp(socket, prefix, sizeof(prefix) - 1) != 0 || !socket[sizeof(prefix) - 1]
        || strchr(socket, '/') || !runtime || runtime[0] != '/'
        || stat(runtime, &info) != 0 || !S_ISDIR(info.st_mode)
        || info.st_uid != getuid() || (info.st_mode & 077) != 0) {
        fprintf(stderr, "Refusing input: a private mandarina-test-* socket and 0700 runtime are required.\n");
        return 2;
    }
    alarm(25); // Bounds this helper only if the private compositor stops replying.
    display = wl_display_connect(NULL);
    if (!display) { perror("private Wayland connection"); return 2; }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &listener, NULL);
    if (wl_display_roundtrip(display) < 0 || !fake || fake_version < 4) {
        fprintf(stderr, "Private compositor does not provide fake-input version 4+.\n");
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return 2;
    }
    org_kde_kwin_fake_input_authenticate(fake, "Mandarina isolated test",
        "Input for the fixture in this private test compositor only");
    int result = synchronize() < 0 ? 1 : 0;
    if (!result && argc > 1) {
        char line[256] = {0};
        size_t used = 0;
        for (int i = 1; i < argc; ++i) {
            const int written = snprintf(line + used, sizeof(line) - used, "%s%s", i == 1 ? "" : " ", argv[i]);
            if (written < 0 || (size_t)written >= sizeof(line) - used) { result = 2; break; }
            used += (size_t)written;
        }
        if (!result && step(line) < 0) result = 1;
    } else if (!result) {
        char line[256];
        while (fgets(line, sizeof(line), stdin)) {
            const int status = step(line);
            if (status != 0) { if (status < 0) result = 1; break; }
        }
    }
    for (int key = KEY_MAX; key >= 0; --key)
        if (key_down[key]) org_kde_kwin_fake_input_keyboard_key(fake, (uint32_t)key, 0);
    if (button_down) org_kde_kwin_fake_input_button(fake, BTN_LEFT, 0);
    synchronize();
    if (fake_version >= 5) org_kde_kwin_fake_input_destroy(fake);
    else wl_proxy_destroy((struct wl_proxy *)fake);
    wl_registry_destroy(registry);
    wl_display_flush(display);
    wl_display_disconnect(display);
    return result;
}
