/* SPDX-License-Identifier: GPL-2.0-only */
// Persistent L2 bridge. Never opens/closes BootNet, resets hardware, or logs payload.
// Production requires the OFFICIAL OpenVPN tap-windows6 tap-windows.h, supplied
// externally via NnpiTapIncludeDir. No TAP IOCTL numbers are reproduced here.
// Protocol references (documentation, not a substitute for header provenance):
// https://github.com/OpenVPN/tap-windows6
// https://github.com/OpenVPN/openvpn/blob/master/src/openvpn/tun.c
// NNPI_BRIDGE_DEPENDENCY_CHECK compiles Windows plumbing but MUST NOT open devices.
#include "../include/nnpi_net_data_abi.h"
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <limits>

namespace bridge {
constexpr size_t FrameBytes = NNPI_NET_RX_FRAME_BYTES, QueueSize = 32;
constexpr uint32_t Busy = 170; // Win32 ERROR_BUSY: driver STATUS_DEVICE_BUSY, not accepted.
constexpr uint32_t Aborted = 995;
struct Identity { uint64_t session, generation, epoch; };
template<class T> void bind(T& v, Identity id, uint32_t version) {
    v = {}; v.version = version; v.size = sizeof(v);
    v.session_id = id.session; v.generation = id.generation; v.epoch = id.epoch;
}
template<class T> bool identity(const T& v, Identity id, uint32_t version) {
    return v.version == version && v.size == sizeof(v) && v.session_id == id.session &&
        v.generation == id.generation && v.epoch == id.epoch;
}
struct Frame { uint32_t size = 0; std::array<uint8_t, FrameBytes> data{}; };
struct Queue {
    std::array<Frame, QueueSize> frames{}; size_t first = 0, count = 0;
    size_t free() const { return QueueSize - count; }
    Frame& front() { if (!count) throw std::logic_error("empty queue"); return frames[first]; }
    void push(const uint8_t* p, uint32_t n) {
        if (!free() || n > FrameBytes) throw std::logic_error("queue bound");
        auto& f = frames[(first + count) % QueueSize]; f.size = n;
        std::memcpy(f.data.data(), p, n); ++count;
    }
    void pop() { if (!count) throw std::logic_error("empty queue"); first = (first + 1) % QueueSize; --count; }
};
enum Lane : size_t { TapRead, NetTx, NetRx, TapWrite, LaneCount };
struct Op {
    std::array<uint8_t, sizeof(nnpi_net_rx_batch_response)> in{}, out{};
    uint32_t inBytes = 0, outBytes = 0, bytes = 0, error = 0;
    uint64_t started = 0;
    bool active = false;
    template<class T> void input(const T& v) {
        static_assert(sizeof(T) <= sizeof(in), "input bound");
        std::memcpy(in.data(), &v, sizeof(v)); inBytes = sizeof(v);
    }
    template<class T> T output() const {
        T v{}; static_assert(sizeof(T) <= sizeof(out), "output bound");
        std::memcpy(&v, out.data(), sizeof(v)); return v;
    }
};
struct Stats {
    uint64_t tapReadFrames = 0, tapReadBytes = 0, txAccepted = 0, txBytes = 0;
    uint64_t rxFrames = 0, rxBytes = 0, tapWritten = 0, tapWrittenBytes = 0;
    uint64_t shortDrops = 0, busy = 0, errors = 0, uncertainTx = 0, shutdownDrops = 0;
};
// Backend owns OS events, not packet memory. cancelDrain MUST finish all requests
// before returning. No IO can reference these objects after Engine::stop().
template<class Backend> class Engine {
public:
    Backend& backend; Identity id; Queue toNet{}, toTap{};
    std::array<Op, LaneCount> ops{}; Stats stats{};
    bool claimed = false, running = false, stopped = false;
    uint64_t nextTx = 0;
    explicit Engine(Backend& b, Identity i) : backend(b), id(i) {}
    ~Engine() { stop(); }
    bool begin() {
        if (running || stopped) return false;
        if (!backend.claim(id)) { ++stats.errors; return false; }
        claimed = true;
        if (!backend.activate()) { ++stats.errors; return false; }
        running = true; return true;
    }
    void fail() { ++stats.errors; running = false; }
    void submit(Lane lane, uint64_t now) {
        auto& op = ops[lane]; op.bytes = 0; op.error = 0;
        op.started = now; op.active = true; backend.start(lane, op);
    }
    bool tick(uint64_t now) {
        if (!running) return false;
        // At most one completion/submission per lane per tick: neither direction
        // can starve the other, even for synchronously completed operations.
        for (size_t i = 0; i < LaneCount; ++i) {
            auto& op = ops[i]; if (!op.active) continue;
            if (!backend.poll(static_cast<Lane>(i), op)) {
                // An idle TAP read is expected. A stuck TX/write/control is fatal;
                // its disposition is unknown and it is NEVER replayed.
                if (i != TapRead && i != NetRx && now - op.started >= 5000) {
                    if (i == NetTx) ++stats.uncertainTx;
                    fail(); return false;
                }
                continue;
            }
            op.active = false;
            if (op.error) {
                if (i == NetTx && op.error == Busy) { ++stats.busy; nextTx = now; continue; }
                if (i == NetTx) ++stats.uncertainTx;
                fail(); return false;
            }
            if (i == TapRead) {
                if (op.bytes > FrameBytes) { fail(); return false; }
                if (op.bytes < 14) { ++stats.shortDrops; continue; }
                toNet.push(op.out.data(), op.bytes); ++stats.tapReadFrames; stats.tapReadBytes += op.bytes;
            } else if (i == NetTx) {
                const auto r = op.output<nnpi_net_tx_response>();
                if (op.bytes != sizeof(r) || !identity(r, id, NNPI_NET_DATA_VERSION) ||
                    r.accepted != 1 || r.slot >= 16 || r.reserved[0] || r.reserved[1]) {
                    ++stats.uncertainTx; fail(); return false;
                }
                ++stats.txAccepted; stats.txBytes += toNet.front().size; toNet.pop();
            } else if (i == NetRx) {
                const auto r = op.output<nnpi_net_rx_batch_response>();
                nnpi_net_rx_batch_request req{}; std::memcpy(&req, op.in.data(), sizeof(req));
                if (op.bytes != sizeof(r) || !identity(r, id, NNPI_NET_RX_VERSION) ||
                    r.count > req.max_frames || r.count > toTap.free() ||
                    r.remaining > NNPI_NET_RX_QUEUE_DEPTH || r.reserved[0] || r.reserved[1]) {
                    fail(); return false;
                }
                // Validate the WHOLE batch before exposing any frame.
                for (size_t j = 0; j < NNPI_NET_RX_QUEUE_DEPTH; ++j) {
                    const auto& f = r.frames[j];
                    if (f.reserved || f.length > FrameBytes || (j >= r.count && f.length)) { fail(); return false; }
                    for (size_t k = (j < r.count ? f.length : 0); k < FrameBytes; ++k)
                        if (f.frame[k]) { fail(); return false; }
                }
                for (size_t j = 0; j < r.count; ++j) {
                    const auto& f = r.frames[j];
                    if (f.length < 14) { ++stats.shortDrops; continue; }
                    toTap.push(f.frame, f.length); ++stats.rxFrames; stats.rxBytes += f.length;
                }
                if (!r.count) { fail(); return false; } // wait ABI never returns empty success
            } else {
                if (op.bytes != toTap.front().size) { fail(); return false; }
                ++stats.tapWritten; stats.tapWrittenBytes += op.bytes; toTap.pop();
            }
        }
        if (!ops[TapRead].active && toNet.free()) {
            ops[TapRead].inBytes = 0; ops[TapRead].outBytes = static_cast<uint32_t>(FrameBytes);
            submit(TapRead, now);
        }
        if (!ops[NetTx].active && toNet.count && now >= nextTx) {
            nnpi_net_tx_request r{}; bind(r, id, NNPI_NET_DATA_VERSION);
            r.length = toNet.front().size; std::memcpy(r.frame, toNet.front().data.data(), r.length);
            ops[NetTx].input(r); ops[NetTx].outBytes = sizeof(nnpi_net_tx_response); submit(NetTx, now);
        }
        if (!ops[NetRx].active && toTap.free()) {
            nnpi_net_rx_batch_request r{}; bind(r, id, NNPI_NET_RX_VERSION);
            r.max_frames = static_cast<uint32_t>((std::min)(toTap.free(), size_t(NNPI_NET_RX_QUEUE_DEPTH)));
            ops[NetRx].input(r); ops[NetRx].outBytes = sizeof(nnpi_net_rx_batch_response); submit(NetRx, now);
        }
        if (!ops[TapWrite].active && toTap.count) {
            auto& op = ops[TapWrite]; op.inBytes = toTap.front().size; op.outBytes = 0;
            std::memcpy(op.in.data(), toTap.front().data.data(), op.inBytes); submit(TapWrite, now);
        }
        return true;
    }
    void stop() noexcept {
        if (stopped) return;
        stopped = true; running = false;
        // Conservatively mark any submitted TX without an observed terminal
        // reply as uncertain, including one cancelled by Ctrl+C. Never replay it.
        if (ops[NetTx].active && !stats.uncertainTx) ++stats.uncertainTx;
        backend.cancelDrain(ops); // complete cancellation BEFORE release or freeing memory
        stats.shutdownDrops += toNet.count + toTap.count;
        for (auto& op : ops) op.active = false;
        if (claimed) {
            if (!backend.release(id)) ++stats.errors;
            claimed = false;
        }
        backend.deactivate();
    }
};
} // namespace bridge

