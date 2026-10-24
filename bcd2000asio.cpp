// bcd2000asio.cpp - user-mode ASIO driver (x64) for the Behringer BCD2000 (USB 1397:00BD)
// Copyright (C) 2026 Graziano Spinelli
//
// Audio format derived from static analysis of BCD2000.SYS and verified on hardware:
//   44100 Hz nominal, 4 channels of little-endian int16, interleaved (8 bytes/frame),
//   isochronous OUT endpoint 0x02, isochronous IN endpoint 0x83, packets of 44/45 frames (352/360 bytes).
//   Output channels: 0-1 = master, 2-3 = headphones. There is no feedback endpoint: the card runs
//   its own clock (~+62 ppm), so the output mirrors the size of the packets received on the input
//   (implicit feedback).
//
// Requires the libusbK driver (installed with Zadig) on the card's single USB interface.
// Build (MinGW-w64, static libusb 1.0.30 and the ASIO SDK 2.3.4 extracted to ASIOSDK/):
//   x86_64-w64-mingw32-g++ -std=gnu++17 -O2 -shared -D_WIN32_WINNT=0x0A00 -I<libusb>/include
//     -I<ASIOSDK>/common -o bcd2000asio.dll bcd2000asio.cpp <libusb>/MinGW64/static/libusb-1.0.a
//     -static -static-libgcc -static-libstdc++ -lavrt -lwinmm -lole32 -luuid -ladvapi32 bcd2000asio_res.o
//   (the file version info comes from bcd2000asio.rc:
//    x86_64-w64-mingw32-windres bcd2000asio.rc -O coff -o bcd2000asio_res.o)
// Registration: regsvr32 bcd2000asio.dll (from an elevated prompt), or run install.bat.
// License: GNU GPL v3 or later (uses Steinberg's ASIO SDK 2.3.4 under the GPLv3).
// The MIDI/USB protocol comes from the Linux snd-bcd2000 driver (GPL).
// ASIO is a trademark of Steinberg Media Technologies GmbH.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>

#if __has_include(<libusb-1.0/libusb.h>)
#include <libusb-1.0/libusb.h>
#else
#include <libusb.h>
#endif

#include <objbase.h>
#ifndef interface
#define interface struct
#endif

#pragma pack(push, 4)
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"
#pragma pack(pop)

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

// ------------------------------------------------------------------ identity
// {25EBEB2D-C17B-47F5-8C48-AFBCCC017EFA}
static const GUID CLSID_BCD2000Asio = {0x25ebeb2d, 0xc17b, 0x47f5, {0x8c, 0x48, 0xaf, 0xbc, 0xcc, 0x01, 0x7e, 0xfa}};
static const wchar_t *kClsidStr = L"{25EBEB2D-C17B-47F5-8C48-AFBCCC017EFA}";
static const wchar_t *kDriverName = L"Behringer BCD2000";
static const wchar_t *kDriverDesc = L"Behringer BCD2000 (Graziano Spinelli 2026)";
static const char *kDriverNameA = "Behringer BCD2000";

// ------------------------------------------------------------------ device constants
static const uint16_t VID = 0x1397, PID = 0x00BD;
static const uint8_t EP_ISO_OUT = 0x02, EP_ISO_IN = 0x83;
static const int CHANNELS = 4;
static const int FRAME_BYTES = 8;
static const int MAXPKT = 384;       // bytes
static const int MAXFRAMES = 48;     // frames per packet
static const int PKT_IN = 2, NIN = 6;    // 2 ms per transfer, 6 in flight
static const int PKT_OUT = 2, NOUT = 10;   // 20 ms of output queue
static const int RING_CAP = 1 << 14; // frames (~370 ms)
static const double SAMPLE_RATE = 44100.0;

static HINSTANCE g_hInst = nullptr;
static std::atomic<long> g_objects{0};
static std::atomic<long> g_locks{0};

// ------------------------------------------------------------------ Windows 11: no background throttling
// An ASIO driver does not open a Windows audio session, so a background host can be treated as a
// "non-audio" app and slowed down. We disable throttling for our own threads (and, optionally,
// for the host process while audio is flowing).
static void disableThreadThrottling() {
    typedef BOOL(WINAPI * SetThreadInformationFn)(HANDLE, int, LPVOID, DWORD);
    static SetThreadInformationFn fn =
        (SetThreadInformationFn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadInformation");
    if (!fn) return;
    struct { ULONG Version, ControlMask, StateMask; } s = {1, 1, 0};  // ThreadPowerThrottling, EXECUTION_SPEED
    fn(GetCurrentThread(), 3, &s, sizeof s);
}
[[maybe_unused]] static void setProcessThrottling(bool disable) {
    PROCESS_POWER_THROTTLING_STATE s;
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = disable ? (PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION) : 0u;
    s.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof s);
}
static uint64_t qpcUs() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000ull + (uint64_t)(c.QuadPart % f.QuadPart) * 1000000ull / (uint64_t)f.QuadPart;
}

// ------------------------------------------------------------------ diagnostic log (%TEMP%\\bcd2000asio.log)
// Off by default: enable it with Log=1 in the [General] section of bcd2000asio.ini.
static std::atomic<bool> g_logEnabled{false};
static SRWLOCK g_logLock = SRWLOCK_INIT;
static void iniPath(wchar_t *path) {   // bcd2000asio.ini next to the DLL (buffer of MAX_PATH + 32)
    GetModuleFileNameW(g_hInst, path, MAX_PATH);
    wchar_t *sl = wcsrchr(path, L'\\');
    if (sl) *(sl + 1) = 0;
    wcscat(path, L"bcd2000asio.ini");
}
static void loadLogConfig() {
    wchar_t path[MAX_PATH + 32];
    iniPath(path);
    g_logEnabled = GetPrivateProfileIntW(L"General", L"Log", 0, path) != 0;
}
static void logf(const char *fmt, ...) {
    if (!g_logEnabled) return;
    char path[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (!n || n > MAX_PATH - 20) return;
    std::strcat(path, "bcd2000asio.log");
    char line[600];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int k = std::snprintf(line, sizeof line, "%02d:%02d:%02d.%03d [%lu] ", st.wHour, st.wMinute, st.wSecond,
                          st.wMilliseconds, GetCurrentProcessId());
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line + k, sizeof line - (size_t)k, fmt, ap);
    va_end(ap);
    AcquireSRWLockExclusive(&g_logLock);
    FILE *f = std::fopen(path, "a");
    if (f) { std::fprintf(f, "%s\n", line); std::fclose(f); }
    ReleaseSRWLockExclusive(&g_logLock);
}

