// OpenWebCam virtual camera: a Media Foundation media source loaded by the
// Windows Camera Frame Server (registered via MFCreateVirtualCamera by setup).
//
// While an app has the camera open, a receiver thread connects to the phone
// (USB tethering gateway, TCP 5000), decodes H.264 to NV12, rotates/scales it
// to the size the app picked and keeps the newest frame; each RequestSample
// gets that frame. Without a signal the camera shows colour bars.
// Diagnostics go to %ProgramData%\OpenWebCam\log.txt.

#include "convert.h"
#include "../common/net.h"
#include "../common/openwebcam.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <wrl/module.h>

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <share.h>
#include <thread>

using namespace Microsoft::WRL;

namespace {

const UINT32 kFps = 30;

// Sizes offered to apps; the first is the default. Phone frames are fitted into
// whichever one the app picks.
const struct {
    UINT32 w, h;
} kSizes[] = {{1920, 1080}, {3840, 2160}, {2560, 1440}, {1280, 720}, {960, 540},
              {640, 480},   {640, 360},   {2160, 3840}, {1080, 1920}, {720, 1280}};

std::mutex g_logLock;

void Log(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");

    wchar_t path[MAX_PATH];
    ExpandEnvironmentStringsW(OWC_DATA_DIR L"\\log.txt", path, MAX_PATH);
    std::lock_guard<std::mutex> lock(g_logLock);
    FILE* f = _wfsopen(path, L"a", _SH_DENYNO);
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d [%lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            GetCurrentProcessId(), msg);
    fclose(f);
}

// 75% colour bars (BT.601 limited range) with a white stripe sweeping down,
// so a frozen pipeline is obvious.
void DrawPattern(BYTE* base, LONG pitch, UINT32 width, UINT32 height, uint64_t frame) {
    static const BYTE bars[8][3] = {{180, 128, 128}, {162, 44, 142}, {131, 156, 44}, {112, 72, 58},
                                    {84, 184, 198},  {65, 100, 212}, {35, 212, 114}, {16, 128, 128}};
    const UINT32 stripe = static_cast<UINT32>(frame * 8 % height), stripeH = 16;
    for (UINT32 row = 0; row < height; ++row) {
        BYTE* y = base + static_cast<size_t>(row) * pitch;
        bool white = row - stripe < stripeH;  // unsigned wrap: rows above the stripe are huge
        for (UINT32 x = 0; x < width; ++x) y[x] = white ? 235 : bars[x * 8 / width][0];
    }
    BYTE* uvBase = base + static_cast<size_t>(pitch) * height;
    for (UINT32 row = 0; row < height / 2; ++row) {
        BYTE* uv = uvBase + static_cast<size_t>(row) * pitch;
        bool white = row * 2 - stripe < stripeH;
        for (UINT32 x = 0; x < width; x += 2) {
            const BYTE* b = bars[x * 8 / width];
            uv[x] = white ? 128 : b[1];
            uv[x + 1] = white ? 128 : b[2];
        }
    }
}

#define OWC_EVENT_GENERATOR                                                                            \
    IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** e) override { return queue_->GetEvent(flags, e); } \
    IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override {                     \
        return queue_->BeginGetEvent(cb, state);                                                      \
    }                                                                                                  \
    IFACEMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override {                        \
        return queue_->EndGetEvent(r, e);                                                             \
    }                                                                                                  \
    IFACEMETHODIMP QueueEvent(MediaEventType t, REFGUID ext, HRESULT hr, const PROPVARIANT* v) override { \
        return queue_->QueueEventParamVar(t, ext, hr, v);                                             \
    }

// Virtual camera sources must expose IKsControl; we support no KS properties.
#define OWC_NO_KS_CONTROL                                                                    \
    IFACEMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override {          \
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);                                     \
    }                                                                                        \
    IFACEMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override {              \
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);                                     \
    }                                                                                        \
    IFACEMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override {                \
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);                                     \
    }

