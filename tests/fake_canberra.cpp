#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {
std::mutex mutex;
std::condition_variable changed;
int counts[5] = {}; // create, play, destroy, cancel, properties
int errors = 0;
bool blockInitialization = false;
bool failNextPlay = false;
std::string filename;

bool fileExists(const std::string &path)
{
    struct stat info {};
    return !path.empty() && stat(path.c_str(), &info) == 0;
}

void validateWave(const std::string &path)
{
    struct stat info {};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)
        || (info.st_mode & 0777) != 0600) {
        ++errors;
        return;
    }
    std::ifstream file(path, std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
    constexpr std::size_t samples = 24000 * 48 / 1000;
    if (bytes.size() != 44 + samples * 2) {
        ++errors;
        return;
    }
    auto word = [&bytes](int offset) -> std::uint32_t {
        return bytes[offset] | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8);
    };
    auto dword = [&bytes](int offset) -> std::uint32_t {
        return bytes[offset] | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
            | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
            | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    };
    if (std::memcmp(bytes.data(), "RIFF", 4) != 0
        || dword(4) != bytes.size() - 8
        || std::memcmp(bytes.data() + 8, "WAVEfmt ", 8) != 0
        || dword(16) != 16 || word(20) != 1 || word(22) != 1
        || dword(24) != 24000 || dword(28) != 48000 || word(32) != 2
        || word(34) != 16 || std::memcmp(bytes.data() + 36, "data", 4) != 0
        || dword(40) != samples * 2 || word(44) != 0
        || word(static_cast<int>(bytes.size()) - 2) != 0)
        ++errors;
    int peak = 0;
    for (std::size_t i = 0; i < samples; ++i) {
        const int encoded = static_cast<int>(word(44 + static_cast<int>(i) * 2));
        const int sample = encoded >= 32768 ? encoded - 65536 : encoded;
        const int magnitude = sample < 0 ? -sample : sample;
        if (magnitude > peak)
            peak = magnitude;
    }
    if (peak < 100 || peak > 8192)
        ++errors;
}
} // namespace

// Only this test target links this library. It has the real library's SONAME
// and ABI but deliberately has no output device, driver, or audio operations.
struct ca_context { std::string filename; };

extern "C" int ca_context_create(ca_context **context)
{
    std::unique_lock<std::mutex> lock(mutex);
    ++counts[0];
    changed.notify_all();
    changed.wait(lock, [] { return !blockInitialization; });
    *context = new ca_context;
    return 0;
}

extern "C" int ca_context_change_props(ca_context *, ...)
{
    std::lock_guard<std::mutex> lock(mutex);
    ++counts[4];
    return 0;
}

extern "C" int ca_context_play(ca_context *context, std::uint32_t id, ...)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (id != 1)
        ++errors;
    va_list args;
    va_start(args, id);
    const char *key = nullptr;
    std::string path;
    while ((key = va_arg(args, const char *)) != nullptr) {
        const char *value = va_arg(args, const char *);
        if (std::strcmp(key, "media.filename") == 0 && value)
            path = value;
    }
    va_end(args);
    if (path.empty())
        ++errors;
    else
        validateWave(path);
    filename = path;
    context->filename = path;
    ++counts[1];
    changed.notify_all();
    if (failNextPlay) {
        failNextPlay = false;
        return -1;
    }
    return 0;
}

extern "C" int ca_context_cancel(ca_context *context, std::uint32_t id)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (id != 1)
        ++errors;
    // On the first cancel the sample has not yet been passed to play().
    // Afterward, cancellation must run before its scoped file is removed.
    if (!context->filename.empty() && !fileExists(context->filename))
        ++errors;
    ++counts[3];
    return 0;
}

extern "C" int ca_context_destroy(ca_context *context)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!fileExists(context->filename))
        ++errors;
    delete context;
    ++counts[2];
    changed.notify_all();
    return 0;
}

extern "C" void fakeAudioBlockInitialization(int block)
{
    std::lock_guard<std::mutex> lock(mutex);
    blockInitialization = block != 0;
    changed.notify_all();
}

extern "C" int fakeAudioCount(int kind)
{
    std::lock_guard<std::mutex> lock(mutex);
    return counts[kind];
}

extern "C" int fakeAudioWait(int kind, int count, int timeoutMs)
{
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), [=] {
        return counts[kind] >= count;
    });
}

extern "C" int fakeAudioErrors()
{
    std::lock_guard<std::mutex> lock(mutex);
    return errors;
}

extern "C" int fakeAudioFileExists()
{
    std::lock_guard<std::mutex> lock(mutex);
    return fileExists(filename);
}

extern "C" void fakeAudioFailNextPlay()
{
    std::lock_guard<std::mutex> lock(mutex);
    failNextPlay = true;
}
