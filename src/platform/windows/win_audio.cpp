#include "platform/windows/win_audio.hpp"

#include "platform/windows/win_hotkey.hpp"
#include "platform/windows/win_raii.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace shine::platform::win {

namespace {

struct CoTaskMemFreeDeleter {
    void operator()(WAVEFORMATEX* p) const noexcept {
        if (p != nullptr) {
            ::CoTaskMemFree(p);
        }
    }
};

class ScopedCaptureRelease {
public:
    ScopedCaptureRelease(IAudioCaptureClient* client, UINT32 frames) noexcept
        : client_(client), frames_(frames) {}
    ~ScopedCaptureRelease() noexcept {
        if (client_ != nullptr) {
            client_->ReleaseBuffer(frames_);
        }
    }
    ScopedCaptureRelease(const ScopedCaptureRelease&) = delete;
    ScopedCaptureRelease& operator=(const ScopedCaptureRelease&) = delete;

private:
    IAudioCaptureClient* client_{nullptr};
    UINT32 frames_{0};
};

} // namespace

WasapiMicCapture::~WasapiMicCapture() {
    stop();
}

core::Result<void> WasapiMicCapture::start(
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

    const unsigned int pttVk = win_hotkey_to_vk(pttKey);
    std::promise<core::Result<void>> initPromise;
    auto initFuture = initPromise.get_future();

    worker_ = std::thread(
        &WasapiMicCapture::worker_main,
        this,
        std::move(onChunk),
        clockEpoch,
        pttEnabled,
        pttVk,
        std::move(initPromise));

    core::Result<void> res = initFuture.get();
    if (!res) {
        stop();
    }
    return res;
}

void WasapiMicCapture::stop() {
    stopRequested_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false, std::memory_order_release);
}

