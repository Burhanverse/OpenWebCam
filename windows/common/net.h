// Phone discovery and the length-prefixed H.264 wire protocol, shared by
// owc-probe and the virtual camera. Caller must call WSAStartup first.
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>
#include <vector>

namespace owc {

const uint16_t kPort = 5000;
const uint32_t kMaxPacket = 8 * 1024 * 1024;  // a 4K keyframe fits comfortably

// USB tethering shows up as an RNDIS or NCM network adapter.
inline bool IsTetherAdapter(const IP_ADAPTER_ADDRESSES* a) {
    std::wstring desc = a->Description;
    std::transform(desc.begin(), desc.end(), desc.begin(), towupper);
    return desc.find(L"NDIS") != std::wstring::npos || desc.find(L"NCM") != std::wstring::npos;
}

// Runs fn on every adapter (GetAdaptersAddresses with the given family/flags).
template <class Fn>
bool ForEachAdapter(ULONG family, ULONG flags, Fn&& fn) {
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf;
    ULONG rc;
    do {
        buf.resize(size);
        rc = GetAdaptersAddresses(family, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    } while (rc == ERROR_BUFFER_OVERFLOW);
    if (rc != NO_ERROR) return false;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) fn(a);
    return true;
}

// IPv4 gateways of adapters that are up. The phone is the gateway of the
// tethering adapter; RNDIS/NCM adapters are tried first.
inline std::vector<std::string> CandidateHosts() {
    std::vector<std::string> usb, other;
    ForEachAdapter(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, [&](const IP_ADAPTER_ADDRESSES* a) {
        if (a->OperStatus != IfOperStatusUp) return;
        bool isUsb = IsTetherAdapter(a);
        for (auto* g = a->FirstGatewayAddress; g; g = g->Next) {
            char ip[INET_ADDRSTRLEN] = {};
            auto* sin = reinterpret_cast<sockaddr_in*>(g->Address.lpSockaddr);
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);
            (isUsb ? usb : other).push_back(ip);
        }
    });
    usb.insert(usb.end(), other.begin(), other.end());
    return usb;
}

inline SOCKET ConnectWithTimeout(const char* host, uint16_t port, int timeoutMs) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) return INVALID_SOCKET;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr);

    fd_set w, e;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(s, &w);
    FD_SET(s, &e);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (select(0, nullptr, &w, &e, &tv) != 1 || FD_ISSET(s, &e)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    nonBlocking = 0;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    DWORD recvTimeoutMs = 2000;  // stream runs at 30 fps; 2 s of silence = dead link
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recvTimeoutMs), sizeof recvTimeoutMs);
    return s;
}

inline bool RecvAll(SOCKET s, uint8_t* p, size_t n) {
    while (n) {
        int r = recv(s, reinterpret_cast<char*>(p), static_cast<int>(n), 0);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

// Wire protocol v2 (must match Protocol in the Android app):
//   [u32 BE length of type + payload][u8 type][payload]
const uint8_t kTypeVideo = 1;  // payload: one H.264 Annex-B access unit
const uint8_t kTypeInfo = 2;   // payload: u16 BE width, height, rotation (clockwise degrees to turn upright)

struct StreamInfo {
    uint16_t width = 0, height = 0, rotation = 0;
};

inline bool ReadPacket(SOCKET s, uint8_t& type, std::vector<uint8_t>& payload) {
    uint8_t h[5];
    if (!RecvAll(s, h, 5)) return false;
    uint32_t len = (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) | (uint32_t(h[2]) << 8) | h[3];
    if (len == 0 || len > kMaxPacket) return false;
    type = h[4];
    payload.resize(len - 1);
    return len == 1 || RecvAll(s, payload.data(), len - 1);
}

inline bool ParseInfo(const std::vector<uint8_t>& p, StreamInfo& info) {
    if (p.size() < 6) return false;
    info.width = static_cast<uint16_t>(p[0] << 8 | p[1]);
    info.height = static_cast<uint16_t>(p[2] << 8 | p[3]);
    info.rotation = static_cast<uint16_t>(p[4] << 8 | p[5]);
    return info.width >= 2 && info.height >= 2 && info.width % 2 == 0 && info.height % 2 == 0 &&
           info.rotation % 90 == 0 && info.rotation < 360;
}

// Bitmask of NAL unit types present (bit 5 = IDR, bit 7 = SPS).
inline uint32_t NalTypes(const std::vector<uint8_t>& b) {
    uint32_t mask = 0;
    for (size_t i = 0; i + 3 < b.size(); ++i) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            mask |= 1u << (b[i + 3] & 0x1F);
            i += 2;
        }
    }
    return mask;
}

// Tries each candidate; returns a connected socket whose first packet is a
// valid stream-info message (parsed into info), or INVALID_SOCKET. log receives
// one line per attempt.
template <class Log>
SOCKET FindPhone(const std::vector<std::string>& hosts, StreamInfo& info, Log&& log) {
    std::vector<uint8_t> payload;
    for (const auto& host : hosts) {
        SOCKET s = ConnectWithTimeout(host.c_str(), kPort, 1000);
        uint8_t type = 0;
        bool ok = s != INVALID_SOCKET && ReadPacket(s, type, payload) && type == kTypeInfo && ParseInfo(payload, info);
        log(host.c_str(), ok);
        if (ok) return s;
        if (s != INVALID_SOCKET) closesocket(s);
    }
    return INVALID_SOCKET;
}

}  // namespace owc
