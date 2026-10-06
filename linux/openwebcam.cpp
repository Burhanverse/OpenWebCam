// OpenWebCam for Linux: receives the phone's H.264 stream over USB tethering
// and writes it to a v4l2loopback device, so every V4L2 app sees a webcam.
//
//   openwebcam [--device /dev/video10] [--size 1920x1080] [--interface usb0] [phone-ip]
//
// Same wire protocol as the Windows camera (see TECHNICAL.md). Without a
// signal the device shows colour bars. Logs go to stderr (journald under systemd).

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

const uint16_t kPort = 5000;
const uint32_t kMaxPacket = 8 * 1024 * 1024;
const uint8_t kTypeVideo = 1;  // payload: one H.264 Annex-B access unit
const uint8_t kTypeInfo = 2;   // payload: u16 BE width, height, rotation (clockwise degrees to turn upright)

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_quit{false};
std::atomic<int> g_sock{-1};

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void OnSignal(int) {
    g_quit = true;
    int s = g_sock.load();
    if (s >= 0) shutdown(s, SHUT_RDWR);  // unblocks recv()
}

void SleepUnlessQuitting(std::chrono::milliseconds d) {
    for (auto end = Clock::now() + d; !g_quit && Clock::now() < end;)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ---- Phone discovery and wire protocol ----

// USB tethering shows up as an RNDIS or NCM network interface.
bool IsTether(const std::string& ifname) {
    char buf[PATH_MAX];
    ssize_t n = readlink(("/sys/class/net/" + ifname + "/device/driver").c_str(), buf, sizeof buf - 1);
    if (n <= 0) return false;
    buf[n] = 0;
    const char* slash = strrchr(buf, '/');
    std::string driver = slash ? slash + 1 : buf;
    return driver == "rndis_host" || driver == "cdc_ncm";
}

// IPv4 gateways from the routing table. The phone is the gateway of the
// tethering interface; those are tried first. only: restrict to one interface.
std::vector<std::string> CandidateHosts(const std::string& only) {
    std::vector<std::string> usb, other;
    std::ifstream f("/proc/net/route");
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
        char ifname[64];
        unsigned dest, gw, flags;
        if (sscanf(line.c_str(), "%63s %x %x %x", ifname, &dest, &gw, &flags) != 4) continue;
        if (!(flags & RTF_UP) || !(flags & RTF_GATEWAY) || !gw) continue;
        if (!only.empty() && only != ifname) continue;
        in_addr a{};
        a.s_addr = gw;  // already in network byte order
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &a, ip, sizeof ip);
        auto& list = IsTether(ifname) ? usb : other;
        if (std::find(list.begin(), list.end(), ip) == list.end()) list.push_back(ip);
    }
    usb.insert(usb.end(), other.begin(), other.end());
    return usb;
}

int ConnectWithTimeout(const std::string& host, int timeoutMs) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) return -1;
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0) return -1;
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 && errno != EINPROGRESS) {
        close(s);
        return -1;
    }
    pollfd p{s, POLLOUT, 0};
    int err = 0;
    socklen_t len = sizeof err;
    if (poll(&p, 1, timeoutMs) != 1 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err) {
        close(s);
        return -1;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK);
    timeval tv{2, 0};  // stream runs at 30 fps; 2 s of silence = dead link
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    // Disable Nagle algorithm and quicken ACKs to minimize network latency.
    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
#ifdef TCP_QUICKACK
    setsockopt(s, IPPROTO_TCP, TCP_QUICKACK, &nodelay, sizeof nodelay);
#endif
    // Keep socket buffer bounded to prevent stale frames accumulating during temporary hiccups.
    int rcvbuf = 256 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);

    return s;
}

