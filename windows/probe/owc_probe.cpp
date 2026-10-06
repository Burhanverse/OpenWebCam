// owc-probe: finds the phone over USB tethering, decodes its H.264 stream with
// the inbox Media Foundation decoder, prints stats and dumps one frame to frame.bmp.
// Dev/test tool only; not part of the user install.
//
// Usage: owc-probe [phone-ip]

#include "../common/h264_decoder.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

static const UINT32 kFps = 30;
static const uint64_t kDumpFrame = 60;

#define CHECK(expr)                                                                   \
    do {                                                                              \
        HRESULT hr_ = (expr);                                                         \
        if (FAILED(hr_)) {                                                            \
            fprintf(stderr, "%s failed: 0x%08lX (line %d)\n", #expr, (unsigned long)hr_, \
                    __LINE__);                                                        \
            exit(1);                                                                  \
        }                                                                             \
    } while (0)

static uint8_t Clamp(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

// Tightly packed NV12 (BT.601 limited range) -> 24-bit top-down BMP.
static bool WriteBmp(const char* path, const std::vector<uint8_t>& nv12, UINT32 w, UINT32 h) {
    const BYTE* uv = nv12.data() + static_cast<size_t>(w) * h;
    UINT32 rowBytes = (w * 3 + 3) & ~3u;

    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof ih;
    ih.biWidth = static_cast<LONG>(w);
    ih.biHeight = -static_cast<LONG>(h);
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = rowBytes * h;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    std::vector<uint8_t> row(rowBytes, 0);
    for (UINT32 y = 0; y < h; ++y) {
        for (UINT32 x = 0; x < w; ++x) {
            int c = nv12[static_cast<size_t>(y) * w + x] - 16;
            const BYTE* p = uv + static_cast<size_t>(y / 2) * w + (x & ~1u);
            int d = p[0] - 128, e = p[1] - 128;
            row[x * 3 + 0] = Clamp((298 * c + 516 * d + 128) >> 8);
            row[x * 3 + 1] = Clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
            row[x * 3 + 2] = Clamp((298 * c + 409 * e + 128) >> 8);
        }
        fwrite(row.data(), rowBytes, 1, f);
    }
    fclose(f);
    return true;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);  // stats visible even when output is piped
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    CHECK(MFStartup(MF_VERSION, MFSTARTUP_LITE));

    std::vector<std::string> hosts = argc > 1 ? std::vector<std::string>{argv[1]} : owc::CandidateHosts();
    owc::StreamInfo info;
    SOCKET s = owc::FindPhone(hosts, info, [](const char* host, bool ok) {
        printf("Trying %s:%u ... %s\n", host, owc::kPort, ok ? "ok" : "no");
    });
    if (s == INVALID_SOCKET) {
        fprintf(stderr, "Phone not found. Is USB tethering on and the app streaming?\n");
        return 1;
    }
    printf("Phone sends %ux%u, rotation %u\n", info.width, info.height, info.rotation);

    uint64_t frames = 0, totalFrames = 0, bytes = 0;
    LONGLONG latencySum = 0, lastPrint = MFGetSystemTime();
    std::unique_ptr<owc::H264Decoder> dec;
    auto onFrame = [&](IMFSample* sample) {
        LONGLONG t = 0;
        sample->GetSampleTime(&t);
        latencySum += MFGetSystemTime() - t;
        ++frames;
        if (++totalFrames != kDumpFrame) return;
        std::vector<uint8_t> nv12;  // as sent by the phone, before rotation
        bool ok = SUCCEEDED(owc::CopyNV12(sample, info.width, info.height, dec->stride, nv12)) &&
                  WriteBmp("frame.bmp", nv12, info.width, info.height);
        printf(ok ? "Wrote frame.bmp (unrotated)\n" : "frame.bmp write failed\n");
    };
    auto newDecoder = [&] {
        dec = std::make_unique<owc::H264Decoder>();
        CHECK(dec->Init(info.width, info.height, kFps));
    };
    newDecoder();

    uint8_t type = 0;
    std::vector<uint8_t> pkt;
    while (owc::ReadPacket(s, type, pkt)) {
        bytes += pkt.size() + 5;
        if (type == owc::kTypeVideo) {
            CHECK(dec->FeedPacket(pkt, MFGetSystemTime(), onFrame));
        } else if (type == owc::kTypeInfo) {
            owc::StreamInfo next;
            if (!owc::ParseInfo(pkt, next)) continue;
            bool resized = next.width != info.width || next.height != info.height;
            info = next;
            printf("Phone sends %ux%u, rotation %u\n", info.width, info.height, info.rotation);
            if (resized) newDecoder();
        }

        LONGLONG now = MFGetSystemTime();
        if (now - lastPrint >= 10'000'000) {
            double sec = (now - lastPrint) / 1e7;
            printf("%ux%u  %5.1f fps  %5.2f Mbit/s  decode %5.1f ms\n", info.width, info.height,
                   frames / sec, bytes * 8 / sec / 1e6, frames ? latencySum / 1e4 / frames : 0.0);
            frames = bytes = 0;
            latencySum = 0;
            lastPrint = now;
        }
    }

    printf("Stream ended (disconnect, 2 s timeout, or bad packet length).\n");
    closesocket(s);
    MFShutdown();
    return 0;
}
