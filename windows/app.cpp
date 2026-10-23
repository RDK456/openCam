// openCam tray app: finds the phone (USB through adb, Wi-Fi through its UDP beacon), decodes the
// H.264 stream, feeds the virtual camera through shared memory and mirrors every phone control.
// Protocol: PROTOCOL.md. Run with --install (elevated, done automatically once) to register the camera DLL.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "common.h"
#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <mfvirtualcamera.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <sddl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <wininet.h>
#include <bcrypt.h>
#include <msdelta.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

// ================================================================== tiny JSON

struct Json {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::map<std::string, Json> o;

    const Json& operator[](const std::string& k) const {
        static const Json nul;
        auto it = o.find(k);
        return it == o.end() ? nul : it->second;
    }
    double num(double d = 0) const { return t == Num ? n : d; }
    bool boolean(bool d = false) const { return t == Bool ? b : d; }
    std::string str(const std::string& d = "") const { return t == Str ? s : d; }
};

static void Ws(const char*& p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; }

static Json Parse(const char*& p) {
    Ws(p);
    Json j;
    if (*p == '{') {
        j.t = Json::Obj;
        p++;
        while (*p) {
            Ws(p);
            if (*p == '}') { p++; break; }
            Json k = Parse(p);
            Ws(p);
            if (*p == ':') p++;
            j.o[k.s] = Parse(p);
            Ws(p);
            if (*p == ',') p++;
        }
    } else if (*p == '[') {
        j.t = Json::Arr;
        p++;
        while (*p) {
            Ws(p);
            if (*p == ']') { p++; break; }
            j.a.push_back(Parse(p));
            Ws(p);
            if (*p == ',') p++;
        }
    } else if (*p == '"') {
        j.t = Json::Str;
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'n') j.s += '\n';
                else if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {
                    unsigned v = strtoul(std::string(p + 1, 4).c_str(), nullptr, 16);
                    p += 4;
                    if (v < 0x80) j.s += (char)v;
                    else if (v < 0x800) { j.s += (char)(0xC0 | (v >> 6)); j.s += (char)(0x80 | (v & 63)); }
                    else { j.s += (char)(0xE0 | (v >> 12)); j.s += (char)(0x80 | ((v >> 6) & 63)); j.s += (char)(0x80 | (v & 63)); }
                } else j.s += *p;
                p++;
            } else j.s += *p++;
        }
        if (*p == '"') p++;
    } else if (!strncmp(p, "true", 4)) { j.t = Json::Bool; j.b = true; p += 4; }
    else if (!strncmp(p, "false", 5)) { j.t = Json::Bool; p += 5; }
    else if (!strncmp(p, "null", 4)) { p += 4; }
    else if (*p == '-' || (*p >= '0' && *p <= '9')) { j.t = Json::Num; char* e; j.n = strtod(p, &e); p = e; }
    else if (*p) p++;
    return j;
}

static Json ParseStr(const std::string& s) { const char* p = s.c_str(); return Parse(p); }

static std::string JNum(double v) {
    char b[64];
    if (v == std::floor(v) && std::fabs(v) < 1e15) snprintf(b, sizeof b, "%lld", (long long)v);
    else snprintf(b, sizeof b, "%.4f", v);
    return b;
}
static std::string JStr(const std::string& s) { return "\"" + s + "\""; }
static std::string JBool(bool v) { return v ? "true" : "false"; }

static std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// ================================================================== shared state

static constexpr UINT WM_APP_TRAY = WM_APP + 1, WM_APP_UPDATE = WM_APP + 2;
static constexpr UINT32 ACCENT = 0xFFC93C, WHITE = 0xFFFFFF, RED = 0xEF4444, GREEN = 0x22C55E, DIMC = 0x9A9A9A;

static std::mutex g_m; // guards everything in this block
static Json g_caps, g_state;
static std::vector<BYTE> g_preview; // latest NV12 frame for the window, compact (stride == width)
static UINT g_pw, g_ph;
static ULONGLONG g_previewTick;

static std::atomic<bool> g_connected{false}, g_usb{false}, g_quit{false}, g_visible{false};
static std::atomic<UINT> g_flags{0};
static std::mutex g_sendM;
static SOCKET g_sock = INVALID_SOCKET;
static HWND g_hwnd;
static SharedFrame* g_sf;
static ComPtr<IMFVirtualCamera> g_vcam;
static std::wstring g_vcamError;

static void Invalidate() { if (g_hwnd) PostMessageW(g_hwnd, WM_APP_UPDATE, 0, 0); }

/** Appends one line to %TEMP%\openCam.log. */
static void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat_s(path, "openCam.log");
    FILE* f;
    if (fopen_s(&f, path, "a") == 0) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "%02d:%02d:%02d %s\n", t.wHour, t.wMinute, t.wSecond, line);
        fclose(f);
    }
}

static void SendMsg(BYTE type, const std::string& payload) {
    std::lock_guard<std::mutex> l(g_sendM);
    if (g_sock == INVALID_SOCKET) return;
    BYTE h[5];
    h[0] = type;
    u_long n = htonl((u_long)payload.size());
    memcpy(h + 1, &n, 4);
    send(g_sock, (const char*)h, 5, 0);
    send(g_sock, payload.data(), (int)payload.size(), 0);
}
static void SendJson(const std::string& s) { SendMsg(3, s); }

/** Change one phone setting. Applied locally at once so the UI never lags the click. */
static void Set(const std::string& k, const std::string& v) {
    {
        std::lock_guard<std::mutex> l(g_m);
        g_state.t = Json::Obj;
        g_state.o[k] = ParseStr(v);
    }
    SendJson("{\"t\":\"set\",\"k\":\"" + k + "\",\"v\":" + v + "}");
    Invalidate();
}

static void Toggle(const std::string& k, bool def = false) {
    bool cur;
    {
        std::lock_guard<std::mutex> l(g_m);
        cur = g_state[k].boolean(def);
    }
    Set(k, JBool(!cur));
}

// ================================================================== shared memory to the camera DLL

static void OpenShared() {
    if (g_sf) return;
    PSECURITY_DESCRIPTOR sd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)(A;;GA;;;SY)", SDDL_REVISION_1, &sd, nullptr);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(SharedFrame), OPENCAM_MAP_NAME);
    if (!map) map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, OPENCAM_MAP_NAME);
    if (sd) LocalFree(sd);
    if (map) g_sf = (SharedFrame*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedFrame));
}

/** Some Windows app is pulling frames from the virtual camera right now. */
static bool ConsumerActive() { return g_sf && (LONGLONG)GetTickCount64() - g_sf->consumerTick < 1500; }

static bool Demand() { return g_visible || ConsumerActive(); }

static void PublishFrame(const BYTE* p, UINT w, UINT h, UINT stride, UINT planeH) {
    h &= ~1u;
    if (!w || !h || w > MAX_W || h > MAX_H) return;
    if (g_sf) {
        InterlockedIncrement(&g_sf->seq);
        g_sf->width = w;
        g_sf->height = h;
        g_sf->flags = g_flags;
        for (UINT y = 0; y < h; y++) memcpy(g_sf->data + (size_t)y * w, p + (size_t)y * stride, w);
        const BYTE* uv = p + (size_t)stride * planeH;
        BYTE* duv = g_sf->data + (size_t)w * h;
        for (UINT y = 0; y < h / 2; y++) memcpy(duv + (size_t)y * w, uv + (size_t)y * stride, w);
        g_sf->frameTick = (LONGLONG)GetTickCount64();
        InterlockedIncrement(&g_sf->seq);
    }
    if (g_visible) {
        std::lock_guard<std::mutex> l(g_m);
        g_preview.resize((size_t)w * h * 3 / 2);
        for (UINT y = 0; y < h; y++) memcpy(&g_preview[(size_t)y * w], p + (size_t)y * stride, w);
        const BYTE* uv = p + (size_t)stride * planeH;
        for (UINT y = 0; y < h / 2; y++) memcpy(&g_preview[(size_t)w * h + (size_t)y * w], uv + (size_t)y * stride, w);
        g_pw = w;
        g_ph = h;
        g_previewTick = GetTickCount64();
        Invalidate();
    }
}

// ================================================================== H.264 decoder (Media Foundation)

struct Decoder {
    ComPtr<IMFTransform> t;
    UINT w = 0, h = 0, stride = 0, dispH = 0;