static void LIBUSB_CALL libusbLog(libusb_context *, enum libusb_log_level level, const char *str) {
    char buf[400];
    std::strncpy(buf, str ? str : "", sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (size_t i = std::strlen(buf); i > 0 && (buf[i - 1] == '\n' || buf[i - 1] == '\r'); i--) buf[i - 1] = 0;
    logf("libusb[%d]: %s", (int)level, buf);
}

// ------------------------------------------------------------------ SPSC frame ring
struct FrameRing {
    std::vector<int16_t> data;
    std::atomic<size_t> w{0}, r{0};
    FrameRing() : data((size_t)RING_CAP * CHANNELS, 0) {}
    void reset() { w.store(0); r.store(0); std::fill(data.begin(), data.end(), 0); }
    size_t avail() const { return w.load(std::memory_order_acquire) - r.load(std::memory_order_acquire); }
    size_t space() const { return RING_CAP - avail(); }
    int16_t *slot(size_t absIdx) { return &data[(absIdx & (RING_CAP - 1)) * CHANNELS]; }
};

// ------------------------------------------------------------------ MIDI (interrupt EP 0x01/0x81 <-> loopMIDI ports)
// Protocol (snd-bcd2000): OUT = 03 00 <len> <MIDI data> in 64-byte packets (52-byte init sequence);
// IN = <len> <MIDI data>. The virtual ports are created by loopMIDI: "BCD2000 to DAW" (the DAW reads)
// and "DAW to BCD2000" (the DAW writes); the names can be changed in HKCU\Software\BCD2000Asio
// (MidiToHost, MidiFromHost, MidiEnabled=0 to disable, MidiSendInit=0 to skip the init sequence).
static const uint8_t EP_INT_OUT = 0x01, EP_INT_IN = 0x81;
static const unsigned char MIDI_INIT_SEQ[52] = {
    0x07, 0x00, 0x00, 0x00, 0x78, 0x48, 0x1c, 0x81, 0xc4, 0x00, 0x00, 0x00, 0x5e, 0x53, 0x4a, 0xf7,
    0x18, 0xfa, 0x11, 0xff, 0x6c, 0xf3, 0x90, 0xff, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x18, 0xfa, 0x11, 0xff, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf2, 0x34, 0x4a, 0xf7,
    0x18, 0xfa, 0x11, 0xff};

// Splits the card's raw MIDI stream into single messages (running status, sysex, real-time).
class MidiSplitter {
public:
    std::vector<std::vector<uint8_t>> feed(const std::vector<uint8_t> &data) {
        std::vector<std::vector<uint8_t>> out;
        for (uint8_t b : data) {
            if (b >= 0xF8) { out.push_back({b}); continue; }
            if (b & 0x80) {
                if (b == 0xF0) { msg_.assign(1, b); sysex_ = true; }
                else if (b == 0xF7) {
                    if (sysex_) { msg_.push_back(b); out.push_back(msg_); }
                    msg_.clear(); sysex_ = false;
                } else {
                    sysex_ = false;
                    status_ = b < 0xF0 ? b : 0;
                    int hi = b & 0xF0;
                    need_ = (hi == 0xC0 || hi == 0xD0) ? 2 : (hi >= 0x80 && hi <= 0xE0) ? 3 : 1;
                    msg_.assign(1, b);
                    if (need_ == 1) { out.push_back(msg_); msg_.clear(); }
                }
                continue;
            }
            if (sysex_) { msg_.push_back(b); if (msg_.size() > 4096) { msg_.clear(); sysex_ = false; } continue; }
            if (!status_) continue;
            if (msg_.empty()) msg_.assign(1, status_);   // running status
            msg_.push_back(b);
            if ((int)msg_.size() == need_) { out.push_back(msg_); msg_.clear(); }
        }
        return out;
    }
private:
    std::vector<uint8_t> msg_;
    uint8_t status_ = 0;
    int need_ = 3;
    bool sysex_ = false;
};

static bool regStr(const wchar_t *name, std::wstring &out) {
    wchar_t buf[128];
    DWORD sz = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\BCD2000Asio", name, RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) {
        out = buf;
        return true;
    }
    return false;
}
static DWORD regDword(const wchar_t *name, DWORD def) {
    DWORD v = 0, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\BCD2000Asio", name, RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) return v;
    return def;
}
static std::wstring lowerW(std::wstring s) {
    for (auto &c : s) c = (wchar_t)towlower(c);
    return s;
}
static int findPort(const std::wstring &needle, bool input) {
    const std::wstring nd = lowerW(needle);
    int sub = -1;
    UINT n = input ? midiInGetNumDevs() : midiOutGetNumDevs();
    for (UINT i = 0; i < n; i++) {
        std::wstring name;
        if (input) { MIDIINCAPSW c; if (midiInGetDevCapsW(i, &c, sizeof c) != MMSYSERR_NOERROR) continue; name = c.szPname; }
        else { MIDIOUTCAPSW c; if (midiOutGetDevCapsW(i, &c, sizeof c) != MMSYSERR_NOERROR) continue; name = c.szPname; }
        const std::wstring ln = lowerW(name);
        if (ln == nd) return (int)i;
        if (sub < 0 && ln.find(nd) != std::wstring::npos) sub = (int)i;
    }
    return sub;
}
// Jog wheel conversion: from an absolute value (a 0..127 counter going up/down) to a relative one.
// mode 1 = 2's complement (0x01 forward, 0x7F backward), 2 = 3Fh/41h (0x41 forward, 0x3F backward),
// 3 = signed bit (0x01 forward, 0x41 backward). Returns -1 if there is nothing to send.
static int jogConvert(int mode, int last, int v) {
    if (last < 0) return -1;                       // first event: only stored
    const int delta = ((v - last + 64) & 127) - 64;   // -64..63, handles the 127 -> 0 wrap
    if (delta == 0) return -1;
    int out;
    switch (mode) {
        case 1: out = delta > 0 ? delta : 128 + delta; break;
        case 2: out = 64 + delta; break;
        case 3: out = delta > 0 ? delta : 64 - delta; break;
        default: return -2;
    }
    return std::max(0, std::min(127, out));
}

static int midiMsgLen(unsigned st) {
    if (st < 0x80) return 0;
    if (st < 0xF0) return ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) ? 2 : 3;
    switch (st) {
        case 0xF1: case 0xF3: return 2;
        case 0xF2: return 3;
        case 0xF6: return 1;
        default: return st >= 0xF8 ? 1 : 0;
    }
}

class MidiBridge {
public:
    unsigned long long rxBytes_ = 0, txBytes_ = 0, errors_ = 0;
    ~MidiBridge() { stop(); }
    bool active() const { return running_; }

