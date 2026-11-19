#include "shine/core/clip_library.hpp"
#include "shine/core/hotkey.hpp"
#include "shine/core/mp4_validator.hpp"
#include "shine/core/replay_buffer.hpp"
#include "shine/core/result.hpp"
#include "shine/core/settings.hpp"
#include "shine/core/types.hpp"
#include "shine/ipc/protocol.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

namespace {

int g_failures = 0;

#define SHINE_CHECK(cond)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "]: "     \
                      << #cond << '\n';                                       \
            ++g_failures;                                                     \
        }                                                                     \
    } while (false)

void write_be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
}

std::vector<std::uint8_t> make_box(const char (&type)[5], const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> box;
    write_be32(box, static_cast<std::uint32_t>(8U + payload.size()));
    box.push_back(static_cast<std::uint8_t>(type[0]));
    box.push_back(static_cast<std::uint8_t>(type[1]));
    box.push_back(static_cast<std::uint8_t>(type[2]));
    box.push_back(static_cast<std::uint8_t>(type[3]));
    box.insert(box.end(), payload.begin(), payload.end());
    return box;
}

void append_bytes(std::vector<std::uint8_t>& dst, const std::vector<std::uint8_t>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

void test_result_and_config() {
    auto okVal = shine::core::Result<int>::make_ok(42);
    SHINE_CHECK(okVal.ok());
    SHINE_CHECK(static_cast<bool>(okVal));
    SHINE_CHECK(okVal.value() == 42);

    auto ptrRes = shine::core::Result<std::unique_ptr<int>>::make_ok(std::make_unique<int>(99));
    SHINE_CHECK(ptrRes.ok());
    auto taken = std::move(ptrRes).take();
    SHINE_CHECK(taken != nullptr && *taken == 99);

    auto errRes = shine::core::Result<void>::make_err(
        shine::core::ErrorCode::InvalidArgument, "bad input");
    SHINE_CHECK(!errRes.ok());
    SHINE_CHECK(errRes.error().code == shine::core::ErrorCode::InvalidArgument);
    SHINE_CHECK(errRes.error().message == "bad input");

    shine::core::RecordingConfig cfg{};
    SHINE_CHECK(!cfg.validate().ok());

    cfg.outputPath = "out.mp4";
    SHINE_CHECK(cfg.validate().ok());

    cfg.fps = 0;
    SHINE_CHECK(!cfg.validate().ok());
    cfg.fps = 241;
    SHINE_CHECK(!cfg.validate().ok());
    cfg.fps = 60;

    cfg.bitrateKbps = 499;
    SHINE_CHECK(!cfg.validate().ok());
    cfg.bitrateKbps = 8000;
    SHINE_CHECK(cfg.validate().ok());
}

void test_ipc_roundtrip() {
    {
        shine::ipc::IpcRequest req{};
        req.command = shine::ipc::IpcCommand::Start;
        shine::core::RecordingConfig cfg{};
        cfg.outputPath = std::filesystem::path(u8"C:\\Users\\Test User\\Videos\\Shine\\clip 01_ü.mp4");
        cfg.displayIndex = 1;
        cfg.fps = 60;
        cfg.bitrateKbps = 12000;
        cfg.captureCursor = false;
        cfg.preferGpu = true;
        req.startConfig = cfg;

        const std::string wire = shine::ipc::serialize_request(req);
        auto parsed = shine::ipc::parse_request(wire);
        SHINE_CHECK(parsed.ok());
        if (parsed.ok()) {
            SHINE_CHECK(parsed.value().command == shine::ipc::IpcCommand::Start);
            SHINE_CHECK(parsed.value().startConfig.has_value());
            SHINE_CHECK(parsed.value().startConfig->outputPath == cfg.outputPath);
            SHINE_CHECK(parsed.value().startConfig->displayIndex == 1U);
            SHINE_CHECK(parsed.value().startConfig->fps == 60U);
            SHINE_CHECK(parsed.value().startConfig->bitrateKbps == 12000U);
            SHINE_CHECK(!parsed.value().startConfig->captureCursor);
            SHINE_CHECK(parsed.value().startConfig->preferGpu);
        }
    }

    {
        shine::ipc::IpcResponse resp{};
        resp.ok = true;
        shine::core::RecordingStats st{};
        st.state = shine::core::RecorderState::Recording;
        st.elapsedMs = 1520;
        st.framesEncoded = 91;
        st.framesDropped = 2;
        st.currentFps = 59.868;
        st.memoryUsageMb = 42.5;
        st.gpuAccelerated = true;
        st.gpuBackend = shine::core::GpuBackend::WindowsD3D11MediaFoundation;
        st.encoderName = "AMD H264 Hardware MFT Encoder";
        st.outputPath = std::filesystem::path(u8"/tmp/shine test/output.mp4");
        resp.stats = st;

        const std::string wire = shine::ipc::serialize_response(resp);
        auto parsed = shine::ipc::parse_response(wire);
        SHINE_CHECK(parsed.ok());
        if (parsed.ok()) {
            SHINE_CHECK(parsed.value().ok);
            SHINE_CHECK(parsed.value().stats.has_value());
            const auto& got = *parsed.value().stats;
            SHINE_CHECK(got.state == shine::core::RecorderState::Recording);
            SHINE_CHECK(got.elapsedMs == 1520U);
            SHINE_CHECK(got.framesEncoded == 91U);
            SHINE_CHECK(got.framesDropped == 2U);
            SHINE_CHECK(got.gpuAccelerated);
            SHINE_CHECK(got.gpuBackend == shine::core::GpuBackend::WindowsD3D11MediaFoundation);
            SHINE_CHECK(got.encoderName == st.encoderName);
            SHINE_CHECK(got.outputPath == st.outputPath);
        }
    }

    {
        shine::ipc::IpcResponse resp{};
        resp.ok = true;
        resp.message = "displays";
        resp.displays.push_back({0U, 1920U, 1080U, 180U, true, R"(\\.\DISPLAY1)"});
        resp.displays.push_back({1U, 2560U, 1440U, 144U, false, "HDMI-A-1 (Main)"});

        const std::string wire = shine::ipc::serialize_response(resp);
        auto parsed = shine::ipc::parse_response(wire);
        SHINE_CHECK(parsed.ok());
        if (parsed.ok()) {
            SHINE_CHECK(parsed.value().displays.size() == 2U);
            SHINE_CHECK(parsed.value().displays[0].refreshHz == 180U);
            SHINE_CHECK(parsed.value().displays[0].isPrimary);
            SHINE_CHECK(parsed.value().displays[1].name == "HDMI-A-1 (Main)");
        }
    }

    {
        auto badTok = shine::ipc::decode_token("abc%2");
        SHINE_CHECK(!badTok.ok());
    }
}

void test_mp4_validator() {
    const auto tempDir = std::filesystem::temp_directory_path() / "shine_test_core";
    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);

    const auto missing = shine::core::inspect_mp4(tempDir / "does_not_exist.mp4");
    SHINE_CHECK(!missing.valid);

    const auto corruptPath = tempDir / "corrupt.mp4";
    {
        std::ofstream out(corruptPath, std::ios::binary | std::ios::trunc);
        out << "not an mp4 file";
    }
    const auto corrupt = shine::core::inspect_mp4(corruptPath);
    SHINE_CHECK(!corrupt.valid);

    // Synthesize minimal valid ISO-BMFF file
    std::vector<std::uint8_t> mvhdPayload(20, 0);
    // timescale = 1000, duration = 1000 -> 1.0s
    mvhdPayload[14] = 0x03;
    mvhdPayload[15] = 0xE8;
    mvhdPayload[18] = 0x03;
    mvhdPayload[19] = 0xE8;

    std::vector<std::uint8_t> hdlrPayload(12, 0);
    hdlrPayload[8] = 'v';
    hdlrPayload[9] = 'i';
    hdlrPayload[10] = 'd';
    hdlrPayload[11] = 'e';

    std::vector<std::uint8_t> stsdPayload(16, 0);
    stsdPayload[7] = 1; // entry_count = 1
    stsdPayload[11] = 8; // dummy sample entry size
    stsdPayload[12] = 'a';
    stsdPayload[13] = 'v';
    stsdPayload[14] = 'c';
    stsdPayload[15] = '1';

    std::vector<std::uint8_t> stszPayload(12, 0);
    stszPayload[11] = 60; // sample_count = 60

    std::vector<std::uint8_t> stblPayload;
    append_bytes(stblPayload, make_box("stsd", stsdPayload));
    append_bytes(stblPayload, make_box("stsz", stszPayload));

    std::vector<std::uint8_t> minfPayload;
    append_bytes(minfPayload, make_box("stbl", stblPayload));

    std::vector<std::uint8_t> mdiaPayload;
    append_bytes(mdiaPayload, make_box("hdlr", hdlrPayload));
    append_bytes(mdiaPayload, make_box("minf", minfPayload));

    std::vector<std::uint8_t> trakPayload;
    append_bytes(trakPayload, make_box("mdia", mdiaPayload));

    std::vector<std::uint8_t> moovPayload;
    append_bytes(moovPayload, make_box("mvhd", mvhdPayload));
    append_bytes(moovPayload, make_box("trak", trakPayload));

    std::vector<std::uint8_t> mp4Bytes;
    append_bytes(mp4Bytes, make_box("ftyp", {'i', 's', 'o', 'm'}));
    append_bytes(mp4Bytes, make_box("mdat", {1, 2, 3, 4}));
    append_bytes(mp4Bytes, make_box("moov", moovPayload));

    const auto synthPath = tempDir / "synthetic.mp4";
    {
        std::ofstream out(synthPath, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(mp4Bytes.data()),
                  static_cast<std::streamsize>(mp4Bytes.size()));
    }

    const auto report = shine::core::inspect_mp4(synthPath);
    SHINE_CHECK(report.valid);
    SHINE_CHECK(report.hasFtyp);
    SHINE_CHECK(report.hasMoov);
    SHINE_CHECK(report.hasMdat);
    SHINE_CHECK(report.videoFrameCount == 60U);
    SHINE_CHECK(report.videoCodecFourCC == "avc1");
    SHINE_CHECK(report.durationSeconds > 0.99 && report.durationSeconds < 1.01);

    std::filesystem::remove_all(tempDir, ec);
}