    bool Init() {
        if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t)))) return false;
        ComPtr<IMFAttributes> a;
        if (SUCCEEDED(t->GetAttributes(&a))) a->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
        ComPtr<IMFMediaType> in;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        if (FAILED(t->SetInputType(0, in.Get(), 0)) || !SetOut()) { t.Reset(); return false; }
        t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    bool SetOut() {
        for (DWORD i = 0;; i++) {
            ComPtr<IMFMediaType> mt;
            if (FAILED(t->GetOutputAvailableType(0, i, &mt))) return false;
            GUID sub;
            mt->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub != MFVideoFormat_NV12) continue;
            if (FAILED(t->SetOutputType(0, mt.Get(), 0))) return false;
            MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h);
            stride = MFGetAttributeUINT32(mt.Get(), MF_MT_DEFAULT_STRIDE, w);
            MFVideoArea area;
            dispH = SUCCEEDED(mt->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof area, nullptr)) ? area.Area.cy : h;
            return true;
        }
    }

    void Feed(const BYTE* data, size_t n, LONGLONG ptsUs) {
        if (!t && !Init()) return;
        ComPtr<IMFSample> s;
        ComPtr<IMFMediaBuffer> b;
        MFCreateSample(&s);
        MFCreateMemoryBuffer((DWORD)n, &b);
        BYTE* p;
        b->Lock(&p, nullptr, nullptr);
        memcpy(p, data, n);
        b->Unlock();
        b->SetCurrentLength((DWORD)n);
        s->AddBuffer(b.Get());
        s->SetSampleTime(ptsUs * 10);
        if (t->ProcessInput(0, s.Get(), 0) == MF_E_NOTACCEPTING) {
            Drain();
            t->ProcessInput(0, s.Get(), 0);
        }
        Drain();
    }

    void Drain() {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO si = {};
            t->GetOutputStreamInfo(0, &si);
            bool provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            ComPtr<IMFSample> os;
            if (!provides) {
                ComPtr<IMFMediaBuffer> ob;
                MFCreateSample(&os);
                MFCreateMemoryBuffer(si.cbSize, &ob);
                os->AddBuffer(ob.Get());
            }
            MFT_OUTPUT_DATA_BUFFER out = {};
            out.pSample = os.Get();
            DWORD status = 0;
            HRESULT hr = t->ProcessOutput(0, 1, &out, &status);
            if (out.pEvents) out.pEvents->Release();
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { if (!SetOut()) return; continue; }
            if (FAILED(hr)) return; // MF_E_TRANSFORM_NEED_MORE_INPUT or a decode error
            ComPtr<IMFMediaBuffer> cb;
            if (SUCCEEDED(out.pSample->ConvertToContiguousBuffer(&cb))) {
                BYTE* p;
                DWORD len;
                if (SUCCEEDED(cb->Lock(&p, nullptr, &len))) {
                    if (len >= (DWORD)stride * h * 3 / 2) PublishFrame(p, w, dispH, stride, h);
                    cb->Unlock();
                }
            }
            if (provides) out.pSample->Release();
        }
    }
};

// ================================================================== networking

/** A phone heard on Wi-Fi: its beacon, or its answer to Scan(). */
struct Phone { std::string ip, model; ULONGLONG tick; };
static std::mutex g_bm; // guards the block below
static std::vector<Phone> g_phones;
static std::string g_target;   // phone tried first: picked in the UI, typed as a code or IP, or the last one used
static int g_wantCode = -1;    // pairing code being looked up: the low 16 bits of the phone's IPv4 address
static std::wstring g_findMsg; // result of the last code / IP entry
static ULONGLONG g_scanTick;
static SOCKET g_udp = INVALID_SOCKET;
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12) // mstcpip.h
#endif

static std::wstring RegStr(const wchar_t* name) {
    wchar_t v[256];
    DWORD n = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\openCam", name, RRF_RT_REG_SZ, nullptr, v, &n) ? L"" : v;
}

static void SetRegStr(const wchar_t* name, const std::wstring& v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\openCam", name, REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

/** UDP 47101 (PROTOCOL.md): phone beacons and scan answers come in; phones searching for PCs get our name. */
static void BeaconThread() {
    static const char P[] = "OPENCAM1|";
    char buf[256];
    while (!g_quit) {
        sockaddr_in from = {};
        int fl = sizeof from;
        int n = recvfrom(g_udp, buf, sizeof buf - 1, 0, (sockaddr*)&from, &fl);
        if (n <= 0) { if (WSAGetLastError() != WSAETIMEDOUT) Sleep(50); continue; }
        buf[n] = 0;
        if (!strcmp(buf, "OPENCAMPC?")) {
            char host[MAX_COMPUTERNAME_LENGTH + 1];
            DWORD hn = sizeof host;
            GetComputerNameA(host, &hn);
            std::string r = std::string("OPENCAMPC|") + host;
            sendto(g_udp, r.data(), (int)r.size(), 0, (sockaddr*)&from, fl);
            continue;
        }
        if (strncmp(buf, P, sizeof P - 1)) continue;
        std::string model = buf + sizeof P - 1;
        model = model.substr(0, model.find('|'));
        char ip[64];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        {
            std::lock_guard<std::mutex> l(g_bm);
            auto it = std::find_if(g_phones.begin(), g_phones.end(), [&](const Phone& p) { return p.ip == ip; });
            if (it == g_phones.end()) g_phones.push_back({ip, model, GetTickCount64()});
            else { it->model = model; it->tick = GetTickCount64(); }
            if (g_wantCode >= 0 && (int)(ntohl(from.sin_addr.s_addr) & 0xFFFF) == g_wantCode) {
                g_target = ip;
                g_wantCode = -1;
                g_findMsg = L"Found " + Wide(model) + L", connecting…";
            }
        }
        Invalidate();
    }
}

/** Sends "OPENCAM?" to every address of each local IPv4 subnet up to /22, or with code >= 0 only to the address
 *  that pairing code names on each subnet. Phones answer from UDP 47101; Windows Firewall lets those answers in
 *  even without an inbound rule because they reply to our own packets. */
static void Scan(int code = -1) {
    ULONG sz = 64 << 10;
    std::vector<BYTE> b(sz);
    auto* a = (IP_ADAPTER_ADDRESSES*)b.data();
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, a, &sz)) return;
    if (code < 0) {
        std::lock_guard<std::mutex> l(g_bm);
        g_scanTick = GetTickCount64();
    }
    static const char Q[] = "OPENCAM?";
    auto send = [](u_long ip) {
        sockaddr_in to = {};
        to.sin_family = AF_INET;
        to.sin_port = htons(47101);
        to.sin_addr.s_addr = htonl(ip);
        sendto(g_udp, Q, sizeof Q - 1, 0, (sockaddr*)&to, sizeof to);
    };
    for (; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            UINT8 pl = u->OnLinkPrefixLength;
            u_long ip = ntohl(((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr);
            if (code >= 0) { send((ip & 0xFFFF0000) | (u_long)code); continue; }
            if (pl < 22 || pl > 30) continue;
            u_long mask = ~0ul << (32 - pl), net = ip & mask;
            for (u_long h = 1; h < ~mask; h++) if ((net | h) != ip) send(net | h);
        }
    }
}

/** Code or IP typed in the window. A code is the phone's address minus the network part this PC already knows. */
static void Find(const std::wstring& text) {
    std::string s(text.begin(), text.end());
    in_addr a;
    std::lock_guard<std::mutex> l(g_bm);
    if (s.find('.') != std::string::npos) {
        if (inet_pton(AF_INET, s.c_str(), &a) != 1) { g_findMsg = L"That isn't a valid IP address"; return; }
        g_target = s;
        g_findMsg = L"Connecting to " + text + L"…";
    } else if (!s.empty() && s.size() <= 5 && atoi(s.c_str()) <= 0xFFFF) {
        g_wantCode = atoi(s.c_str());
        g_findMsg = L"Looking for code " + text + L"…";
        std::thread([c = g_wantCode] { Scan(c); }).detach();
    } else g_findMsg = L"Enter the 5-digit code shown on the phone, or its IP address";
}

static std::wstring FindAdb() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring local = std::wstring(p).substr(0, std::wstring(p).rfind(L'\\')) + L"\\adb.exe";
    if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) return local;
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, p, nullptr)) return p;
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Android\\Sdk\\platform-tools\\adb.exe", p, MAX_PATH);
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return p;
    return L"";
}

/** Runs a console program hidden; its exit code, or 1 when it fails to start or finish. */
static DWORD Run(std::wstring cmd) {
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return 1;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 8000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

/** `adb forward tcp:47100 tcp:47100`. Succeeds only with an authorised phone on USB. */
static bool AdbForward() {
    static std::wstring adb = FindAdb();
    return !adb.empty() && Run(L"\"" + adb + L"\" forward tcp:47100 tcp:47100") == 0;
}

static SOCKET Connect(const char* ip, int timeoutMs) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(47100);
    inet_pton(AF_INET, ip, &a.sin_addr);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, (sockaddr*)&a, sizeof a);
    fd_set wr, ex;
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    FD_SET(s, &wr);
    FD_SET(s, &ex);
    timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (select(0, nullptr, &wr, &ex, &tv) != 1 || FD_ISSET(s, &ex)) { closesocket(s); return INVALID_SOCKET; }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof on);
    int rcv = 4 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rcv, sizeof rcv);
    return s;
}