    bool start(libusb_device_handle *h) {
        if (running_) return true;
        if (regDword(L"MidiEnabled", 1) == 0) { logf("MIDI: disabled by registry"); return false; }
        loadJogConfig();
        std::wstring toHost = L"BCD2000 to DAW", fromHost = L"DAW to BCD2000";
        regStr(L"MidiToHost", toHost);
        regStr(L"MidiFromHost", fromHost);
        const int oi = findPort(toHost, false), ii = findPort(fromHost, true);
        if (oi < 0 || ii < 0) {
            logf("MIDI: loopMIDI ports not found (output '%ls': %s, input '%ls': %s)", toHost.c_str(),
                 oi < 0 ? "no" : "ok", fromHost.c_str(), ii < 0 ? "no" : "ok");
            return false;
        }
        h_ = h;
        if (midiOutOpen(&out_, (UINT)oi, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { out_ = nullptr; logf("MIDI: failed to open the output port"); return false; }
        stopping_ = false;
        if (midiInOpen(&in_, (UINT)ii, (DWORD_PTR)&MidiBridge::midiInProc, (DWORD_PTR)this, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
            in_ = nullptr;
            midiOutClose(out_); out_ = nullptr;
            logf("MIDI: failed to open the input port (already used by another program?)");
            return false;
        }
        for (int i = 0; i < 4; i++) {
            inBuf_[i].assign(1024, 0);
            std::memset(&inHdr_[i], 0, sizeof inHdr_[i]);
            inHdr_[i].lpData = inBuf_[i].data();
            inHdr_[i].dwBufferLength = 1024;
            midiInPrepareHeader(in_, &inHdr_[i], sizeof(MIDIHDR));
            midiInAddBuffer(in_, &inHdr_[i], sizeof(MIDIHDR));
        }
        evStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        evRx_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        evTx_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        evTxDone_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        rxQ_.clear(); txFifo_.clear(); readd_.clear();
        rxBytes_ = txBytes_ = errors_ = 0; rxErrRun_ = 0; txBusy_ = false; usbInflight_ = 0;
        rxX_ = libusb_alloc_transfer(0);
        txX_ = libusb_alloc_transfer(0);
        running_ = true;
        worker_ = CreateThread(nullptr, 0, &MidiBridge::workerProc, this, 0, nullptr);

        if (regDword(L"MidiSendInit", 1) != 0) {   // enables buttons and controls, as snd-bcd2000 does
            std::memcpy(txBuf_, MIDI_INIT_SEQ, sizeof MIDI_INIT_SEQ);
            libusb_fill_interrupt_transfer(txX_, h_, EP_INT_OUT, txBuf_, (int)sizeof MIDI_INIT_SEQ, &MidiBridge::cbTx, this, 1000);
            txBusy_ = true; usbInflight_++;
            if (libusb_submit_transfer(txX_) < 0) { txBusy_ = false; usbInflight_--; errors_++; logf("MIDI: failed to send the start-up sequence"); }
        }
        libusb_fill_interrupt_transfer(rxX_, h_, EP_INT_IN, rxBuf_, 64, &MidiBridge::cbRx, this, 0);
        usbInflight_++;
        if (libusb_submit_transfer(rxX_) < 0) { usbInflight_--; errors_++; logf("MIDI: failed to start reading"); }
        midiInStart(in_);
        logf("MIDI: active (output to DAW '%ls', input from DAW '%ls')", toHost.c_str(), fromHost.c_str());
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) { cleanupPorts(); return; }
        stopping_ = true;
        if (in_) { midiInStop(in_); midiInReset(in_); }
        DWORD t0 = GetTickCount();
        while (usbInflight_ > 0 && GetTickCount() - t0 < 2000) {
            if (rxX_) libusb_cancel_transfer(rxX_);
            if (txX_) libusb_cancel_transfer(txX_);
            Sleep(10);
        }
        if (evStop_) SetEvent(evStop_);
        if (worker_) { WaitForSingleObject(worker_, 2000); CloseHandle(worker_); worker_ = nullptr; }
        if (rxX_) { libusb_free_transfer(rxX_); rxX_ = nullptr; }
        if (txX_) { libusb_free_transfer(txX_); txX_ = nullptr; }
        cleanupPorts();
        HANDLE *evs[4] = {&evStop_, &evRx_, &evTx_, &evTxDone_};
        for (HANDLE *e : evs) if (*e) { CloseHandle(*e); *e = nullptr; }
        logf("MIDI: stopped (received %llu bytes, sent %llu bytes, errors %llu)", rxBytes_, txBytes_, errors_);
    }

private:
    void cleanupPorts() {
        if (in_) {
            for (int i = 0; i < 4; i++) midiInUnprepareHeader(in_, &inHdr_[i], sizeof(MIDIHDR));
            midiInClose(in_);
            in_ = nullptr;
        }
        if (out_) {
            if (outHdrPrepared_) { waitOutHdr(); midiOutUnprepareHeader(out_, &outHdr_, sizeof outHdr_); outHdrPrepared_ = false; }
            midiOutClose(out_);
            out_ = nullptr;
        }
    }

    static void LIBUSB_CALL cbRx(libusb_transfer *t) {
        MidiBridge *m = (MidiBridge *)t->user_data;
        if (m->stopping_ || t->status == LIBUSB_TRANSFER_CANCELLED || t->status == LIBUSB_TRANSFER_NO_DEVICE) { m->usbInflight_--; return; }
        if (t->status == LIBUSB_TRANSFER_COMPLETED) {
            m->rxErrRun_ = 0;
            if (t->actual_length >= 2) {
                int n = t->buffer[0], avail = t->actual_length - 1;
                if (n > avail) n = avail;
                if (n > 0) {
                    AcquireSRWLockExclusive(&m->lock_);
                    if (m->rxQ_.size() < 65536) m->rxQ_.insert(m->rxQ_.end(), t->buffer + 1, t->buffer + 1 + n);
                    ReleaseSRWLockExclusive(&m->lock_);
                    m->rxBytes_ += (unsigned)n;
                    SetEvent(m->evRx_);
                }
            }
        } else {
            m->errors_++;
            if (m->rxErrRun_++ < 5) logf("MIDI: read error, status=%d", (int)t->status);
            if (m->rxErrRun_ > 200) { logf("MIDI: too many read errors, stopping reads"); m->usbInflight_--; return; }
        }
        if (libusb_submit_transfer(t) < 0) { m->errors_++; m->usbInflight_--; logf("MIDI: read resubmit failed"); }
    }

    static void LIBUSB_CALL cbTx(libusb_transfer *t) {
        MidiBridge *m = (MidiBridge *)t->user_data;
        if (t->status != LIBUSB_TRANSFER_COMPLETED && t->status != LIBUSB_TRANSFER_CANCELLED) {
            m->errors_++;
            logf("MIDI: write error, status=%d", (int)t->status);
        }
        m->txBusy_ = false;
        m->usbInflight_--;
        SetEvent(m->evTxDone_);
    }

    static void CALLBACK midiInProc(HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR) {
        MidiBridge *m = (MidiBridge *)inst;
        if (!m || m->stopping_) return;
        if (msg == MIM_DATA) {
            const unsigned st = (unsigned)(p1 & 0xFF);
            const int len = midiMsgLen(st);
            if (len > 0 && st != 0xFE && st != 0xF8) {   // no active sensing / clock towards the card
                const uint8_t b[3] = {(uint8_t)st, (uint8_t)((p1 >> 8) & 0xFF), (uint8_t)((p1 >> 16) & 0xFF)};
                m->enqueueTx(b, (size_t)len);
            }
        } else if (msg == MIM_LONGDATA) {
            MIDIHDR *hd = (MIDIHDR *)p1;
            if (hd->dwBytesRecorded) m->enqueueTx((const uint8_t *)hd->lpData, hd->dwBytesRecorded);
            AcquireSRWLockExclusive(&m->lock_);
            m->readd_.push_back(hd);
            ReleaseSRWLockExclusive(&m->lock_);
            SetEvent(m->evTx_);
        }
    }

    void enqueueTx(const uint8_t *d, size_t n) {
        AcquireSRWLockExclusive(&lock_);
        if (txFifo_.size() + n < 65536) txFifo_.insert(txFifo_.end(), d, d + n);
        else errors_++;
        ReleaseSRWLockExclusive(&lock_);
        SetEvent(evTx_);
    }

    void waitOutHdr() {
        for (int i = 0; i < 100 && (outHdr_.dwFlags & MHDR_INQUEUE); i++) Sleep(2);
    }

    void loadJogConfig() {
        wchar_t path[MAX_PATH + 32];
        iniPath(path);
        jogMode_ = (int)GetPrivateProfileIntW(L"MIDI", L"JogMode", 4, path);
        jogLog_ = GetPrivateProfileIntW(L"MIDI", L"JogLog", 0, path) != 0;
        wchar_t list[128];
        GetPrivateProfileStringW(L"MIDI", L"JogCC", L"18,19", list, 128, path);
        std::memset(jogCc_, 0, sizeof jogCc_);
        for (wchar_t *p = list; *p;) {
            while (*p && (*p < L'0' || *p > L'9')) p++;
            if (!*p) break;
            long n = wcstol(p, &p, 10);
            if (n >= 0 && n < 128) jogCc_[n] = true;
        }
        for (auto &row : jogLast_) for (auto &x : row) x = -1;
        jogLogCount_ = otherLogCount_ = 0;
        logf("MIDI: JogMode=%d JogLog=%d JogCC=%ls (ini: %ls)", jogMode_, (int)jogLog_, list, path);
    }

    // Filter for messages going from the card to the DAW. Returns true if the message was handled.
    bool filterFromDevice(const std::vector<uint8_t> &m) {
        if (m.size() == 3 && (m[0] & 0xF0) == 0xB0 && jogCc_[m[1] & 0x7F]) {
            const int ch = m[0] & 0x0F, cc = m[1] & 0x7F, v = m[2] & 0x7F;
            if (jogLog_ && jogLogCount_++ < 400) logf("JOG ch=%d cc=%d value=%d", ch + 1, cc, v);
            if (jogMode_ == 0) return false;
            if (jogMode_ == 4) {
                // The card sends 0x41 for one direction and 0x40 for the other; Traktor (Rel 3Fh/41h) reads 0x40 as
                // "no movement". Backward must be 0x3F: 0x40 -> 0x3F, everything else unchanged.
                const int out = (v == 64) ? 63 : v;
                midiOutShortMsg(out_, (DWORD)m[0] | ((DWORD)cc << 8) | ((DWORD)out << 16));
                return true;
            }
            const int last = jogLast_[ch][cc];
            jogLast_[ch][cc] = v;
            const int out = jogConvert(jogMode_, last, v);
            if (out >= 0) midiOutShortMsg(out_, (DWORD)m[0] | ((DWORD)cc << 8) | ((DWORD)out << 16));
            return true;
        }
        if (m.size() >= 1 && m.size() <= 3 && m[0] < 0xF0 && otherLogCount_++ < 80)
            logf("MIDI from card: %02X %02X %02X", m[0], m.size() > 1 ? m[1] : 0, m.size() > 2 ? m[2] : 0);
        return false;
    }

    void sendToHost(const std::vector<uint8_t> &m) {
        if (!out_ || m.empty()) return;
        if (filterFromDevice(m)) return;
        if (m[0] != 0xF0 && m.size() <= 3) {
            DWORD msg = m[0];
            if (m.size() > 1) msg |= (DWORD)m[1] << 8;
            if (m.size() > 2) msg |= (DWORD)m[2] << 16;
            midiOutShortMsg(out_, msg);
            return;
        }
        if (outHdrPrepared_) { waitOutHdr(); midiOutUnprepareHeader(out_, &outHdr_, sizeof outHdr_); outHdrPrepared_ = false; }
        const size_t n = std::min<size_t>(m.size(), sizeof outBuf_);
        std::memcpy(outBuf_, m.data(), n);
        std::memset(&outHdr_, 0, sizeof outHdr_);
        outHdr_.lpData = outBuf_;
        outHdr_.dwBufferLength = (DWORD)n;
        if (midiOutPrepareHeader(out_, &outHdr_, sizeof outHdr_) == MMSYSERR_NOERROR) {
            outHdrPrepared_ = true;
            midiOutLongMsg(out_, &outHdr_, sizeof outHdr_);
        }
    }

    void pumpTx() {
        if (stopping_ || txBusy_) return;
        size_t n = 0;
        AcquireSRWLockExclusive(&lock_);
        n = std::min<size_t>(61, txFifo_.size());
        if (n) {
            txBuf_[0] = 0x03; txBuf_[1] = 0x00; txBuf_[2] = (unsigned char)n;
            std::memcpy(txBuf_ + 3, txFifo_.data(), n);
            txFifo_.erase(txFifo_.begin(), txFifo_.begin() + (ptrdiff_t)n);
        }
        ReleaseSRWLockExclusive(&lock_);
        if (!n) return;
        std::memset(txBuf_ + 3 + n, 0, 64 - 3 - n);
        libusb_fill_interrupt_transfer(txX_, h_, EP_INT_OUT, txBuf_, 64, &MidiBridge::cbTx, this, 1000);
        txBusy_ = true; usbInflight_++;
        if (libusb_submit_transfer(txX_) < 0) { txBusy_ = false; usbInflight_--; errors_++; logf("MIDI: send failed"); }
        else txBytes_ += n;
    }

    void worker() {
        HANDLE evs[4] = {evStop_, evRx_, evTx_, evTxDone_};
        while (true) {
            if (WaitForMultipleObjects(4, evs, FALSE, 100) == WAIT_OBJECT_0) break;
            std::vector<MIDIHDR *> ra;
            std::vector<uint8_t> bytes;
            AcquireSRWLockExclusive(&lock_);
            ra.swap(readd_);
            bytes.swap(rxQ_);
            ReleaseSRWLockExclusive(&lock_);
            if (!stopping_ && in_)
                for (MIDIHDR *hd : ra) { hd->dwBytesRecorded = 0; midiInAddBuffer(in_, hd, sizeof(MIDIHDR)); }
            if (!bytes.empty())
                for (auto &msg : split_.feed(bytes)) sendToHost(msg);
            pumpTx();
        }
    }
    static DWORD WINAPI workerProc(LPVOID p) { ((MidiBridge *)p)->worker(); return 0; }

    libusb_device_handle *h_ = nullptr;
    std::atomic<bool> running_{false}, stopping_{false}, txBusy_{false};
    std::atomic<int> usbInflight_{0};
    HMIDIOUT out_ = nullptr;
    HMIDIIN in_ = nullptr;
    HANDLE worker_ = nullptr, evStop_ = nullptr, evRx_ = nullptr, evTx_ = nullptr, evTxDone_ = nullptr;
    libusb_transfer *rxX_ = nullptr, *txX_ = nullptr;
    unsigned char rxBuf_[64] = {0}, txBuf_[64] = {0};
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::vector<uint8_t> rxQ_, txFifo_;
    std::vector<MIDIHDR *> readd_;
    MIDIHDR inHdr_[4];
    std::vector<char> inBuf_[4];
    MIDIHDR outHdr_{};
    char outBuf_[4096];
    bool outHdrPrepared_ = false;
    MidiSplitter split_;
    int rxErrRun_ = 0;
    int jogMode_ = 0, jogLogCount_ = 0, otherLogCount_ = 0;
    bool jogLog_ = true;
    bool jogCc_[128] = {};
    int jogLast_[16][128];
};

// ------------------------------------------------------------------ driver
class BCD2000Asio final : public IASIO {
public:
    BCD2000Asio() : refs_(1) { g_objects++; std::strcpy(err_, "No error"); }
    ~BCD2000Asio() { shutdown(); g_objects--; }

    // IUnknown (the ASIO host uses the CLSID as the IID)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == CLSID_BCD2000Asio) {
            *ppv = static_cast<IASIO *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        long n = --refs_;
        if (n == 0) delete this;
        return (ULONG)n;
    }

    // IASIO
    ASIOBool init(void *sysHandle) override {
        hwnd_ = (HWND)sysHandle;
        if (inited_) return ASIOTrue;
        loadLogConfig();
        logf("driver loaded (v11, Graziano Spinelli 2026)");
        {   // preferred ASIO buffer size: hosts such as Traktor have no buffer setting and use this value
            wchar_t ip[MAX_PATH + 32];
            iniPath(ip);
            long b = (long)GetPrivateProfileIntW(L"Audio", L"BufferSize", 512, ip);
            if (b < 128 || b > 2048 || (b & (b - 1))) {
                logf("init: invalid BufferSize=%ld in the ini file (valid: 128, 256, 512, 1024, 2048), using 512", b);
                b = 512;
            }
            prefBuf_ = b;
            logf("init: preferred buffer size %ld samples", prefBuf_);
        }
        const libusb_version *lv = libusb_get_version();
        logf("init: libusb %d.%d.%d.%d", lv->major, lv->minor, lv->micro, lv->nano);
        if (libusb_init(&ctx_) < 0) { setErr("libusb_init failed"); logf("init: libusb_init failed"); ctx_ = nullptr; return ASIOFalse; }
        libusb_set_option(ctx_, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
        libusb_set_log_cb(ctx_, libusbLog, LIBUSB_LOG_CB_CONTEXT);
        dev_ = libusb_open_device_with_vid_pid(ctx_, VID, PID);
        if (!dev_) {
            setErr("BCD2000 not found: check the connection and the libusbK driver (Zadig)");
            logf("init: card 1397:00BD cannot be opened");
            libusb_exit(ctx_); ctx_ = nullptr;
            return ASIOFalse;
        }
        int r = libusb_claim_interface(dev_, 0);
        if (r < 0) {
            setErr("BCD2000 is in use by another program (claim_interface)");
            logf("init: claim_interface failed: %s", libusb_error_name(r));
            libusb_close(dev_); dev_ = nullptr;
            libusb_exit(ctx_); ctx_ = nullptr;
            return ASIOFalse;
        }
        inited_ = true;
        evtStop_ = false;
        evThread_ = CreateThread(nullptr, 0, &BCD2000Asio::evThreadProc, this, 0, nullptr);
        midi_.start(dev_);
        logf("init: OK");
        return ASIOTrue;
    }
    void getDriverName(char *name) override { std::strcpy(name, kDriverNameA); }
    long getDriverVersion() override { return 1; }
    void getErrorMessage(char *string) override { std::strncpy(string, err_, 120); string[120] = 0; }

    ASIOError start() override {
        if (!inited_) return ASE_NotPresent;
        if (!callbacks_ || bufSize_ == 0) return ASE_InvalidMode;
        if (running_) return ASE_OK;
        logf("start: buffer=%ld timeInfo=%d", bufSize_, (int)timeInfoMode_);
        timeBeginPeriod(1);
        firstIn_ = false; firstPeriod_ = false; startMs_ = timeGetTime(); logged_ = 0;
        inRing_.reset();
        outRing_.reset();
        primed_ = false; fatal_ = false; resetAsked_ = false;
        periods_ = 0; nextIndex_ = 0;
        underruns_ = overruns_ = fallbackPkts_ = pktErrors_ = xferErrors_ = 0;
        maxWakeUs_ = lateWakes_ = maxCbUs_ = 0;
        sizeQ_.clear(); fbAcc_ = 0;
        samplePos_.store(0); timeStamp_.store(0);
        // the host has already filled output buffer B (index 1): it is queued first
        pushOutput(1);

        {   // after a cancelled session the isochronous pipes may be left in an error state: reset
            int r1 = libusb_clear_halt(dev_, EP_ISO_OUT), r2 = libusb_clear_halt(dev_, EP_ISO_IN);
            logf("start: clear_halt OUT=%s IN=%s", libusb_error_name(r1), libusb_error_name(r2));
            outErrRun_ = 0;
        }
        outRestart_ = false; outInflight_ = 0; inInflight_ = 0; restarts_ = 0; anomIn_ = 0; badSizes_ = 0;
        fgOurs_ = true; lastHb_ = timeGetTime(); wMaxWakeUs_ = wMaxCbUs_ = 0;
        running_ = true;
        pumpEvt_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        for (int i = 0; i < NIN; i++) {
            libusb_transfer *t = libusb_alloc_transfer(PKT_IN);
            inBufs_[i].assign((size_t)PKT_IN * MAXPKT, 0);
            libusb_fill_iso_transfer(t, dev_, EP_ISO_IN, inBufs_[i].data(), (int)inBufs_[i].size(), PKT_IN,
                                     &BCD2000Asio::cbIn, this, 1000);
            libusb_set_iso_packet_lengths(t, MAXPKT);
            inX_[i] = t;
        }
        for (int i = 0; i < NOUT; i++) {
            libusb_transfer *t = libusb_alloc_transfer(PKT_OUT);
            outBufs_[i].assign((size_t)PKT_OUT * MAXPKT, 0);
            libusb_fill_iso_transfer(t, dev_, EP_ISO_OUT, outBufs_[i].data(), (int)outBufs_[i].size(), PKT_OUT,
                                     &BCD2000Asio::cbOut, this, 1000);
            outX_[i] = t;
        }
        pumpThread_ = CreateThread(nullptr, 0, &BCD2000Asio::pumpThreadProc, this, 0, nullptr);

        // output first (silence), then input
        for (int i = 0; i < NOUT; i++) fillOut(outX_[i]);   // all filled before submitting (no races with the event thread)
        for (int i = 0; i < NOUT; i++) {
            if (libusb_submit_transfer(outX_[i]) < 0) { setErr("Isochronous output submit failed"); logf("start: submit OUT failed"); stopInternal(); return ASE_HWMalfunction; }
            g_inflight_++; outInflight_++;
        }
        for (int i = 0; i < NIN; i++) {
            if (libusb_submit_transfer(inX_[i]) < 0) { setErr("Isochronous input read failed"); logf("start: submit IN failed"); stopInternal(); return ASE_HWMalfunction; }
            g_inflight_++; inInflight_++;
        }
        logf("start: transfers started");
        return ASE_OK;
    }

    ASIOError stop() override {
        if (!running_) return ASE_OK;
        stopInternal();
        return ASE_OK;
    }

    ASIOError getChannels(long *in, long *out) override {
        if (!inited_) return ASE_NotPresent;
        *in = CHANNELS; *out = CHANNELS;
        return ASE_OK;
    }
    ASIOError getLatencies(long *in, long *out) override {
        if (!inited_) return ASE_NotPresent;
        long n = bufSize_ ? bufSize_ : prefBuf_;
        *in = n + PKT_IN * 45;
        *out = n + NOUT * PKT_OUT * 45;
        return ASE_OK;
    }
    ASIOError getBufferSize(long *minSize, long *maxSize, long *preferredSize, long *granularity) override {
        if (!inited_) return ASE_NotPresent;
        *minSize = 128; *maxSize = 2048; *preferredSize = prefBuf_; *granularity = -1;  // powers of two
        return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate r) override { return (r == SAMPLE_RATE) ? ASE_OK : ASE_NoClock; }
    ASIOError getSampleRate(ASIOSampleRate *r) override { *r = SAMPLE_RATE; return ASE_OK; }
    ASIOError setSampleRate(ASIOSampleRate r) override { return (r == SAMPLE_RATE) ? ASE_OK : ASE_NoClock; }
    ASIOError getClockSources(ASIOClockSource *clocks, long *num) override {
        if (!clocks || !num) return ASE_InvalidParameter;
        clocks->index = 0;
        clocks->associatedChannel = -1;
        clocks->associatedGroup = -1;
        clocks->isCurrentSource = ASIOTrue;
        std::strcpy(clocks->name, "Internal");
        *num = 1;
        return ASE_OK;
    }
    ASIOError setClockSource(long ref) override { return ref == 0 ? ASE_OK : ASE_InvalidParameter; }

    ASIOError getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp) override {
        if (!running_) return ASE_SPNotAdvancing;
        uint64_t p = samplePos_.load(), t = timeStamp_.load();
        sPos->hi = (unsigned long)(p >> 32); sPos->lo = (unsigned long)(p & 0xffffffffu);
        tStamp->hi = (unsigned long)(t >> 32); tStamp->lo = (unsigned long)(t & 0xffffffffu);
        return ASE_OK;
    }

    ASIOError getChannelInfo(ASIOChannelInfo *info) override {
        if (!inited_) return ASE_NotPresent;
        if (info->channel < 0 || info->channel >= CHANNELS) return ASE_InvalidParameter;
        static const char *outNames[CHANNELS] = {"Master L", "Master R", "Phones L", "Phones R"};
        static const char *inNames[CHANNELS] = {"Input 1", "Input 2", "Input 3", "Input 4"};
        info->type = ASIOSTInt16LSB;
        info->channelGroup = info->channel / 2;
        info->isActive = ASIOFalse;
        const int c = (int)info->channel;
        if (info->isInput) {
            info->isActive = inActive_[c] ? ASIOTrue : ASIOFalse;
            std::strcpy(info->name, inNames[c]);
        } else {
            info->isActive = outActive_[c] ? ASIOTrue : ASIOFalse;
            std::strcpy(info->name, outNames[c]);
        }
        return ASE_OK;
    }

    ASIOError createBuffers(ASIOBufferInfo *infos, long numChannels, long bufferSize, ASIOCallbacks *cb) override {
        if (!inited_) return ASE_NotPresent;
        if (!infos || !cb || numChannels <= 0) return ASE_InvalidParameter;
        if (bufferSize < 128 || bufferSize > 2048 || (bufferSize & (bufferSize - 1))) return ASE_InvalidMode;
        disposeBuffers();
        for (long i = 0; i < numChannels; i++)
            if (infos[i].channelNum < 0 || infos[i].channelNum >= CHANNELS) return ASE_InvalidMode;
        bufSize_ = bufferSize;
        callbacks_ = cb;
        logf("createBuffers: channels=%ld buffer=%ld", numChannels, bufferSize);
        for (int c = 0; c < CHANNELS; c++) { inActive_[c] = outActive_[c] = false; inPtr_[c] = outPtr_[c] = nullptr; }
        for (long i = 0; i < numChannels; i++) {
            const int c = (int)infos[i].channelNum;
            std::vector<int16_t> &store = infos[i].isInput ? inStore_[c] : outStore_[c];
            store.assign((size_t)bufferSize * 2, 0);
            int16_t *base = store.data();
            infos[i].buffers[0] = base;
            infos[i].buffers[1] = base + bufferSize;
            if (infos[i].isInput) { inActive_[c] = true; inPtr_[c] = base; }
            else { outActive_[c] = true; outPtr_[c] = base; }
        }
        timeInfoMode_ = (cb->asioMessage && cb->asioMessage(kAsioSupportsTimeInfo, 0, 0, 0) == 1);
        return ASE_OK;
    }
    ASIOError disposeBuffers() override {
        stop();
        bufSize_ = 0;
        callbacks_ = nullptr;
        for (int c = 0; c < CHANNELS; c++) {
            inStore_[c].clear(); inStore_[c].shrink_to_fit();
            outStore_[c].clear(); outStore_[c].shrink_to_fit();
            inActive_[c] = outActive_[c] = false; inPtr_[c] = outPtr_[c] = nullptr;
        }
        return ASE_OK;
    }

    ASIOError controlPanel() override {
        char msg[512];
        std::snprintf(msg, sizeof msg,
                      "Behringer BCD2000 - ASIO driver (libusbK)\n"
                      "Graziano Spinelli 2026\n\n"
                      "44100 Hz, 4 inputs / 4 outputs, int16\n"
                      "Buffer: %ld samples\n"
                      "MIDI: %s (received %llu bytes, sent %llu bytes, errors %llu)\n\n"
                      "Last session statistics:\n"
                      "  periods processed: %llu\n"
                      "  output underruns: %llu\n"
                      "  input overruns: %llu\n"
                      "  fallback-size packets: %llu\n"
                      "  USB errors: %llu packets, %llu transfers\n"
                      "  output restarts: %llu\n"
                      "  slowest wake-up: %.1f ms (over 5 ms: %llu times)\n"
                      "  longest host callback: %.1f ms",
                      bufSize_, midi_.active() ? "active" : "inactive", midi_.rxBytes_, midi_.txBytes_, midi_.errors_,
                      (unsigned long long)periods_, (unsigned long long)underruns_,
                      (unsigned long long)overruns_, (unsigned long long)fallbackPkts_,
                      (unsigned long long)pktErrors_, (unsigned long long)xferErrors_, (unsigned long long)restarts_,
                      maxWakeUs_ / 1000.0, (unsigned long long)lateWakes_, maxCbUs_ / 1000.0);
        MessageBoxA(hwnd_, msg, "BCD2000 ASIO", MB_OK | MB_ICONINFORMATION);
        return ASE_OK;
    }
    ASIOError future(long selector, void *) override {
        if (selector == kAsioCanTimeInfo) return ASE_SUCCESS;
        if (selector == kAsioCanTimeCode) return ASE_NotPresent;
        return ASE_InvalidParameter;
    }
    ASIOError outputReady() override { return ASE_NotPresent; }

private:
    // ---------------------------------------------------------- utilities
    void setErr(const char *m) { std::strncpy(err_, m, sizeof err_ - 1); err_[sizeof err_ - 1] = 0; }

    static uint64_t nowNs() { return (uint64_t)timeGetTime() * 1000000ull; }

    void shutdown() {
        stop();
        disposeBuffers();
        midi_.stop();
        if (evThread_) { evtStop_ = true; WaitForSingleObject(evThread_, 2000); CloseHandle(evThread_); evThread_ = nullptr; }
        if (dev_) { libusb_release_interface(dev_, 0); libusb_close(dev_); dev_ = nullptr; }
        if (ctx_) { libusb_exit(ctx_); ctx_ = nullptr; }
        inited_ = false;
    }

    void stopInternal() {
        logf("stop: periods=%llu underruns=%llu overruns=%llu fallback=%llu packetErrors=%llu transferErrors=%llu "
             "maxWake=%.1fms (>5ms: %llu) maxCallback=%.1fms restarts=%llu anomalies=%llu",
             periods_, underruns_, overruns_, fallbackPkts_, pktErrors_, xferErrors_, maxWakeUs_ / 1000.0,
             lateWakes_, maxCbUs_ / 1000.0, restarts_, anomIn_ + badSizes_);
        running_ = false;
        // cancel the transfers and wait for the event thread to release them
        DWORD t0 = GetTickCount();
        while (g_inflight_ > 0 && GetTickCount() - t0 < 2500) {
            for (int i = 0; i < NIN; i++) if (inX_[i]) libusb_cancel_transfer(inX_[i]);
            for (int i = 0; i < NOUT; i++) if (outX_[i]) libusb_cancel_transfer(outX_[i]);
            Sleep(20);
        }
        if (pumpEvt_) SetEvent(pumpEvt_);
        if (pumpThread_) { WaitForSingleObject(pumpThread_, 2000); CloseHandle(pumpThread_); pumpThread_ = nullptr; }
        for (int i = 0; i < NIN; i++) { if (inX_[i]) { libusb_free_transfer(inX_[i]); inX_[i] = nullptr; } }
        for (int i = 0; i < NOUT; i++) { if (outX_[i]) { libusb_free_transfer(outX_[i]); outX_[i] = nullptr; } }
        if (pumpEvt_) { CloseHandle(pumpEvt_); pumpEvt_ = nullptr; }
        g_inflight_ = 0;
        if (dev_) { libusb_clear_halt(dev_, EP_ISO_OUT); libusb_clear_halt(dev_, EP_ISO_IN); }
        timeEndPeriod(1);
    }

    // ---------------------------------------------------------- output: ring -> packets
    // Size of the next output packet: in-order copy of the sizes of the packets received on the
    // input (implicit feedback); if none are available, the nominal pattern.
    int nextOutFrames() {
        if (sizeQ_.size() > 0) {
            int f = sizeQ_.front();
            sizeQ_.erase(sizeQ_.begin());
            if (f >= 40 && f <= MAXFRAMES) return f;
            badSizes_++;   // anomalous input packet (empty/too short): must not be mirrored on the output
        }
        fallbackPkts_++;
        fbAcc_ += 44100;
        int f = fbAcc_ / 1000;
        fbAcc_ %= 1000;
        return f;
    }

    void fillOut(libusb_transfer *t) {
        uint8_t *p = t->buffer;
        int total = 0;
        for (int i = 0; i < PKT_OUT; i++) {
            int frames = nextOutFrames();
            if (frames > MAXFRAMES) frames = MAXFRAMES;
            const int len = frames * FRAME_BYTES;
            if (primed_.load(std::memory_order_acquire) && outRing_.avail() >= (size_t)frames) {
                size_t rd = outRing_.r.load(std::memory_order_relaxed);
                for (int f = 0; f < frames; f++) {
                    std::memcpy(p + f * FRAME_BYTES, outRing_.slot(rd + f), FRAME_BYTES);
                }
                outRing_.r.store(rd + frames, std::memory_order_release);
            } else {
                std::memset(p, 0, (size_t)len);
                if (primed_.load(std::memory_order_acquire) && frames > 0) underruns_++;
            }
            t->iso_packet_desc[i].length = len;
            p += len;
            total += len;
        }
        t->length = total;
    }

    static void LIBUSB_CALL cbOut(libusb_transfer *t) {
        BCD2000Asio *d = (BCD2000Asio *)t->user_data;
        if (!d->running_ || t->status == LIBUSB_TRANSFER_CANCELLED || t->status == LIBUSB_TRANSFER_NO_DEVICE) {
            if (t->status == LIBUSB_TRANSFER_NO_DEVICE) d->fatal_ = true;
            d->g_inflight_--; d->outInflight_--;
            return;
        }
        if (d->outRestart_) {   // output restart in progress: do not resubmit
            d->g_inflight_--; d->outInflight_--;
            return;
        }
        if (t->status != LIBUSB_TRANSFER_COMPLETED) {
            d->xferErrors_++;
            if (d->logged_++ < 40)
                logf("OUT: error status=%d after %lu ms, ring=%lu frames, in flight=%d", (int)t->status,
                     (unsigned long)(timeGetTime() - d->startMs_), (unsigned long)d->outRing_.avail(),
                     d->outInflight_.load());
            if (d->logged_++ < 40)
                logf("OUT: packet lengths %d,%d, total %d", (int)t->iso_packet_desc[0].length,
                     (int)t->iso_packet_desc[PKT_OUT - 1].length, t->length);
            d->outRestart_ = true;   // cancel everything: the event thread performs the clean restart
            for (int j = 0; j < NOUT; j++)
                if (d->outX_[j] && d->outX_[j] != t) libusb_cancel_transfer(d->outX_[j]);
            for (int j = 0; j < NIN; j++)
                if (d->inX_[j]) libusb_cancel_transfer(d->inX_[j]);
            d->g_inflight_--; d->outInflight_--;
            return;
        }
        for (int i = 0; i < t->num_iso_packets; i++)
            if (t->iso_packet_desc[i].status != LIBUSB_TRANSFER_COMPLETED) {
                d->pktErrors_++;
                if (d->logged_++ < 40) logf("OUT: packet %d status %d", i, (int)t->iso_packet_desc[i].status);
            }
        d->fillOut(t);
        if (libusb_submit_transfer(t) < 0) { d->fatal_ = true; d->g_inflight_--; d->outInflight_--; logf("OUT: resubmit failed"); }
    }

    // Clean restart (event thread): all OUT and IN transfers have finished.
    void restartOut() {
        int r = libusb_clear_halt(dev_, EP_ISO_OUT);
        int r2 = libusb_clear_halt(dev_, EP_ISO_IN);
        const size_t av = outRing_.avail(), keep = (size_t)bufSize_ * 2;
        if (av > keep) outRing_.r.store(outRing_.r.load() + (av - keep), std::memory_order_release);
        sizeQ_.clear();
        restarts_++;
        logf("FULL RESTART #%llu (clear_halt OUT=%s IN=%s, outRing=%lu frames)", restarts_, libusb_error_name(r),
             libusb_error_name(r2), (unsigned long)outRing_.avail());
        if (restarts_ > 200) { fatal_ = true; outRestart_ = false; return; }
        for (int i = 0; i < NOUT; i++) fillOut(outX_[i]);
        for (int i = 0; i < NOUT && running_; i++) {
            if (libusb_submit_transfer(outX_[i]) < 0) { fatal_ = true; logf("OUT: restart, submit failed"); break; }
            g_inflight_++; outInflight_++;
        }
        for (int i = 0; i < NIN && running_ && !fatal_; i++) {
            if (libusb_submit_transfer(inX_[i]) < 0) { fatal_ = true; logf("IN: restart, submit failed"); break; }
            g_inflight_++; inInflight_++;
        }
        outRestart_ = false;
    }

    // Monitor (event thread): focus changes and a status line every 2 s, to correlate errors in the log.
    void monitor() {
        if (!running_ || !g_logEnabled) return;   // only needed to write the log
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        const bool ours = (pid == GetCurrentProcessId());
        if (ours != fgOurs_) {
            fgOurs_ = ours;
            logf("FOCUS: host %s (periods=%llu outRing=%lu inRing=%lu)", ours ? "in foreground" : "NOT in foreground",
                 periods_, (unsigned long)outRing_.avail(), (unsigned long)inRing_.avail());
        }
        const DWORD now = timeGetTime();
        if (now - lastHb_ >= 2000) {
            lastHb_ = now;
            logf("status: periods=%llu underruns=%llu overruns=%llu outRing=%lu inRing=%lu wake2s=%.1fms callback2s=%.1fms "
                 "restarts=%llu anomalies=%llu fg=%d",
                 periods_, underruns_, overruns_, (unsigned long)outRing_.avail(), (unsigned long)inRing_.avail(),
                 wMaxWakeUs_ / 1000.0, wMaxCbUs_ / 1000.0, restarts_, anomIn_ + badSizes_, ours ? 1 : 0);
            wMaxWakeUs_ = 0; wMaxCbUs_ = 0;
        }
    }

    // ---------------------------------------------------------- input: packets -> ring
    static void LIBUSB_CALL cbIn(libusb_transfer *t) {
        BCD2000Asio *d = (BCD2000Asio *)t->user_data;
        if (!d->running_ || t->status == LIBUSB_TRANSFER_CANCELLED || t->status == LIBUSB_TRANSFER_NO_DEVICE) {
            if (t->status == LIBUSB_TRANSFER_NO_DEVICE) d->fatal_ = true;
            d->g_inflight_--; d->inInflight_--;
            return;
        }
        if (d->outRestart_) {   // full restart in progress: do not resubmit
            d->g_inflight_--; d->inInflight_--;
            return;
        }
        if (t->status != LIBUSB_TRANSFER_COMPLETED) {
            d->xferErrors_++;
            if (d->logged_++ < 40) logf("IN: transfer status %d", (int)t->status);
        }
        bool any = false;
        for (int i = 0; i < t->num_iso_packets; i++) {
            const libusb_iso_packet_descriptor &pd = t->iso_packet_desc[i];
            if (pd.status != LIBUSB_TRANSFER_COMPLETED) {
                d->pktErrors_++;
                if (d->logged_++ < 20) logf("IN: packet %d status %d", i, (int)pd.status);
                continue;
            }
            int frames = (int)pd.actual_length / FRAME_BYTES;
            if (frames > MAXFRAMES) frames = MAXFRAMES;
            if (frames < 40 && d->anomIn_++ < 30)
                logf("IN: anomalous packet %d bytes (packet %d) after %lu ms", (int)pd.actual_length, i,
                     (unsigned long)(timeGetTime() - d->startMs_));
            // sizes kept in order for the implicit feedback (max 64 queued)
            if (d->sizeQ_.size() >= 64) d->sizeQ_.erase(d->sizeQ_.begin());
            d->sizeQ_.push_back(frames);
            if (frames == 0) continue;
            const unsigned char *b = libusb_get_iso_packet_buffer_simple(t, i);
            if (!b) continue;
            if (d->inRing_.space() < (size_t)frames) { d->overruns_++; continue; }
            size_t wr = d->inRing_.w.load(std::memory_order_relaxed);
            for (int f = 0; f < frames; f++)
                std::memcpy(d->inRing_.slot(wr + f), b + f * FRAME_BYTES, FRAME_BYTES);
            d->inRing_.w.store(wr + frames, std::memory_order_release);
            any = true;
        }
        if (any && !d->firstIn_) { d->firstIn_ = true; logf("first IN data after %lu ms", (unsigned long)(timeGetTime() - d->startMs_)); }
        if (any && d->pumpEvt_) { d->sigUs_.store(qpcUs()); SetEvent(d->pumpEvt_); }
        if (libusb_submit_transfer(t) < 0) { d->fatal_ = true; d->g_inflight_--; d->inInflight_--; logf("IN: resubmit failed"); }
    }

    // ---------------------------------------------------------- thread
    static DWORD WINAPI evThreadProc(LPVOID p) {
        BCD2000Asio *d = (BCD2000Asio *)p;
        DWORD idx = 0;
        HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        disableThreadThrottling();
        while (!d->evtStop_) {
            timeval tv{0, 20000};
            libusb_handle_events_timeout_completed(d->ctx_, &tv, nullptr);
            if (d->outRestart_ && d->outInflight_ == 0 && d->inInflight_ == 0 && d->running_ && !d->fatal_) d->restartOut();
            d->monitor();
        }
        if (mm) AvRevertMmThreadCharacteristics(mm);
        return 0;
    }

    static DWORD WINAPI pumpThreadProc(LPVOID p) {
        BCD2000Asio *d = (BCD2000Asio *)p;
        DWORD idx = 0;
        HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        disableThreadThrottling();
        d->pump();
        if (mm) AvRevertMmThreadCharacteristics(mm);
        return 0;
    }

    void pushOutput(int half) {
        const size_t n = (size_t)bufSize_;
        if (outRing_.space() < n) { overruns_++; return; }
        size_t wr = outRing_.w.load(std::memory_order_relaxed);
        for (size_t f = 0; f < n; f++) {
            int16_t *dst = outRing_.slot(wr + f);
            for (int c = 0; c < CHANNELS; c++)
                dst[c] = outPtr_[c] ? outPtr_[c][(size_t)half * n + f] : 0;
        }
        outRing_.w.store(wr + n, std::memory_order_release);
    }

    void pump() {
        const size_t n = (size_t)bufSize_;
        ASIOTime at;
        std::memset(&at, 0, sizeof at);
        at.timeInfo.speed = 1.0;
        at.timeInfo.sampleRate = SAMPLE_RATE;
        while (running_ && !evtStop_) {
            if (WaitForSingleObject(pumpEvt_, 100) == WAIT_OBJECT_0) {
                const uint64_t sig = sigUs_.load(), now = qpcUs();
                if (now > sig) {
                    const uint64_t lat = now - sig;
                    if (lat > maxWakeUs_) maxWakeUs_ = lat;
                    if (lat > wMaxWakeUs_) wMaxWakeUs_ = lat;
                    if (lat > 5000) lateWakes_++;
                }
            }
            if (fatal_ && !resetAsked_) {
                resetAsked_ = true;
                setErr("USB error: card unplugged or stream interrupted");
                logf("fatal: asking the host to reset");
                if (callbacks_ && callbacks_->asioMessage) callbacks_->asioMessage(kAsioResetRequest, 0, 0, 0);
            }
            while (running_ && inRing_.avail() >= n) {
                const int half = nextIndex_;
                size_t rd = inRing_.r.load(std::memory_order_relaxed);
                for (size_t f = 0; f < n; f++) {
                    const int16_t *src = inRing_.slot(rd + f);
                    for (int c = 0; c < CHANNELS; c++)
                        if (inPtr_[c]) inPtr_[c][(size_t)half * n + f] = src[c];
                }
                inRing_.r.store(rd + n, std::memory_order_release);

                const uint64_t pos = (uint64_t)periods_ * n;
                const uint64_t ts = nowNs();
                samplePos_.store(pos);
                timeStamp_.store(ts);

                const uint64_t cb0 = qpcUs();
                if (timeInfoMode_) {
                    at.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
                    at.timeInfo.systemTime.hi = (unsigned long)(ts >> 32);
                    at.timeInfo.systemTime.lo = (unsigned long)(ts & 0xffffffffu);
                    at.timeInfo.samplePosition.hi = (unsigned long)(pos >> 32);
                    at.timeInfo.samplePosition.lo = (unsigned long)(pos & 0xffffffffu);
                    callbacks_->bufferSwitchTimeInfo(&at, half, ASIOFalse);
                } else {
                    callbacks_->bufferSwitch(half, ASIOFalse);
                }
                {
                    const uint64_t dur = qpcUs() - cb0;
                    if (dur > maxCbUs_) maxCbUs_ = dur;
                    if (dur > wMaxCbUs_) wMaxCbUs_ = dur;
                }

                pushOutput(half);
                if (!firstPeriod_) { firstPeriod_ = true; logf("first ASIO period after %lu ms", (unsigned long)(timeGetTime() - startMs_)); }
                primed_.store(true, std::memory_order_release);
                nextIndex_ ^= 1;
                periods_++;
            }
        }
    }

    // ---------------------------------------------------------- state
    std::atomic<long> refs_;
    char err_[128];
    HWND hwnd_ = nullptr;
    bool inited_ = false;
    libusb_context *ctx_ = nullptr;
    libusb_device_handle *dev_ = nullptr;

    ASIOCallbacks *callbacks_ = nullptr;
    long bufSize_ = 0;
    long prefBuf_ = 512;   // preferred buffer size from the ini file
    bool timeInfoMode_ = false;
    std::vector<int16_t> inStore_[CHANNELS], outStore_[CHANNELS];
    int16_t *inPtr_[CHANNELS] = {nullptr, nullptr, nullptr, nullptr};
    int16_t *outPtr_[CHANNELS] = {nullptr, nullptr, nullptr, nullptr};
    bool inActive_[CHANNELS] = {false, false, false, false};
    bool outActive_[CHANNELS] = {false, false, false, false};

    std::atomic<bool> running_{false}, evtStop_{false}, primed_{false}, fatal_{false};
    bool resetAsked_ = false;
    std::atomic<long> g_inflight_{0};
    HANDLE pumpEvt_ = nullptr, pumpThread_ = nullptr, evThread_ = nullptr;
    libusb_transfer *inX_[NIN] = {nullptr}, *outX_[NOUT] = {nullptr};
    std::vector<unsigned char> inBufs_[NIN], outBufs_[NOUT];

    MidiBridge midi_;
    FrameRing inRing_, outRing_;
    std::vector<int> sizeQ_;   // used only by the event thread
    int fbAcc_ = 0;
    int nextIndex_ = 0;
    unsigned long long periods_ = 0;
    std::atomic<uint64_t> samplePos_{0}, timeStamp_{0};
    unsigned long long underruns_ = 0, overruns_ = 0, fallbackPkts_ = 0, pktErrors_ = 0, xferErrors_ = 0;
    unsigned long long maxWakeUs_ = 0, lateWakes_ = 0, maxCbUs_ = 0;
    std::atomic<uint64_t> sigUs_{0};
    std::atomic<int> logged_{0};
    int outErrRun_ = 0;
    std::atomic<bool> outRestart_{false};
    std::atomic<int> outInflight_{0};
    unsigned long long restarts_ = 0, anomIn_ = 0, badSizes_ = 0;
    std::atomic<int> inInflight_{0};
    bool fgOurs_ = true;
    DWORD lastHb_ = 0;
    unsigned long long wMaxWakeUs_ = 0, wMaxCbUs_ = 0;
    bool firstIn_ = false, firstPeriod_ = false;
    DWORD startMs_ = 0;
};

// ------------------------------------------------------------------ COM
class Factory : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = static_cast<IClassFactory *>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID riid, void **ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        BCD2000Asio *d = new (std::nothrow) BCD2000Asio();
        if (!d) return E_OUTOFMEMORY;
        HRESULT hr = d->QueryInterface(riid, ppv);
        d->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { if (lock) g_locks++; else g_locks--; return S_OK; }
};
static Factory g_factory;