class MediaStream
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                          ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>, IKsControl> {
public:
    HRESULT RuntimeClassInitialize(IMFMediaSource* source) {
        source_ = source;
        RETURN_IF_FAILED_(MFCreateEventQueue(&queue_));
        RETURN_IF_FAILED_(MFCreateAttributes(&attrs_, 4));
        RETURN_IF_FAILED_(attrs_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
        RETURN_IF_FAILED_(attrs_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0));
        RETURN_IF_FAILED_(attrs_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
        RETURN_IF_FAILED_(attrs_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color));

        std::vector<ComPtr<IMFMediaType>> types;
        for (const auto& size : kSizes) {
            ComPtr<IMFMediaType> type;
            RETURN_IF_FAILED_(MFCreateMediaType(&type));
            RETURN_IF_FAILED_(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
            RETURN_IF_FAILED_(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
            RETURN_IF_FAILED_(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, size.w, size.h));
            RETURN_IF_FAILED_(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, kFps, 1));
            RETURN_IF_FAILED_(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
            RETURN_IF_FAILED_(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
            RETURN_IF_FAILED_(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
            RETURN_IF_FAILED_(type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
            RETURN_IF_FAILED_(type->SetUINT32(MF_MT_DEFAULT_STRIDE, size.w));
            RETURN_IF_FAILED_(type->SetUINT32(MF_MT_SAMPLE_SIZE, size.w * size.h * 3 / 2));
            types.push_back(type);
        }
        std::vector<IMFMediaType*> raw;
        for (auto& t : types) raw.push_back(t.Get());
        RETURN_IF_FAILED_(MFCreateStreamDescriptor(0, static_cast<DWORD>(raw.size()), raw.data(), &sd_));
        ComPtr<IMFMediaTypeHandler> handler;
        RETURN_IF_FAILED_(sd_->GetMediaTypeHandler(&handler));
        RETURN_IF_FAILED_(handler->SetCurrentMediaType(raw[0]));
        return attrs_->CopyAllItems(sd_.Get());
    }

    IMFAttributes* Attributes() { return attrs_.Get(); }

    HRESULT Start(UINT32 width, UINT32 height) {
        if (!width || !height) width = kSizes[0].w, height = kSizes[0].h;
        Log("Start %ux%u", width, height);
        {
            std::lock_guard<std::mutex> lock(lock_);
            if (shutdown_) return MF_E_SHUTDOWN;
            state_ = MF_STREAM_STATE_RUNNING;
            outW_ = width;
            outH_ = height;
            if (!thread_.joinable()) {
                quit_ = false;
                thread_ = std::thread(&MediaStream::Run, this);
                recvThread_ = std::thread(&MediaStream::Receive, this);
            }
        }
        return queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
    }

    HRESULT Stop() {
        StopThreads();
        {
            std::lock_guard<std::mutex> lock(lock_);
            if (shutdown_) return MF_E_SHUTDOWN;
            state_ = MF_STREAM_STATE_STOPPED;
            tokens_.clear();
        }
        return queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    }

    void Shutdown() {
        StopThreads();
        std::lock_guard<std::mutex> lock(lock_);
        shutdown_ = true;
        tokens_.clear();
        queue_->Shutdown();
        source_.Reset();  // breaks the source <-> stream reference cycle
    }

    // IMFMediaEventGenerator
    OWC_EVENT_GENERATOR

    // IMFMediaStream
    IFACEMETHODIMP GetMediaSource(IMFMediaSource** source) override {
        std::lock_guard<std::mutex> lock(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return source_.CopyTo(source);
    }
    IFACEMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** sd) override {
        std::lock_guard<std::mutex> lock(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return sd_.CopyTo(sd);
    }
    IFACEMETHODIMP RequestSample(IUnknown* token) override {
        std::lock_guard<std::mutex> lock(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_MEDIA_SOURCE_WRONGSTATE;
        tokens_.emplace_back(token);
        cv_.notify_all();
        return S_OK;
    }

    // IMFMediaStream2
    IFACEMETHODIMP SetStreamState(MF_STREAM_STATE state) override {
        std::lock_guard<std::mutex> lock(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;
        state_ = state;
        if (state != MF_STREAM_STATE_RUNNING) tokens_.clear();
        return S_OK;
    }
    IFACEMETHODIMP GetStreamState(MF_STREAM_STATE* state) override {
        std::lock_guard<std::mutex> lock(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;
        *state = state_;
        return S_OK;
    }

    // IKsControl
    OWC_NO_KS_CONTROL

private:
    void StopThreads() {
        {
            std::lock_guard<std::mutex> lock(lock_);
            quit_ = true;
        }
        cv_.notify_all();
        SOCKET s = sock_.exchange(INVALID_SOCKET);  // unblocks recv() in Receive
        if (s != INVALID_SOCKET) closesocket(s);
        if (thread_.joinable()) thread_.join();
        if (recvThread_.joinable()) recvThread_.join();
    }

    bool Quitting() {
        std::lock_guard<std::mutex> lock(lock_);
        return quit_;
    }

    void WaitUnlessQuitting(std::chrono::seconds d) {
        std::unique_lock<std::mutex> lock(lock_);
        cv_.wait_for(lock, d, [this] { return quit_; });
    }

    // Connects to the phone, decodes, fits frames to the app's size and
    // publishes them; reconnects until Stop.
    void Receive() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        bool searching = false, nearestOnly = false;
        owc::Scaler scaler;
        std::vector<uint8_t> pkt, decoded, rotated, scaled, fitted;
        while (!Quitting()) {
            owc::StreamInfo info;
            SOCKET s = owc::FindPhone(owc::CandidateHosts(), info, [](const char*, bool) {});
            if (s == INVALID_SOCKET) {
                if (!searching) Log("Phone not found; retrying every 2 s");
                searching = true;
                WaitUnlessQuitting(std::chrono::seconds(2));
                continue;
            }
            searching = false;
            sock_ = s;
            Log("Phone connected: %ux%u, rotation %u", info.width, info.height, info.rotation);

            std::unique_ptr<owc::H264Decoder> dec;
            auto onFrame = [&](IMFSample* sample) {
                if (dec->width < info.width || dec->height < info.height) return;  // stale size
                if (FAILED(owc::CopyNV12(sample, info.width, info.height, dec->stride, decoded))) return;
                UINT32 w = info.width, h = info.height;
                std::vector<uint8_t>* frame = &decoded;
                if (info.rotation) {
                    owc::RotateNV12(decoded, w, h, info.rotation, rotated);
                    if (info.rotation != 180) std::swap(w, h);
                    frame = &rotated;
                }
                UINT32 ow, oh;
                {
                    std::lock_guard<std::mutex> lock(lock_);
                    ow = outW_;
                    oh = outH_;
                }
                if (w != ow || h != oh) {
                    UINT32 dw, dh;
                    owc::FitSize(w, h, ow, oh, dw, dh);
                    if (dw != w || dh != h) {
                        HRESULT hr = nearestOnly ? E_FAIL : scaler.Scale(*frame, w, h, dw, dh, scaled);
                        if (FAILED(hr)) {
                            if (!nearestOnly) Log("Video processor failed (0x%08lX); using basic scaling", static_cast<unsigned long>(hr));
                            nearestOnly = true;
                            owc::ScaleNearest(*frame, w, h, dw, dh, scaled);
                        }
                        frame = &scaled;
                        w = dw;
                        h = dh;
                    }
                    if (w != ow || h != oh) {
                        owc::Letterbox(*frame, w, h, ow, oh, fitted);
                        frame = &fitted;
                    }
                }
                std::lock_guard<std::mutex> lock(lock_);
                if (ow != outW_ || oh != outH_) return;  // app switched size meanwhile
                frame_.swap(*frame);
                frameW_ = ow;
                frameH_ = oh;
                haveNewFrame_ = true;
                lastFrameAt_ = std::chrono::steady_clock::now();
                cv_.notify_all();
            };

            dec = std::make_unique<owc::H264Decoder>();
            HRESULT hr = dec->Init(info.width, info.height, kFps);
            if (FAILED(hr)) Log("H.264 decoder init failed: 0x%08lX", static_cast<unsigned long>(hr));
            uint8_t type = 0;
            while (SUCCEEDED(hr) && !Quitting() && owc::ReadPacket(s, type, pkt)) {
                if (type == owc::kTypeVideo) {
                    hr = dec->FeedPacket(pkt, MFGetSystemTime(), onFrame);
                    if (FAILED(hr)) Log("Decode failed: 0x%08lX", static_cast<unsigned long>(hr));
                } else if (type == owc::kTypeInfo) {
                    owc::StreamInfo next;
                    if (!owc::ParseInfo(pkt, next)) continue;
                    if (next.width != info.width || next.height != info.height) {
                        dec = std::make_unique<owc::H264Decoder>();  // new size: phone restarts from a keyframe
                        hr = dec->Init(next.width, next.height, kFps);
                        if (FAILED(hr)) Log("H.264 decoder init failed: 0x%08lX", static_cast<unsigned long>(hr));
                    }
                    if (next.width != info.width || next.height != info.height || next.rotation != info.rotation)
                        Log("Phone now sends %ux%u, rotation %u", next.width, next.height, next.rotation);
                    info = next;
                }
            }
            SOCKET mine = sock_.exchange(INVALID_SOCKET);
            if (mine != INVALID_SOCKET) closesocket(mine);
            Log("Phone disconnected");
            if (FAILED(hr)) WaitUnlessQuitting(std::chrono::seconds(10));  // e.g. no decoder: don't hammer the phone
        }
        WSACleanup();
        CoUninitialize();
    }

    // Answers RequestSample: each new phone frame immediately; colour bars at
    // 30 fps once no frame has arrived for a second.
    void Run() {
        const auto interval = std::chrono::microseconds(1'000'000 / kFps);
        auto nextPattern = std::chrono::steady_clock::now() + interval;
        std::unique_lock<std::mutex> lock(lock_);
        for (;;) {
            cv_.wait_until(lock, nextPattern, [this] { return quit_ || (haveNewFrame_ && !tokens_.empty()); });
            if (quit_) break;
            auto now = std::chrono::steady_clock::now();
            bool live = now - lastFrameAt_ < std::chrono::seconds(1);
            bool sendFrame = haveNewFrame_ && !tokens_.empty();
            bool sendPattern = !sendFrame && !live && now >= nextPattern && !tokens_.empty();
            if (now >= nextPattern) nextPattern = now + interval;
            if (state_ != MF_STREAM_STATE_RUNNING || !(sendFrame || sendPattern)) continue;

            ComPtr<IUnknown> token = std::move(tokens_.front());
            tokens_.pop_front();
            if (sendFrame) haveNewFrame_ = false;
            ComPtr<IMFSample> sample;
            HRESULT hr = MakeSample(token.Get(), sendFrame, &sample);
            if (SUCCEEDED(hr)) hr = queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
            if (FAILED(hr)) Log("Sample delivery failed: 0x%08lX", static_cast<unsigned long>(hr));
        }
    }

    // Called with lock_ held.
    HRESULT MakeSample(IUnknown* token, bool live, IMFSample** out) {
        const UINT32 w = outW_, h = outH_;
        if (frameW_ != w || frameH_ != h) live = false;  // app switched size; next frame will match
        ComPtr<IMFMediaBuffer> buf;
        RETURN_IF_FAILED_(MFCreate2DMediaBuffer(w, h, MFVideoFormat_NV12.Data1, FALSE, &buf));
        ComPtr<IMF2DBuffer2> buf2d;
        RETURN_IF_FAILED_(buf.As(&buf2d));
        BYTE *scan0 = nullptr, *start = nullptr;
        LONG pitch = 0;
        DWORD len = 0;
        RETURN_IF_FAILED_(buf2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scan0, &pitch, &start, &len));
        if (live) {
            for (UINT32 row = 0; row < h * 3 / 2; ++row)  // Y rows, then interleaved UV rows
                memcpy(scan0 + static_cast<size_t>(row) * pitch, &frame_[static_cast<size_t>(row) * w], w);
        } else {
            DrawPattern(scan0, pitch, w, h, patternFrame_++);
        }
        buf2d->Unlock2D();
        RETURN_IF_FAILED_(buf->SetCurrentLength(len));

        ComPtr<IMFSample> sample;
        RETURN_IF_FAILED_(MFCreateSample(&sample));
        RETURN_IF_FAILED_(sample->AddBuffer(buf.Get()));
        RETURN_IF_FAILED_(sample->SetSampleTime(MFGetSystemTime()));
        RETURN_IF_FAILED_(sample->SetSampleDuration(10'000'000 / kFps));
        RETURN_IF_FAILED_(sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE));
        if (token) RETURN_IF_FAILED_(sample->SetUnknown(MFSampleExtension_Token, token));
        *out = sample.Detach();
        return S_OK;
    }

    std::mutex lock_;
    std::condition_variable cv_;
    std::thread thread_, recvThread_;
    std::atomic<SOCKET> sock_{INVALID_SOCKET};
    bool quit_ = false;
    bool shutdown_ = false;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    std::deque<ComPtr<IUnknown>> tokens_;
    UINT32 outW_ = 0, outH_ = 0;      // size the app picked
    std::vector<uint8_t> frame_;      // newest phone frame fitted to frameW_ x frameH_, tightly packed NV12
    UINT32 frameW_ = 0, frameH_ = 0;
    bool haveNewFrame_ = false;
    std::chrono::steady_clock::time_point lastFrameAt_{};
    uint64_t patternFrame_ = 0;
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFAttributes> attrs_;
    ComPtr<IMFStreamDescriptor> sd_;
};

class MediaSource
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                          ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>, IMFGetService,
                          IKsControl> {
public:
    HRESULT RuntimeClassInitialize() {
        wchar_t user[256] = L"?", image[MAX_PATH] = L"?";
        DWORD userLen = ARRAYSIZE(user);
        GetUserNameW(user, &userLen);
        GetModuleFileNameW(nullptr, image, MAX_PATH);
        Log("MediaSource created in %ls as %ls", image, user);

        RETURN_IF_FAILED_(MFStartup(MF_VERSION, MFSTARTUP_LITE));
        mfStarted_ = true;
        RETURN_IF_FAILED_(MFCreateEventQueue(&queue_));
        RETURN_IF_FAILED_(MFCreateAttributes(&attrs_, 1));
        RETURN_IF_FAILED_(MakeAndInitialize<MediaStream>(&stream_, static_cast<IMFMediaSource*>(this)));
        ComPtr<IMFStreamDescriptor> sd;
        RETURN_IF_FAILED_(stream_->GetStreamDescriptor(&sd));
        RETURN_IF_FAILED_(MFCreatePresentationDescriptor(1, sd.GetAddressOf(), &pd_));
        return pd_->SelectStream(0);
    }

    ~MediaSource() {
        if (mfStarted_) MFShutdown();
    }

    // IMFMediaEventGenerator
    OWC_EVENT_GENERATOR

    // IMFMediaSource
    IFACEMETHODIMP GetCharacteristics(DWORD* c) override {
        std::lock_guard<std::mutex> lock(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;
        *c = MFMEDIASOURCE_IS_LIVE;
        return S_OK;
    }
    IFACEMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pd) override {
        std::lock_guard<std::mutex> lock(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return pd_->Clone(pd);
    }
    IFACEMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT* startPos) override {
        if (!pd || !startPos) return E_INVALIDARG;
        if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
        std::lock_guard<std::mutex> lock(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;

        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        RETURN_IF_FAILED_(pd->GetStreamDescriptorByIndex(0, &selected, &sd));
        if (selected) {
            UINT32 w = 0, h = 0;
            ComPtr<IMFMediaTypeHandler> handler;
            ComPtr<IMFMediaType> type;
            if (SUCCEEDED(sd->GetMediaTypeHandler(&handler)) && SUCCEEDED(handler->GetCurrentMediaType(&type)))
                MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
            RETURN_IF_FAILED_(queue_->QueueEventParamUnk(streamActive_ ? MEUpdatedStream : MENewStream, GUID_NULL,
                                                         S_OK, static_cast<IMFMediaStream2*>(stream_.Get())));
            streamActive_ = true;
            RETURN_IF_FAILED_(stream_->Start(w, h));
        }
        return queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, startPos);
    }
    IFACEMETHODIMP Stop() override {
        std::lock_guard<std::mutex> lock(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;
        Log("Stop");
        RETURN_IF_FAILED_(stream_->Stop());
        return queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
    }
    IFACEMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
    IFACEMETHODIMP Shutdown() override {
        {
            std::lock_guard<std::mutex> lock(lock_);
            if (shutdown_) return MF_E_SHUTDOWN;
            shutdown_ = true;
        }
        Log("Shutdown");
        stream_->Shutdown();
        queue_->Shutdown();
        return S_OK;
    }

    // IMFMediaSourceEx
    IFACEMETHODIMP GetSourceAttributes(IMFAttributes** a) override {
        std::lock_guard<std::mutex> lock(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return attrs_.CopyTo(a);
    }
    IFACEMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** a) override {
        std::lock_guard<std::mutex> lock(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        *a = stream_->Attributes();
        (*a)->AddRef();
        return S_OK;
    }
    IFACEMETHODIMP SetD3DManager(IUnknown*) override { return S_OK; }  // system-memory samples only

    // IMFGetService
    IFACEMETHODIMP GetService(REFGUID, REFIID, LPVOID*) override { return MF_E_UNSUPPORTED_SERVICE; }

    // IKsControl
    OWC_NO_KS_CONTROL

private:
    std::mutex lock_;
    bool shutdown_ = false;
    bool streamActive_ = false;
    bool mfStarted_ = false;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFAttributes> attrs_;
    ComPtr<IMFPresentationDescriptor> pd_;
    ComPtr<MediaStream> stream_;
};

}  // namespace

// Frame Server CoCreates this CLSID (OWC_CLSID_STRING) and expects an IMFActivate.
class __declspec(uuid("EAE9430F-FC57-419D-A8BE-9599849F761A")) Activator
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFActivate, IMFAttributes>> {
public:
    Activator() { MFCreateAttributes(&attrs_, 4); }

    // IMFActivate
    IFACEMETHODIMP ActivateObject(REFIID riid, void** ppv) override {
        ComPtr<MediaSource> source;
        HRESULT hr = MakeAndInitialize<MediaSource>(&source);
        if (FAILED(hr)) {
            Log("MediaSource init failed: 0x%08lX", static_cast<unsigned long>(hr));
            return hr;
        }
        source_ = source;
        return source.CopyTo(riid, ppv);
    }
    IFACEMETHODIMP ShutdownObject() override {
        if (source_) source_->Shutdown();
        source_.Reset();
        return S_OK;
    }
    IFACEMETHODIMP DetachObject() override {
        source_.Reset();
        return S_OK;
    }

    // IMFAttributes: delegate to an attribute store.
    IFACEMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return attrs_->GetItem(k, v); }
    IFACEMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return attrs_->GetItemType(k, t); }
    IFACEMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return attrs_->CompareItem(k, v, r); }
    IFACEMETHODIMP Compare(IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override {
        return attrs_->Compare(a, m, r);
    }
    IFACEMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return attrs_->GetUINT32(k, v); }
    IFACEMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return attrs_->GetUINT64(k, v); }
    IFACEMETHODIMP GetDouble(REFGUID k, double* v) override { return attrs_->GetDouble(k, v); }
    IFACEMETHODIMP GetGUID(REFGUID k, GUID* v) override { return attrs_->GetGUID(k, v); }
    IFACEMETHODIMP GetStringLength(REFGUID k, UINT32* n) override { return attrs_->GetStringLength(k, n); }
    IFACEMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 n, UINT32* len) override {
        return attrs_->GetString(k, s, n, len);
    }
    IFACEMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* len) override {
        return attrs_->GetAllocatedString(k, s, len);
    }
    IFACEMETHODIMP GetBlobSize(REFGUID k, UINT32* n) override { return attrs_->GetBlobSize(k, n); }
    IFACEMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* len) override {
        return attrs_->GetBlob(k, b, n, len);
    }
    IFACEMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) override {
        return attrs_->GetAllocatedBlob(k, b, n);
    }
    IFACEMETHODIMP GetUnknown(REFGUID k, REFIID riid, LPVOID* p) override { return attrs_->GetUnknown(k, riid, p); }
    IFACEMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return attrs_->SetItem(k, v); }
    IFACEMETHODIMP DeleteItem(REFGUID k) override { return attrs_->DeleteItem(k); }
    IFACEMETHODIMP DeleteAllItems() override { return attrs_->DeleteAllItems(); }
    IFACEMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return attrs_->SetUINT32(k, v); }
    IFACEMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return attrs_->SetUINT64(k, v); }
    IFACEMETHODIMP SetDouble(REFGUID k, double v) override { return attrs_->SetDouble(k, v); }
    IFACEMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return attrs_->SetGUID(k, v); }
    IFACEMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return attrs_->SetString(k, v); }
    IFACEMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return attrs_->SetBlob(k, b, n); }
    IFACEMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override { return attrs_->SetUnknown(k, u); }
    IFACEMETHODIMP LockStore() override { return attrs_->LockStore(); }
    IFACEMETHODIMP UnlockStore() override { return attrs_->UnlockStore(); }
    IFACEMETHODIMP GetCount(UINT32* n) override { return attrs_->GetCount(n); }
    IFACEMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override {
        return attrs_->GetItemByIndex(i, k, v);
    }
    IFACEMETHODIMP CopyAllItems(IMFAttributes* dest) override { return attrs_->CopyAllItems(dest); }

private:
    ComPtr<IMFAttributes> attrs_;
    ComPtr<MediaSource> source_;
};

CoCreatableClass(Activator);

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    return Module<InProc>::GetModule().GetClassObject(clsid, riid, ppv);
}

STDAPI DllCanUnloadNow() {
    return Module<InProc>::GetModule().GetObjectCount() == 0 ? S_OK : S_FALSE;
}