static bool RecvAll(SOCKET s, BYTE* p, size_t n) {
    while (n) {
        int r = recv(s, (char*)p, (int)std::min<size_t>(n, 1 << 20), 0);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

static void HandleJson(const std::string& text) {
    Json j = ParseStr(text);
    std::string t = j["t"].str();
    if (t == "state") {
        std::lock_guard<std::mutex> l(g_m);
        g_state = j;
        g_flags = (j["mirror"].boolean() ? FLAG_MIRROR : 0) | (j["rot180"].boolean() ? FLAG_ROT180 : 0);
    } else if (t == "caps") {
        std::lock_guard<std::mutex> l(g_m);
        g_caps = j;
    } else return;
    Invalidate();
}

/** One phone link. Returns when it drops. */
static void RunSession(SOCKET s, bool usb) {
    DWORD to = 5000; // the phone pings every second
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
    std::vector<BYTE> buf, csd, joined;
    BYTE hdr[5];
    // adb accepts the connection even when the app isn't running; wait for the phone to speak first.
    if (!RecvAll(s, hdr, 5)) return;
    {
        std::lock_guard<std::mutex> l(g_sendM);
        g_sock = s;
    }
    g_connected = true;
    g_usb = usb;
    Log("connected via %s", usb ? "USB" : "Wi-Fi");
    Invalidate();

    // On Wi-Fi, look for the phone on USB every 3 s. Dropping this link makes NetThread reconnect over USB.
    std::atomic<bool> done{false};
    std::thread usbWatch;
    if (!usb) usbWatch = std::thread([&] {
        while (!done) {
            for (int i = 0; i < 30 && !done; i++) Sleep(100);
            if (!done && AdbForward()) { Log("switched to USB"); shutdown(s, SD_BOTH); return; }
        }
    });

    Decoder dec;
    bool demand = false, waitKey = true, first = true;
    ULONGLONG lastPing = 0;
    for (;;) {
        if (!first && !RecvAll(s, hdr, 5)) break;
        first = false;
        UINT len = ntohl(*(u_long*)(hdr + 1));
        if (len > (32u << 20)) break;
        buf.resize(len);
        if (len && !RecvAll(s, buf.data(), len)) break;

        if (hdr[0] == 1) {
            csd = buf;
            waitKey = true;
        } else if (hdr[0] == 2 && len > 9 && demand) {
            bool key = buf[0] != 0;
            if (key) waitKey = false;
            if (!waitKey) {
                LONGLONG pts = 0;
                for (int i = 1; i <= 8; i++) pts = (pts << 8) | buf[i];
                if (key && !csd.empty()) {
                    joined.assign(csd.begin(), csd.end());
                    joined.insert(joined.end(), buf.begin() + 9, buf.end());
                    dec.Feed(joined.data(), joined.size(), pts);
                } else dec.Feed(buf.data() + 9, len - 9, pts);
            }
        } else if (hdr[0] == 3) {
            HandleJson(std::string(buf.begin(), buf.end()));
        }

        ULONGLONG now = GetTickCount64();
        bool want = Demand();
        if (want != demand) {
            demand = want;
            waitKey = true;
            SendJson(std::string("{\"t\":\"demand\",\"v\":") + JBool(want) + "}");
            if (want) SendJson("{\"t\":\"key\"}");
        }
        if (now - lastPing > 2000) {
            lastPing = now;
            SendJson("{\"t\":\"ping\"}");
            OpenShared();
        }
    }
    done = true;
    if (usbWatch.joinable()) usbWatch.join();
    {
        std::lock_guard<std::mutex> l(g_sendM);
        g_sock = INVALID_SOCKET;
    }
    g_connected = false;
    Log("disconnected");
    Invalidate();
}

static void NetThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    g_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL on = TRUE, off = FALSE;
    DWORD to = 1000, got;
    setsockopt(g_udp, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
    setsockopt(g_udp, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
    // Scanning hits addresses with nothing on 47101; don't let their ICMP replies fail later receives.
    WSAIoctl(g_udp, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &got, nullptr, nullptr);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(47101);
    bind(g_udp, (sockaddr*)&a, sizeof a);
    std::thread(BeaconThread).detach();
    std::wstring last = RegStr(L"LastPhone");
    g_target = std::string(last.begin(), last.end());
    while (!g_quit) {
        OpenShared();
        // USB first: lower latency and no Wi-Fi needed. RunSession drops a Wi-Fi link once USB appears.
        if (AdbForward()) {
            SOCKET s = Connect("127.0.0.1", 1000);
            if (s != INVALID_SOCKET) { RunSession(s, true); closesocket(s); continue; }
        }
        // Wi-Fi: the chosen / last phone first (works even where UDP is blocked), then any phone heard lately.
        std::vector<std::string> ips;
        bool scan;
        {
            std::lock_guard<std::mutex> l(g_bm);
            ULONGLONG now = GetTickCount64();
            if (!g_target.empty()) ips.push_back(g_target);
            for (auto& p : g_phones) if (now - p.tick < 6000 && p.ip != g_target) ips.push_back(p.ip);
            scan = now - g_scanTick > 5000;
        }
        if (scan) Scan();
        bool ran = false;
        for (auto& ip : ips) {
            SOCKET s = Connect(ip.c_str(), 1500);
            if (s == INVALID_SOCKET) continue;
            {
                std::lock_guard<std::mutex> l(g_bm);
                g_target = ip;
                g_findMsg.clear();
            }
            SetRegStr(L"LastPhone", Wide(ip));
            RunSession(s, false);
            closesocket(s);
            ran = true;
            break;
        }
        if (!ran) Sleep(1000);
    }
}

// ================================================================== files and deltas

static std::string ReadAll(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

static bool WriteAll(const std::wstring& path, const std::string& d) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(d.data(), d.size());
    return f.good();
}

static std::string Sha256(const std::string& d) {
    BYTE h[32];
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)d.data(), (ULONG)d.size(), h, 32);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf_s(hex + 2 * i, 3, "%02x", h[i]);
    return hex;
}

/** Windows' own binary delta (msdelta), used for the exe and the DLL. Empty on failure. */
static std::string MsDelta(const std::string& src, const std::string& dst, bool create) {
    DELTA_INPUT s = {}, t = {}, none = {};
    s.lpcStart = src.data();
    s.uSize = src.size();
    t.lpcStart = dst.data();
    t.uSize = dst.size();
    DELTA_OUTPUT o = {};
    BOOL ok = create ? CreateDeltaB(DELTA_FILE_TYPE_SET_EXECUTABLES, DELTA_FLAG_NONE, DELTA_FLAG_NONE, s, t, none, none, none,
                                    nullptr, 0, &o)
                     : ApplyDeltaB(DELTA_FLAG_NONE, s, t, &o); // t is the delta here
    if (!ok) return {};
    std::string r((const char*)o.lpStart, o.uSize);
    DeltaFree(o.lpStart);
    return r;
}

/** APK delta, applied by Updater.kt: "OCD1", then big-endian ops until the new file is complete:
 *  'C' u32 offset, u32 length = copy from the installed APK; 'A' u32 length, bytes = literal.
 *  Unchanged zip entries keep their compressed bytes, so a rolling-hash block match finds them. */
static std::string BlockDelta(const std::string& a, const std::string& b) {
    const size_t B = 64;
    auto sums = [&](const std::string& s, size_t i, uint32_t& x, uint32_t& y) {
        x = y = 0;
        for (size_t k = 0; k < B; k++) { x += (BYTE)s[i + k]; y += x; }
    };
    auto key = [](uint32_t x, uint32_t y) { return (x & 0xFFFF) | y << 16; };
    std::unordered_map<uint32_t, size_t> idx;
    uint32_t x = 0, y = 0;
    for (size_t i = 0; i + B <= a.size(); i += B) { sums(a, i, x, y); idx.emplace(key(x, y), i); }

    std::string out = "OCD1";
    auto u32 = [&](size_t v) { for (int s = 24; s >= 0; s -= 8) out += (char)(v >> s); };
    size_t lit = 0, i = 0;
    auto flush = [&](size_t end) {
        if (end > lit) { out += 'A'; u32(end - lit); out.append(b, lit, end - lit); }
    };
    if (b.size() >= B) sums(b, 0, x, y);
    while (i + B <= b.size()) {
        auto it = idx.find(key(x, y));
        if (it != idx.end() && !memcmp(&a[it->second], &b[i], B)) {
            size_t j = it->second, n = B;
            while (j + n < a.size() && i + n < b.size() && a[j + n] == b[i + n]) n++;
            while (i > lit && j > 0 && a[j - 1] == b[i - 1]) { i--; j--; n++; }
            flush(i);
            out += 'C';
            u32(j);
            u32(n);
            i += n;
            lit = i;
            if (i + B <= b.size()) sums(b, i, x, y);
            continue;
        }
        if (i + B < b.size()) { // roll the window one byte
            BYTE o = b[i], nb = b[i + B];
            x += nb - o;
            y += x - (uint32_t)(B * o);
        }
        i++;
    }
    flush(b.size());
    return out;
}

/** Inverse of BlockDelta; only --selftest uses it (the phone has its own copy). */
static std::string ApplyBlockDelta(const std::string& a, const std::string& d) {
    std::string r;
    auto u32 = [&](size_t& p) { size_t v = 0; for (int k = 0; k < 4; k++) v = v << 8 | (BYTE)d[p++]; return v; };
    for (size_t p = 4; p < d.size();) {
        char op = d[p++];
        size_t v = u32(p);
        if (op == 'C') r.append(a, v, u32(p));
        else { r.append(d, p, v); p += v; }
    }
    return r;
}

/** `openCam.exe --mkdelta <old> <new> <out>`, used by release.ps1. */
static int MakeDelta() {
    int n;
    LPWSTR* av = CommandLineToArgvW(GetCommandLineW(), &n);
    if (n < 5) return 2;
    std::string a = ReadAll(av[2]), b = ReadAll(av[3]);
    std::string d = wcsstr(av[3], L".apk") ? BlockDelta(a, b) : MsDelta(a, b, true);
    return !a.empty() && !d.empty() && WriteAll(av[4], d) ? 0 : 1;
}

// ================================================================== install / autostart / virtual camera

static std::wstring ExePath() { wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH); return p; }
static std::wstring ExeDir() { std::wstring p = ExePath(); return p.substr(0, p.rfind(L'\\')); }
static std::wstring InstallDir() { wchar_t p[MAX_PATH]; ExpandEnvironmentStringsW(L"%ProgramFiles%\\openCam", p, MAX_PATH); return p; }
static const wchar_t* CLSID_KEY = L"Software\\Classes\\CLSID\\" OPENCAM_CLSID_STR L"\\InprocServer32";

static int CallDll(const std::wstring& dll, const char* fn) {
    HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) return 1;
    auto f = (HRESULT(STDAPICALLTYPE*)())GetProcAddress(m, fn);
    HRESULT hr = f ? f() : E_FAIL;
    FreeLibrary(m);
    return SUCCEEDED(hr) ? 0 : 2;
}