void test_hotkeys() {
    const auto f8 = shine::core::parse_hotkey("F8");
    SHINE_CHECK(f8.has_value());
    SHINE_CHECK(f8->valid());
    SHINE_CHECK(f8->modifiers == 0U);
    SHINE_CHECK(f8->key == "F8");
    SHINE_CHECK(f8->to_string() == "F8");

    const auto combo = shine::core::parse_hotkey("ctrl + shift + f9");
    SHINE_CHECK(combo.has_value());
    SHINE_CHECK(combo->valid());
    SHINE_CHECK(shine::core::has_modifier(combo->modifiers, shine::core::KeyModifier::Ctrl));
    SHINE_CHECK(shine::core::has_modifier(combo->modifiers, shine::core::KeyModifier::Shift));
    SHINE_CHECK(!shine::core::has_modifier(combo->modifiers, shine::core::KeyModifier::Alt));
    SHINE_CHECK(combo->to_string() == "Ctrl+Shift+F9");

    const auto altV = shine::core::parse_hotkey("Alt+V");
    SHINE_CHECK(altV.has_value());
    SHINE_CHECK(shine::core::has_modifier(altV->modifiers, shine::core::KeyModifier::Alt));
    SHINE_CHECK(altV->to_string() == "Alt+V");

    SHINE_CHECK(!shine::core::parse_hotkey("").has_value());
    SHINE_CHECK(!shine::core::parse_hotkey("Ctrl+").has_value());
    SHINE_CHECK(!shine::core::parse_hotkey("NotAKeyXYZ").has_value());
}