#ifndef NNPI_BRIDGE_MOCK
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <cstdlib>
#include <initguid.h>
#include "../include/nnpi_interface.h"
#include <memory>
#include <vector>
#include <cwchar>
#ifndef NNPI_BRIDGE_DEPENDENCY_CHECK
#include <tap-windows.h> // Official external header, NOT a locally fabricated substitute.
#ifndef TAP_WIN_IOCTL_GET_VERSION
#error Official tap-windows.h with TAP_WIN_IOCTL_GET_VERSION is required
#endif
#ifndef TAP_WIN_IOCTL_SET_MEDIA_STATUS
#error Official tap-windows.h with TAP_WIN_IOCTL_SET_MEDIA_STATUS is required
#endif
#endif
#pragma comment(lib, "setupapi.lib")
namespace bridge {
struct WinError : std::runtime_error {
    DWORD code;
    explicit WinError(const char* op, DWORD c = GetLastError()) : std::runtime_error(op), code(c) {}
};
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v = INVALID_HANDLE_VALUE) : value(v) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete; Handle& operator=(const Handle&) = delete;
    void open(HANDLE v) { if (v == INVALID_HANDLE_VALUE || !v) throw WinError("open handle"); value = v; }
};
struct DeviceSet {
    HDEVINFO value;
    explicit DeviceSet(HDEVINFO v) : value(v) { if (v == INVALID_HANDLE_VALUE) throw WinError("enumerate NNPI"); }
    ~DeviceSet() { SetupDiDestroyDeviceInfoList(value); }
};
std::wstring devicePath(const std::wstring& instance) {
    if (instance.size() < 4 || _wcsnicmp(instance.c_str(), L"PCI\\", 4))
        throw std::runtime_error("device-instance must be an exact PCI instance ID");
    DeviceSet set(SetupDiGetClassDevsW(&GUID_DEVINTERFACE_NNPI_INFO, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    std::wstring found;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA face{}; face.cbSize = sizeof(face);
        if (!SetupDiEnumDeviceInterfaces(set.value, nullptr, &GUID_DEVINTERFACE_NNPI_INFO, i, &face)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            throw WinError("enumerate interface");
        }
        DWORD n = 0;
        if (SetupDiGetDeviceInterfaceDetailW(set.value, &face, nullptr, 0, &n, nullptr) ||
            GetLastError() != ERROR_INSUFFICIENT_BUFFER || n < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
            throw WinError("interface size", ERROR_INVALID_DATA);
        std::unique_ptr<void, decltype(&std::free)> mem(std::malloc(n), &std::free);
        if (!mem) throw std::bad_alloc();
        auto* detail = static_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(mem.get());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA dev{}; dev.cbSize = sizeof(dev); DWORD returned = 0;
        if (!SetupDiGetDeviceInterfaceDetailW(set.value, &face, detail, n, &returned, &dev))
            throw WinError("interface detail");
        wchar_t name[MAX_DEVICE_ID_LEN]{};
        if (!SetupDiGetDeviceInstanceIdW(set.value, &dev, name, MAX_DEVICE_ID_LEN, nullptr))
            throw WinError("device instance");
        if (_wcsicmp(name, instance.c_str())) continue;
        if (!found.empty()) throw std::runtime_error("ambiguous PCI instance");
        const size_t offset = offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath);
        if (returned > n || returned < offset + sizeof(wchar_t) ||
            !std::wmemchr(detail->DevicePath, 0, (returned - offset) / sizeof(wchar_t)))
            throw std::runtime_error("invalid interface path");
        found = detail->DevicePath;
    }
    if (found.empty()) throw std::runtime_error("exact NNPI PCI instance not found");
    return found;
}
bool validGuid(const std::wstring& s) {
    if (s.size() != 38 || s.front() != L'{' || s.back() != L'}') return false;
    for (size_t i = 1; i < 37; ++i) {
        if (i == 9 || i == 14 || i == 19 || i == 24) { if (s[i] != L'-') return false; }
        else if (!((s[i] >= L'0' && s[i] <= L'9') || (s[i] >= L'a' && s[i] <= L'f') ||
            (s[i] >= L'A' && s[i] <= L'F'))) return false;
    }
    return true;
}
HANDLE stopEvent = nullptr;
BOOL WINAPI stopHandler(DWORD code) {
    if (code == CTRL_C_EVENT || code == CTRL_BREAK_EVENT || code == CTRL_CLOSE_EVENT ||
        code == CTRL_LOGOFF_EVENT || code == CTRL_SHUTDOWN_EVENT) {
        if (stopEvent) SetEvent(stopEvent);
        return TRUE;
    }
    return FALSE;
}
struct WinBackend {
    Handle net, tap, stop;
    std::array<Handle, LaneCount> events{};
    std::array<OVERLAPPED, LaneCount> overlapped{};
    std::array<bool, LaneCount> pending{};
    bool media = false;
    WinBackend(const std::wstring& device, const std::wstring& guid) {
#ifdef NNPI_BRIDGE_DEPENDENCY_CHECK
        (void)device; (void)guid;
        throw std::runtime_error("TAP official header missing: dependency-check build cannot access devices");
#else
        stop.open(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        for (auto& e : events) e.open(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        net.open(CreateFileW(device.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        const std::wstring path = L"\\\\.\\Global\\" + guid + L".tap";
        tap.open(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_SYSTEM, nullptr));
        ULONG version[3]{}; DWORD bytes = 0;
        if (!control(tap.value, TAP_WIN_IOCTL_GET_VERSION, version, sizeof(version),
                version, sizeof(version), bytes, false) || bytes != sizeof(version) || version[0] != 9)
            throw std::runtime_error("TAP version/protocol query failed; require verified tap-windows6 9.x");
        std::printf("tap_version=%lu.%lu debug=%lu\n", version[0], version[1], version[2]);
#endif
    }
    // Stack OVERLAPPED and buffers stay alive through terminal completion, even
    // when cancellation is slow. Never timeout-close a handle with live buffers.
    bool control(HANDLE h, DWORD code, void* in, DWORD inN, void* out, DWORD outN,
            DWORD& bytes, bool cancellable = true) noexcept {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!event.value) return false;
        OVERLAPPED ov{}; ov.hEvent = event.value;
        if (DeviceIoControl(h, code, in, inN, out, outN, &bytes, &ov)) return true;
        if (GetLastError() != ERROR_IO_PENDING) return false;
        HANDLE waits[] = {event.value, stop.value};
        const DWORD waited = WaitForMultipleObjects(cancellable ? 2 : 1, waits, FALSE, 5000);
        if (waited != WAIT_OBJECT_0) {
            CancelIoEx(h, &ov);
            GetOverlappedResult(h, &ov, &bytes, TRUE); // drain, never replay
            return false;
        }
        return GetOverlappedResult(h, &ov, &bytes, FALSE) != FALSE;
    }
    bool claim(Identity id) {
        nnpi_net_rx_claim_request q{}; bind(q, id, NNPI_NET_RX_VERSION);
        nnpi_net_rx_claim_response r{}; DWORD bytes = 0;
        return control(net.value, NNPI_NET_RX_CLAIM_IOCTL, &q, sizeof(q), &r, sizeof(r), bytes) &&
            bytes == sizeof(r) && identity(r, id, NNPI_NET_RX_VERSION) && r.claimed == 1 &&
            r.queue_depth == NNPI_NET_RX_QUEUE_DEPTH && r.max_frame_bytes == FrameBytes && !r.reserved;
    }
    bool release(Identity id) noexcept {
        nnpi_net_rx_claim_request q{}; bind(q, id, NNPI_NET_RX_VERSION);
        nnpi_net_rx_claim_response r{}; DWORD bytes = 0;
        return control(net.value, NNPI_NET_RX_RELEASE_IOCTL, &q, sizeof(q), &r, sizeof(r), bytes, false) &&
            bytes == sizeof(r) && identity(r, id, NNPI_NET_RX_VERSION) && !r.claimed && !r.reserved;
    }
    bool activate() {
#ifdef NNPI_BRIDGE_DEPENDENCY_CHECK
        return false;
#else
        ULONG status = 1; DWORD bytes = 0;
        // Even an unconfirmed media-up is undone on exit; never redo it blindly.
        media = true;
        return control(tap.value, TAP_WIN_IOCTL_SET_MEDIA_STATUS, &status, sizeof(status),
            &status, sizeof(status), bytes);
#endif
    }
    void deactivate() noexcept {
#ifndef NNPI_BRIDGE_DEPENDENCY_CHECK
        if (media) {
            ULONG status = 0; DWORD bytes = 0;
            if (!control(tap.value, TAP_WIN_IOCTL_SET_MEDIA_STATUS, &status, sizeof(status),
                    &status, sizeof(status), bytes, false))
                std::fputs("warning: TAP media-down unconfirmed\n", stderr);
            media = false;
        }
#endif
    }
    HANDLE handle(Lane lane) const { return lane == TapRead || lane == TapWrite ? tap.value : net.value; }
    void start(Lane lane, Op& op) {
        auto& ov = overlapped[lane]; ov = {}; ov.hEvent = events[lane].value;
        ResetEvent(ov.hEvent); BOOL ok; DWORD bytes = 0;
        if (lane == TapRead) ok = ReadFile(tap.value, op.out.data(), op.outBytes, &bytes, &ov);
        else if (lane == TapWrite) ok = WriteFile(tap.value, op.in.data(), op.inBytes, &bytes, &ov);
        else ok = DeviceIoControl(net.value, lane == NetTx ? NNPI_NET_TX_IOCTL : NNPI_NET_RX_WAIT_IOCTL,
            op.in.data(), op.inBytes, op.out.data(), op.outBytes, &bytes, &ov);
        op.bytes = bytes;
        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        pending[lane] = error == ERROR_IO_PENDING;
        if (!pending[lane]) op.error = error;
    }
    bool poll(Lane lane, Op& op) {
        if (!pending[lane]) return true;
        DWORD bytes = 0;
        if (!GetOverlappedResult(handle(lane), &overlapped[lane], &bytes, FALSE)) {
            op.error = GetLastError();
            if (op.error == ERROR_IO_INCOMPLETE) { op.error = 0; return false; }
        } else op.error = 0;
        op.bytes = bytes;
        pending[lane] = false; return true;
    }
    void cancelDrain(std::array<Op, LaneCount>& ops) noexcept {
        for (size_t i = 0; i < LaneCount; ++i)
            if (pending[i]) CancelIoEx(handle(static_cast<Lane>(i)), &overlapped[i]);
        for (size_t i = 0; i < LaneCount; ++i) if (pending[i]) {
            DWORD bytes = 0;
            GetOverlappedResult(handle(static_cast<Lane>(i)), &overlapped[i], &bytes, TRUE);
            ops[i].bytes = bytes; pending[i] = false;
        }
    }
    void wait(const std::array<Op, LaneCount>& ops, uint64_t nextTx, bool txQueued) {
        HANDLE handles[LaneCount + 1]{}; DWORD count = 1; handles[0] = stop.value;
        for (size_t i = 0; i < LaneCount; ++i) {
            if (ops[i].active && !pending[i]) return; // immediate completion
            if (pending[i]) handles[count++] = events[i].value;
        }
        const uint64_t now = GetTickCount64();
        uint64_t timeout = INFINITE;
        for (size_t i : {size_t(NetTx), size_t(TapWrite)}) if (ops[i].active) {
            const uint64_t due = ops[i].started + 5000;
            timeout = (std::min)(timeout, due > now ? due - now : uint64_t(0));
        }
        if (txQueued && !ops[NetTx].active)
            timeout = (std::min)(timeout, nextTx > now ? nextTx - now : uint64_t(0));
        WaitForMultipleObjects(count, handles, FALSE, static_cast<DWORD>(timeout));
    }
};
uint64_t number(const std::wstring& s) {
    if (s.empty()) throw std::runtime_error("empty identity");
    uint64_t n = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9' || n > (UINT64_MAX - 1 - static_cast<unsigned>(c - L'0')) / 10)
            throw std::runtime_error("identity must be decimal uint64, excluding UINT64_MAX");
        n = n * 10 + static_cast<unsigned>(c - L'0');
    }
    return n;
}
int run(int argc, wchar_t** argv) {
    if (argc == 2 && !std::wcscmp(argv[1], L"--help")) {
        std::puts("nnpi-net-bridge --device-instance <PCI-instance-ID> --tap-guid <{GUID}> --session <u64> --generation <u64> --epoch <u64>\n"
            "Requires a separately installed, configured, verified official TAP adapter. No installation, IP configuration, Boot/Net close, reset or payload logging.\n"
            "Acceptance target: Windows TAP 192.168.115.1 -> card 192.168.115.2, NOT localhost. Ctrl+C cancels/drains IO then releases claim.");
        return 0;
    }
    std::wstring instance, guid; Identity id{}; unsigned seen = 0;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) throw std::runtime_error("missing option value");
        const std::wstring key = argv[i], value = argv[i + 1]; unsigned bit = 0;
        if (key == L"--device-instance") { bit = 1; instance = value; }
        else if (key == L"--tap-guid") { bit = 2; guid = value; }
        else if (key == L"--session") { bit = 4; id.session = number(value); }
        else if (key == L"--generation") { bit = 8; id.generation = number(value); }
        else if (key == L"--epoch") { bit = 16; id.epoch = number(value); }
        else throw std::runtime_error("unknown option");
        if (seen & bit) throw std::runtime_error("duplicate option");
        seen |= bit;
    }
    if (seen != 31 || !validGuid(guid)) throw std::runtime_error("all five options and canonical braced TAP GUID required; use --help");
#ifdef NNPI_BRIDGE_DEPENDENCY_CHECK
    // Keep the production event loop type-checked, but reject before enumeration.
    if (seen == 31) throw std::runtime_error("dependency-check-only build: official tap-windows.h unavailable; no device access permitted");
#endif
    WinBackend backend(devicePath(instance), guid);
    auto owner = std::make_unique<Engine<WinBackend>>(backend, id);
    auto& engine = *owner;
    stopEvent = backend.stop.value;
    if (!SetConsoleCtrlHandler(stopHandler, TRUE)) { stopEvent = nullptr; throw WinError("console handler"); }
    struct HandlerScope {
        ~HandlerScope() { SetConsoleCtrlHandler(stopHandler, FALSE); stopEvent = nullptr; }
    } handlerScope;
    const bool began = engine.begin();
    if (began) {
        // Identity only. No packet bytes or authentication material are emitted.
        std::wprintf(L"device_instance=%ls tap_guid=%ls session=%llu generation=%llu epoch=%llu\n",
            instance.c_str(), guid.c_str(), id.session, id.generation, id.epoch);
        while (WaitForSingleObject(backend.stop.value, 0) == WAIT_TIMEOUT && engine.tick(GetTickCount64()))
            backend.wait(engine.ops, engine.nextTx, engine.toNet.count != 0);
    }
    engine.stop();
    const auto& s = engine.stats;
    std::printf("tap_read_frames=%llu tap_read_bytes=%llu tx_accepted=%llu tx_bytes=%llu rx_frames=%llu rx_bytes=%llu "
        "tap_written=%llu tap_written_bytes=%llu short_drops=%llu tx_busy=%llu uncertain_tx=%llu shutdown_drops=%llu errors=%llu\n",
        s.tapReadFrames, s.tapReadBytes, s.txAccepted, s.txBytes, s.rxFrames, s.rxBytes,
        s.tapWritten, s.tapWrittenBytes, s.shortDrops, s.busy, s.uncertainTx, s.shutdownDrops, s.errors);
    return began && !s.errors ? 0 : 1;
}
} // namespace bridge
int wmain(int argc, wchar_t** argv) {
    try { return bridge::run(argc, argv); }
    catch (const bridge::WinError& e) { std::fprintf(stderr, "bridge error: %s win32=%lu\n", e.what(), e.code); }
    catch (const std::exception& e) { std::fprintf(stderr, "bridge error: %s\n", e.what()); }
    return 1;
}
#endif
