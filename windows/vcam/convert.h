// Fits a decoded phone frame into the size the PC app picked: rotate (CPU),
// scale (inbox Video Processor MFT, nearest-neighbour fallback), letterbox.
// All buffers are tightly packed NV12.
#pragma once

#include "../common/h264_decoder.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace owc {

// Clockwise rotation by 90/180/270 degrees.
inline void RotateNV12(const std::vector<uint8_t>& src, UINT32 w, UINT32 h, int rotation, std::vector<uint8_t>& dst) {
    const bool swap = rotation == 90 || rotation == 270;
    dst.resize(src.size());
    // One plane of sw x sh elements, bpp bytes each (Y: 1, interleaved UV: 2).
    auto plane = [&](const uint8_t* s, uint8_t* d, UINT32 sw, UINT32 sh, UINT32 bpp) {
        const UINT32 ow = swap ? sh : sw, oh = swap ? sw : sh;
        for (UINT32 y = 0; y < oh; ++y) {
            uint8_t* row = d + static_cast<size_t>(y) * ow * bpp;
            for (UINT32 x = 0; x < ow; ++x) {
                UINT32 sx, sy;
                if (rotation == 90) {
                    sx = y;
                    sy = sh - 1 - x;
                } else if (rotation == 180) {
                    sx = sw - 1 - x;
                    sy = sh - 1 - y;
                } else {  // 270
                    sx = sw - 1 - y;
                    sy = x;
                }
                const uint8_t* p = s + (static_cast<size_t>(sy) * sw + sx) * bpp;
                row[x * bpp] = p[0];
                if (bpp == 2) row[x * bpp + 1] = p[1];
            }
        }
    };
    plane(src.data(), dst.data(), w, h, 1);
    plane(src.data() + static_cast<size_t>(w) * h, dst.data() + static_cast<size_t>(w) * h, w / 2, h / 2, 2);
}

// Largest even size with the source's aspect ratio that fits in ow x oh.
inline void FitSize(UINT32 sw, UINT32 sh, UINT32 ow, UINT32 oh, UINT32& dw, UINT32& dh) {
    double scale = (std::min)(static_cast<double>(ow) / sw, static_cast<double>(oh) / sh);
    dw = (std::max)(2u, (std::min)(ow, static_cast<UINT32>(sw * scale + 0.5)) & ~1u);
    dh = (std::max)(2u, (std::min)(oh, static_cast<UINT32>(sh * scale + 0.5)) & ~1u);
}

// Centres src (sw x sh) in a black ow x oh frame.
inline void Letterbox(const std::vector<uint8_t>& src, UINT32 sw, UINT32 sh, UINT32 ow, UINT32 oh,
                      std::vector<uint8_t>& dst) {
    const size_t ySize = static_cast<size_t>(ow) * oh;
    dst.assign(ySize, 16);
    dst.resize(ySize * 3 / 2, 128);
    const UINT32 x0 = ((ow - sw) / 2) & ~1u, y0 = ((oh - sh) / 2) & ~1u;
    for (UINT32 y = 0; y < sh; ++y)
        memcpy(&dst[static_cast<size_t>(y0 + y) * ow + x0], &src[static_cast<size_t>(y) * sw], sw);
    for (UINT32 y = 0; y < sh / 2; ++y)
        memcpy(&dst[ySize + static_cast<size_t>(y0 / 2 + y) * ow + x0],
               &src[static_cast<size_t>(sw) * sh + static_cast<size_t>(y) * sw], sw);
}

inline void ScaleNearest(const std::vector<uint8_t>& src, UINT32 sw, UINT32 sh, UINT32 dw, UINT32 dh,
                         std::vector<uint8_t>& dst) {
    dst.resize(static_cast<size_t>(dw) * dh * 3 / 2);
    for (UINT32 y = 0; y < dh; ++y) {
        const uint8_t* s = &src[static_cast<size_t>(y * sh / dh) * sw];
        uint8_t* d = &dst[static_cast<size_t>(y) * dw];
        for (UINT32 x = 0; x < dw; ++x) d[x] = s[x * sw / dw];
    }
    const uint8_t* suv = &src[static_cast<size_t>(sw) * sh];
    uint8_t* duv = &dst[static_cast<size_t>(dw) * dh];
    for (UINT32 y = 0; y < dh / 2; ++y) {
        const uint8_t* s = suv + static_cast<size_t>(y * sh / dh) * sw;
        uint8_t* d = duv + static_cast<size_t>(y) * dw;
        for (UINT32 x = 0; x < dw / 2; ++x) {
            UINT32 sx = (x * sw / dw) * 2;
            d[x * 2] = s[sx];
            d[x * 2 + 1] = s[sx + 1];
        }
    }
}