void test_settings_roundtrip() {
    std::error_code ec;
    const auto tempDir = std::filesystem::temp_directory_path() / "shine_test_core_settings";
    std::filesystem::remove_all(tempDir, ec);
    const auto settingsPath = tempDir / "settings.ini";

    const auto def = shine::core::load_settings(settingsPath);
    SHINE_CHECK(!def.firstRunCompleted);
    SHINE_CHECK(def.clipHotkey == "F8");
    SHINE_CHECK(def.clipLengthSeconds == 30U);
    SHINE_CHECK(def.replayEnabled);
    SHINE_CHECK(!def.voiceEnabled);
    SHINE_CHECK(!def.voicePttEnabled);
    SHINE_CHECK(def.voicePttHotkey == "V");

    shine::core::AppSettings custom{};
    custom.firstRunCompleted = true;
    custom.clipHotkey = "Ctrl+Alt+F10";
    custom.clipLengthSeconds = 60U;
    custom.replayEnabled = true;
    custom.voiceEnabled = true;
    custom.voicePttEnabled = true;
    custom.voicePttHotkey = "G";
    custom.displayIndex = 1U;
    custom.fps = 30U;
    custom.bitrateKbps = 6000U;
    custom.captureCursor = false;
    custom.outputDirectory = tempDir / "clips";

    const auto saveRes = shine::core::save_settings(custom, settingsPath);
    SHINE_CHECK(saveRes.ok());

    const auto loaded = shine::core::load_settings(settingsPath);
    SHINE_CHECK(loaded.firstRunCompleted);
    SHINE_CHECK(loaded.clipHotkey == "Ctrl+Alt+F10");
    SHINE_CHECK(loaded.clipLengthSeconds == 60U);
    SHINE_CHECK(loaded.replayEnabled);
    SHINE_CHECK(loaded.voiceEnabled);
    SHINE_CHECK(loaded.voicePttEnabled);
    SHINE_CHECK(loaded.voicePttHotkey == "G");
    SHINE_CHECK(loaded.displayIndex == 1U);
    SHINE_CHECK(loaded.fps == 30U);
    SHINE_CHECK(loaded.bitrateKbps == 6000U);
    SHINE_CHECK(!loaded.captureCursor);
    SHINE_CHECK(loaded.outputDirectory == custom.outputDirectory);

    std::filesystem::remove_all(tempDir, ec);
}