void WasapiMicCapture::worker_main(
    std::function<void(core::AudioPcmChunk)> onChunk,
    std::chrono::steady_clock::time_point clockEpoch,
    bool pttEnabled,
    unsigned int pttVk,
    std::promise<core::Result<void>> initPromise) {
    ComScope comScope;
    if (!comScope.ok()) {
        initPromise.set_value(core::Result<void>::make_err(
            core::ErrorCode::InternalError,
            "Audio CoInitializeEx failed: " + hr_to_string(comScope.hr())));
        return;
    }

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = ::CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        initPromise.set_value(core::Result<void>::make_err(
            core::ErrorCode::NotSupported,
            "CoCreateInstance(MMDeviceEnumerator) failed: " + hr_to_string(hr)));
        return;
    }

    Microsoft::WRL::ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    if (FAILED(hr) || !device) {
        initPromise.set_value(core::Result<void>::make_err(
            core::ErrorCode::NotSupported,
            "No default audio capture endpoint: " + hr_to_string(hr)));
        return;
    }

    Microsoft::WRL::ComPtr<IAudioClient> audioClient;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audioClient);
    if (FAILED(hr) || !audioClient) {
        initPromise.set_value(core::Result<void>::make_err(
            core::ErrorCode::NotSupported,
            "IMMDevice::Activate(IAudioClient) failed: " + hr_to_string(hr)));
        return;
    }

    WAVEFORMATEX targetFmt{};
    targetFmt.wFormatTag = WAVE_FORMAT_PCM;
    targetFmt.nChannels = 1;
    targetFmt.nSamplesPerSec = 48000;
    targetFmt.wBitsPerSample = 16;
    targetFmt.nBlockAlign = 2;
    targetFmt.nAvgBytesPerSec = 96000;
    targetFmt.cbSize = 0;

    bool directPcm48k = false;
    bool mixIsFloat = false;
    UINT32 mixChannels = 1;
    UINT32 mixSampleRate = 48000;
    UINT32 mixBits = 16;

    const DWORD autoFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    constexpr REFERENCE_TIME kBufferDuration100Ns = 1'000'000; // 100ms

    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED, autoFlags, kBufferDuration100Ns, 0, &targetFmt, nullptr);
    if (SUCCEEDED(hr)) {
        directPcm48k = true;
    } else {
        WAVEFORMATEX* rawMix = nullptr;
        hr = audioClient->GetMixFormat(&rawMix);
        if (FAILED(hr) || rawMix == nullptr) {
            initPromise.set_value(core::Result<void>::make_err(
                core::ErrorCode::NotSupported,
                "IAudioClient::GetMixFormat failed: " + hr_to_string(hr)));
            return;
        }
        std::unique_ptr<WAVEFORMATEX, CoTaskMemFreeDeleter> mixGuard(rawMix);

        mixChannels = std::max<UINT32>(1, rawMix->nChannels);
        mixSampleRate = std::max<UINT32>(8000, rawMix->nSamplesPerSec);
        mixBits = rawMix->wBitsPerSample;

        if (rawMix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            mixIsFloat = true;
        } else if (rawMix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                   rawMix->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(rawMix);
            mixIsFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
        }

        hr = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration100Ns, 0, rawMix, nullptr);
        if (FAILED(hr)) {
            initPromise.set_value(core::Result<void>::make_err(
                core::ErrorCode::NotSupported,
                "IAudioClient::Initialize failed: " + hr_to_string(hr)));
            return;
        }
    }

    Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient;
    hr = audioClient->GetService(IID_PPV_ARGS(&captureClient));
    if (FAILED(hr) || !captureClient) {
        initPromise.set_value(core::Result<void>::make_err(
            core::ErrorCode::NotSupported,
            "GetService(IAudioCaptureClient) failed: " + hr_to_string(hr)));
        return;
    }

    hr = audioClient->Start();
    if (FAILED(hr)) {
        initPromise.set_value(core::Result<void>::make_err(
            core::ErrorCode::NotSupported,
            "IAudioClient::Start failed: " + hr_to_string(hr)));
        return;
    }

    running_.store(true, std::memory_order_release);
    initPromise.set_value(core::Result<void>::make_ok());

    while (!stopRequested_.load(std::memory_order_acquire)) {
        UINT32 packetFrames = 0;
        hr = captureClient->GetNextPacketSize(&packetFrames);
        if (FAILED(hr)) {
            break;
        }

        if (packetFrames == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        while (packetFrames > 0 && !stopRequested_.load(std::memory_order_acquire)) {
            BYTE* data = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;

            hr = captureClient->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                break;
            }
            ScopedCaptureRelease releaseGuard(captureClient.Get(), numFrames);

            if (numFrames > 0) {
                const bool pttMuted =
                    pttEnabled && pttVk != 0U && ((::GetAsyncKeyState(static_cast<int>(pttVk)) & 0x8000) == 0);
                const bool silent = pttMuted || ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) || (data == nullptr);

                const auto now = std::chrono::steady_clock::now();
                const std::int64_t nowUs =
                    std::chrono::duration_cast<std::chrono::microseconds>(now - clockEpoch).count();
                const std::int64_t chunkDurUs =
                    (static_cast<std::int64_t>(numFrames) * 1'000'000LL) /
                    static_cast<std::int64_t>(directPcm48k ? 48000U : mixSampleRate);

                core::AudioPcmChunk chunk{};
                chunk.sampleRate = 48000U;
                chunk.channels = 1U;
                chunk.ptsUs = std::max<std::int64_t>(0, nowUs - chunkDurUs);

                if (directPcm48k) {
                    chunk.samples.resize(numFrames, 0);
                    if (!silent) {
                        std::memcpy(chunk.samples.data(), data, numFrames * sizeof(std::int16_t));
                    }
                } else {
                    const std::size_t outFrames = std::max<std::size_t>(
                        1U,
                        (static_cast<std::size_t>(numFrames) * 48000ULL) / mixSampleRate);
                    chunk.samples.resize(outFrames, 0);
                    if (!silent) {
                        std::vector<float> monoIn(numFrames, 0.0f);
                        for (UINT32 f = 0; f < numFrames; ++f) {
                            float acc = 0.0f;
                            for (UINT32 c = 0; c < mixChannels; ++c) {
                                const std::size_t idx = static_cast<std::size_t>(f) * mixChannels + c;
                                if (mixIsFloat && mixBits == 32) {
                                    acc += reinterpret_cast<const float*>(data)[idx];
                                } else if (mixBits == 16) {
                                    acc += static_cast<float>(
                                               reinterpret_cast<const std::int16_t*>(data)[idx]) /
                                           32768.0f;
                                }
                            }
                            monoIn[f] = acc / static_cast<float>(mixChannels);
                        }
                        for (std::size_t o = 0; o < outFrames; ++o) {
                            const double srcPos =
                                (static_cast<double>(o) * static_cast<double>(numFrames - 1U)) /
                                static_cast<double>(std::max<std::size_t>(1U, outFrames - 1U));
                            const auto i0 = static_cast<std::size_t>(srcPos);
                            const auto i1 = std::min<std::size_t>(i0 + 1U, numFrames - 1U);
                            const float frac = static_cast<float>(srcPos - static_cast<double>(i0));
                            const float sample = monoIn[i0] + (monoIn[i1] - monoIn[i0]) * frac;
                            const int clamped = static_cast<int>(
                                std::clamp(std::lround(sample * 32767.0f), -32768L, 32767L));
                            chunk.samples[o] = static_cast<std::int16_t>(clamped);
                        }
                    }
                }

                onChunk(std::move(chunk));
            }

            hr = captureClient->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) {
                break;
            }
        }
    }

    (void)audioClient->Stop();
    running_.store(false, std::memory_order_release);
}

} // namespace shine::platform::win
