#include "shine/core/mp4_validator.hpp"
#include "shine/core/result.hpp"
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

} // namespace

int main() {
    test_result_and_config();
    test_ipc_roundtrip();
    test_mp4_validator();

    if (g_failures > 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "shine_test_core: all checks passed.\n";
    return 0;
}