static std::wstring Netsh(const std::wstring& args) {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\netsh.exe", p, MAX_PATH);
    return L"\"" + std::wstring(p) + L"\" advfirewall firewall " + args;
}

/** Elevated: copy the DLL where Frame Server (LocalService) can read it and register its CLSID, and let phone
 *  beacons (UDP 47101 only) through Windows Firewall on any network profile. */
static int Install() {
    std::wstring dst = InstallDir() + L"\\opencam_vcam.dll";
    CreateDirectoryW(InstallDir().c_str(), nullptr);
    CopyFileW((ExeDir() + L"\\opencam_vcam.dll").c_str(), dst.c_str(), FALSE); // may fail if in use; keep the old one
    Run(Netsh(L"delete rule name=openCam"));
    Run(Netsh(L"add rule name=openCam dir=in action=allow protocol=UDP localport=47101 profile=any program=\"" + ExePath() + L"\""));
    return CallDll(dst, "DllRegisterServer");
}

static int Uninstall() {
    Run(Netsh(L"delete rule name=openCam"));
    std::wstring dst = InstallDir() + L"\\opencam_vcam.dll";
    int r = CallDll(dst, "DllUnregisterServer");
    DeleteFileW(dst.c_str());
    RemoveDirectoryW(InstallDir().c_str());
    return r;
}

static bool NeedsInstall() {
    wchar_t v[MAX_PATH];
    DWORD n = sizeof v;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, CLSID_KEY, nullptr, RRF_RT_REG_SZ, nullptr, v, &n)) return true;
    if (_wcsicmp(v, (InstallDir() + L"\\opencam_vcam.dll").c_str())) return true;
    std::string local = ReadAll(ExeDir() + L"\\opencam_vcam.dll");
    if (!local.empty() && local != ReadAll(v)) return true; // a different build sits next to the exe
    return Run(Netsh(L"show rule name=openCam")) != 0;
}

static void EnsureInstalled() {
    if (!NeedsInstall()) return;
    std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei = {sizeof sei};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = L"--install";
    sei.nShow = SW_HIDE;
    if (ShellExecuteExW(&sei) && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 30000);
        CloseHandle(sei.hProcess);
    }
}

static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static bool AutostartOn() {
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, L"openCam", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

static void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k)) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\" --tray";
        RegSetValueExW(k, L"openCam", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, L"openCam");
    RegCloseKey(k);
}

/** Start with Windows by default; respect the user's choice after the first run. */
static void FirstRunAutostart() {
    HKEY k;
    DWORD init = 0, n = sizeof init;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\openCam", L"Init", RRF_RT_REG_DWORD, nullptr, &init, &n) == ERROR_SUCCESS) return;
    SetAutostart(true);
    if (!RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\openCam", 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr)) {
        init = 1;
        RegSetValueExW(k, L"Init", 0, REG_DWORD, (const BYTE*)&init, sizeof init);
        RegCloseKey(k);
    }
}

static void StartVirtualCamera() {
    HRESULT hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
                                       MFVirtualCameraAccess_CurrentUser, L"openCam", OPENCAM_CLSID_STR, nullptr, 0, &g_vcam);
    if (SUCCEEDED(hr)) hr = g_vcam->Start(nullptr);
    Log("virtual camera: 0x%08X", (unsigned)hr);
    if (FAILED(hr)) {
        wchar_t b[96];
        swprintf_s(b, L"Virtual camera failed (0x%08X)", (unsigned)hr);
        g_vcamError = b;
        g_vcam.Reset();
    }
}

// ================================================================== updater

static const wchar_t* RELEASES = L"https://github.com/RDK456/openCam/releases/latest/download/";
static const char* UPDATE_FILES[] = {"openCam.exe", "opencam_vcam.dll"};
static std::string g_update;     // guarded by g_m: staged version waiting for the camera to go idle
static std::wstring g_updStatus; // guarded by g_m: shown in the settings drawer
static std::atomic<bool> g_restart{false}, g_updNow{false};
static HANDLE g_updWake = CreateEventW(nullptr, FALSE, FALSE, nullptr); // "Check now" / "Restart now"

static void UpdStatus(const std::wstring& s) {
    {
        std::lock_guard<std::mutex> l(g_m);
        g_updStatus = s;
    }
    Invalidate();
}

/** HTTPS GET; WinINet follows GitHub's redirect to its download host. */
static bool Fetch(const std::wstring& url, std::string& out) {
    out.clear();
    HINTERNET net = InternetOpenW(L"openCam", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    HINTERNET h = net ? InternetOpenUrlW(net, url.c_str(), nullptr, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0) : nullptr;
    DWORD code = 0, n = sizeof code;
    bool ok = h && HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &n, nullptr) && code == 200;
    char buf[65536];
    DWORD got;
    while (ok && InternetReadFile(h, buf, sizeof buf, &got) && got) out.append(buf, got);
    if (h) InternetCloseHandle(h);
    if (net) InternetCloseHandle(net);
    return ok;
}

static int Ver(const std::string& v) {
    int a = 0, b = 0, c = 0;
    sscanf_s(v.c_str(), "%d.%d.%d", &a, &b, &c);
    return a * 1000000 + b * 1000 + c;
}

/** A minute after start, then every 6 h or on "Check now": when GitHub has a newer release, fetch it (a delta when
 *  one matches this version, else the full file; both checked against the manifest's SHA-256), stage it as *.new,
 *  wait until nothing uses the camera and the window is closed (or "Restart now"), then swap the files and restart. */
static void UpdateThread() {
    for (WaitForSingleObject(g_updWake, 60 * 1000);; WaitForSingleObject(g_updWake, 6 * 3600 * 1000)) {
        UpdStatus(L"Checking for updates…");
        std::string m;
        if (!Fetch(std::wstring(RELEASES) + L"manifest.json", m)) { UpdStatus(L"Couldn't reach GitHub"); continue; }
        Json j = ParseStr(m);
        std::string ver = j["version"].str();
        if (Ver(ver) <= Ver(OPENCAM_VERSION)) { UpdStatus(L"Up to date"); continue; }
        UpdStatus(L"Downloading " + Wide(ver) + L"…");
        bool ok = true;
        for (const char* name : UPDATE_FILES) {
            std::wstring path = ExeDir() + L"\\" + Wide(name);
            std::string want = j[name].str(), cur = ReadAll(path), next, d;
            if (Sha256(cur) == want) continue;
            if (Fetch(RELEASES + Wide(name) + L".from-" + Wide(OPENCAM_VERSION) + L".delta", d)) next = MsDelta(cur, d, false);
            if (Sha256(next) != want && !(Fetch(RELEASES + Wide(name), next) && Sha256(next) == want)) ok = false;
            if (!ok || !WriteAll(path + L".new", next)) { ok = false; break; }
        }
        Log(ok ? "update %s staged" : "update %s failed", ver.c_str());
        if (!ok) { UpdStatus(L"Download of " + Wide(ver) + L" failed"); continue; }
        {
            std::lock_guard<std::mutex> l(g_m);
            g_update = ver;
        }
        UpdStatus(Wide(ver) + L" is ready");
        // never restart under a running call unless asked to
        while (!g_updNow && (g_visible || ConsumerActive())) WaitForSingleObject(g_updWake, 5000);
        for (const char* name : UPDATE_FILES) {
            std::wstring path = ExeDir() + L"\\" + Wide(name);
            if (GetFileAttributesW((path + L".new").c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING); // a running exe can be renamed
            MoveFileExW((path + L".new").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        PostMessageW(g_hwnd, WM_COMMAND, 5, 0);
        return;
    }
}

// ================================================================== UI (Direct2D)

static ComPtr<ID2D1Factory> g_d2d;
static ComPtr<IDWriteFactory> g_dw;
static ComPtr<ID2D1HwndRenderTarget> g_rt;
static ComPtr<ID2D1SolidColorBrush> g_br;
static ComPtr<ID2D1Bitmap> g_bmp;
static UINT g_bmpW, g_bmpH;
static std::vector<DWORD> g_bgra;
static std::map<int, ComPtr<IDWriteTextFormat>> g_fonts;
static NOTIFYICONDATAW g_nid;
static int g_iconState = -1;

struct Btn { D2D1_RECT_F r; std::function<void()> f; };
static std::vector<Btn> g_btns;
static bool g_drawer = false;
static D2D1_RECT_F g_drawerRect;
static size_t g_drawerFirst;

/** The setting the slider currently edits (same scales as the phone UI). */
struct Spec { std::string key; double min = 0, max = 1; bool log = false; int kind = 0; bool on = false; };
static Spec g_spec;
static D2D1_RECT_F g_track;
static bool g_dragging = false;

static float Scale() { return g_hwnd ? GetDpiForWindow(g_hwnd) / 96.f : 1.f; }

static constexpr UINT32 SURFACE = 0x1C1C1E, MUTED = 0xA1A1AA;
// Segoe Fluent Icons (ships with Windows 11) code points
#define IC_TORCH L""
#define IC_ROTATE L""
#define IC_MIRROR L""
#define IC_SETTINGS L""
#define IC_FLIP L""
#define IC_CLOSE L""
#define IC_PHONE L""
#define IC_REFRESH L""

static D2D1_POINT_2F g_mouse = {-1, -1};
static bool g_editing = false; // the pairing-code field has focus
static std::wstring g_edit;

static bool In(D2D1_RECT_F r, D2D1_POINT_2F p) { return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom; }

static bool IsIcon(const std::wstring& s) { return s.size() == 1 && s[0] >= 0xE000 && s[0] <= 0xF8FF; }

static IDWriteTextFormat* Font(float size, bool bold, bool icon = false) {
    int key = ((int)(size * 10) * 2 + (bold ? 1 : 0)) * 2 + (icon ? 1 : 0);
    auto& f = g_fonts[key];
    if (!f) {
        g_dw->CreateTextFormat(icon ? L"Segoe Fluent Icons" : L"Segoe UI Variable Text", nullptr,
                               bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return f.Get();
}

static void Text(const std::wstring& s, D2D1_RECT_F r, float size, UINT32 rgb, float alpha = 1,
                 DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_CENTER, bool bold = true, bool wrap = false) {
    IDWriteTextFormat* f = Font(size, bold, IsIcon(s));
    f->SetTextAlignment(al);
    if (wrap) f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    g_br->SetColor(D2D1::ColorF(rgb, alpha));
    g_rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, g_br.Get());
    if (wrap) f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
}

static float TextW(const std::wstring& s, float size, bool bold = true) {
    ComPtr<IDWriteTextLayout> l;
    DWRITE_TEXT_METRICS m = {};
    if (SUCCEEDED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), Font(size, bold, IsIcon(s)), 4096, 100, &l))) l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

static void Fill(D2D1_RECT_F r, UINT32 rgb, float a, float radius) {
    g_br->SetColor(D2D1::ColorF(rgb, a));
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), g_br.Get());
}