void test_replay_buffer() {
    shine::core::ReplayRingBuffer buf;
    buf.configure(5U);
    buf.set_video_format(1920U, 1080U, 30U, 8000U, {0x00U, 0x00U, 0x00U, 0x01U, 0x67U, 0x42U});

    for (std::int64_t i = 0; i < 300; ++i) {
        shine::core::EncodedVideoPacket pkt{};
        pkt.ptsUs = i * 33333LL;
        pkt.durationUs = 33333LL;
        pkt.isKeyframe = (i % 30 == 0);
        pkt.data.assign(pkt.isKeyframe ? 512U : 128U, static_cast<std::uint8_t>(i & 0xFF));
        buf.push_video(std::move(pkt));
    }

    for (std::int64_t k = 0; k < 500; ++k) {
        shine::core::AudioPcmChunk chunk{};
        chunk.ptsUs = k * 20000LL;
        chunk.sampleRate = 48000U;
        chunk.channels = 1U;
        chunk.samples.assign(960U, static_cast<std::int16_t>(k % 100));
        buf.push_audio(std::move(chunk));
    }

    SHINE_CHECK(buf.buffered_seconds() <= 6U);
    SHINE_CHECK(buf.memory_bytes() > 0U);

    const auto snap = buf.snapshot_last(3U);
    SHINE_CHECK(snap.width == 1920U);
    SHINE_CHECK(snap.height == 1080U);
    SHINE_CHECK(!snap.codecConfig.empty());
    SHINE_CHECK(!snap.videoPackets.empty());
    SHINE_CHECK(snap.videoPackets.front().isKeyframe);
    SHINE_CHECK(snap.videoPackets.front().ptsUs == 0LL);

    bool monotonic = true;
    for (std::size_t i = 1; i < snap.videoPackets.size(); ++i) {
        if (snap.videoPackets[i].ptsUs < snap.videoPackets[i - 1U].ptsUs) {
            monotonic = false;
            break;
        }
    }
    SHINE_CHECK(monotonic);

    const std::int64_t spanUs = snap.videoPackets.back().ptsUs - snap.videoPackets.front().ptsUs;
    SHINE_CHECK(spanUs >= 2500000LL && spanUs <= 5500000LL);
    SHINE_CHECK(!snap.audioChunks.empty());
    SHINE_CHECK(snap.audioChunks.front().ptsUs >= 0LL);

    buf.clear();
    SHINE_CHECK(buf.memory_bytes() == 0U);
}

