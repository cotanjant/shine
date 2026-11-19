#include "platform/linux/linux_audio.hpp"

#include "platform/linux/linux_hotkey.hpp"
#include "platform/linux/linux_raii.hpp"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cerrno>
#include <memory>
#include <vector>

namespace shine::platform::linux_impl {

namespace {

struct SndPcmDeleter {
    void operator()(snd_pcm_t* handle) const noexcept {
        if (handle != nullptr) {
            ::snd_pcm_close(handle);
        }
    }
};
using SndPcmPtr = std::unique_ptr<snd_pcm_t, SndPcmDeleter>;

[[nodiscard]] bool is_x11_key_pressed(Display* dpy, KeyCode keycode) noexcept {
    if (dpy == nullptr || keycode == 0) {
        return false;
    }
    char keys[32]{};
    ::XQueryKeymap(dpy, keys);
    const std::size_t byteIdx = static_cast<std::size_t>(keycode) / 8U;
    const unsigned int bitMask = 1U << (static_cast<unsigned int>(keycode) % 8U);
    return byteIdx < sizeof(keys) && (static_cast<unsigned char>(keys[byteIdx]) & bitMask) != 0U;
}

} // namespace

AlsaMicCapture::~AlsaMicCapture() {
    stop();
}

core::Result<void> AlsaMicCapture::start(
    std::function<void(core::AudioPcmChunk)> onChunk,
    std::chrono::steady_clock::time_point clockEpoch,
    bool pttEnabled,
    std::string pttKey) {
    if (running_.load(std::memory_order_acquire)) {
        return core::Result<void>::make_ok();
    }
    if (!onChunk) {
        return core::Result<void>::make_err(
            core::ErrorCode::InvalidArgument, "Audio chunk callback must not be empty");
    }

    stop();
    stopRequested_.store(false, std::memory_order_release);

    std::promise<core::Result<void>> initPromise;
    auto initFuture = initPromise.get_future();

    worker_ = std::thread(
        &AlsaMicCapture::worker_main,
        this,
        std::move(onChunk),
        clockEpoch,
        pttEnabled,
        std::move(pttKey),
        std::move(initPromise));

    core::Result<void> res = initFuture.get();
    if (!res) {
        stop();
    }
    return res;
}

void AlsaMicCapture::stop() {
    stopRequested_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false, std::memory_order_release);
}

void AlsaMicCapture::worker_main(
    std::function<void(core::AudioPcmChunk)> onChunk,
    std::chrono::steady_clock::time_point clockEpoch,
    bool pttEnabled,
    std::string pttKey,
    std::promise<core::Result<void>> initPromise) {
    snd_pcm_t* rawPcm = nullptr;
    int err = ::snd_pcm_open(&rawPcm, "default", SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0 || rawPcm == nullptr) {
        initPromise.set_value(core::Result<void>::make_err(
            core::ErrorCode::NotSupported,
            std::string("snd_pcm_open failed: ") + ::snd_strerror(err)));
        return;
    }
    SndPcmPtr pcm(rawPcm);

    unsigned int sampleRate = 48000U;
    err = ::snd_pcm_set_params(
        pcm.get(),
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED,
        1U,
        sampleRate,
        1,
        20000); // 20 ms latency
    if (err < 0) {
        initPromise.set_value(core::Result<void>::make_err(
            core::ErrorCode::NotSupported,
            std::string("snd_pcm_set_params failed: ") + ::snd_strerror(err)));
        return;
    }

    XDisplayPtr pttDpy;
    KeyCode pttKeycode = 0;
    if (pttEnabled) {
        const KeySym sym = linux_hotkey_to_keysym(pttKey);
        if (sym != 0UL) {
            pttDpy.reset(::XOpenDisplay(nullptr));
            if (pttDpy) {
                pttKeycode = ::XKeysymToKeycode(pttDpy.get(), sym);
            }
        }
    }

    running_.store(true, std::memory_order_release);
    initPromise.set_value(core::Result<void>::make_ok());

    constexpr snd_pcm_uframes_t kPeriodFrames = 960; // 20ms @ 48kHz
    std::vector<std::int16_t> pcmBuf(kPeriodFrames, 0);

    while (!stopRequested_.load(std::memory_order_acquire)) {
        const snd_pcm_sframes_t readFrames = ::snd_pcm_readi(pcm.get(), pcmBuf.data(), kPeriodFrames);
        if (readFrames < 0) {
            if (readFrames == -EPIPE || readFrames == -ESTRPIPE) {
                ::snd_pcm_recover(pcm.get(), static_cast<int>(readFrames), 1);
                continue;
            }
            if (readFrames == -EAGAIN || readFrames == -EINTR) {
                continue;
            }
            break;
        }
        if (readFrames == 0) {
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        const std::int64_t nowUs =
            std::chrono::duration_cast<std::chrono::microseconds>(now - clockEpoch).count();
        const std::int64_t durUs = (static_cast<std::int64_t>(readFrames) * 1'000'000LL) / sampleRate;

        const bool muted = pttEnabled && pttKeycode != 0 && !is_x11_key_pressed(pttDpy.get(), pttKeycode);

        core::AudioPcmChunk chunk{};
        chunk.sampleRate = sampleRate;
        chunk.channels = 1U;
        chunk.ptsUs = std::max<std::int64_t>(0, nowUs - durUs);
        if (muted) {
            chunk.samples.assign(static_cast<std::size_t>(readFrames), 0);
        } else {
            chunk.samples.assign(pcmBuf.begin(), pcmBuf.begin() + readFrames);
        }

        onChunk(std::move(chunk));
    }

    running_.store(false, std::memory_order_release);
}

} // namespace shine::platform::linux_impl