static void Stroke(D2D1_RECT_F r, UINT32 rgb, float a, float radius) {
    g_br->SetColor(D2D1::ColorF(rgb, a));
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), g_br.Get(), 1);
}

/** Pill toggle: highlighted when on, a faint wash under the mouse, dimmed and inert when unsupported. */
static void Chip(D2D1_RECT_F r, const std::wstring& label, bool on, bool supported, std::function<void()> f, float size = 13,
                 UINT32 rgb = WHITE) {
    float rad = (r.bottom - r.top) / 2;
    if (on) Fill(r, WHITE, 0.18f, rad);
    else if (supported && In(r, g_mouse)) Fill(r, WHITE, 0.08f, rad);
    Text(label, r, size, on ? ACCENT : rgb, supported ? 1.f : 0.3f);
    if (supported) g_btns.push_back({r, f});
}

/** Rectangular action button; primary is the accent fill. */
static void Button(D2D1_RECT_F r, const std::wstring& label, bool primary, std::function<void()> f) {
    bool hov = In(r, g_mouse);
    if (primary) Fill(r, ACCENT, hov ? 0.85f : 1, 8);
    else Fill(r, WHITE, hov ? 0.16f : 0.1f, 8);
    Text(label, r, 13, primary ? 0x000000 : WHITE);
    g_btns.push_back({r, f});
}

static std::wstring FmtZoom(double z) {
    wchar_t b[16];
    if (std::fabs(z - std::round(z)) < 0.05) swprintf_s(b, L"%d", (int)std::round(z));
    else swprintf_s(b, L"%.1f", z);
    return b;
}

static std::wstring Shutter(double ns) {
    wchar_t b[24];
    if (ns <= 0) return L"S –";
    if (ns >= 5e8) swprintf_s(b, L"%.1fs", ns / 1e9);
    else swprintf_s(b, L"1/%d", (int)std::round(1e9 / ns));
    return b;
}

static std::wstring Focus(double d) {
    if (d < 0.05) return L"∞";
    wchar_t b[24];
    swprintf_s(b, L"%d cm", (int)std::round(100 / d));
    return b;
}

static std::wstring SpecLabel(const Spec& s, double v, const Json& caps) {
    wchar_t b[32];
    if (s.key == "zoom") swprintf_s(b, L"%.1fx", v);
    else if (s.key == "ev") swprintf_s(b, L"%+.1f EV", v * caps["evStep"].num(1));
    else if (s.key == "iso") swprintf_s(b, L"ISO %d", (int)std::round(v));
    else if (s.key == "exp") return Shutter(v);
    else if (s.key == "focus") return Focus(v);
    else swprintf_s(b, L"%.2f", v);
    return b;
}

static double SpecFrom(const Spec& s, double t) {
    if (s.log && s.min > 0) return s.min * std::pow(s.max / s.min, t);
    return s.min + (s.max - s.min) * t;
}

static double SpecTo(const Spec& s, double v) {
    if (s.max <= s.min) return 0;
    v = std::clamp(v, s.min, s.max);
    if (s.log && s.min > 0) return std::log(v / s.min) / std::log(s.max / s.min);
    return (v - s.min) / (s.max - s.min);
}

static void SpecSet(const Spec& s, double t) {
    double v = SpecFrom(s, std::clamp(t, 0.0, 1.0));
    v = s.kind ? std::round(v) : std::round(v * 100) / 100;
    Set(s.key, JNum(v));
}

static void OpenSpec(const std::string& key) {
    Json c;
    double fps;
    {
        std::lock_guard<std::mutex> l(g_m);
        c = g_caps;
        fps = g_state["fps"].num(30);
    }
    Spec s;
    s.key = key;
    s.on = true;
    if (key == "zoom") { s.min = c["zoomMin"].num(1); s.max = c["zoomMax"].num(1); s.log = true; }
    else if (key == "ev") { s.min = c["evMin"].num(); s.max = c["evMax"].num(); s.kind = 1; }
    else if (key == "iso") { s.min = c["isoMin"].num(100); s.max = c["isoMax"].num(3200); s.log = true; s.kind = 1; }
    else if (key == "exp") { s.min = c["expMin"].num(1e5); s.max = std::min(c["expMax"].num(1e8), 1e9 / fps); s.log = true; s.kind = 2; }
    else if (key == "focus") { s.min = 0; s.max = c["focusMax"].num(10); }
    g_spec = s;
    Invalidate();
}

static void Cycle(const std::string& key, const std::vector<std::string>& vals, const std::string& cur) {
    if (vals.empty()) return;
    size_t i = std::find(vals.begin(), vals.end(), cur) - vals.begin();
    Set(key, vals[(i + 1) % vals.size()]);
}

