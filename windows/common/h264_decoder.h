// H.264 (Annex-B access units) -> NV12 using the inbox Media Foundation
// decoder, shared by owc-probe and the virtual camera. Caller must have called
// CoInitializeEx and MFStartup on the thread/process.
#pragma once

#include "net.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#ifndef RETURN_IF_FAILED_
#define RETURN_IF_FAILED_(expr)      \
    do {                             \
        HRESULT hr_ = (expr);        \
        if (FAILED(hr_)) return hr_; \
    } while (0)
#endif

namespace owc {

class H264Decoder {
public:
    using FrameFn = std::function<void(IMFSample*)>;

    // Current output geometry (updated on stream changes).
    UINT32 width = 0, height = 0;
    LONG stride = 0;

    HRESULT Init(UINT32 w, UINT32 h, UINT32 fps) {
        RETURN_IF_FAILED_(CoCreateInstance(CLSID_MSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_)));
        Microsoft::WRL::ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(mft_->GetAttributes(&attrs))) attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);

        Microsoft::WRL::ComPtr<IMFMediaType> in;
        RETURN_IF_FAILED_(MFCreateMediaType(&in));
        RETURN_IF_FAILED_(in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        RETURN_IF_FAILED_(in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
        RETURN_IF_FAILED_(in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
        RETURN_IF_FAILED_(MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, w, h));
        RETURN_IF_FAILED_(MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps, 1));
        RETURN_IF_FAILED_(mft_->SetInputType(0, in.Get(), 0));
        RETURN_IF_FAILED_(SelectNV12());
        RETURN_IF_FAILED_(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
        return mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }

    // One wire packet. Packets before the first IDR are dropped; the latest
    // SPS/PPS-only packet is replayed ahead of it if the IDR lacks its own.
    HRESULT FeedPacket(const std::vector<uint8_t>& pkt, LONGLONG time, const FrameFn& onFrame) {
        if (!started_) {
            uint32_t nal = NalTypes(pkt);
            if (nal & (1u << 7)) config_ = pkt;
            if (!(nal & (1u << 5))) return S_OK;
            started_ = true;
            if (!(nal & (1u << 7)) && !config_.empty()) RETURN_IF_FAILED_(Feed(config_, time, onFrame));
        }
        return Feed(pkt, time, onFrame);
    }

private:
    HRESULT SelectNV12() {
        for (DWORD i = 0;; ++i) {
            Microsoft::WRL::ComPtr<IMFMediaType> t;
            RETURN_IF_FAILED_(mft_->GetOutputAvailableType(0, i, &t));  // MF_E_NO_MORE_TYPES: no NV12
            GUID sub;
            if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub)) || sub != MFVideoFormat_NV12) continue;
            RETURN_IF_FAILED_(mft_->SetOutputType(0, t.Get(), 0));
            RETURN_IF_FAILED_(MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &width, &height));
            stride = static_cast<LONG>(MFGetAttributeUINT32(t.Get(), MF_MT_DEFAULT_STRIDE, width));
            MFT_OUTPUT_STREAM_INFO info{};
            RETURN_IF_FAILED_(mft_->GetOutputStreamInfo(0, &info));
            outSize_ = info.cbSize;
            return S_OK;
        }
    }

    // Software decoder: we allocate output samples (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES is not set).
    HRESULT Drain(const FrameFn& onFrame) {
        for (;;) {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
            RETURN_IF_FAILED_(MFCreateMemoryBuffer(outSize_, &buf));
            Microsoft::WRL::ComPtr<IMFSample> s;
            RETURN_IF_FAILED_(MFCreateSample(&s));
            RETURN_IF_FAILED_(s->AddBuffer(buf.Get()));
            MFT_OUTPUT_DATA_BUFFER out{};
            out.pSample = s.Get();
            DWORD status = 0;
            HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
            if (out.pEvents) out.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                RETURN_IF_FAILED_(SelectNV12());
                continue;
            }
            RETURN_IF_FAILED_(hr);
            onFrame(s.Get());
        }
    }

    HRESULT Feed(const std::vector<uint8_t>& pkt, LONGLONG time, const FrameFn& onFrame) {
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
        RETURN_IF_FAILED_(MFCreateMemoryBuffer(static_cast<DWORD>(pkt.size()), &buf));
        BYTE* p = nullptr;
        RETURN_IF_FAILED_(buf->Lock(&p, nullptr, nullptr));
        memcpy(p, pkt.data(), pkt.size());
        buf->Unlock();
        RETURN_IF_FAILED_(buf->SetCurrentLength(static_cast<DWORD>(pkt.size())));
        Microsoft::WRL::ComPtr<IMFSample> s;
        RETURN_IF_FAILED_(MFCreateSample(&s));
        RETURN_IF_FAILED_(s->AddBuffer(buf.Get()));
        RETURN_IF_FAILED_(s->SetSampleTime(time));
        for (;;) {
            HRESULT hr = mft_->ProcessInput(0, s.Get(), 0);
            if (hr != MF_E_NOTACCEPTING) {
                RETURN_IF_FAILED_(hr);
                break;
            }
            RETURN_IF_FAILED_(Drain(onFrame));
        }
        return Drain(onFrame);
    }

    Microsoft::WRL::ComPtr<IMFTransform> mft_;
    DWORD outSize_ = 0;
    bool started_ = false;
    std::vector<uint8_t> config_;
};

// Copies a decoded NV12 sample (decoder stride, possibly padded height) into a
// tightly packed w*h*3/2 buffer.
inline HRESULT CopyNV12(IMFSample* sample, UINT32 w, UINT32 h, LONG stride, std::vector<uint8_t>& out) {
    Microsoft::WRL::ComPtr<IMFMediaBuffer> b;
    RETURN_IF_FAILED_(sample->ConvertToContiguousBuffer(&b));
    BYTE* p = nullptr;
    DWORD len = 0;
    RETURN_IF_FAILED_(b->Lock(&p, nullptr, &len));
    UINT32 allocH = stride > 0 ? len * 2 / (3 * static_cast<DWORD>(stride)) : 0;
    HRESULT hr = S_OK;
    if (stride < static_cast<LONG>(w) || allocH < h) {
        hr = E_UNEXPECTED;
    } else {
        out.resize(static_cast<size_t>(w) * h * 3 / 2);
        for (UINT32 y = 0; y < h; ++y) memcpy(&out[static_cast<size_t>(y) * w], p + static_cast<size_t>(y) * stride, w);
        const BYTE* uv = p + static_cast<size_t>(stride) * allocH;
        BYTE* dstUV = &out[static_cast<size_t>(w) * h];
        for (UINT32 y = 0; y < h / 2; ++y) memcpy(dstUV + static_cast<size_t>(y) * w, uv + static_cast<size_t>(y) * stride, w);
    }
    b->Unlock();
    return hr;
}

}  // namespace owc