bool RecvAll(int s, uint8_t* p, size_t n) {
    while (n) {
        ssize_t r = recv(s, p, n, 0);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

// [u32 BE length of type + payload][u8 type][payload]
bool ReadPacket(int s, uint8_t& type, std::vector<uint8_t>& payload) {
    uint8_t h[5];
    if (!RecvAll(s, h, 5)) return false;
    uint32_t len = (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) | (uint32_t(h[2]) << 8) | h[3];
    if (len == 0 || len > kMaxPacket) return false;
    type = h[4];
    payload.resize(len - 1);
    return len == 1 || RecvAll(s, payload.data(), len - 1);
}

struct StreamInfo {
    int width = 0, height = 0, rotation = 0;
};

bool ParseInfo(const std::vector<uint8_t>& p, StreamInfo& info) {
    if (p.size() < 6) return false;
    info.width = p[0] << 8 | p[1];
    info.height = p[2] << 8 | p[3];
    info.rotation = p[4] << 8 | p[5];
    return info.width >= 2 && info.height >= 2 && info.width % 2 == 0 && info.height % 2 == 0 &&
           info.rotation % 90 == 0 && info.rotation < 360;
}

// Returns a connected socket whose first packet is a valid INFO message, or -1.
int FindPhone(const std::vector<std::string>& hosts, StreamInfo& info) {
    std::vector<uint8_t> payload;
    for (const auto& host : hosts) {
        int s = ConnectWithTimeout(host, 1000);
        uint8_t type = 0;
        if (s >= 0 && ReadPacket(s, type, payload) && type == kTypeInfo && ParseInfo(payload, info)) return s;
        if (s >= 0) close(s);
    }
    return -1;
}

// ---- v4l2loopback output (I420) ----

class Output {
public:
    ~Output() {
        if (fd_ >= 0) close(fd_);
    }

    bool Open(const char* path, int w, int h) {
        fd_ = open(path, O_WRONLY | O_CLOEXEC);
        if (fd_ < 0) {
            Log("Cannot open %s: %s (is v4l2loopback loaded?)", path, strerror(errno));
            return false;
        }
        v4l2_format f{};
        f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        f.fmt.pix.width = w;
        f.fmt.pix.height = h;
        f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
        f.fmt.pix.field = V4L2_FIELD_NONE;
        f.fmt.pix.bytesperline = w;
        f.fmt.pix.sizeimage = w * h * 3 / 2;
        f.fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
        if (ioctl(fd_, VIDIOC_S_FMT, &f) < 0) {
            Log("Cannot set %dx%d on %s: %s (is another program writing to it?)", w, h, path, strerror(errno));
            return false;
        }
        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        parm.parm.output.timeperframe = {1, 30};
        ioctl(fd_, VIDIOC_S_PARM, &parm);  // advisory only
        size_ = static_cast<size_t>(w) * h * 3 / 2;
        return true;
    }

    void Write(const std::vector<uint8_t>& frame) {
        std::lock_guard<std::mutex> lock(lock_);
        if (write(fd_, frame.data(), size_) < 0 && !warned_) {
            Log("Write to device failed: %s", strerror(errno));
            warned_ = true;
        }
    }

private:
    int fd_ = -1;
    size_t size_ = 0;
    bool warned_ = false;
    std::mutex lock_;
};

// 75% colour bars (BT.601 limited range) with a white stripe sweeping down,
// so a frozen pipeline is obvious.
void DrawPattern(std::vector<uint8_t>& out, int w, int h, uint64_t frame) {
    static const uint8_t bars[8][3] = {{180, 128, 128}, {162, 44, 142}, {131, 156, 44}, {112, 72, 58},
                                       {84, 184, 198},  {65, 100, 212}, {35, 212, 114}, {16, 128, 128}};
    const int stripe = static_cast<int>(frame * 8 % h), stripeH = 16;
    uint8_t* y = out.data();
    uint8_t* u = y + w * h;
    uint8_t* v = u + w * h / 4;
    for (int row = 0; row < h; ++row) {
        bool white = row >= stripe && row < stripe + stripeH;
        for (int x = 0; x < w; ++x) y[row * w + x] = white ? 235 : bars[x * 8 / w][0];
    }
    for (int row = 0; row < h / 2; ++row) {
        bool white = row * 2 >= stripe && row * 2 < stripe + stripeH;
        for (int x = 0; x < w / 2; ++x) {
            const uint8_t* b = bars[x * 2 * 8 / w];
            u[row * w / 2 + x] = white ? 128 : b[1];
            v[row * w / 2 + x] = white ? 128 : b[2];
        }
    }
}

// ---- Decode and fit ----

class Decoder {
public:
    ~Decoder() {
        av_frame_free(&frame_);
        av_packet_free(&pkt_);
        avcodec_free_context(&ctx_);
    }

    bool Open() {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) return false;
        ctx_ = avcodec_alloc_context3(codec);
        pkt_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (!ctx_ || !pkt_ || !frame_) return false;
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
        ctx_->thread_type = FF_THREAD_SLICE;  // slice threading has zero frame latency
        ctx_->thread_count = 0;               // automatic multi-core slice decoding
        return avcodec_open2(ctx_, codec, nullptr) == 0;
    }

    // Packets before the phone's first SPS/PPS + IDR are rejected by the decoder and skipped.
    template <class Fn>
    bool Feed(std::vector<uint8_t>& data, Fn&& onFrame) {
        pkt_->data = data.data();
        pkt_->size = static_cast<int>(data.size());
        int r = avcodec_send_packet(ctx_, pkt_);
        if (r < 0 && r != AVERROR_INVALIDDATA) return false;
        while ((r = avcodec_receive_frame(ctx_, frame_)) == 0) onFrame(frame_);
        return r == AVERROR(EAGAIN) || r == AVERROR_EOF;
    }

private:
    AVCodecContext* ctx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
};

// Clockwise rotation of one 8-bit plane (sw x sh) into d, row stride dstride.
// Uses cache-friendly block tiling and hoists rotation checks outside loops.
void RotatePlane(const uint8_t* s, int sw, int sh, int rotation, uint8_t* d, int dstride) {
    if (rotation == 180) {
        for (int y = 0; y < sh; ++y) {
            uint8_t* row = d + y * dstride;
            const uint8_t* src_row = s + (sh - 1 - y) * sw;
            for (int x = 0; x < sw; ++x) row[x] = src_row[sw - 1 - x];
        }
        return;
    }

    const int ow = sh, oh = sw;
    constexpr int BLOCK = 32;

    if (rotation == 90) {
        for (int by = 0; by < oh; by += BLOCK) {
            int max_y = std::min(by + BLOCK, oh);
            for (int bx = 0; bx < ow; bx += BLOCK) {
                int max_x = std::min(bx + BLOCK, ow);
                for (int y = by; y < max_y; ++y) {
                    uint8_t* row = d + y * dstride;
                    for (int x = bx; x < max_x; ++x) {
                        row[x] = s[(sh - 1 - x) * sw + y];
                    }
                }
            }
        }
    } else if (rotation == 270) {
        for (int by = 0; by < oh; by += BLOCK) {
            int max_y = std::min(by + BLOCK, oh);
            for (int bx = 0; bx < ow; bx += BLOCK) {
                int max_x = std::min(bx + BLOCK, ow);
                for (int y = by; y < max_y; ++y) {
                    uint8_t* row = d + y * dstride;
                    for (int x = bx; x < max_x; ++x) {
                        row[x] = s[x * sw + (sw - 1 - y)];
                    }
                }
            }
        }
    }
}

// Fits a decoded frame into the ow x oh I420 output: scale (libswscale, keeping
// the aspect ratio), rotate clockwise, centre on black. Scaling happens before
// rotation so rotation works on the smaller image.
class Fitter {
public:
    ~Fitter() { sws_freeContext(sws_); }

    bool Fit(const AVFrame* f, int rotation, int ow, int oh, std::vector<uint8_t>& out) {
        const bool swap = rotation == 90 || rotation == 270;
        const int rw = swap ? f->height : f->width, rh = swap ? f->width : f->height;
        const double scale = std::min(static_cast<double>(ow) / rw, static_cast<double>(oh) / rh);
        const int dw = std::max(2, std::min(ow, static_cast<int>(rw * scale + 0.5)) & ~1);
        const int dh = std::max(2, std::min(oh, static_cast<int>(rh * scale + 0.5)) & ~1);
        const int sw = swap ? dh : dw, sh = swap ? dw : dh;  // scaled size before rotation

        sws_ = sws_getCachedContext(sws_, f->width, f->height, static_cast<AVPixelFormat>(f->format), sw, sh,
                                    AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) return false;

        const size_t ySize = static_cast<size_t>(ow) * oh;
        out.resize(ySize * 3 / 2);
        // Letterbox margins only need clearing when the frame doesn't fill the output buffer.
        if (dw != ow || dh != oh) {
            std::fill(out.begin(), out.begin() + ySize, 16);  // black
            std::fill(out.begin() + ySize, out.end(), 128);
        }
        const int x0 = ((ow - dw) / 2) & ~1, y0 = ((oh - dh) / 2) & ~1;
        uint8_t* planes[3] = {out.data() + y0 * ow + x0, out.data() + ySize + (y0 / 2) * (ow / 2) + x0 / 2,
                              out.data() + ySize * 5 / 4 + (y0 / 2) * (ow / 2) + x0 / 2};
        const int strides[3] = {ow, ow / 2, ow / 2};

        if (rotation == 0) {
            sws_scale(sws_, f->data, f->linesize, 0, f->height, planes, strides);
            return true;
        }
        tmp_.resize(static_cast<size_t>(sw) * sh * 3 / 2);
        uint8_t* t[3] = {tmp_.data(), tmp_.data() + sw * sh, tmp_.data() + sw * sh * 5 / 4};
        const int ts[3] = {sw, sw / 2, sw / 2};
        sws_scale(sws_, f->data, f->linesize, 0, f->height, t, ts);
        RotatePlane(t[0], sw, sh, rotation, planes[0], strides[0]);
        RotatePlane(t[1], sw / 2, sh / 2, rotation, planes[1], strides[1]);
        RotatePlane(t[2], sw / 2, sh / 2, rotation, planes[2], strides[2]);
        return true;
    }

private:
    SwsContext* sws_ = nullptr;
    std::vector<uint8_t> tmp_;
};

// ---- Main loop ----

struct Options {
    std::string device = "/dev/video10";
    std::string iface;
    std::string host;
    int width = 1920, height = 1080;
};

// Connects to the phone, decodes and writes frames; reconnects until quit.
void Receive(const Options& o, Output& out, std::atomic<int64_t>& lastFrameMs) {
    bool searching = false;
    std::vector<uint8_t> pkt, frame;
    Fitter fitter;
    while (!g_quit) {
        StreamInfo info;
        int s = FindPhone(o.host.empty() ? CandidateHosts(o.iface) : std::vector<std::string>{o.host}, info);
        if (s < 0) {
            if (!searching) Log("Phone not found; retrying every 2 s");
            searching = true;
            SleepUnlessQuitting(std::chrono::seconds(2));
            continue;
        }
        searching = false;
        g_sock = s;
        if (g_quit) shutdown(s, SHUT_RDWR);  // signal arrived before g_sock was set
        Log("Phone connected: %dx%d, rotation %d", info.width, info.height, info.rotation);

        auto dec = std::make_unique<Decoder>();
        bool ok = dec->Open();
        if (!ok) Log("H.264 decoder init failed");
        auto onFrame = [&](const AVFrame* f) {
            if (!fitter.Fit(f, info.rotation, o.width, o.height, frame)) return;
            out.Write(frame);
            lastFrameMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        };
        uint8_t type = 0;
        while (ok && !g_quit && ReadPacket(s, type, pkt)) {
            if (type == kTypeVideo) {
                ok = dec->Feed(pkt, onFrame);
                if (!ok) Log("Decode failed");
            } else if (type == kTypeInfo) {
                StreamInfo next;
                if (!ParseInfo(pkt, next)) continue;
                if (next.width != info.width || next.height != info.height) {
                    dec = std::make_unique<Decoder>();  // new size: phone restarts from a keyframe
                    ok = dec->Open();
                }
                if (next.width != info.width || next.height != info.height || next.rotation != info.rotation)
                    Log("Phone now sends %dx%d, rotation %d", next.width, next.height, next.rotation);
                info = next;
            }
        }
        g_sock = -1;
        close(s);
        Log("Phone disconnected");
        if (!ok) SleepUnlessQuitting(std::chrono::seconds(10));  // decoder broken: don't hammer the phone
    }
}

void Usage() {
    fprintf(stderr,
            "Usage: openwebcam [--device /dev/video10] [--size 1920x1080] [--interface IFACE] [phone-ip]\n");
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        bool hasValue = i + 1 < argc;
        if (a == "--device" && hasValue) {
            o.device = argv[++i];
        } else if (a == "--interface" && hasValue) {
            o.iface = argv[++i];
        } else if (a == "--size" && hasValue) {
            if (sscanf(argv[++i], "%dx%d", &o.width, &o.height) != 2 || o.width < 2 || o.height < 2 ||
                o.width % 2 || o.height % 2) {
                fprintf(stderr, "--size needs even WIDTHxHEIGHT, e.g. 1920x1080\n");
                return 2;
            }
        } else if (a[0] != '-' && o.host.empty()) {
            o.host = a;
        } else {
            Usage();
            return 2;
        }
    }

    struct sigaction sa {};
    sa.sa_handler = OnSignal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
    av_log_set_level(AV_LOG_FATAL);  // the decoder complains about every frame before the first keyframe

    Output out;
    if (!out.Open(o.device.c_str(), o.width, o.height)) return 1;
    Log("Writing %dx%d to %s", o.width, o.height, o.device.c_str());

    std::atomic<int64_t> lastFrameMs{0};
    std::thread receiver(Receive, std::cref(o), std::ref(out), std::ref(lastFrameMs));

    // Colour bars at 10 fps whenever no phone frame arrived for a second.
    std::vector<uint8_t> pattern(static_cast<size_t>(o.width) * o.height * 3 / 2);
    uint64_t n = 0;
    while (!g_quit) {
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        if (now - lastFrameMs > 1000) {
            DrawPattern(pattern, o.width, o.height, n++);
            out.Write(pattern);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    receiver.join();
    return 0;
}