void test_clip_library_and_mp4_audio() {
    std::error_code ec;
    const auto tempDir = std::filesystem::temp_directory_path() / "shine_test_core_clips";
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    std::vector<std::uint8_t> mvhdPayload(20, 0);
    mvhdPayload[14] = 0x03;
    mvhdPayload[15] = 0xE8;
    mvhdPayload[18] = 0x03;
    mvhdPayload[19] = 0xE8;

    std::vector<std::uint8_t> videHdlr(12, 0);
    videHdlr[8] = 'v';
    videHdlr[9] = 'i';
    videHdlr[10] = 'd';
    videHdlr[11] = 'e';

    std::vector<std::uint8_t> avcStsd(16, 0);
    avcStsd[7] = 1;
    avcStsd[11] = 8;
    avcStsd[12] = 'a';
    avcStsd[13] = 'v';
    avcStsd[14] = 'c';
    avcStsd[15] = '1';

    std::vector<std::uint8_t> stszPayload(12, 0);
    stszPayload[11] = 30;

    std::vector<std::uint8_t> videStbl;
    append_bytes(videStbl, make_box("stsd", avcStsd));
    append_bytes(videStbl, make_box("stsz", stszPayload));
    std::vector<std::uint8_t> videMinf;
    append_bytes(videMinf, make_box("stbl", videStbl));
    std::vector<std::uint8_t> videMdia;
    append_bytes(videMdia, make_box("hdlr", videHdlr));
    append_bytes(videMdia, make_box("minf", videMinf));
    std::vector<std::uint8_t> videTrak;
    append_bytes(videTrak, make_box("mdia", videMdia));

    std::vector<std::uint8_t> sounHdlr(12, 0);
    sounHdlr[8] = 's';
    sounHdlr[9] = 'o';
    sounHdlr[10] = 'u';
    sounHdlr[11] = 'n';

    std::vector<std::uint8_t> aacStsd(16, 0);
    aacStsd[7] = 1;
    aacStsd[11] = 8;
    aacStsd[12] = 'm';
    aacStsd[13] = 'p';
    aacStsd[14] = '4';
    aacStsd[15] = 'a';

    std::vector<std::uint8_t> sounStbl;
    append_bytes(sounStbl, make_box("stsd", aacStsd));
    std::vector<std::uint8_t> sounMinf;
    append_bytes(sounMinf, make_box("stbl", sounStbl));
    std::vector<std::uint8_t> sounMdia;
    append_bytes(sounMdia, make_box("hdlr", sounHdlr));
    append_bytes(sounMdia, make_box("minf", sounMinf));
    std::vector<std::uint8_t> sounTrak;
    append_bytes(sounTrak, make_box("mdia", sounMdia));

    std::vector<std::uint8_t> moovPayload;
    append_bytes(moovPayload, make_box("mvhd", mvhdPayload));
    append_bytes(moovPayload, make_box("trak", videTrak));
    append_bytes(moovPayload, make_box("trak", sounTrak));

    std::vector<std::uint8_t> mp4Bytes;
    append_bytes(mp4Bytes, make_box("ftyp", {'i', 's', 'o', 'm'}));
    append_bytes(mp4Bytes, make_box("mdat", {1, 2, 3, 4}));
    append_bytes(mp4Bytes, make_box("moov", moovPayload));

    const auto clipFile = shine::core::generate_clip_filename(tempDir);
    const auto recFile = tempDir / "Recording 2026-01-01.mp4";
    for (const auto& p : {clipFile, recFile}) {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(mp4Bytes.data()),
                  static_cast<std::streamsize>(mp4Bytes.size()));
    }

    const auto report = shine::core::inspect_mp4(clipFile);
    SHINE_CHECK(report.valid);
    SHINE_CHECK(report.hasAudioTrack);
    SHINE_CHECK(report.videoCodecFourCC == "avc1");
    SHINE_CHECK(report.audioCodecFourCC == "mp4a");

    const auto scanned = shine::core::scan_clips(tempDir);
    SHINE_CHECK(scanned.size() == 2U);

    const auto delRes = shine::core::delete_clip(recFile);
    SHINE_CHECK(delRes.ok());
    SHINE_CHECK(!std::filesystem::exists(recFile));

    const auto scannedAfter = shine::core::scan_clips(tempDir);
    SHINE_CHECK(scannedAfter.size() == 1U);

    std::filesystem::remove_all(tempDir, ec);
}