static void NV12toBGRA(const BYTE* src, UINT sw, UINT sh, UINT flags, DWORD* dst, UINT dw, UINT dh) {
    bool fy = (flags & FLAG_ROT180) != 0;
    bool fx = ((flags & FLAG_MIRROR) != 0) != fy;
    const BYTE* uvBase = src + (size_t)sw * sh;
    for (UINT y = 0; y < dh; y++) {
        UINT sy = (UINT)((UINT64)y * sh / dh);
        if (fy) sy = sh - 1 - sy;
        const BYTE* Y = src + (size_t)sy * sw;
        const BYTE* UV = uvBase + (size_t)(sy / 2) * sw;
        DWORD* d = dst + (size_t)y * dw;
        for (UINT x = 0; x < dw; x++) {
            UINT sx = (UINT)((UINT64)x * sw / dw);
            if (fx) sx = sw - 1 - sx;
            int c = Y[sx] - 16, u = UV[sx & ~1u] - 128, v = UV[(sx & ~1u) + 1] - 128;
            int r = (298 * c + 409 * v + 128) >> 8, g = (298 * c - 100 * u - 208 * v + 128) >> 8, b = (298 * c + 516 * u + 128) >> 8;
            r = std::clamp(r, 0, 255); g = std::clamp(g, 0, 255); b = std::clamp(b, 0, 255);
            d[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

static void DrawPreview(D2D1_RECT_F vf, bool& drew) {
    std::lock_guard<std::mutex> l(g_m);
    drew = false;
    if (g_preview.empty() || GetTickCount64() - g_previewTick > 2000) return;
    float vw = vf.right - vf.left, vh = vf.bottom - vf.top;
    float aspect = (float)g_pw / g_ph;
    float w = vw, h = vw / aspect;
    if (h > vh) { h = vh; w = vh * aspect; }
    D2D1_RECT_F r = {vf.left + (vw - w) / 2, vf.top + (vh - h) / 2, vf.left + (vw + w) / 2, vf.top + (vh + h) / 2};
    UINT bw = std::min<UINT>((UINT)(w * Scale()), std::min<UINT>(g_pw, 1600)), bh = std::max<UINT>(1, (UINT)((float)bw / aspect));
    if (!g_bmp || bw != g_bmpW || bh != g_bmpH) {
        g_bmp.Reset();
        g_rt->CreateBitmap(D2D1::SizeU(bw, bh), D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)), &g_bmp);
        g_bmpW = bw;
        g_bmpH = bh;
    }
    if (!g_bmp) return;
    g_bgra.resize((size_t)bw * bh);
    NV12toBGRA(g_preview.data(), g_pw, g_ph, g_flags, g_bgra.data(), bw, bh);
    g_bmp->CopyFromMemory(nullptr, g_bgra.data(), bw * 4);
    g_rt->DrawBitmap(g_bmp.Get(), r);
    drew = true;
}

/** Shown while no phone is connected: phones found on the network, and pairing code / IP entry. */
static void ConnectPanel(D2D1_RECT_F vf) {
    std::vector<Phone> phones;
    std::wstring msg;
    bool scanning;
    {
        std::lock_guard<std::mutex> l(g_bm);
        ULONGLONG now = GetTickCount64();
        for (auto& p : g_phones) if (now - p.tick < 10000) phones.push_back(p);
        msg = g_findMsg;
        scanning = now - g_scanTick < 1500;
    }
    float w = std::min(460.f, vf.right - vf.left - 48), x = (vf.left + vf.right - w) / 2, r = x + w;
    float h = 300 + std::max<size_t>(1, phones.size()) * 64.f;
    float y = std::max(vf.top + 16, (vf.top + vf.bottom - h) / 2);
    auto header = [&](const wchar_t* t) { Text(t, {x + 2, y, r, y + 20}, 11, MUTED, 1, DWRITE_TEXT_ALIGNMENT_LEADING); };

    Text(IC_PHONE, {x, y, r, y + 40}, 30, WHITE);
    y += 44;
    Text(L"Connect your phone", {x, y, r, y + 30}, 22, WHITE);
    y += 32;
    Text(L"Open openCam on the phone. On the same Wi-Fi it connects by itself; for USB, turn on USB debugging.",
         {x + 16, y, r - 16, y + 40}, 13, MUTED, 1, DWRITE_TEXT_ALIGNMENT_CENTER, false, true);
    y += 60;

    header(L"PHONES ON THIS NETWORK");
    if (scanning) Text(L"Scanning…", {x, y, r - 4, y + 20}, 12, MUTED, 1, DWRITE_TEXT_ALIGNMENT_TRAILING, false);
    else Chip({r - 104, y - 6, r, y + 26}, L"Scan again", false, true, [] { std::thread([] { Scan(); }).detach(); }, 12);
    y += 30;
    if (phones.empty()) {
        Fill({x, y, r, y + 56}, SURFACE, 1, 12);
        Text(L"None yet. Keep openCam open on the phone.", {x, y, r, y + 56}, 13, MUTED, 1, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        y += 64;
    }
    for (auto& p : phones) {
        D2D1_RECT_F row = {x, y, r, y + 56};
        Fill(row, In(row, g_mouse) ? 0x2C2C2E : SURFACE, 1, 12);
        Text(IC_PHONE, {x + 12, y, x + 44, y + 56}, 18, WHITE);
        Text(Wide(p.model), {x + 52, y + 8, r - 100, y + 30}, 14, WHITE, 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        Text(Wide(p.ip), {x + 52, y + 28, r - 100, y + 48}, 12, MUTED, 1, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        Text(L"Connect", {r - 100, y, r - 16, y + 56}, 13, ACCENT, 1, DWRITE_TEXT_ALIGNMENT_TRAILING);
        g_btns.push_back({row, [p] {
            std::lock_guard<std::mutex> l(g_bm);
            g_target = p.ip;
            g_findMsg = L"Connecting to " + Wide(p.model) + L"…";
        }});
        y += 64;
    }
    y += 12;

    header(L"OR ENTER THE PAIRING CODE");
    y += 26;
    D2D1_RECT_F field = {x, y, r - 108, y + 40};
    Fill(field, SURFACE, 1, 8);
    Stroke(field, g_editing ? ACCENT : WHITE, g_editing ? 1 : 0.14f, 8);
    D2D1_RECT_F tr = {field.left + 14, field.top, field.right - 14, field.bottom};
    if (g_edit.empty() && !g_editing) Text(L"5-digit code or IP address", tr, 14, MUTED, 1, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    else {
        Text(g_edit, tr, 15, WHITE, 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        if (g_editing && GetTickCount64() / 530 % 2 == 0) {
            float cx = tr.left + TextW(g_edit, 15) + 1;
            Fill({cx, field.top + 10, cx + 1.5f, field.bottom - 10}, ACCENT, 1, 0);
        }
    }
    g_btns.push_back({field, [] { g_editing = true; }});
    Button({r - 96, y, r, y + 40}, L"Connect", true, [] { g_editing = false; Find(g_edit); });
    y += 48;
    Text(msg.empty() ? L"The phone shows its code under “Connect to PC”." : msg, {x + 2, y, r, y + 20}, 12, MUTED, 1,
         DWRITE_TEXT_ALIGNMENT_LEADING, false);
}

static void Paint() {
    if (!g_rt) {
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        float dpi = 96 * Scale();
        g_d2d->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi, dpi),
                                      D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt);
        if (!g_rt) return;
        g_rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &g_br);
        g_bmp.Reset();
    }
    Json st, caps;
    {
        std::lock_guard<std::mutex> l(g_m);
        st = g_state;
        caps = g_caps;
    }
    bool conn = g_connected;
    D2D1_SIZE_F sz = g_rt->GetSize();
    float W = sz.width, H = sz.height;
    g_btns.clear();
    g_track = {};

    g_rt->BeginDraw();
    g_rt->Clear(D2D1::ColorF(0x000000));

    // ---- top bar: quick toggles, status, settings
    float x = 10;
    bool af = st["af"].boolean(true);
    if (conn) {
        auto top = [&](const std::wstring& label, bool on, bool sup, std::function<void()> f) {
            Chip({x, 8, x + 48, 40}, label, on, sup, f, IsIcon(label) ? 16.f : 13.f);
            x += 52;
        };
        top(IC_TORCH, st["torch"].boolean(), caps["torch"].boolean(), [] { Toggle("torch"); });
        top(af ? L"AF" : L"MF", !af, caps["af"].boolean(), [] { Toggle("af", true); });
        top(L"EIS", st["stab"].boolean(), caps["stab"].boolean(), [] { Toggle("stab"); });
        top(IC_ROTATE, st["rot180"].boolean(), true, [] { Toggle("rot180"); });
        top(IC_MIRROR, st["mirror"].boolean(), true, [] { Toggle("mirror"); });
    }

    std::string res = st["res"].str("1920x1080");
    std::wstring hp = Wide(res.substr(res.find('x') + 1));
    std::wstring status = conn ? Wide(st["model"].str("Phone")) + (g_usb ? L"  ·  USB  ·  " : L"  ·  Wi-Fi  ·  ") + hp + L"p" +
                                     std::to_wstring((int)st["fps"].num(30))
                               : L"Searching for your phone";
    float sw = TextW(status, 13, false);
    float sx = W - 60 - sw;
    g_br->SetColor(D2D1::ColorF(conn ? GREEN : DIMC));
    g_rt->FillEllipse(D2D1::Ellipse({sx - 12, 24}, 4, 4), g_br.Get());
    Text(status, {sx, 8, W - 60, 40}, 13, conn ? WHITE : MUTED, 1, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    Chip({W - 52, 8, W - 12, 40}, IC_SETTINGS, g_drawer, true, [] { g_drawer = !g_drawer; Invalidate(); }, 16);

    // ---- viewfinder
    const float SIDE = conn ? 150.f : 0.f;
    D2D1_RECT_F vf = {0, 48, W - SIDE, H};
    bool drew = false;
    if (conn) DrawPreview(vf, drew);
    if (!g_vcamError.empty()) Text(g_vcamError, {vf.left, vf.top, vf.right, vf.top + 28}, 13, RED, 1, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    if (!conn) ConnectPanel(vf);
    else if (!drew) {
        std::wstring msg = st["mode"].str() == "portrait" && !st["cam"].boolean() ? L"Starting portrait mode…"
                         : st["live"].boolean(true) ? L"Waiting for video…" : L"Paused";
        Text(msg, vf, 15, MUTED, 1, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    }

    if (conn) {
        std::string mode = st["mode"].str("video");
        float cx = (vf.left + vf.right) / 2;
        float y = vf.bottom - 56;

        // zoom bar: presets like the stock camera, the active one shows the live value
        double zmin = caps["zoomMin"].num(1), zmax = caps["zoomMax"].num(1), z = st["zoom"].num(1);
        std::vector<double> presets;
        if (zmin < 0.95) presets.push_back(std::round(zmin * 10) / 10);
        presets.push_back(1);
        for (double p : {2.0, 5.0, 10.0}) if (p <= zmax + 1e-6) presets.push_back(p);
        double active = presets[0];
        for (double p : presets) if (p <= z + 1e-3) active = p;
        float zw = presets.size() * 48.f + 8;
        Fill({cx - zw / 2, y, cx + zw / 2, y + 40}, 0x000000, 0.55f, 20);
        float zx = cx - zw / 2 + 4;
        for (double p : presets) {
            bool on = p == active;
            Chip({zx, y + 3, zx + 44, y + 37}, on ? FmtZoom(z) + L"x" : FmtZoom(p), on, true, [p, on] {
                if (on) OpenSpec("zoom"); else Set("zoom", JNum(p));
            }, 12);
            zx += 48;
        }
        Fill({vf.left + 14, y, vf.left + 54, y + 40}, 0x000000, 0.55f, 20);
        Chip({vf.left + 14, y, vf.left + 54, y + 40}, L"±", st["ev"].num() != 0 || (g_spec.on && g_spec.key == "ev"), mode != "pro",
             [] { OpenSpec("ev"); }, 18);

        // pro row
        float rowY = y - 46;
        if (mode == "pro") {
            bool manual = caps["manual"].boolean(), canFocus = caps["focusMax"].num() > 0;
            std::vector<std::string> wbs;
            for (auto& v : caps["wb"].a) wbs.push_back(v.s);
            std::vector<std::string> aps;
            for (auto& v : caps["apertures"].a) aps.push_back(JNum(v.n));
            wchar_t iso[24], ap[24];
            swprintf_s(iso, L"ISO %d", (int)st["iso"].num());
            swprintf_s(ap, L"f/%.1f", st["aperture"].num(aps.empty() ? 0 : caps["apertures"].a[0].n));
            std::string wb = st["wb"].str("auto"), apCur = JNum(st["aperture"].num());
            struct P { std::wstring l; bool on, sup; std::function<void()> f; };
            std::vector<P> ps = {
                {iso, g_spec.on && g_spec.key == "iso", manual, [] { OpenSpec("iso"); }},
                {Shutter(st["exp"].num()), g_spec.on && g_spec.key == "exp", manual, [] { OpenSpec("exp"); }},
                {af ? L"AF" : Focus(st["focus"].num()), g_spec.on && g_spec.key == "focus", canFocus, [] { Set("af", "false"); OpenSpec("focus"); }},
                {L"WB " + Wide(wb), wb != "auto", true, [wbs, wb] { Cycle("wb", wbs, wb); }},
                {ap, false, aps.size() > 1, [aps, apCur] { Cycle("aperture", aps, apCur); }},
            };
            float pw = 88, px = cx - ps.size() * pw / 2;
            Fill({px - 6, rowY, px + ps.size() * pw + 6, rowY + 36}, 0x000000, 0.55f, 18);
            for (auto& p : ps) {
                Chip({px, rowY + 2, px + pw - 4, rowY + 34}, p.l, p.on, p.sup, p.f, 12);
                px += pw;
            }
            rowY -= 46;
        }

        // slider for the active setting
        if (g_spec.on) {
            double v = st[g_spec.key].num(g_spec.min);
            float sw = std::min(460.f, vf.right - vf.left - 40);
            D2D1_RECT_F box = {cx - sw / 2, rowY, cx + sw / 2, rowY + 36};
            Fill(box, 0x000000, 0.6f, 18);
            Text(SpecLabel(g_spec, v, caps), {box.left + 10, box.top, box.left + 92, box.bottom}, 12, ACCENT);
            g_track = {box.left + 100, box.top + 8, box.right - 44, box.bottom - 8};
            float ty = (box.top + box.bottom) / 2;
            float kx = g_track.left + (float)SpecTo(g_spec, v) * (g_track.right - g_track.left);
            Fill({g_track.left, ty - 2, g_track.right, ty + 2}, WHITE, 0.25f, 2);
            Fill({g_track.left, ty - 2, kx, ty + 2}, ACCENT, 1, 2);
            g_br->SetColor(D2D1::ColorF(WHITE));
            g_rt->FillEllipse(D2D1::Ellipse({kx, ty}, 8, 8), g_br.Get());
            Chip({box.right - 40, box.top + 2, box.right - 6, box.bottom - 2}, IC_CLOSE, false, true, [] { g_spec.on = false; Invalidate(); }, 11);
        }

        // ---- sidebar: modes · LIVE · flip
        float sx = W - SIDE, scx = W - SIDE / 2;
        g_br->SetColor(D2D1::ColorF(0x0E0E10));
        g_rt->FillRectangle({sx, 48, W, H}, g_br.Get());
        const char* modes[] = {"pro", "video", "portrait"};
        const wchar_t* names[] = {L"Pro", L"Video", L"Portrait"};
        for (int i = 0; i < 3; i++) {
            std::string m = modes[i];
            bool sup = m != "portrait" || caps["portrait"].boolean();
            Chip({sx + 20, 70.f + i * 46, W - 20, 108.f + i * 46}, names[i], m == mode, sup, [m] {
                if (m != "pro" && (g_spec.key == "iso" || g_spec.key == "exp" || g_spec.key == "focus")) g_spec.on = false;
                Set("mode", JStr(m));
            }, 14);
        }
        bool live = st["live"].boolean(true);
        float ly = std::max(260.f, H / 2 + 40);
        g_br->SetColor(D2D1::ColorF(WHITE));
        g_rt->DrawEllipse(D2D1::Ellipse({scx, ly}, 36, 36), g_br.Get(), 3);
        g_br->SetColor(D2D1::ColorF(live ? RED : 0x444444));
        g_rt->FillEllipse(D2D1::Ellipse({scx, ly}, 29, 29), g_br.Get());
        Text(live ? L"LIVE" : L"PAUSED", {scx - 30, ly - 12, scx + 30, ly + 12}, 11, WHITE);
        g_btns.push_back({{scx - 36, ly - 36, scx + 36, ly + 36}, [] { Toggle("live", true); }});
        bool canFlip = caps["facings"].a.size() > 1;
        Fill({scx - 28, H - 92, scx + 28, H - 36}, WHITE, 0.13f, 28);
        Chip({scx - 28, H - 92, scx + 28, H - 36}, IC_FLIP, false, canFlip, [st] {
            Set("facing", JStr(st["facing"].str() == "front" ? "back" : "front"));
        }, 20);
    }

    // ---- settings drawer
    g_drawerFirst = g_btns.size();
    g_drawerRect = {};
    if (g_drawer) {
        float dx = W - 372, dy = 48;
        std::vector<std::string> modesList;
        for (auto& v : caps["modes"].a) modesList.push_back(v.s);
        std::vector<std::string> resList;
        for (auto& m : modesList) {
            std::string r = m.substr(0, m.find('@'));
            if (std::find(resList.begin(), resList.end(), r) == resList.end()) resList.push_back(r);
        }
        float h = 12.f + (conn ? 4 : 0) * 66 + 2 * 66 + 30;
        g_drawerRect = {dx, dy, W - 12, dy + h};
        Fill(g_drawerRect, SURFACE, 0.98f, 14);
        Stroke(g_drawerRect, WHITE, 0.08f, 14);
        float cy = dy + 12;
        auto header = [&](const wchar_t* t) {
            Text(t, {dx + 20, cy, W - 24, cy + 20}, 11, MUTED, 1, DWRITE_TEXT_ALIGNMENT_LEADING);
            cy += 22;
        };
        auto row = [&](const std::vector<std::wstring>& labels, std::function<bool(int)> on, std::function<bool(int)> sup,
                       std::function<void(int)> f) {
            float rx = dx + 14;
            for (int i = 0; i < (int)labels.size(); i++) {
                float w = TextW(labels[i], 13) + 28;
                D2D1_RECT_F r = {rx, cy, rx + w, cy + 32};
                if (!on(i) && sup(i)) Fill(r, WHITE, 0.05f, 16);
                Chip(r, labels[i], on(i), sup(i), [f, i] { f(i); });
                rx += w + 6;
            }
            cy += 44;
        };
        auto any = [](int) { return true; };
        if (conn) {
            std::string fpsCur = std::to_string((int)st["fps"].num(30));
            header(L"RESOLUTION");
            std::vector<std::wstring> rl;
            for (auto& r : resList) rl.push_back(Wide(r.substr(r.find('x') + 1)) + L"p");
            row(rl, [&](int i) { return resList[i] == res; }, any, [resList](int i) { Set("res", JStr(resList[i])); });
            header(L"FRAME RATE");
            row({L"30 fps", L"60 fps"}, [&](int i) { return fpsCur == (i ? "60" : "30"); },
                [&](int i) { return std::find(modesList.begin(), modesList.end(), res + (i ? "@60" : "@30")) != modesList.end(); },
                [](int i) { Set("fps", i ? "60" : "30"); });
            header(L"BITRATE (MBPS)");
            int rates[] = {0, 8, 16, 32, 50};
            int cur = (int)st["mbps"].num();
            row({L"Auto", L"8", L"16", L"32", L"50"}, [&](int i) { return rates[i] == cur; }, any,
                [](int i) { static const int r[] = {0, 8, 16, 32, 50}; Set("mbps", JNum(r[i])); });
            header(L"PC OUTPUT");
            row({L"Mirror", L"Rotate 180°"}, [&](int i) { return st[i ? "rot180" : "mirror"].boolean(); }, any,
                [](int i) { Toggle(i ? "rot180" : "mirror"); });
        }
        header(L"WINDOWS");
        bool autostart = AutostartOn();
        float qx = dx + 14 + TextW(L"Start with Windows", 13) + 34;
        row({L"Start with Windows"}, [&](int) { return autostart; }, any, [autostart](int) { SetAutostart(!autostart); Invalidate(); });
        Chip({qx, cy - 44, qx + TextW(L"Quit openCam", 13) + 28, cy - 12}, L"Quit openCam", false, true,
             [] { PostMessageW(g_hwnd, WM_COMMAND, 4, 0); }, 13, RED);

        header(L"UPDATES");
        std::string upd;
        std::wstring us;
        {
            std::lock_guard<std::mutex> l(g_m);
            upd = g_update;
            us = g_updStatus;
        }
        float bx = g_drawerRect.right - 14;
        if (upd.empty()) Button({bx - 112, cy, bx, cy + 32}, L"Check now", false, [] { SetEvent(g_updWake); });
        else Button({bx - 112, cy, bx, cy + 32}, L"Restart now", true, [] { g_updNow = true; SetEvent(g_updWake); });
        std::wstring line = us.empty() ? L"Checks GitHub every 6 hours" : us;
        if (!upd.empty()) line += L" · installs when the camera is idle";
        Text(line, {dx + 20, cy - 4, bx - 120, cy + 36}, 12, WHITE, 0.85f, DWRITE_TEXT_ALIGNMENT_LEADING, false, true);
        cy += 44;
        Text(L"openCam " + Wide(OPENCAM_VERSION), {dx + 20, cy, bx, cy + 20}, 11, MUTED, 1, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    }

    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        g_rt.Reset();
        g_br.Reset();
        g_bmp.Reset();
    }
}

// ================================================================== tray

static HICON MakeIcon(UINT32 rgb) {
    const int S = 32;
    BITMAPV5HEADER bh = {};
    bh.bV5Size = sizeof bh;
    bh.bV5Width = S;
    bh.bV5Height = -S;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;
    void* bits;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bh, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    DWORD* px = (DWORD*)bits;
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            // the app icon's open ring with its light in the gap; the centre shows the link state
            float dx = x + 0.5f - 16, dy = y + 0.5f - 16, d = std::sqrt(dx * dx + dy * dy);
            float deg = std::atan2(dy, dx) * 57.2958f, lx = dx - 8.66f, ly = dy + 8.66f;
            bool ring = d >= 10 && d <= 14.5f && !(deg > -77 && deg < -13), light = lx * lx + ly * ly <= 6.25f;
            px[y * S + x] = d <= 6.5f ? 0xFF000000u | rgb : ring || light ? 0xFFFFFFFFu : 0;
        }
    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return h;
}

static void UpdateTray() {
    bool live;
    std::string model;
    {
        std::lock_guard<std::mutex> l(g_m);
        live = g_state["live"].boolean(true);
        model = g_state["model"].str("phone");
    }
    int s = !g_connected ? 0 : live ? 1 : 2;
    if (s == g_iconState) return;
    g_iconState = s;
    if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
    g_nid.hIcon = MakeIcon(s == 0 ? 0x777777 : s == 1 ? GREEN : ACCENT);
    std::wstring tip = s == 0 ? L"openCam – searching for phone" : L"openCam – " + Wide(model) + (s == 1 ? L" live" : L" paused");
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void ShowMain(bool show) {
    if (show) {
        ShowWindow(g_hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_hwnd);
        SetTimer(g_hwnd, 1, 33, nullptr);
    } else {
        ShowWindow(g_hwnd, SW_HIDE);
        KillTimer(g_hwnd, 1);
    }
    g_visible = show;
}

static void Quit() {
    g_quit = true;
    {
        std::lock_guard<std::mutex> l(g_sendM);
        if (g_sock != INVALID_SOCKET) shutdown(g_sock, SD_BOTH);
    }
    if (g_vcam) { g_vcam->Remove(); g_vcam->Shutdown(); g_vcam.Reset(); }
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    DestroyWindow(g_hwnd);
}

static void TrayMenu() {
    bool live;
    {
        std::lock_guard<std::mutex> l(g_m);
        live = g_state["live"].boolean(true);
    }
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Open openCam");
    AppendMenuW(m, MF_STRING | (g_connected ? 0 : MF_GRAYED), 2, live ? L"Pause video" : L"Resume video");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (AutostartOn() ? MF_CHECKED : 0), 3, L"Start with Windows");
    AppendMenuW(m, MF_STRING, 4, L"Quit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
}

// ================================================================== window

static D2D1_POINT_2F Dip(LPARAM lp) {
    float s = Scale();
    return {GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s};
}

static void Drag(D2D1_POINT_2F p) {
    float w = g_track.right - g_track.left;
    if (w > 0) SpecSet(g_spec, (p.x - g_track.left) / w);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        Paint();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE:
        if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        if (wp == SIZE_MINIMIZED) g_visible = false;
        else if (IsWindowVisible(h)) g_visible = true;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        g_rt.Reset();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
    case WM_APP_UPDATE:
        UpdateTray();
        if (g_visible) InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        D2D1_POINT_2F p = Dip(lp);
        if (g_spec.on && In({g_track.left - 8, g_track.top - 8, g_track.right + 8, g_track.bottom + 8}, p)) {
            g_dragging = true;
            SetCapture(h);
            Drag(p);
            return 0;
        }
        g_editing = false; // the field's own button turns it back on
        size_t first = g_drawer && In(g_drawerRect, p) ? g_drawerFirst : 0;
        for (size_t i = g_btns.size(); i-- > first;) {
            if (In(g_btns[i].r, p)) {
                auto f = g_btns[i].f;
                f();
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
        }
        if (g_drawer && !In(g_drawerRect, p)) { g_drawer = false; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_mouse.x < 0) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
        }
        g_mouse = Dip(lp);
        if (g_dragging) Drag(g_mouse);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_mouse = {-1, -1};
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            bool hand = false;
            for (auto& b : g_btns) hand = hand || In(b.r, g_mouse);
            SetCursor(LoadCursor(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_CHAR:
        if (!g_editing) return 0;
        if (wp == '\b') { if (!g_edit.empty()) g_edit.pop_back(); }
        else if (wp == '\r') { g_editing = false; Find(g_edit); }
        else if (wp == 27) g_editing = false;
        else if (wp == 22 && OpenClipboard(h)) { // Ctrl+V
            if (HANDLE d = GetClipboardData(CF_UNICODETEXT)) {
                for (const wchar_t* c = (const wchar_t*)GlobalLock(d); c && *c; c++)
                    if ((iswdigit(*c) || *c == '.') && g_edit.size() < 15) g_edit += *c;
                GlobalUnlock(d);
            }
            CloseClipboard();
        } else if ((iswdigit((wint_t)wp) || wp == '.') && g_edit.size() < 15) g_edit += (wchar_t)wp;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (g_dragging) { g_dragging = false; ReleaseCapture(); }
        return 0;
    case WM_MOUSEWHEEL: {
        // wheel = pinch zoom
        double z, zmin, zmax;
        {
            std::lock_guard<std::mutex> l(g_m);
            z = g_state["zoom"].num(1);
            zmin = g_caps["zoomMin"].num(1);
            zmax = g_caps["zoomMax"].num(1);
        }
        z = std::clamp(z * std::pow(1.1, GET_WHEEL_DELTA_WPARAM(wp) / 120.0), zmin, zmax);
        Set("zoom", JNum(std::round(z * 100) / 100));
        return 0;
    }
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) ShowMain(!IsWindowVisible(h));
        else if (LOWORD(lp) == WM_RBUTTONUP) TrayMenu();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case 1: ShowMain(true); break;
        case 2: Toggle("live", true); break;
        case 3: SetAutostart(!AutostartOn()); break;
        case 4: Quit(); break;
        case 5: g_restart = true; Quit(); break; // the updater swapped in new files
        }
        return 0;
    case WM_CLOSE:
        ShowMain(false); // closing the window keeps the camera running in the tray
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/** `openCam.exe --selftest`: exit code 0 when the JSON parser reads a real phone state message. */
static int SelfTest() {
    Json j = ParseStr(R"({"t":"state","zoom":2.5,"af":false,"res":"1920x1080","model":"Pixel\/8 é","modes":["1280x720@30","1920x1080@60"],"x":{"y":[1,-2e3]},"n":null})");
    bool ok = j["t"].str() == "state" && j["zoom"].num() == 2.5 && !j["af"].boolean(true) && j["res"].str() == "1920x1080" &&
              j["model"].str() == "Pixel/8 \xC3\xA9" && j["modes"].a.size() == 2 && j["modes"].a[1].s == "1920x1080@60" &&
              j["x"]["y"].a[1].num() == -2000 && j["n"].t == Json::Null && j["missing"].num(7) == 7 &&
              JNum(3) == "3" && JNum(16000000) == "16000000" && JNum(0.25) == "0.2500";
    // deltas: an APK-like file with an inserted, a removed and a changed region; the exe against a patched copy
    std::string a;
    for (uint32_t i = 0, r = 1; i < 300000; i++) a += (char)((r = r * 1103515245 + 12345) >> 16);
    std::string b = a.substr(0, 1000) + "inserted" + a.substr(1000, 150000) + a.substr(160000);
    b[200000] ^= 1;
    ok = ok && ApplyBlockDelta(a, BlockDelta(a, b)) == b && BlockDelta(a, b).size() < 200 && ApplyBlockDelta(a, BlockDelta(a, a)) == a;
    std::string exe = ReadAll(ExePath()), exe2 = exe;
    exe2[exe2.size() / 2] ^= 0x55;
    std::string d = MsDelta(exe, exe2, true);
    ok = ok && !d.empty() && d.size() < 4096 && MsDelta(exe, d, false) == exe2;
    ok = ok && Ver("1.0.10") > Ver("1.0.9") && Ver("2.0.0") > Ver("1.99.99");
    return ok ? 0 : 1;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmd, int) {
    if (wcsstr(cmd, L"--selftest")) return SelfTest();
    if (wcsstr(cmd, L"--mkdelta")) return MakeDelta();
    if (wcsstr(cmd, L"--install")) return Install();
    if (wcsstr(cmd, L"--uninstall")) return Uninstall();

    // After an update the old instance is still exiting: wait for it to let go.
    HANDLE single = CreateMutexW(nullptr, FALSE, L"openCamTray");
    if (WaitForSingleObject(single, wcsstr(cmd, L"--updated") ? 15000 : 0) == WAIT_TIMEOUT) {
        HWND other = FindWindowW(L"openCamWnd", nullptr);
        if (other) PostMessageW(other, WM_COMMAND, 1, 0);
        return 0;
    }
    for (const char* name : UPDATE_FILES) DeleteFileW((ExeDir() + L"\\" + Wide(name) + L".old").c_str());
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);
    g_state.t = g_caps.t = Json::Obj;

    EnsureInstalled();
    FirstRunAutostart();
    OpenShared();
    StartVirtualCamera();

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)g_dw.GetAddressOf());

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"openCamWnd";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"openCam", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 680,
                             nullptr, nullptr, hi, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);

    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = MakeIcon(0x777777);
    wcscpy_s(g_nid.szTip, L"openCam");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_iconState = 0;

    ShowMain(!wcsstr(cmd, L"--tray"));
    std::thread(NetThread).detach();
    std::thread(UpdateThread).detach();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    MFShutdown();
    if (g_restart) {
        std::wstring c = L"\"" + ExeDir() + L"\\openCam.exe\" --tray --updated";
        STARTUPINFOW si = {sizeof si};
        PROCESS_INFORMATION pi;
        if (CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    return 0;
}