// NV12 resize through the inbox Video Processor MFT (software mode, no D3D).
class Scaler {
public:
    HRESULT Scale(const std::vector<uint8_t>& src, UINT32 sw, UINT32 sh, UINT32 dw, UINT32 dh,
                  std::vector<uint8_t>& dst) {
        if (!mft_ || sw != sw_ || sh != sh_ || dw != dw_ || dh != dh_) RETURN_IF_FAILED_(Configure(sw, sh, dw, dh));

        Microsoft::WRL::ComPtr<IMFMediaBuffer> inBuf;
        RETURN_IF_FAILED_(MFCreateMemoryBuffer(static_cast<DWORD>(src.size()), &inBuf));
        BYTE* p = nullptr;
        RETURN_IF_FAILED_(inBuf->Lock(&p, nullptr, nullptr));
        memcpy(p, src.data(), src.size());
        inBuf->Unlock();
        RETURN_IF_FAILED_(inBuf->SetCurrentLength(static_cast<DWORD>(src.size())));
        Microsoft::WRL::ComPtr<IMFSample> in;
        RETURN_IF_FAILED_(MFCreateSample(&in));
        RETURN_IF_FAILED_(in->AddBuffer(inBuf.Get()));
        RETURN_IF_FAILED_(in->SetSampleTime(MFGetSystemTime()));
        RETURN_IF_FAILED_(in->SetSampleDuration(333333));
        RETURN_IF_FAILED_(mft_->ProcessInput(0, in.Get(), 0));

        MFT_OUTPUT_STREAM_INFO info{};
        RETURN_IF_FAILED_(mft_->GetOutputStreamInfo(0, &info));
        const bool provides = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        Microsoft::WRL::ComPtr<IMFSample> out;
        if (!provides) {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> outBuf;
            RETURN_IF_FAILED_(MFCreateMemoryBuffer(info.cbSize, &outBuf));
            RETURN_IF_FAILED_(MFCreateSample(&out));
            RETURN_IF_FAILED_(out->AddBuffer(outBuf.Get()));
        }
        MFT_OUTPUT_DATA_BUFFER ob{};
        ob.pSample = out.Get();
        DWORD status = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        if (provides && ob.pSample) out.Attach(ob.pSample);
        RETURN_IF_FAILED_(hr);
        return CopyNV12(out.Get(), dw, dh, static_cast<LONG>(dw), dst);
    }

private:
    static HRESULT MakeType(UINT32 w, UINT32 h, IMFMediaType** out) {
        Microsoft::WRL::ComPtr<IMFMediaType> t;
        RETURN_IF_FAILED_(MFCreateMediaType(&t));
        RETURN_IF_FAILED_(t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        RETURN_IF_FAILED_(t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
        RETURN_IF_FAILED_(MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, w, h));
        RETURN_IF_FAILED_(MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, 30, 1));
        RETURN_IF_FAILED_(MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
        RETURN_IF_FAILED_(t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
        RETURN_IF_FAILED_(t->SetUINT32(MF_MT_DEFAULT_STRIDE, w));
        RETURN_IF_FAILED_(t->SetUINT32(MF_MT_SAMPLE_SIZE, w * h * 3 / 2));
        RETURN_IF_FAILED_(t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
        *out = t.Detach();
        return S_OK;
    }

    HRESULT Configure(UINT32 sw, UINT32 sh, UINT32 dw, UINT32 dh) {
        mft_.Reset();
        Microsoft::WRL::ComPtr<IMFTransform> mft;
        RETURN_IF_FAILED_(CoCreateInstance(CLSID_VideoProcessorMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft)));
        Microsoft::WRL::ComPtr<IMFMediaType> in, out;
        RETURN_IF_FAILED_(MakeType(sw, sh, &in));
        RETURN_IF_FAILED_(MakeType(dw, dh, &out));
        RETURN_IF_FAILED_(mft->SetInputType(0, in.Get(), 0));
        RETURN_IF_FAILED_(mft->SetOutputType(0, out.Get(), 0));
        RETURN_IF_FAILED_(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
        mft_ = mft;
        sw_ = sw, sh_ = sh, dw_ = dw, dh_ = dh;
        return S_OK;
    }

    Microsoft::WRL::ComPtr<IMFTransform> mft_;
    UINT32 sw_ = 0, sh_ = 0, dw_ = 0, dh_ = 0;
};

}  // namespace owc