void test_ipc_roundtrip_extended() {
    shine::ipc::IpcRequest setHotkey{};
    setHotkey.command = shine::ipc::IpcCommand::SetHotkey;
    setHotkey.hotkey = "Ctrl+Shift+F9";
    const auto parsedHotkey = shine::ipc::parse_request(shine::ipc::serialize_request(setHotkey));
    SHINE_CHECK(parsedHotkey.ok());
    SHINE_CHECK(parsedHotkey.value().command == shine::ipc::IpcCommand::SetHotkey);
    SHINE_CHECK(parsedHotkey.value().hotkey == "Ctrl+Shift+F9");

    shine::ipc::IpcRequest replayReq{};
    replayReq.command = shine::ipc::IpcCommand::ReplayStart;
    shine::core::ReplayConfig cfg{};
    cfg.displayIndex = 1U;
    cfg.fps = 60U;
    cfg.bitrateKbps = 12000U;
    cfg.clipLengthSeconds = 60U;
    cfg.voiceEnabled = true;
    cfg.voicePttEnabled = true;
    cfg.voicePttHotkey = "B";
    cfg.outputDirectory = "C:/Clips/Team Highlights";
    replayReq.replayConfig = cfg;

    const auto parsedReplay = shine::ipc::parse_request(shine::ipc::serialize_request(replayReq));
    SHINE_CHECK(parsedReplay.ok());
    SHINE_CHECK(parsedReplay.value().command == shine::ipc::IpcCommand::ReplayStart);
    SHINE_CHECK(parsedReplay.value().replayConfig.has_value());
    SHINE_CHECK(parsedReplay.value().replayConfig->clipLengthSeconds == 60U);
    SHINE_CHECK(parsedReplay.value().replayConfig->voiceEnabled);
    SHINE_CHECK(parsedReplay.value().replayConfig->voicePttEnabled);
    SHINE_CHECK(parsedReplay.value().replayConfig->voicePttHotkey == "B");

    shine::ipc::IpcRequest saveClipReq{};
    saveClipReq.command = shine::ipc::IpcCommand::SaveClip;
    saveClipReq.clipOutputPath = "C:/Clips/out clip.mp4";
    saveClipReq.clipSeconds = 30U;
    const auto parsedSaveClip = shine::ipc::parse_request(shine::ipc::serialize_request(saveClipReq));
    SHINE_CHECK(parsedSaveClip.ok());
    SHINE_CHECK(parsedSaveClip.value().command == shine::ipc::IpcCommand::SaveClip);
    SHINE_CHECK(parsedSaveClip.value().clipOutputPath == std::filesystem::path("C:/Clips/out clip.mp4"));
    SHINE_CHECK(parsedSaveClip.value().clipSeconds == 30U);

    shine::ipc::IpcResponse resp{};
    resp.ok = true;
    resp.stats = shine::core::RecordingStats{};
    resp.stats->replayActive = true;
    resp.stats->replayBufferedSec = 28U;
    resp.stats->voiceActive = true;
    resp.stats->activeClipHotkey = "F8";
    resp.stats->lastClipPath = "C:/Clips/last.mp4";
    resp.stats->clipsSavedCount = 3U;

    const auto parsedResp = shine::ipc::parse_response(shine::ipc::serialize_response(resp));
    SHINE_CHECK(parsedResp.ok());
    SHINE_CHECK(parsedResp.value().stats.has_value());
    SHINE_CHECK(parsedResp.value().stats->replayActive);
    SHINE_CHECK(parsedResp.value().stats->replayBufferedSec == 28U);
    SHINE_CHECK(parsedResp.value().stats->voiceActive);
    SHINE_CHECK(parsedResp.value().stats->activeClipHotkey == "F8");
    SHINE_CHECK(parsedResp.value().stats->clipsSavedCount == 3U);
}

} // namespace

int main() {
    test_result_and_config();
    test_ipc_roundtrip();
    test_mp4_validator();
    test_hotkeys();
    test_settings_roundtrip();
    test_replay_buffer();
    test_clip_library_and_mp4_audio();
    test_ipc_roundtrip_extended();

    if (g_failures > 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "shine_test_core: all checks passed.\n";
    return 0;
}