extern "C" {
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_hInst = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}
__declspec(dllexport) HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv) {
    if (clsid != CLSID_BCD2000Asio) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}
__declspec(dllexport) HRESULT __stdcall DllCanUnloadNow() {
    return (g_objects == 0 && g_locks == 0) ? S_OK : S_FALSE;
}

static LONG setStr(const wchar_t *path, const wchar_t *name, const wchar_t *val) {
    HKEY k;
    LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &k, nullptr);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r;
}

__declspec(dllexport) HRESULT __stdcall DllRegisterServer() {
    wchar_t dll[MAX_PATH];
    GetModuleFileNameW(g_hInst, dll, MAX_PATH);
    wchar_t key[256];
    swprintf(key, 256, L"SOFTWARE\\Classes\\CLSID\\%ls", kClsidStr);
    LONG r = setStr(key, nullptr, kDriverName);
    wchar_t key2[300];
    swprintf(key2, 300, L"%ls\\InprocServer32", key);
    if (r == ERROR_SUCCESS) r = setStr(key2, nullptr, dll);
    if (r == ERROR_SUCCESS) r = setStr(key2, L"ThreadingModel", L"Apartment");
    wchar_t key3[300];
    swprintf(key3, 300, L"SOFTWARE\\ASIO\\%ls", kDriverName);
    if (r == ERROR_SUCCESS) r = setStr(key3, L"CLSID", kClsidStr);
    if (r == ERROR_SUCCESS) r = setStr(key3, L"Description", kDriverDesc);
    return r == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(r);
}

__declspec(dllexport) HRESULT __stdcall DllUnregisterServer() {
    wchar_t key[256];
    swprintf(key, 256, L"SOFTWARE\\Classes\\CLSID\\%ls", kClsidStr);
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID", 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
        RegDeleteTreeW(k, kClsidStr);
        RegCloseKey(k);
    }
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
        RegDeleteTreeW(k, kDriverName);
        RegCloseKey(k);
    }
    return S_OK;
}
}
