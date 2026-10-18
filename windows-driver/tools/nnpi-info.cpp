/* SPDX-License-Identifier: GPL-2.0-only */
// Windows 11 x64, C++17, Windows SDK; link SetupAPI.lib/Bcrypt.lib (no WDK required).
// Example (Developer Command Prompt, from tools):
// cl /nologo /std:c++17 /EHsc /W4 nnpi-info.cpp /link SetupAPI.lib
// No arguments or "list": enumerate only. "info --index N": explicit selection.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <bcrypt.h>
#include <initguid.h> // Instantiate the shared interface GUID in this TU only.
#include "../include/nnpi_interface.h"
#include "../include/nnpi_info_abi.h"
#include "../include/nnpi_diag_abi.h"
#include "../include/nnpi_status_abi.h"
#include "../include/nnpi_snapshot_abi.h"
#include "../include/nnpi_dmacheck_abi.h"
#include "../include/nnpi_resetcaps_abi.h"
#include "../include/nnpi_reset_abi.h"
#include "../include/nnpi_hostready.h"
#include "../include/nnpi_sysinfo_abi.h"
#include "../include/nnpi_boot_abi.h"
#include "../include/nnpi_boot_session.h"
#include "../include/nnpi_net_abi.h"
#include "../include/nnpi_runtime_info_abi.h"
#include "../include/nnpi_net_data_abi.h"
#include <cstring>
#include <iomanip>
#include <algorithm>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <limits>
#include <locale>
#include <memory>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "SetupAPI.lib")
#pragma comment(lib, "Bcrypt.lib")

static_assert(sizeof(nnpi_boot_request) == 64, "BOOT request ABI mismatch");
static_assert(sizeof(nnpi_boot_response) == 192, "BOOT response ABI mismatch");
static_assert(NNPI_BOOT_IMAGE_BYTES == UINT64_C(188744192), "Unexpected boot image size");
static_assert(NNPI_BOOT_CHUNK_MAX == 1048576U, "Unexpected boot chunk size");

static_assert(sizeof(nnpi_net_request) == 64, "NET request ABI mismatch");
static_assert(sizeof(nnpi_net_response) == 128, "NET response ABI mismatch");
static_assert(sizeof(nnpi_runtime_info_request) == 16, "RUNTIME INFO request ABI mismatch");
static_assert(sizeof(nnpi_runtime_info_response) == 128, "RUNTIME INFO response ABI mismatch");
static_assert(NNPI_RUNTIME_INFO_IOCTL == 0x80006050U, "RUNTIME INFO must remain buffered/read-only function 0x814");

static_assert(sizeof(void*) == 8, "Build this tool for Windows x64");
static_assert(sizeof(nnpi_info_request) == 16, "Request ABI mismatch");
static_assert(sizeof(nnpi_info_response) == 576, "Response ABI mismatch");

namespace {
constexpr DWORD kWaitMilliseconds = 5000;

class Handle {
public:
    explicit Handle(HANDLE value) noexcept : value_(value) {}
    ~Handle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const noexcept { return value_; }
private:
    HANDLE value_;
};

class DeviceInfoSet {
public:
    explicit DeviceInfoSet(HDEVINFO value) noexcept : value_(value) {}
    ~DeviceInfoSet() {
        if (value_ != INVALID_HANDLE_VALUE)
            SetupDiDestroyDeviceInfoList(value_);
    }
    DeviceInfoSet(const DeviceInfoSet&) = delete;
    DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;
    HDEVINFO get() const noexcept { return value_; }
private:
    HDEVINFO value_;
};

struct Win32Failure {
    const char* operation;
    DWORD code;
};

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        throw std::runtime_error("UTF-16 string is too long");
    const int length = static_cast<int>(text.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text.data(), length, nullptr, 0, nullptr, nullptr);
    if (!bytes) throw Win32Failure{"WideCharToMultiByte(size)", GetLastError()};
    std::string result(static_cast<size_t>(bytes), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length,
            result.data(), bytes, nullptr, nullptr))
        throw Win32Failure{"WideCharToMultiByte(convert)", GetLastError()};
    return result;
}

// Input is UTF-8, either converted strictly from UTF-16 or an ASCII literal.
std::string jsonString(const std::string& text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 0x20) {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else {
            result += static_cast<char>(c);
        }
    }
    result += '"';
    return result;
}

std::string win32Message(DWORD code) {
    wchar_t buffer[2048]{};
    const DWORD count = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buffer,
        static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])), nullptr);
    if (!count) return "Windows did not provide a message for this error code";
    return utf8(std::wstring(buffer, count));
}

void appendWin32(std::ostream& out, const char* operation, DWORD code) {
    out << "{\"operation\":" << jsonString(operation)
        << ",\"win32_code\":" << code
        << ",\"message\":" << jsonString(win32Message(code)) << '}';
}

void appendScope(std::ostream& out) {
    out << ",\"data_scope\":\"cache-only\""
        << ",\"hardware_boot_verified\":false,\"compute_verified\":false"
        << ",\"resource_index_semantics\":\"translated resource index, not a PCI BAR index\"";
}

int emit(const std::string& document, int exitCode) {
    // Write UTF-8 bytes without changing the console code page or other settings.
    const bool written = std::fwrite(document.data(), 1, document.size(), stdout)
        == document.size();
    if (!written || std::fputc('\n', stdout) == EOF || std::fflush(stdout) != 0) {
        std::fputs("{\"ok\":false,\"error\":\"stdout write/flush failed\"}\n", stderr);
        return 1;
    }
    return exitCode;
}

int error(const char* kind, const char* message, int exitCode) {
    std::ostringstream out;
    out << "{\"ok\":false,\"error\":{\"kind\":" << jsonString(kind)
        << ",\"message\":" << jsonString(message) << '}';
    appendScope(out);
    out << '}';
    return emit(out.str(), exitCode);
}

std::vector<std::wstring> enumerateInterfaces() {
    const DeviceInfoSet devices(SetupDiGetClassDevsW(&GUID_DEVINTERFACE_NNPI_INFO,
        nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (devices.get() == INVALID_HANDLE_VALUE)
        throw Win32Failure{"SetupDiGetClassDevsW", GetLastError()};

    std::vector<std::wstring> paths;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(devices.get(), nullptr,
                &GUID_DEVINTERFACE_NNPI_INFO, index, &interfaceData)) {
            const DWORD code = GetLastError();
            if (code == ERROR_NO_MORE_ITEMS) break;
            throw Win32Failure{"SetupDiEnumDeviceInterfaces", code};
        }
        DWORD required = 0;
        const BOOL sized = SetupDiGetDeviceInterfaceDetailW(devices.get(),
            &interfaceData, nullptr, 0, &required, nullptr);
        const DWORD sizeError = sized ? ERROR_SUCCESS : GetLastError();
        if (sized || sizeError != ERROR_INSUFFICIENT_BUFFER)
            throw Win32Failure{"SetupDiGetDeviceInterfaceDetailW(size)",
                sized ? ERROR_INVALID_DATA : sizeError};
        if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
            throw Win32Failure{"SetupDiGetDeviceInterfaceDetailW(size)", ERROR_INVALID_DATA};

        // malloc supplies the alignment required by the variable-sized SDK struct.
        std::unique_ptr<SP_DEVICE_INTERFACE_DETAIL_DATA_W, decltype(&std::free)> detail(
            static_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(std::malloc(required)),
            &std::free);
        if (!detail) throw std::bad_alloc();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        DWORD returned = 0;
        if (!SetupDiGetDeviceInterfaceDetailW(devices.get(), &interfaceData,
                detail.get(), required, &returned, nullptr))
            throw Win32Failure{"SetupDiGetDeviceInterfaceDetailW", GetLastError()};
        const size_t offset = offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath);
        if (returned > required || returned < offset + sizeof(wchar_t))
            throw Win32Failure{"SetupDiGetDeviceInterfaceDetailW(length)", ERROR_INVALID_DATA};
        const size_t chars = (returned - offset) / sizeof(wchar_t);
        const wchar_t* end = std::wmemchr(detail->DevicePath, L'\0', chars);
        if (!end || end == detail->DevicePath)
            throw Win32Failure{"SetupDiGetDeviceInterfaceDetailW(path)", ERROR_INVALID_DATA};
        paths.emplace_back(detail->DevicePath,
            static_cast<size_t>(end - detail->DevicePath));
        if (index == (std::numeric_limits<DWORD>::max)())
            throw Win32Failure{"SetupDiEnumDeviceInterfaces(index overflow)", ERROR_ARITHMETIC_OVERFLOW};
    }
    return paths;
}

template<typename Response>
struct BasicQueryResult {
    Response response{};
    DWORD bytes = 0;
    DWORD code = ERROR_SUCCESS;
    const char* operation = "DeviceIoControl";
    bool timedOut = false;
    bool cancelled = false;
    DWORD cancelCode = ERROR_SUCCESS;
    DWORD completionCode = ERROR_SUCCESS;
};

using QueryResult = BasicQueryResult<nnpi_info_response>;
using DiagnosticResult = BasicQueryResult<nnpi_diag_response>;

template<typename Response>
void cancelAndDrain(HANDLE device, OVERLAPPED& overlapped, BasicQueryResult<Response>& result) noexcept {
    result.cancelled = true;
    if (!CancelIoEx(device, &overlapped)) result.cancelCode = GetLastError();
    // ERROR_NOT_FOUND can mean completion won the cancellation race. Still drain.
    // No exception, buffer destruction or handle close is allowed before completion.
    // This wait can exceed five seconds; cancellation is NOT a hard real-time bound.
    for (;;) {
        if (GetOverlappedResult(device, &overlapped, &result.bytes, TRUE)) {
            result.completionCode = ERROR_SUCCESS;
            return;
        }
        result.completionCode = GetLastError();
        if (result.completionCode != ERROR_IO_INCOMPLETE &&
            result.completionCode != ERROR_IO_PENDING) return;
        // Defensive: keep the live request's storage even if not yet completed.
        Sleep(1);
    }
}

template<typename Response>
BasicQueryResult<Response> queryBuffer(HANDLE device, DWORD ioctl, void* request, DWORD requestBytes) {
    BasicQueryResult<Response> result;
    const Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.get()) throw Win32Failure{"CreateEventW", GetLastError()};
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    // Exactly one IOCTL; no probe/retry/fallback. All storage stays live through drain.
    const BOOL completed = DeviceIoControl(device, ioctl,
        request, requestBytes,
        &result.response, static_cast<DWORD>(sizeof(result.response)),
        &result.bytes, &overlapped);
    if (completed) return result;
    result.code = GetLastError();
    if (result.code != ERROR_IO_PENDING) return result;

    const DWORD wait = WaitForSingleObject(event.get(), kWaitMilliseconds);
    if (wait == WAIT_TIMEOUT) {
        result.timedOut = true;
        result.code = ERROR_TIMEOUT;
        result.operation = "WaitForSingleObject";
        cancelAndDrain(device, overlapped, result);
        // Never replace the timeout with a racing successful completion.
        return result;
    }
    if (wait != WAIT_OBJECT_0) {
        result.code = wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
        result.operation = "WaitForSingleObject";
        cancelAndDrain(device, overlapped, result);
        return result;
    }
    result.operation = "GetOverlappedResult";
    if (GetOverlappedResult(device, &overlapped, &result.bytes, FALSE)) {
        result.code = ERROR_SUCCESS;
    } else {
        result.code = GetLastError();
        if (result.code == ERROR_IO_INCOMPLETE || result.code == ERROR_IO_PENDING)
            cancelAndDrain(device, overlapped, result);
    }
    return result;
}

template<typename Response, typename Request>
BasicQueryResult<Response> query(HANDLE device, DWORD ioctl, uint32_t version, const Request* supplied = nullptr) {
    Request request{};
    request.version = version;
    request.size = static_cast<uint32_t>(sizeof(request));
    if (supplied) request = *supplied;
    return queryBuffer<Response>(device, ioctl, &request, static_cast<DWORD>(sizeof(request)));
}

const char* resourceType(uint32_t type) {
    switch (type) {
    case NNPI_INFO_RESOURCE_PORT: return "port";
    case NNPI_INFO_RESOURCE_INTERRUPT: return "interrupt";
    case NNPI_INFO_RESOURCE_MEMORY: return "memory";
    case NNPI_INFO_RESOURCE_DEVICE_PRIVATE: return "device-private";
    default: return "unknown"; // Unreachable after shared validation.
    }
}

std::string hex64(uint64_t value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "0x" << std::hex << value;
    return out.str();
}

int runDiagnostic(HANDLE device, size_t index, const std::string& path) {
    const DiagnosticResult result = query<nnpi_diag_response, nnpi_diag_request>(
        device, NNPI_DIAG_IOCTL, NNPI_DIAG_VERSION);
    const bool valid = result.code == ERROR_SUCCESS &&
        nnpi_diag_response_valid(&result.response, result.bytes);
    const bool associated = valid && result.response.reason == NNPI_BAR_OK;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"ok\":" << (associated ? "true" : "false")
        << ",\"command\":\"diag\",\"index\":" << index
        << ",\"interface_path\":" << jsonString(path)
        << ",\"data_scope\":\"prepare-time-pci-config-and-resource-cache\""
        << ",\"config_source\":\"BUS_INTERFACE_STANDARD.GetBusData\""
        << ",\"mmio_enabled\":false,\"status_read_supported\":false"
        << ",\"boot_supported\":false,\"ssh_verified\":false"
        << ",\"hardware_boot_verified\":false,\"compute_verified\":false"
        << ",\"capability_scope\":\"these legacy booleans describe diag cache-only behavior, not driver-wide capability\""
        << ",\"status_command\":\"explicit status --index N; availability checked by separate IOCTL\"";
    if (result.code != ERROR_SUCCESS) {
        out << ",\"error\":";
        appendWin32(out, result.operation, result.code);
        out << ",\"timed_out\":" << (result.timedOut ? "true" : "false");
        if (result.cancelled) {
            out << ",\"cancel_result\":";
            appendWin32(out, "CancelIoEx", result.cancelCode);
            out << ",\"final_completion\":";
            appendWin32(out, "GetOverlappedResult(wait=TRUE)", result.completionCode);
            out << ",\"timeout_note\":\"5000 ms initial wait; cancellation drain may take longer; no hard real-time guarantee\"";
        }
    } else if (!valid) {
        out << ",\"error\":{\"kind\":\"invalid_response\",\"validator\":\"nnpi_diag_response_valid\","
            << "\"bytes_returned\":" << result.bytes << ",\"expected_bytes\":416}";
    } else {
        const auto& r = result.response;
        out << ",\"version\":" << r.version << ",\"size\":" << r.size
            << ",\"flags\":" << r.flags
            << ",\"generation\":" << jsonString(std::to_string(r.generation))
            << ",\"association_ok\":" << (associated ? "true" : "false")
            << ",\"reason\":" << r.reason << ",\"reason_name\":" << jsonString(nnpi_bar_reason(r.reason))
            << ",\"pci_status\":" << jsonString(hex64(r.pci_status))
            << ",\"config_bytes\":" << r.config_bytes
            << ",\"raw_count\":" << r.raw_count << ",\"translated_count\":" << r.translated_count;
        std::string config;
        constexpr char hex[] = "0123456789abcdef";
        for (uint32_t i = 0; i < r.config_bytes; ++i) {
            config += hex[r.config[i] >> 4]; config += hex[r.config[i] & 15];
        }
        out << ",\"pci_config_header_hex\":" << jsonString(config)
            << ",\"bar_count\":" << r.bar_count << ",\"bars\":[";
        for (uint32_t i = 0; i < r.bar_count; ++i) {
            const auto& b = r.bars[i];
            if (i) out << ',';
            out << "{\"bar\":" << b.bar << ",\"width\":" << b.width
                << ",\"type\":" << jsonString(b.type == NNPI_BAR_MEMORY ? "memory" : "port")
                << ",\"prefetch\":" << b.prefetch
                << ",\"raw_index\":" << b.raw_index << ",\"translated_index\":" << b.translated_index
                << ",\"bus_start\":" << jsonString(hex64(b.bus_start))
                << ",\"translated_start\":" << jsonString(hex64(b.translated_start))
                << ",\"length\":" << jsonString(hex64(b.length)) << '}';
        }
        out << ']';
    }
    out << '}';
    // Exit 0 means association diagnostics passed, NEVER MMIO/boot/SSH success.
    return emit(out.str(), associated ? 0 : 1);
}

int runStatus(HANDLE device, size_t index, const std::string& path) {
    const auto result = query<nnpi_status_response, nnpi_status_request>(
        device, NNPI_STATUS_IOCTL, NNPI_STATUS_VERSION);
    const bool valid = result.code == ERROR_SUCCESS &&
        nnpi_status_response_valid(&result.response, result.bytes);
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"ok\":" << (valid ? "true" : "false")
        << ",\"command\":\"status\",\"index\":" << index
        << ",\"interface_path\":" << jsonString(path)
        << ",\"valid\":" << (valid ? "true" : "false")
        << ",\"data_scope\":\"explicit-single-CPU_STATUS0-read\""
        << ",\"hardware_boot_verified\":false,\"compute_verified\":false,\"ssh_verified\":false"
        << ",\"interpretation\":\"raw data only; zero/all-ones do not establish health or boot success\""
        << ",\"timeout_note\":\"5000 ms initial wait; cancellation drain can wait indefinitely for a stalled PCIe read; no hard timeout\"";
    if (result.code != ERROR_SUCCESS) {
        out << ",\"error\":";
        appendWin32(out, result.operation, result.code);
        out << ",\"timed_out\":" << (result.timedOut ? "true" : "false");
        if (result.cancelled) {
            out << ",\"cancel_result\":";
            appendWin32(out, "CancelIoEx", result.cancelCode);
            out << ",\"final_completion\":";
            appendWin32(out, "GetOverlappedResult(wait=TRUE)", result.completionCode);
        }
        out << ",\"read_performed\":null"; // Failed/timeout requests do not prove zero reads.
    } else if (!valid) {
        out << ",\"error\":{\"kind\":\"invalid_response\",\"validator\":\"nnpi_status_response_valid\","
            << "\"bytes_returned\":" << result.bytes << ",\"expected_bytes\":48}";
    } else {
        const auto& r = result.response;
        std::ostringstream raw;
        raw.imbue(std::locale::classic());
        raw << "0x" << std::hex << std::setw(8) << std::setfill('0') << r.value;
        out << ",\"version\":" << r.version << ",\"size\":" << r.size
            << ",\"generation\":" << jsonString(std::to_string(r.generation))
            << ",\"cpu_status0\":" << jsonString(raw.str())
            << ",\"attributes\":" << r.attributes << ",\"read_count\":" << r.read_count
            << ",\"read_performed\":true,\"mmio\":true,\"bar\":0,\"offset\":\"0x1b8\",\"width_bits\":32"
            << ",\"mapping_read_only\":true,\"noncached\":true,\"executable\":false";
    }
    out << '}';
    return emit(out.str(), valid ? 0 : 1);
}

void appendSnapshotFifo(std::ostream& out, uint32_t raw, bool valid,
                        uint32_t used, uint32_t free) {
    out << "{\"raw\":" << jsonString(hex64(raw))
        << ",\"valid\":" << (valid ? "true" : "false")
        << ",\"read_pointer\":" << (raw & NNPI_FIFO_READ_MASK)
        << ",\"write_pointer\":" << ((raw & NNPI_FIFO_WRITE_MASK) >> 8)
        << ",\"used_qwords\":";
    if (valid) out << used; else out << "null";
    out << ",\"free_qwords\":";
    if (valid) out << free; else out << "null";
    out << '}';
}

int runSnapshot(HANDLE device, size_t index, const std::string& path) {
    const auto result = query<nnpi_snapshot_response, nnpi_snapshot_request>(
        device, NNPI_SNAPSHOT_IOCTL, NNPI_SNAPSHOT_VERSION);
    const bool valid = result.code == ERROR_SUCCESS &&
        nnpi_snapshot_response_valid(&result.response, result.bytes);
    const bool fifoValid = valid && result.response.fifo_valid == NNPI_SNAPSHOT_FIFO_VALID;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"ok\":" << (fifoValid ? "true" : "false")
        << ",\"command\":\"snapshot\",\"index\":" << index
        << ",\"interface_path\":" << jsonString(path)
        << ",\"valid\":" << (valid ? "true" : "false")
        << ",\"data_scope\":\"explicit-fixed-control-snapshot\""
        << ",\"atomic\":false,\"doorbell_read_may_notify_peer\":true"
        << ",\"read_order\":[\"0x044\",\"0x060\",\"0x038\"]"
        << ",\"hardware_boot_verified\":false,\"compute_verified\":false,\"ssh_verified\":false"
        << ",\"interpretation\":\"raw boot/error numbers; no boot-success inference; no command sent\""
        << ",\"timeout_note\":\"5000 ms initial wait; cancellation drain can wait indefinitely for stalled PCIe; no hard timeout\"";
    if (result.code != ERROR_SUCCESS) {
        out << ",\"error\":";
        appendWin32(out, result.operation, result.code);
        out << ",\"timed_out\":" << (result.timedOut ? "true" : "false");
        if (result.cancelled) {
            out << ",\"cancel_result\":";
            appendWin32(out, "CancelIoEx", result.cancelCode);
            out << ",\"final_completion\":";
            appendWin32(out, "GetOverlappedResult(wait=TRUE)", result.completionCode);
        }
        out << ",\"read_performed\":null";
    } else if (!valid) {
        out << ",\"error\":{\"kind\":\"invalid_response\",\"validator\":\"nnpi_snapshot_response_valid\","
            << "\"bytes_returned\":" << result.bytes << ",\"expected_bytes\":80}";
    } else {
        const auto& r = result.response;
        if (!fifoValid) out << ",\"error\":{\"kind\":\"invalid_fifo_state\"}";
        out << ",\"version\":" << r.version << ",\"size\":" << r.size
            << ",\"generation\":" << jsonString(std::to_string(r.generation))
            << ",\"attributes\":" << r.attributes << ",\"read_count\":" << r.read_count
            << ",\"raw_valid\":" << r.raw_valid << ",\"fifo_valid\":" << r.fifo_valid
            << ",\"read_performed\":true,\"mmio\":true,\"bar\":0,\"width_bits\":32"
            << ",\"mapping_offset\":\"0x038\",\"mapping_bytes\":44"
            << ",\"mapping_read_only\":true,\"noncached\":true,\"executable\":false"
            << ",\"command_fifo\":";
        appendSnapshotFifo(out, r.command_control, (r.fifo_valid & NNPI_SNAPSHOT_COMMAND_VALID) != 0,
                           r.command_used, r.command_free);
        out << ",\"response_fifo\":";
        appendSnapshotFifo(out, r.response_control, (r.fifo_valid & NNPI_SNAPSHOT_RESPONSE_VALID) != 0,
                           r.response_used, r.response_free);
        out << ",\"card_doorbell\":" << jsonString(hex64(r.card_doorbell))
            << ",\"boot_state\":" << r.boot_state << ",\"error_code\":" << r.error_code;
    }
    out << '}';
    return emit(out.str(), fifoValid ? 0 : 1);
}

int runDmaCheck(HANDLE device, size_t index, const std::string& path) {
    const auto result = query<nnpi_dmacheck_response, nnpi_dmacheck_request>(
        device, NNPI_DMACHECK_IOCTL, NNPI_DMACHECK_VERSION);
    const bool valid = result.code == ERROR_SUCCESS &&
        nnpi_dmacheck_response_valid(&result.response, result.bytes);
    const bool passed = valid && result.response.passed == 1;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"ok\":" << (passed ? "true" : "false")
        << ",\"command\":\"dmacheck\",\"index\":" << index
        << ",\"interface_path\":" << jsonString(path)
        << ",\"valid\":" << (valid ? "true" : "false")
        << ",\"data_scope\":\"prepare-or-explicit-reset-revalidate-common-buffer-CPU-self-test-cache\""
        << ",\"dma_transfer_verified\":false,\"hardware_boot_verified\":false,\"compute_verified\":false,\"ssh_verified\":false"
        << ",\"profile_basis\":\"Linux 64-bit DMA mask; not device DMA validation\""
        << ",\"interpretation\":\"WDF allocation and CPU readback only; address never published, no DMA transaction or SYSINFO request\""
        << ",\"platform_note\":\"WDF/OS adapter management is not claimed to have zero platform side effects\""
        << ",\"timeout_note\":\"5000 ms initial wait; cancellation drain may take longer; no hard timeout\"";
    if (result.code != ERROR_SUCCESS) {
        out << ",\"error\":";
        appendWin32(out, result.operation, result.code);
        out << ",\"timed_out\":" << (result.timedOut ? "true" : "false");
        if (result.cancelled) {
            out << ",\"cancel_result\":";
            appendWin32(out, "CancelIoEx", result.cancelCode);
            out << ",\"final_completion\":";
            appendWin32(out, "GetOverlappedResult(wait=TRUE)", result.completionCode);
        }
    } else if (!valid) {
        out << ",\"error\":{\"kind\":\"invalid_response\",\"validator\":\"nnpi_dmacheck_response_valid\","
            << "\"bytes_returned\":" << result.bytes << ",\"expected_bytes\":64}";
    } else {
        const auto& r = result.response;
        if (!passed) out << ",\"error\":{\"kind\":\"dma_preparation_failed\"}";
        out << ",\"version\":" << r.version << ",\"size\":" << r.size
            << ",\"generation\":" << jsonString(std::to_string(r.generation))
            << ",\"actual_bytes\":" << jsonString(std::to_string(r.actual_bytes))
            << ",\"attributes\":" << r.attributes << ",\"profile\":\"ScatterGather64\""
            << ",\"requested_bytes\":" << r.requested_bytes << ",\"alignment_mask\":" << r.alignment_mask
            << ",\"checks\":" << r.checks << ",\"stage\":" << r.stage
            << ",\"stage_name\":" << jsonString(nnpi_dmacheck_stage_name(r.stage))
            << ",\"ntstatus\":" << jsonString(hex64(r.ntstatus))
            << ",\"passed\":" << (passed ? "true" : "false")
            << ",\"buffer_retained\":" << (r.retained ? "true" : "false");
    }
    out << '}';
    return emit(out.str(), passed ? 0 : 1);
}

int runResetCaps(HANDLE device, size_t index, const std::string& path) {
    const auto result = query<nnpi_resetcaps_response, nnpi_resetcaps_request>(
        device, NNPI_RESETCAPS_IOCTL, NNPI_RESETCAPS_VERSION);
    const bool valid = result.code == ERROR_SUCCESS &&
        nnpi_resetcaps_response_valid(&result.response, result.bytes);
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"ok\":" << (valid ? "true" : "false")
        << ",\"command\":\"resetcaps\",\"index\":" << index
        << ",\"interface_path\":" << jsonString(path)
        << ",\"valid\":" << (valid ? "true" : "false")
        << ",\"data_scope\":\"prepare-time-device-reset-interface-base-capability-cache\""
        << ",\"reset_executed\":false,\"reset_status_called\":false,\"dma_address_published\":false"
        << ",\"hardware_boot_verified\":false,\"compute_verified\":false,\"ssh_verified\":false"
        << ",\"interpretation\":\"absence is valid diagnostic data; supported means base contract, handler and known advertised type only, not a tested reset or SYSINFO release guarantee\""
        << ",\"timeout_note\":\"5000 ms initial wait; cancellation drain may take longer; no hard timeout\"";
    if (result.code != ERROR_SUCCESS) {
        out << ",\"error\":";
        appendWin32(out, result.operation, result.code);
        out << ",\"timed_out\":" << (result.timedOut ? "true" : "false");
        if (result.cancelled) {
            out << ",\"cancel_result\":";
            appendWin32(out, "CancelIoEx", result.cancelCode);
            out << ",\"final_completion\":";
            appendWin32(out, "GetOverlappedResult(wait=TRUE)", result.completionCode);
        }
    } else if (!valid) {
        out << ",\"error\":{\"kind\":\"invalid_response\",\"validator\":\"nnpi_resetcaps_response_valid\","
            << "\"bytes_returned\":" << result.bytes << ",\"expected_bytes\":64}";
    } else {
        const auto& r = result.response;
        out << ",\"version\":" << r.version << ",\"size\":" << r.size
            << ",\"generation\":" << jsonString(std::to_string(r.generation))
            << ",\"requested_size\":" << r.requested_size
            << ",\"requested_version\":" << r.requested_version
            << ",\"query_ntstatus\":" << jsonString(hex64(r.query_ntstatus))
            << ",\"returned_size\":" << r.returned_size
            << ",\"returned_version\":" << r.returned_version
            << ",\"supported_reset_types_raw\":" << jsonString(hex64(r.supported_reset_types))
            << ",\"supported\":" << (r.supported ? "true" : "false")
            << ",\"handler_present\":" << (r.handler_present ? "true" : "false")
            << ",\"contract_valid\":" << (r.contract_valid ? "true" : "false")
            << ",\"function_level_supported\":" << ((r.supported && (r.supported_reset_types & NNPI_RESETCAPS_FUNCTION_LEVEL)) ? "true" : "false")
            << ",\"platform_level_supported\":" << ((r.supported && (r.supported_reset_types & NNPI_RESETCAPS_PLATFORM_LEVEL)) ? "true" : "false")
            << ",\"function_level_advertised\":" << ((r.supported_reset_types & NNPI_RESETCAPS_FUNCTION_LEVEL) ? "true" : "false")
            << ",\"platform_level_advertised\":" << ((r.supported_reset_types & NNPI_RESETCAPS_PLATFORM_LEVEL) ? "true" : "false");
    }
    out << '}';
    return emit(out.str(), valid ? 0 : 1);
}

int runReset(HANDLE device, const char* command, uint64_t generation, uint64_t epoch) {
    nnpi_reset_request request{};
    request.version=NNPI_RESET_VERSION; request.size=sizeof(request);
    const bool observe=std::string(command)=="reset-query";
    request.generation=generation; request.epoch=epoch;
    request.confirmation=observe ? 0 : NNPI_RESET_CONFIRM;
    const DWORD code=observe ? NNPI_RESET_QUERY_IOCTL :
        std::string(command)=="reset-submit" ? NNPI_RESET_SUBMIT_IOCTL : NNPI_RESET_REVALIDATE_IOCTL;
    const auto result=query<nnpi_reset_response, nnpi_reset_request>(device, code, NNPI_RESET_VERSION, &request);
    const bool valid=result.code==ERROR_SUCCESS && nnpi_reset_response_valid(&result.response,result.bytes);
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"ok\":" << (valid ? "true" : "false") << ",\"command\":" << jsonString(command)
        << ",\"hardware_tested\":false,\"observation_only_timeout\":true,\"retry_allowed\":false";
    if (!valid) {
        out << ",\"error\":"; appendWin32(out,result.operation,result.code);
        out << ",\"valid_response\":false,\"bytes_returned\":" << result.bytes
            << ",\"timed_out\":" << (result.timedOut ? "true" : "false")
            << ",\"submit_may_have_executed\":" << (observe ? "false" : "true");
    } else {
        const auto& r=result.response;
        out << ",\"operation_id\":" << jsonString(std::to_string(r.operation_id))
            << ",\"generation\":" << jsonString(std::to_string(r.generation))
            << ",\"epoch\":" << jsonString(std::to_string(r.epoch))
            << ",\"state\":" << r.state << ",\"call_returned\":" << r.call_returned
            << ",\"call_status\":" << jsonString(hex64(r.call_status))
            << ",\"callback_seen\":" << r.callback_seen
            << ",\"completion_status\":" << jsonString(hex64(r.completion_status))
            << ",\"callback_count\":" << r.callback_count
            << ",\"epoch_invalidated\":" << r.epoch_invalidated
            << ",\"revalidated\":" << r.revalidated << ",\"hardware_blocked\":" << r.hardware_blocked
            << ",\"submitted_100ns\":" << jsonString(std::to_string(r.submitted_100ns))
            << ",\"returned_100ns\":" << jsonString(std::to_string(r.returned_100ns))
            << ",\"completed_100ns\":" << jsonString(std::to_string(r.completed_100ns))
            << ",\"post_cpu_selftest_fresh\":" << (r.revalidated ? "true" : "false");
    }
    out << '}'; return emit(out.str(),valid ? 0 : 1);
}

int runHostReady(HANDLE device, const char* command, uint64_t generation, uint64_t epoch) {
    nnpi_hostready_request request{};
    request.version=1; request.size=sizeof(request); request.generation=generation;
    request.epoch=epoch; request.confirmation=NNPI_HOSTREADY_CONFIRM;
    const bool observe=std::string(command)=="host-ready-observe";
    const auto result=query<nnpi_hostready_response,nnpi_hostready_request>(device,
        observe ? NNPI_HOSTREADY_OBSERVE_IOCTL : NNPI_HOSTREADY_SUBMIT_IOCTL,1,&request);
    const bool valid=result.code==ERROR_SUCCESS &&
        nnpi_hostready_reply_matches(&result.response,result.bytes,&request,observe);
    std::ostringstream out;
    out << "{\"ok\":" << (valid ? "true" : "false") << ",\"command\":" << jsonString(command)
        << ",\"dma_address_published\":false,\"reset_executed\":false,\"retry_allowed\":false";
    if (!valid) {
        out << ",\"valid_response\":false,\"bytes_returned\":" << result.bytes
            << ",\"timed_out\":" << (result.timedOut ? "true" : "false")
            << ",\"write_may_have_executed\":" << (observe ? "false" : "true")
            << ",\"error\":";
        appendWin32(out,result.operation,result.code);
    } else {
        const auto& r=result.response;
        out << ",\"generation\":" << jsonString(std::to_string(r.generation))
            << ",\"epoch\":" << jsonString(std::to_string(r.epoch))
            << ",\"phase\":" << r.phase << ",\"hardware_write_count\":" << r.writes
            << ",\"observations\":" << r.observations << ",\"reads\":" << r.reads
            << ",\"host_before\":" << r.host_before << ",\"command_before\":" << r.command_before
            << ",\"response_before\":" << r.response_before << ",\"card_before\":" << r.card_before
            << ",\"card_last\":" << r.card_last;
    }
    out << '}'; return emit(out.str(),valid ? 0 : 1);
}

int runSysinfo(HANDLE device, const char* command, size_t index, const std::string& path,
               uint64_t generation, uint64_t epoch) {
    const bool submit=std::string(command)=="sysinfo-submit";
    nnpi_sysinfo_request request{};
    request.version=NNPI_SYSINFO_VERSION; request.size=sizeof(request);
    if (submit) {
        request.generation=generation; request.epoch=epoch;
        request.confirmation=NNPI_SYSINFO_CONFIRM;
    } // QUERY is strictly zero generation/epoch/confirmation, cache only.
    const auto result=query<nnpi_sysinfo_response,nnpi_sysinfo_request>(device,
        submit ? NNPI_SYSINFO_SUBMIT_IOCTL : NNPI_SYSINFO_QUERY_IOCTL,NNPI_SYSINFO_VERSION,&request);
    const bool valid=result.code==ERROR_SUCCESS &&
        nnpi_sysinfo_response_valid(&result.response,result.bytes) &&
        (!submit || (result.response.generation==generation && result.response.epoch==epoch));
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"ok\":" << (valid ? "true" : "false") << ",\"command\":" << jsonString(command)
        << ",\"index\":" << index << ",\"interface_path\":" << jsonString(path)
        << ",\"cache_only\":" << (submit ? "false" : "true")
        << ",\"retry_allowed\":false,\"driver_automatic_reset_cleanup\":true"
        << ",\"hardware_boot_verified\":false,\"compute_verified\":false,\"ssh_verified\":false"
        << ",\"timeout_note\":\"5000 ms initial IOCTL wait; cancellation drain may take longer; not a hard deadline\"";
    if (!valid) {
        out << ",\"valid_response\":false,\"bytes_returned\":" << result.bytes
            << ",\"timed_out\":" << (result.timedOut ? "true" : "false")
            << ",\"submit_may_have_executed\":" << (submit ? "true" : "false") << ",\"error\":";
        if (result.code!=ERROR_SUCCESS) appendWin32(out,result.operation,result.code);
        else out << "{\"kind\":\"invalid_response\",\"validator\":\"nnpi_sysinfo_response_valid/submit identity\"}";
        if (result.cancelled) {
            out << ",\"cancel_result\":"; appendWin32(out,"CancelIoEx",result.cancelCode);
            out << ",\"final_completion\":"; appendWin32(out,"GetOverlappedResult(wait=TRUE)",result.completionCode);
        }
    } else {
        const auto& r=result.response;
        out << ",\"version\":" << r.version << ",\"size\":" << r.size
            << ",\"publication_id\":" << jsonString(std::to_string(r.publication_id))
            << ",\"generation\":" << jsonString(std::to_string(r.generation))
            << ",\"epoch\":" << jsonString(std::to_string(r.epoch))
            << ",\"phase\":" << r.phase << ",\"ownership\":" << r.ownership
            << ",\"writes_attempted\":" << r.writes_attempted << ",\"observations\":" << r.observations
            << ",\"ready_seen\":" << r.ready_seen << ",\"payload_valid\":" << r.payload_valid
            << ",\"parse_result\":" << r.parse_result << ",\"epoch_invalidated\":" << r.epoch_invalidated
            << ",\"budget_consumed\":" << r.budget_consumed << ",\"cleanup_reserved\":" << r.cleanup_reserved
            << ",\"page_retained\":" << r.page_retained << ",\"timed_out\":" << r.timed_out
            << ",\"card_before\":" << r.card_before << ",\"card_last\":" << r.card_last
            << ",\"command_before\":" << r.command_before
            << ",\"reset_mode\":" << r.reset_mode << ",\"completion_mode\":\"RETURN_ONLY\""
            << ",\"callback_registered\":false,\"payload_hex\":\"";
        constexpr char hex[]="0123456789abcdef";
        for (uint8_t byte : r.payload) out << hex[byte>>4] << hex[byte&15];
        const auto& reset=r.reset;
        out << "\",\"reset\":{\"operation_id\":" << jsonString(std::to_string(reset.operation_id))
            << ",\"generation\":" << jsonString(std::to_string(reset.generation))
            << ",\"epoch\":" << jsonString(std::to_string(reset.epoch))
            << ",\"state\":" << reset.state << ",\"call_returned\":" << reset.call_returned
            << ",\"call_status\":" << reset.call_status << ",\"callback_seen\":" << reset.callback_seen
            << ",\"completion_status\":" << reset.completion_status << ",\"callback_count\":" << reset.callback_count
            << ",\"epoch_invalidated\":" << reset.epoch_invalidated << ",\"revalidated\":" << reset.revalidated
            << ",\"hardware_blocked\":" << reset.hardware_blocked
            << ",\"submitted_100ns\":" << jsonString(std::to_string(reset.submitted_100ns))
            << ",\"returned_100ns\":" << jsonString(std::to_string(reset.returned_100ns))
            << ",\"completed_100ns\":" << jsonString(std::to_string(reset.completed_100ns)) << '}';
    }
    // Exit zero is an ABI-valid cache reply, NOT protocol or cleanup success.
    out << '}'; return emit(out.str(),valid ? 0 : 1);
}

// BOOT never emits image bytes, file paths, DMA addresses, or inferred firmware fields.
std::string bootDigest(const uint8_t* bytes) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (size_t i=0; i<32; ++i) {
        result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15];
    }
    return result;
}

bool bootIdentity(uint64_t value) { return value && value != UINT64_MAX; }

bool bootReplyValid(const nnpi_boot_response& r, DWORD bytes,
                    const nnpi_boot_request& request, DWORD ioctl) {
    if (bytes != sizeof(r) || r.version != NNPI_BOOT_ABI_VERSION || r.size != sizeof(r) ||
        r.reserved[0] || r.reserved[1] || r.session_id == UINT64_MAX ||
        r.generation == UINT64_MAX || r.epoch == UINT64_MAX ||
        r.image_bytes != NNPI_BOOT_IMAGE_BYTES || r.loaded_bytes > r.image_bytes ||
        r.load_state > NNPI_BOOT_LOAD_FAILED || r.phase > NNPI_BS_FINISHED ||
        r.failed_stage > NNPI_BOOT_FAIL_RESET || r.budget_consumed > 1 ||
        r.hostready_writes > 1 || r.sysinfo_halves > 6 || r.image_halves > 6 ||
        (r.published_dma & ~NNPI_BS_ALL_DMA) || (r.stopped_dma & ~r.published_dma) ||
        r.payload_valid > 1 || r.boot_started > 1 || r.driver_ready > 1 || r.closing > 1 ||
        r.reset_used > 1 || r.reset_returned > 1 || r.worker_active > 1 ||
        r.sha256_verified > 1 || r.capsule_rejected > 1) return false;
    if (r.load_state != NNPI_BOOT_LOAD_EMPTY &&
        (!bootIdentity(r.session_id) || !bootIdentity(r.generation) || !bootIdentity(r.epoch))) return false;
    if (r.sha256_verified && (r.loaded_bytes != NNPI_BOOT_IMAGE_BYTES ||
        bootDigest(r.sha256) != NNPI_BOOT_SHA256_HEX)) return false;
    if (ioctl == NNPI_BOOT_QUERY_IOCTL) return true;
    return bootIdentity(r.session_id) && r.generation == request.generation &&
        r.epoch == request.epoch &&
        (ioctl == NNPI_BOOT_BEGIN_IOCTL || r.session_id == request.session_id);
}

struct BootProgress {
    const char* command;
    size_t index;
    const char* stage;
    std::string path;
    bool localVerified = false;
    uint32_t chunks = 0; // Acknowledged chunks only, never inferred after a lost reply.
};

void bootPrefix(std::ostream& out, const BootProgress& p, bool ok, bool valid, bool lost) {
    out << "{\"ok\":" << (ok ? "true" : "false")
        << ",\"command\":" << jsonString(p.command) << ",\"index\":" << p.index
        << ",\"stage\":" << jsonString(p.stage)
        << ",\"interface_path\":" << jsonString(p.path)
        << ",\"cache_only\":" << (std::strcmp(p.command,"boot-query")==0 ? "true" : "false")
        << ",\"retry_allowed\":false,\"valid_response\":" << (valid ? "true" : "false")
        << ",\"lost_reply\":" << (lost ? "true" : "false")
        << ",\"local_sha256_verified\":" << (p.localVerified ? "true" : "false")
        << ",\"chunks_sent\":" << p.chunks
        << ",\"hardware_boot_verified\":false,\"compute_verified\":false,\"ssh_verified\":false"
        << ",\"timeout_note\":\"5000 ms initial IOCTL wait; cancellation drain may wait indefinitely; never retry lost replies\"";
}

int bootLocalError(const BootProgress& p, const char* kind, const char* message) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    bootPrefix(out,p,false,false,false);
    out << ",\"error\":{\"kind\":" << jsonString(kind) << ",\"message\":" << jsonString(message) << "}}";
    return emit(out.str(),1);
}

int bootOutput(const BootProgress& p, const BasicQueryResult<nnpi_boot_response>& result,
               bool valid, bool accepted) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    bootPrefix(out,p,valid && accepted,valid,!valid);
    out << ",\"bytes_returned\":" << result.bytes
        << ",\"timed_out\":" << (result.timedOut ? "true" : "false");
    if (!valid) {
        out << ",\"error\":";
        if (result.code != ERROR_SUCCESS) appendWin32(out,result.operation,result.code);
        else out << "{\"kind\":\"invalid_response\",\"message\":\"BOOT ABI, identity or range validation failed\"}";
        if (result.cancelled) {
            out << ",\"cancel_result\":"; appendWin32(out,"CancelIoEx",result.cancelCode);
            out << ",\"final_completion\":"; appendWin32(out,"GetOverlappedResult(wait=TRUE)",result.completionCode);
        }
    } else {
        const auto& r = result.response;
        if (!accepted) out << ",\"error\":{\"kind\":\"boot_action_rejected\",\"message\":\"Driver response did not confirm the requested state; do not retry\"}";
#define BOOT_NUMBER(field) out << ",\"" #field "\":" << r.field
#define BOOT_DECIMAL(field) out << ",\"" #field "\":" << jsonString(std::to_string(r.field))
        BOOT_NUMBER(version); BOOT_NUMBER(size);
        BOOT_DECIMAL(session_id); BOOT_DECIMAL(generation); BOOT_DECIMAL(epoch);
        BOOT_DECIMAL(image_bytes); BOOT_DECIMAL(loaded_bytes);
        BOOT_NUMBER(load_state); BOOT_NUMBER(phase); BOOT_NUMBER(failed_stage); BOOT_NUMBER(ntstatus);
        BOOT_NUMBER(budget_consumed); BOOT_NUMBER(hostready_writes); BOOT_NUMBER(sysinfo_halves); BOOT_NUMBER(image_halves);
        BOOT_NUMBER(host_before); BOOT_NUMBER(card_before); BOOT_NUMBER(command_before); BOOT_NUMBER(response_before);
        BOOT_NUMBER(card_last); BOOT_NUMBER(observations); BOOT_NUMBER(payload_valid); BOOT_NUMBER(boot_started);
        BOOT_NUMBER(driver_ready); BOOT_NUMBER(published_dma); BOOT_NUMBER(stopped_dma); BOOT_NUMBER(closing);
        BOOT_NUMBER(reset_used); BOOT_NUMBER(reset_returned); BOOT_NUMBER(reset_status); BOOT_NUMBER(worker_active);
        BOOT_NUMBER(sha256_verified); BOOT_NUMBER(capsule_rejected);
#undef BOOT_DECIMAL
#undef BOOT_NUMBER
        out << ",\"sha256\":" << jsonString(bootDigest(r.sha256));
    }
    out << '}';
    return emit(out.str(),valid && accepted ? 0 : 1);
}

class BootHash {
public:
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BootHash() = default;
    BootHash(const BootHash&) = delete;
    BootHash& operator=(const BootHash&) = delete;
    ~BootHash() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
    }
    bool start() {
        return BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0) >= 0 &&
            BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0) >= 0;
    }
};

// One reusable 1 MiB payload plus the 64-byte IOCTL header, allocated on the heap.
struct BootFrame { nnpi_boot_request request; uint8_t payload[NNPI_BOOT_CHUNK_MAX]; };
static_assert(offsetof(BootFrame,payload)==sizeof(nnpi_boot_request),"BOOT chunk layout mismatch");

void bootRead(HANDLE file, uint8_t* buffer, DWORD count) {
    DWORD total = 0;
    while (total < count) {
        DWORD received = 0;
        if (!ReadFile(file,buffer+total,count-total,&received,nullptr))
            throw Win32Failure{"ReadFile(boot image)",GetLastError()};
        if (!received) throw Win32Failure{"ReadFile(boot image premature EOF)",ERROR_HANDLE_EOF};
        total += received;
    }
}

int runBoot(HANDLE device, const char* command, size_t index, uint64_t session,
            uint64_t generation, uint64_t epoch, const wchar_t* image, const std::string& path) {
    const bool load = std::strcmp(command,"boot-load")==0;
    const bool observe = std::strcmp(command,"boot-query")==0;
    BootProgress progress{command,index,load ? "local-validation" : observe ? "query" : "submit",path};
    try {
        nnpi_boot_request request{};
        request.version = NNPI_BOOT_ABI_VERSION; request.size = sizeof(request);
        if (!observe) {
            request.session_id=session; request.generation=generation; request.epoch=epoch;
            request.image_bytes=NNPI_BOOT_IMAGE_BYTES;
        }
        if (!load) {
            const DWORD ioctl = observe ? NNPI_BOOT_QUERY_IOCTL : NNPI_BOOT_SUBMIT_IOCTL;
            const auto result = query<nnpi_boot_response,nnpi_boot_request>(device,ioctl,NNPI_BOOT_ABI_VERSION,&request);
            const bool valid = result.code==ERROR_SUCCESS && bootReplyValid(result.response,result.bytes,request,ioctl);
            const bool accepted = observe || (result.response.load_state==NNPI_BOOT_LOAD_SUBMITTED &&
                result.response.failed_stage==NNPI_BOOT_FAIL_NONE && result.response.sha256_verified==1 &&
                result.response.worker_active==1);
            return bootOutput(progress,result,valid,accepted);
        }

        // Deny concurrent writers/deleters, including already-open incompatible handles.
        // Keep this exact handle alive from preflight through SEAL; never reopen the path.
        const Handle file(CreateFileW(image,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
        if (file.get()==INVALID_HANDLE_VALUE) throw Win32Failure{"CreateFileW(boot image read-only)",GetLastError()};
        if (GetFileType(file.get())!=FILE_TYPE_DISK)
            return bootLocalError(progress,"image_type","Boot image must be a disk file");
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file.get(),&length)) throw Win32Failure{"GetFileSizeEx(boot image)",GetLastError()};
        if (length.QuadPart!=static_cast<LONGLONG>(NNPI_BOOT_IMAGE_BYTES))
            return bootLocalError(progress,"image_size","Boot image must be exactly 188744192 bytes");
        std::unique_ptr<BootFrame> frame(new BootFrame{});
        BootHash hash;
        if (!hash.start()) return bootLocalError(progress,"cng","BCrypt SHA-256 initialization failed");
        for (uint64_t offset=0; offset<NNPI_BOOT_IMAGE_BYTES;) {
            const DWORD count=static_cast<DWORD>((std::min)(static_cast<uint64_t>(NNPI_BOOT_CHUNK_MAX),NNPI_BOOT_IMAGE_BYTES-offset));
            bootRead(file.get(),frame->payload,count);
            if (BCryptHashData(hash.hash,frame->payload,count,0)<0)
                return bootLocalError(progress,"cng","BCryptHashData failed");
            offset += count;
        }
        uint8_t digest[32]{};
        if (BCryptFinishHash(hash.hash,digest,sizeof(digest),0)<0)
            return bootLocalError(progress,"cng","BCryptFinishHash failed");
        if (bootDigest(digest)!=NNPI_BOOT_SHA256_HEX)
            return bootLocalError(progress,"image_hash","Boot image does not match the fixed SHA-256 allowlist");
        progress.localVerified=true;
        LARGE_INTEGER beginning{};
        if (!SetFilePointerEx(file.get(),beginning,nullptr,FILE_BEGIN))
            throw Win32Failure{"SetFilePointerEx(boot image rewind)",GetLastError()};
        // No BOOT IOCTL is sent until the complete local hash and length pass.
        progress.stage="begin";
        auto result=query<nnpi_boot_response,nnpi_boot_request>(device,NNPI_BOOT_BEGIN_IOCTL,NNPI_BOOT_ABI_VERSION,&request);
        bool valid=result.code==ERROR_SUCCESS && bootReplyValid(result.response,result.bytes,request,NNPI_BOOT_BEGIN_IOCTL);
        bool accepted=valid && result.response.load_state==NNPI_BOOT_LOAD_LOADING &&
            result.response.loaded_bytes==0 && result.response.failed_stage==NNPI_BOOT_FAIL_NONE;
        if (!accepted) return bootOutput(progress,result,valid,false);
        request.session_id=result.response.session_id;
        for (uint64_t offset=0; offset<NNPI_BOOT_IMAGE_BYTES;) {
            progress.stage="chunk";
            const DWORD count=static_cast<DWORD>((std::min)(static_cast<uint64_t>(NNPI_BOOT_CHUNK_MAX),NNPI_BOOT_IMAGE_BYTES-offset));
            bootRead(file.get(),frame->payload,count);
            request.offset=offset; request.chunk_bytes=count;
            frame->request=request;
            result=queryBuffer<nnpi_boot_response>(device,NNPI_BOOT_CHUNK_IOCTL,frame.get(),
                static_cast<DWORD>(sizeof(request))+count);
            valid=result.code==ERROR_SUCCESS && bootReplyValid(result.response,result.bytes,request,NNPI_BOOT_CHUNK_IOCTL);
            accepted=valid && result.response.load_state==NNPI_BOOT_LOAD_LOADING &&
                result.response.loaded_bytes==offset+count && result.response.failed_stage==NNPI_BOOT_FAIL_NONE;
            if (!accepted) return bootOutput(progress,result,valid,false);
            ++progress.chunks;
            offset += count;
        }
        progress.stage="seal";
        request.offset=0; request.chunk_bytes=0;
        result=query<nnpi_boot_response,nnpi_boot_request>(device,NNPI_BOOT_SEAL_IOCTL,NNPI_BOOT_ABI_VERSION,&request);
        valid=result.code==ERROR_SUCCESS && bootReplyValid(result.response,result.bytes,request,NNPI_BOOT_SEAL_IOCTL);
        accepted=valid && result.response.load_state==NNPI_BOOT_LOAD_SEALED &&
            result.response.loaded_bytes==NNPI_BOOT_IMAGE_BYTES && result.response.sha256_verified==1 &&
            result.response.failed_stage==NNPI_BOOT_FAIL_NONE && result.response.capsule_rejected==0;
        // Intentionally no SUBMIT, QUERY, retry, or automatic recovery here.
        return bootOutput(progress,result,valid,accepted);
    } catch (const Win32Failure& failure) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        bootPrefix(out,progress,false,false,false);
        out << ",\"error\":"; appendWin32(out,failure.operation,failure.code);
        out << '}'; return emit(out.str(),1);
    } catch (...) {
        return bootLocalError(progress,"internal","BOOT allocation or internal failure; do not retry");
    }
}

uint64_t decimal64(const wchar_t* text) {
    uint64_t value=0;
    if (!*text) throw std::runtime_error("empty integer");
    for (; *text; ++text) {
        if (*text<L'0' || *text>L'9' || value>(UINT64_MAX-static_cast<uint64_t>(*text-L'0'))/10)
            throw std::runtime_error("invalid unsigned integer");
        value=value*10+static_cast<uint64_t>(*text-L'0');
    }
    return value;
}

int runBootCommand(int argc, wchar_t** argv) {
    const bool observe=std::wcscmp(argv[1],L"boot-query")==0;
    const bool load=std::wcscmp(argv[1],L"boot-load")==0;
    const char* command=observe ? "boot-query" : load ? "boot-load" : "boot-submit";
    const char* usage="Usage: boot-query --index N | boot-load --index N --generation G --epoch E --image PATH | boot-submit --index N --session S --generation G --epoch E";
    if (argc!=(observe ? 4 : 10) || std::wcscmp(argv[2],L"--index"))
        return error("arguments",usage,2);
    uint64_t index=0, session=0, generation=0, epoch=0;
    try {
        index=decimal64(argv[3]);
        if (load) {
            if (std::wcscmp(argv[4],L"--generation") || std::wcscmp(argv[6],L"--epoch") ||
                std::wcscmp(argv[8],L"--image") || !argv[9][0]) return error("arguments",usage,2);
            generation=decimal64(argv[5]); epoch=decimal64(argv[7]);
        } else if (!observe) {
            if (std::wcscmp(argv[4],L"--session") || std::wcscmp(argv[6],L"--generation") ||
                std::wcscmp(argv[8],L"--epoch")) return error("arguments",usage,2);
            session=decimal64(argv[5]); generation=decimal64(argv[7]); epoch=decimal64(argv[9]);
        }
    } catch (const std::runtime_error&) {
        return error("arguments","BOOT integers must be unsigned decimal uint64 values",2);
    }
    if ((!observe && (!bootIdentity(generation) || !bootIdentity(epoch))) ||
        (!observe && !load && !bootIdentity(session)))
        return error("arguments","BOOT identities must be in 1..18446744073709551614",2);
    const auto paths=enumerateInterfaces();
    if (index>=paths.size()) return error("arguments","--index is outside the current interface enumeration",2);
    const size_t selected=static_cast<size_t>(index);
    const Handle device(CreateFileW(paths[selected].c_str(),GENERIC_READ | (observe ? 0 : GENERIC_WRITE),
        FILE_SHARE_READ | FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
    if (device.get()==INVALID_HANDLE_VALUE) {
        const DWORD code=GetLastError();
        BootProgress progress{command,selected,"open-device",utf8(paths[selected])};
        std::ostringstream out;
        out.imbue(std::locale::classic());
        bootPrefix(out,progress,false,false,false);
        out << ",\"error\":"; appendWin32(out,"CreateFileW(BOOT device overlapped)",code);
        out << '}'; return emit(out.str(),1);
    }
    return runBoot(device.get(),command,selected,session,generation,epoch,load ? argv[9] : nullptr,utf8(paths[selected]));
}

// 027 NET channel lifecycle. BEGIN submits one non-context channel (CHANNEL_OP ->
// H2C/C2H pools -> ETH_CONFIG -> READY) to the driver worker; QUERY is CPU-only
// (including outside D0). READY is NOT SSH/network success; frame TX/RX are a
// later data-plane ABI. Requests/response are exact 64/128-byte headers.
std::string netMac(const uint8_t* mac) {
    constexpr char hex[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < 6; ++i) {
        if (i) s += ':';
        s += hex[mac[i] >> 4]; s += hex[mac[i] & 15];
    }
    return s;
}

std::string netIp(uint32_t s_addr) {
    // s_addr stores octets little-endian: octet0 | octet1<<8 | octet2<<16 | octet3<<24.
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
        s_addr & 0xffu, (s_addr >> 8) & 0xffu, (s_addr >> 16) & 0xffu, (s_addr >> 24) & 0xffu);
    return buf;
}

uint32_t parseIpv4(const std::string& text) {
    unsigned parts[4] = {0, 0, 0, 0};
    size_t pos = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (i) { if (pos >= text.size() || text[pos++] != '.') throw std::runtime_error("ipv4 dot"); }
        unsigned value = 0; bool any = false;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            value = value * 10 + static_cast<unsigned>(text[pos] - '0');
            if (value > 255) throw std::runtime_error("ipv4 octet range");
            any = true; ++pos;
        }
        if (!any) throw std::runtime_error("ipv4 empty octet");
        parts[i] = value;
    }
    if (pos != text.size()) throw std::runtime_error("ipv4 trailing");
    return static_cast<uint32_t>(parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24));
}

bool parseMac(const std::string& text, uint8_t out[6]) {
    if (text.size() != 17) return false; // "MM:MM:MM:MM:MM:MM"
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < 6; ++i) {
        const size_t base = i * 3;
        if (i && text[base - 1] != ':') return false;
        const int hi = hexVal(text[base]), lo = hexVal(text[base + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool netIdentity(uint64_t value) { return value && value != UINT64_MAX; }

bool netReplyValid(const nnpi_net_response& r, DWORD bytes,
                   const nnpi_net_request& request, DWORD ioctl) {
    if (bytes != sizeof(r) || r.version != NNPI_NET_ABI_VERSION || r.size != sizeof(r) ||
        r.reserved2 || r.reserved[0] || r.reserved[1] || r.reserved[2] ||
        r.reserved[3] || r.reserved[4] || r.reserved[5] ||
        r.session_id == UINT64_MAX || r.generation == UINT64_MAX || r.epoch == UINT64_MAX ||
        r.phase > NNPI_NS_FINISHED || r.failed_stage > NNPI_NET_FAIL_DMA ||
        (r.chan_id != 0 && (r.chan_id < NNPI_CHAN_MIN_NONCTX || r.chan_id > NNPI_CHAN_ID_MASK)) ||
        r.chan_created > 1 || r.h2c_rb_published > 1 || r.c2h_rb_published > 1 ||
        r.eth_configured > 1 || r.worker_active > 1 || r.closing > 1 ||
        r.invalidated > 1 || r.budget_consumed > 1 ||
        (r.published_dma & ~NNPI_NS_ALL_DMA) || (r.stopped_dma & ~r.published_dma))
        return false;
    if (ioctl == NNPI_NET_QUERY_IOCTL) return true;
    // BEGIN echoes the request identity and the configured IP; session_id may be
    // zero on a synchronous failure (kernel assigns it only on success).
    return r.generation == request.generation && r.epoch == request.epoch &&
        r.card_ip == request.card_ip;
}

struct NetProgress {
    const char* command;
    size_t index;
    std::string path;
};

void netPrefix(std::ostream& out, const NetProgress& p, bool ok, bool valid, bool lost) {
    out << "{\"ok\":" << (ok ? "true" : "false")
        << ",\"command\":" << jsonString(p.command) << ",\"index\":" << p.index
        << ",\"interface_path\":" << jsonString(p.path)
        << ",\"cache_only\":" << (std::strcmp(p.command, "net-query") == 0 ? "true" : "false")
        << ",\"retry_allowed\":false,\"valid_response\":" << (valid ? "true" : "false")
        << ",\"lost_reply\":" << (lost ? "true" : "false")
        << ",\"hardware_boot_verified\":false,\"compute_verified\":false,\"ssh_verified\":false"
        << ",\"timeout_note\":\"5000 ms initial IOCTL wait; cancellation drain may wait indefinitely; never retry lost replies\"";
}

int netLocalError(const NetProgress& p, const char* kind, const char* message) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    netPrefix(out, p, false, false, false);
    out << ",\"error\":{\"kind\":" << jsonString(kind) << ",\"message\":" << jsonString(message) << "}}";
    return emit(out.str(), 1);
}

int netOutput(const NetProgress& p, const BasicQueryResult<nnpi_net_response>& result,
              bool valid, bool accepted) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    netPrefix(out, p, valid && accepted, valid, !valid);
    out << ",\"bytes_returned\":" << result.bytes
        << ",\"timed_out\":" << (result.timedOut ? "true" : "false");
    if (!valid) {
        out << ",\"error\":";
        if (result.code != ERROR_SUCCESS) appendWin32(out, result.operation, result.code);
        else out << "{\"kind\":\"invalid_response\",\"message\":\"NET ABI, identity or range validation failed\"}";
        if (result.cancelled) {
            out << ",\"cancel_result\":"; appendWin32(out, "CancelIoEx", result.cancelCode);
            out << ",\"final_completion\":"; appendWin32(out, "GetOverlappedResult(wait=TRUE)", result.completionCode);
        }
    } else {
        const auto& r = result.response;
        if (!accepted) out << ",\"error\":{\"kind\":\"net_action_rejected\",\"message\":\"Driver response did not confirm the requested state; do not retry\"}";
        out << ",\"version\":" << r.version << ",\"size\":" << r.size
            << ",\"session_id\":" << jsonString(std::to_string(r.session_id))
            << ",\"generation\":" << jsonString(std::to_string(r.generation))
            << ",\"epoch\":" << jsonString(std::to_string(r.epoch))
            << ",\"chan_id\":" << r.chan_id << ",\"phase\":" << r.phase
            << ",\"chan_created\":" << r.chan_created << ",\"h2c_rb_published\":" << r.h2c_rb_published
            << ",\"c2h_rb_published\":" << r.c2h_rb_published << ",\"eth_configured\":" << r.eth_configured
            << ",\"published_dma\":" << r.published_dma << ",\"stopped_dma\":" << r.stopped_dma
            << ",\"card_ip\":" << r.card_ip << ",\"card_ip_dotted\":" << jsonString(netIp(r.card_ip))
            << ",\"failed_stage\":" << r.failed_stage << ",\"ntstatus\":" << r.ntstatus
            << ",\"observations\":" << r.observations << ",\"worker_active\":" << r.worker_active
            << ",\"closing\":" << r.closing << ",\"invalidated\":" << r.invalidated
            << ",\"budget_consumed\":" << r.budget_consumed
            << ",\"mac\":" << jsonString(netMac(r.mac));
    }
    out << '}';
    return emit(out.str(), valid && accepted ? 0 : 1);
}

int runNet(HANDLE device, const char* command, size_t index, uint64_t generation,
           uint64_t epoch, uint32_t cardIp, const uint8_t mac[6], const std::string& path) {
    const bool observe = std::strcmp(command, "net-query") == 0;
    NetProgress progress{command, index, path};
    try {
        nnpi_net_request request{};
        request.version = NNPI_NET_ABI_VERSION; request.size = sizeof(request);
        if (!observe) {
            request.generation = generation; request.epoch = epoch;
            request.chan_id = NNPI_NET_CHAN_AUTO;
            request.card_ip = cardIp;
            std::memcpy(request.mac, mac, 6);
        }
        const DWORD ioctl = observe ? NNPI_NET_QUERY_IOCTL : NNPI_NET_BEGIN_IOCTL;
        const auto result = query<nnpi_net_response, nnpi_net_request>(device, ioctl, NNPI_NET_ABI_VERSION, &request);
        const bool valid = result.code == ERROR_SUCCESS &&
            netReplyValid(result.response, result.bytes, request, ioctl);
        const bool accepted = observe || (result.response.worker_active == 1 &&
            result.response.failed_stage == NNPI_NET_FAIL_NONE);
        return netOutput(progress, result, valid, accepted);
    } catch (const Win32Failure& failure) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        netPrefix(out, progress, false, false, false);
        out << ",\"error\":"; appendWin32(out, failure.operation, failure.code);
        out << '}'; return emit(out.str(), 1);
    } catch (...) {
        return netLocalError(progress, "internal", "NET allocation or internal failure; do not retry");
    }
}

int runNetCommand(int argc, wchar_t** argv) {
    const bool observe = std::wcscmp(argv[1], L"net-query") == 0;
    const char* command = observe ? "net-query" : "net-begin";
    const char* usage = "Usage: net-query --index N | net-begin --index N --generation G --epoch E --card-ip A.B.C.D --mac MM:MM:MM:MM:MM:MM";
    if (argc != (observe ? 4 : 12) || std::wcscmp(argv[2], L"--index"))
        return error("arguments", usage, 2);
    uint64_t index = 0, generation = 0, epoch = 0;
    uint32_t cardIp = 0;
    uint8_t mac[6] = {};
    try {
        index = decimal64(argv[3]);
        if (!observe) {
            if (std::wcscmp(argv[4], L"--generation") || std::wcscmp(argv[6], L"--epoch") ||
                std::wcscmp(argv[8], L"--card-ip") || std::wcscmp(argv[10], L"--mac"))
                return error("arguments", usage, 2);
            generation = decimal64(argv[5]); epoch = decimal64(argv[7]);
            cardIp = parseIpv4(utf8(argv[9]));
            if (!parseMac(utf8(argv[11]), mac))
                return error("arguments", "MAC must be six colon-separated hex octets", 2);
        }
    } catch (const std::runtime_error&) {
        return error("arguments", "NET identity or --card-ip must be a valid unsigned/IPv4 value", 2);
    }
    if (!observe && (!netIdentity(generation) || !netIdentity(epoch)))
        return error("arguments", "NET identities must be in 1..18446744073709551614", 2);
    if (!observe && cardIp == 0)
        return error("arguments", "--card-ip must be a non-zero IPv4 address", 2);
    const auto paths = enumerateInterfaces();
    if (index >= paths.size()) return error("arguments", "--index is outside the current interface enumeration", 2);
    const size_t selected = static_cast<size_t>(index);
    const Handle device(CreateFileW(paths[selected].c_str(), GENERIC_READ | (observe ? 0 : GENERIC_WRITE),
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (device.get() == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        NetProgress progress{command, selected, utf8(paths[selected])};
        std::ostringstream out;
        out.imbue(std::locale::classic());
        netPrefix(out, progress, false, false, false);
        out << ",\"error\":"; appendWin32(out, "CreateFileW(NET device overlapped)", code);
        out << '}'; return emit(out.str(), 1);
    }
    return runNet(device.get(), command, selected, generation, epoch, cardIp, mac, utf8(paths[selected]));
}

// Runtime diagnostics consume only the cached, address-free ABI. Success means
// an ABI-valid observation, not healthy runtime, networking, SSH or computation.
bool runtimeInfoReplyValid(const nnpi_runtime_info_response& r, DWORD bytes) {
    return nnpi_runtime_info_response_valid(&r, bytes) &&
        r.generation != UINT64_MAX && r.epoch != UINT64_MAX;
    // Zero identities are permitted before preparation by the shared ABI.
}

const char* runtimeNetReasonText(uint32_t reason) {
    switch (reason) {
    case NNPI_RUNTIME_NET_READY: return "ready";
    case NNPI_RUNTIME_NET_DISABLED_PAGE_LIST_UNIMPLEMENTED: return "page_list_unimplemented";
    case NNPI_RUNTIME_NET_DISABLED_LIFECYCLE_UNVERIFIED: return "lifecycle_unverified";
    case NNPI_RUNTIME_NET_NOT_READY: return "not_ready";
    case NNPI_RUNTIME_NET_CAP_MISSING: return "cap_missing";
    case NNPI_RUNTIME_NET_SESSION_USED: return "session_used";
    case NNPI_RUNTIME_NET_CLOSING: return "closing";
    case NNPI_RUNTIME_NET_UNEXPECTED_CONFIG: return "unexpected_config";
    default: return "unknown";
    }
}

int runtimeInfoOutput(size_t index, const BasicQueryResult<nnpi_runtime_info_response>& result) {
    const bool valid = result.code == ERROR_SUCCESS &&
        runtimeInfoReplyValid(result.response, result.bytes);
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"ok\":" << (valid ? "true" : "false")
        << ",\"command\":\"runtime-info\",\"index\":" << index
        << ",\"cache_only\":true,\"ssh_verified\":false,\"compute_verified\":false"
        << ",\"hardware_boot_verified\":false,\"retry_allowed\":false"
        << ",\"valid_response\":" << (valid ? "true" : "false")
        << ",\"bytes_returned\":" << result.bytes
        << ",\"timed_out\":" << (result.timedOut ? "true" : "false")
        << ",\"interpretation\":\"cached runtime evidence; healthy is not network or SSH readiness\""
        << ",\"timeout_note\":\"5000 ms initial IOCTL wait; cancellation drain may take longer; no hard deadline\"";
    if (!valid) {
        out << ",\"error\":";
        if (result.code != ERROR_SUCCESS) appendWin32(out, result.operation, result.code);
        else out << "{\"kind\":\"invalid_response\",\"validator\":\"nnpi_runtime_info_response_valid/identity\",\"expected_bytes\":"
                 << sizeof(nnpi_runtime_info_response) << '}';
        if (result.cancelled) {
            out << ",\"cancel_result\":"; appendWin32(out, "CancelIoEx", result.cancelCode);
            out << ",\"final_completion\":";
            appendWin32(out, "GetOverlappedResult(wait=TRUE)", result.completionCode);
        }
    } else {
        const auto& r = result.response;
        out << ",\"version\":" << r.version << ",\"size\":" << r.size
            << ",\"generation\":" << jsonString(std::to_string(r.generation))
            << ",\"epoch\":" << jsonString(std::to_string(r.epoch))
            << ",\"compatible\":" << (r.compatible ? "true" : "false")
            << ",\"healthy\":" << (r.healthy ? "true" : "false")
            << ",\"versions\":{\"protocol\":" << r.protocol_version
            << ",\"firmware\":" << r.firmware_version << ",\"channel\":" << r.channel_version << '}'
            << ",\"last_stage\":" << r.last_stage
            << ",\"last_failure_reason\":" << r.last_failure_reason
            << ",\"last_failure_status\":" << r.last_failure_status
            << ",\"keepalive\":{\"updates\":" << jsonString(std::to_string(r.keepalive_updates)) << '}'
            << ",\"irq\":{\"mask\":" << r.irq_mask << ",\"status\":" << r.irq_status
            << ",\"reason\":" << r.irq_reason << ",\"valid\":" << r.irq_valid << '}'
            << ",\"partial_count\":" << r.partial_count
            << ",\"netblocked\":" << (r.net_enabled ? "false" : "true")
            << ",\"net_enabled\":" << (r.net_enabled ? "true" : "false")
            << ",\"net_disabled_reason\":" << r.net_disabled_reason
            << ",\"net_disabled_reason_text\":" << jsonString(runtimeNetReasonText(r.net_disabled_reason))
            << ",\"net_enabled_scope\":\"new_begin_admission_only\""
            << ",\"unexpected_net_config_count\":" << jsonString(std::to_string(r.unexpected_net_config_count));
        // Never serialize raw event/config QWs, paths, pointers or DMA addresses.
    }
    out << '}';
    return emit(out.str(), valid ? 0 : 1);
}

int runRuntimeInfoCommand(int argc, wchar_t** argv) {
    const char* usage = "Usage: nnpi-info runtime-info --index N (zero-based; explicit selection required)";
    if (argc != 4 || std::wcscmp(argv[2], L"--index"))
        return error("arguments", usage, 2);
    uint64_t index = 0;
    try { index = decimal64(argv[3]); }
    catch (const std::runtime_error&) {
        return error("arguments", "runtime-info --index must be an unsigned decimal uint64", 2);
    }
    BasicQueryResult<nnpi_runtime_info_response> result;
    try {
        const auto paths = enumerateInterfaces();
        if (index >= paths.size())
            return error("arguments", "--index is outside the current interface enumeration", 2);
        const Handle device(CreateFileW(paths[static_cast<size_t>(index)].c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (device.get() == INVALID_HANDLE_VALUE)
            throw Win32Failure{"CreateFileW(runtime-info read-only overlapped)", GetLastError()};
        // The generic helper zero-initializes all request reserved words. One
        // READ IOCTL only: no status probe, boot/net action, fallback or retry.
        result = query<nnpi_runtime_info_response, nnpi_runtime_info_request>(
            device.get(), NNPI_RUNTIME_INFO_IOCTL, NNPI_RUNTIME_INFO_VERSION);
    } catch (const Win32Failure& failure) {
        result.code = failure.code; result.operation = failure.operation;
    }
    return runtimeInfoOutput(static_cast<size_t>(index), result);
}

int runNetDataCommand(int argc, wchar_t** argv) {
    const bool observe=std::wcscmp(argv[1],L"net-data-query")==0;
    const char* usage="Usage: net-data-query --index N | net-tx --index N --session-id S --generation G --epoch E --frame-hex HEX (one explicit submission; no retry)";
    if (argc!=(observe?4:12) || std::wcscmp(argv[2],L"--index")) return error("arguments",usage,2);
    uint64_t index=0; nnpi_net_tx_request request{};
    request.version=NNPI_NET_DATA_VERSION; request.size=sizeof(request);
    try {
        index=decimal64(argv[3]);
        if (!observe) {
            if (std::wcscmp(argv[4],L"--session-id") || std::wcscmp(argv[6],L"--generation") ||
                std::wcscmp(argv[8],L"--epoch") || std::wcscmp(argv[10],L"--frame-hex")) return error("arguments",usage,2);
            request.session_id=decimal64(argv[5]); request.generation=decimal64(argv[7]); request.epoch=decimal64(argv[9]);
            if (!netIdentity(request.session_id) || !netIdentity(request.generation) || !netIdentity(request.epoch))
                return error("arguments","TX requires current nonzero session/G/E",2);
            const std::wstring hex(argv[11]);
            if (hex.size()<28 || hex.size()>8192 || hex.size()%2) return error("arguments","Frame must be 14..4096 hex-encoded bytes",2);
            auto digit=[](wchar_t ch)->int { if(ch>=L'0' && ch<=L'9') return ch-L'0';
                if(ch>=L'a' && ch<=L'f') return ch-L'a'+10; if(ch>=L'A' && ch<=L'F') return ch-L'A'+10; return -1; };
            request.length=static_cast<uint32_t>(hex.size()/2);
            for(size_t i=0;i<request.length;++i) {
                int hi=digit(hex[i*2]),lo=digit(hex[i*2+1]);
                if(hi<0 || lo<0) return error("arguments","Frame contains non-hex characters",2);
                request.frame[i]=static_cast<uint8_t>((hi<<4)|lo);
            }
        }
    } catch (const std::runtime_error&) { return error("arguments",usage,2); }
    BasicQueryResult<nnpi_net_data_response> data;
    BasicQueryResult<nnpi_net_tx_response> tx;
    try {
        const auto paths=enumerateInterfaces();
        if(index>=paths.size()) return error("arguments","--index is outside the current interface enumeration",2);
        const Handle device(CreateFileW(paths[static_cast<size_t>(index)].c_str(),GENERIC_READ|(observe?0:GENERIC_WRITE),
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
        if(device.get()==INVALID_HANDLE_VALUE) throw Win32Failure{"CreateFileW(net-data)",GetLastError()};
        if(observe) data=query<nnpi_net_data_response,nnpi_net_data_request>(device.get(),NNPI_NET_DATA_QUERY_IOCTL,NNPI_NET_DATA_VERSION);
        else tx=query<nnpi_net_tx_response,nnpi_net_tx_request>(device.get(),NNPI_NET_TX_IOCTL,NNPI_NET_DATA_VERSION,&request);
    } catch (const Win32Failure& f) {
        if(observe) { data.code=f.code; data.operation=f.operation; }
        else { tx.code=f.code; tx.operation=f.operation; }
    }
    const bool valid=observe ? data.code==ERROR_SUCCESS && nnpi_net_data_response_valid(&data.response,data.bytes) :
        tx.code==ERROR_SUCCESS && tx.bytes==sizeof(tx.response) && tx.response.version==NNPI_NET_DATA_VERSION &&
        tx.response.size==sizeof(tx.response) && tx.response.session_id==request.session_id &&
        tx.response.generation==request.generation && tx.response.epoch==request.epoch && tx.response.accepted==1 &&
        tx.response.slot<16 && !tx.response.reserved[0] && !tx.response.reserved[1];
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "{\"ok\":" << (valid?"true":"false") << ",\"valid_response\":" << (valid?"true":"false")
        << ",\"command\":" << jsonString(observe?"net-data-query":"net-tx") << ",\"index\":" << index
        << ",\"cache_only\":" << (observe?"true":"false") << ",\"retry_allowed\":false,\"ssh_verified\":false"
        << ",\"bytes_returned\":" << (observe?data.bytes:tx.bytes)
        << ",\"timed_out\":" << ((observe?data.timedOut:tx.timedOut)?"true":"false");
    if (!valid) {
        out << ",\"error\":";
        DWORD code=observe?data.code:tx.code;
        if(code!=ERROR_SUCCESS) appendWin32(out,observe?data.operation:tx.operation,code);
        else out << "{\"kind\":\"invalid_response\"}";
    } else if(observe) {
        const auto& r=data.response;
        out << ",\"version\":" << r.version << ",\"size\":" << r.size
            << ",\"session_id\":" << jsonString(std::to_string(r.session_id))
            << ",\"generation\":" << jsonString(std::to_string(r.generation))
            << ",\"epoch\":" << jsonString(std::to_string(r.epoch));
#define NNPI_DATA_JSON(name) out << ",\"" #name "\":" << jsonString(std::to_string(r.name))
        NNPI_DATA_JSON(rx_received); NNPI_DATA_JSON(rx_bytes); NNPI_DATA_JSON(rx_dropped);
        NNPI_DATA_JSON(rx_bad_length); NNPI_DATA_JSON(rx_ack_sent); NNPI_DATA_JSON(tx_submitted);
        NNPI_DATA_JSON(tx_acked); NNPI_DATA_JSON(invalid_descriptor); NNPI_DATA_JSON(unknown_ack);
        NNPI_DATA_JSON(duplicate_rx); NNPI_DATA_JSON(fifo_full);
#undef NNPI_DATA_JSON
        out << ",\"rx_ack_pending\":" << r.rx_ack_pending << ",\"tx_busy\":" << r.tx_busy
            << ",\"tx_pending\":" << r.tx_pending << ",\"rx_queued\":" << r.rx_queued
            << ",\"fault_retained\":" << (r.fault_retained?"true":"false")
            << ",\"last_emit_halves\":" << r.last_emit_halves
            << ",\"interpretation\":\"cached page ownership; bounded RX queue has no Windows network consumer\"";
    } else {
        const auto& r=tx.response;
        out << ",\"version\":" << r.version << ",\"size\":" << r.size
            << ",\"session_id\":" << jsonString(std::to_string(r.session_id))
            << ",\"generation\":" << jsonString(std::to_string(r.generation))
            << ",\"epoch\":" << jsonString(std::to_string(r.epoch))
            << ",\"accepted\":true,\"slot\":" << r.slot
            << ",\"interpretation\":\"queued only; wait for real card ACK; never automatically retry\"";
    }
    out << '}'; return emit(out.str(),valid?0:1);
}

int run(int argc, wchar_t** argv) {
    if (argc>1 && (std::wcscmp(argv[1],L"net-data-query")==0 || std::wcscmp(argv[1],L"net-tx")==0))
        return runNetDataCommand(argc,argv);
    if (argc > 1 && std::wcscmp(argv[1], L"runtime-info") == 0)
        return runRuntimeInfoCommand(argc, argv);
    if (argc>1 && (std::wcscmp(argv[1],L"net-query")==0 ||
        std::wcscmp(argv[1],L"net-begin")==0))
        return runNetCommand(argc,argv);
    if (argc>1 && (std::wcscmp(argv[1],L"boot-query")==0 ||
        std::wcscmp(argv[1],L"boot-load")==0 || std::wcscmp(argv[1],L"boot-submit")==0))
        return runBootCommand(argc,argv);
    const char* resetCommand = nullptr;
    const char* hostReadyCommand = nullptr;
    const char* sysinfoCommand = nullptr;
    uint64_t resetGeneration=0, resetEpoch=0;
    if ((argc==4 && std::wcscmp(argv[1],L"sysinfo-query")==0) ||
        (argc==8 && std::wcscmp(argv[1],L"sysinfo-submit")==0)) {
        if (std::wcscmp(argv[2],L"--index")) return error("arguments","SYSINFO requires --index N",2);
        sysinfoCommand=argc==4 ? "sysinfo-query" : "sysinfo-submit";
        if (argc==8) {
            if (std::wcscmp(argv[4],L"--generation") || std::wcscmp(argv[6],L"--epoch"))
                return error("arguments","SYSINFO submit requires --generation G --epoch E",2);
            resetGeneration=decimal64(argv[5]); resetEpoch=decimal64(argv[7]);
            if (!resetGeneration || !resetEpoch || resetGeneration==UINT64_MAX || resetEpoch==UINT64_MAX)
                return error("arguments","invalid generation/epoch",2);
        }
        argc=4;
    }
    if ((argc==4 && std::wcscmp(argv[1],L"reset-query")==0) ||
        (argc==8 && (std::wcscmp(argv[1],L"reset-submit")==0 ||
                     std::wcscmp(argv[1],L"reset-revalidate")==0))) {
        if (std::wcscmp(argv[2],L"--index")!=0)
            return error("arguments","reset requires --index N",2);
        resetCommand=std::wcscmp(argv[1],L"reset-query")==0 ? "reset-query" :
            std::wcscmp(argv[1],L"reset-submit")==0 ? "reset-submit" : "reset-revalidate";
        if (argc==8) {
            if (std::wcscmp(argv[4],L"--generation") || std::wcscmp(argv[6],L"--epoch"))
                return error("arguments","reset requires --generation G --epoch E",2);
            resetGeneration=decimal64(argv[5]); resetEpoch=decimal64(argv[7]);
            if (!resetGeneration || !resetEpoch) return error("arguments","nonzero generation/epoch required",2);
        }
        argc=4;
    }
    if (argc==8 && (std::wcscmp(argv[1],L"host-ready")==0 ||
                    std::wcscmp(argv[1],L"host-ready-observe")==0)) {
        if (std::wcscmp(argv[2],L"--index") || std::wcscmp(argv[4],L"--generation") ||
            std::wcscmp(argv[6],L"--epoch")) return error("arguments","host-ready requires --index N --generation G --epoch E",2);
        hostReadyCommand=std::wcscmp(argv[1],L"host-ready")==0 ? "host-ready" : "host-ready-observe";
        resetGeneration=decimal64(argv[5]); resetEpoch=decimal64(argv[7]);
        if (!resetGeneration || !resetEpoch || resetGeneration==UINT64_MAX || resetEpoch==UINT64_MAX)
            return error("arguments","invalid generation/epoch",2);
        argc=4;
    }
    bool info = false;
    bool diagnostic = false;
    bool statusCommand = false;
    bool snapshotCommand = false;
    bool dmaCheckCommand = false;
    bool resetCapsCommand = false;
    size_t index = 0;
    if (argc == 1 || (argc == 2 && std::wcscmp(argv[1], L"list") == 0)) {
        // Enumeration only: deliberately never calls CreateFileW or DeviceIoControl.
    } else if (argc == 4 && (resetCommand || hostReadyCommand || sysinfoCommand || std::wcscmp(argv[1], L"info") == 0 ||
            std::wcscmp(argv[1], L"diag") == 0 ||
            std::wcscmp(argv[1], L"status") == 0 ||
            std::wcscmp(argv[1], L"snapshot") == 0 ||
            std::wcscmp(argv[1], L"dmacheck") == 0 ||
            std::wcscmp(argv[1], L"resetcaps") == 0) &&
            std::wcscmp(argv[2], L"--index") == 0 && argv[3][0]) {
        info = true;
        diagnostic = std::wcscmp(argv[1], L"diag") == 0;
        statusCommand = std::wcscmp(argv[1], L"status") == 0;
        snapshotCommand = std::wcscmp(argv[1], L"snapshot") == 0;
        dmaCheckCommand = std::wcscmp(argv[1], L"dmacheck") == 0;
        resetCapsCommand = std::wcscmp(argv[1], L"resetcaps") == 0;
        for (const wchar_t* digit = argv[3]; *digit; ++digit) {
            if (*digit < L'0' || *digit > L'9')
                return error("arguments", "--index must be an unsigned decimal integer", 2);
            const size_t value = static_cast<size_t>(*digit - L'0');
            if (index > ((std::numeric_limits<size_t>::max)() - value) / 10)
                return error("arguments", "--index is too large", 2);
            index = index * 10 + value;
        }
    } else {
        return error("arguments", "Usage: nnpi-info [list] | nnpi-info runtime-info --index N | nnpi-info info --index N | nnpi-info diag --index N | nnpi-info status --index N | nnpi-info snapshot --index N | nnpi-info dmacheck --index N | nnpi-info resetcaps --index N | nnpi-info host-ready[-observe] --index N --generation G --epoch E | nnpi-info sysinfo-submit --index N --generation G --epoch E | nnpi-info sysinfo-query --index N (zero-based; required even for one device)", 2);
    }

    const auto paths = enumerateInterfaces();
    std::ostringstream out;
    out.imbue(std::locale::classic());
    if (!info) {
        out << "{\"ok\":true,\"command\":\"list\",\"enumeration_only\":true"
            << ",\"index_scope\":\"current enumeration; order may change between invocations\""
            << ",\"count\":" << paths.size() << ",\"interfaces\":[";
        for (size_t i = 0; i < paths.size(); ++i) {
            if (i) out << ',';
            out << "{\"index\":" << i << ",\"interface_path\":"
                << jsonString(utf8(paths[i])) << '}';
        }
        out << ']';
        appendScope(out);
        out << '}';
        return emit(out.str(), 0);
    }
    if (index >= paths.size())
        return error("arguments", "--index is outside the current interface enumeration", 2);

    const std::string path = utf8(paths[index]);
    const Handle device(CreateFileW(paths[index].c_str(), GENERIC_READ | ((resetCommand || hostReadyCommand || (sysinfoCommand && std::string(sysinfoCommand)=="sysinfo-submit")) ? GENERIC_WRITE : 0),
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, nullptr));
    if (device.get() == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        out << "{\"ok\":false,\"command\":" << jsonString(sysinfoCommand ? sysinfoCommand : resetCommand ? resetCommand : hostReadyCommand ? hostReadyCommand : resetCapsCommand ? "resetcaps" : dmaCheckCommand ? "dmacheck" : snapshotCommand ? "snapshot" : statusCommand ? "status" : diagnostic ? "diag" : "info") << ",\"index\":" << index
            << ",\"interface_path\":" << jsonString(path) << ",\"error\":";
        appendWin32(out, "CreateFileW(GENERIC_READ, OPEN_EXISTING, FILE_FLAG_OVERLAPPED)", code);
        out << ",\"hardware_boot_verified\":false,\"compute_verified\":false";
        out << '}';
        return emit(out.str(), 1);
    }
    if (sysinfoCommand) return runSysinfo(device.get(),sysinfoCommand,index,path,resetGeneration,resetEpoch);
    if (hostReadyCommand) return runHostReady(device.get(),hostReadyCommand,resetGeneration,resetEpoch);
    if (resetCommand) return runReset(device.get(),resetCommand,resetGeneration,resetEpoch);
    if (resetCapsCommand) return runResetCaps(device.get(), index, path);
    if (dmaCheckCommand) return runDmaCheck(device.get(), index, path);
    if (snapshotCommand) return runSnapshot(device.get(), index, path);
    if (statusCommand) return runStatus(device.get(), index, path);
    if (diagnostic) return runDiagnostic(device.get(), index, path);
    const QueryResult result = query<nnpi_info_response, nnpi_info_request>(
        device.get(), NNPI_INFO_IOCTL, NNPI_INFO_VERSION);
    // Reject short, oversized, unknown or malformed responses via the shared ABI validator.
    const bool valid = result.code == ERROR_SUCCESS &&
        nnpi_info_response_valid(&result.response, result.bytes);
    out << "{\"ok\":" << (valid ? "true" : "false")
        << ",\"command\":\"info\",\"index\":" << index
        << ",\"interface_path\":" << jsonString(path);
    appendScope(out);
    if (result.code != ERROR_SUCCESS) {
        out << ",\"error\":";
        appendWin32(out, result.operation, result.code);
        out << ",\"timed_out\":" << (result.timedOut ? "true" : "false");
        if (result.cancelled) {
            out << ",\"cancel_result\":";
            appendWin32(out, "CancelIoEx", result.cancelCode);
            out << ",\"final_completion\":";
            appendWin32(out, "GetOverlappedResult(wait=TRUE)", result.completionCode);
            out << ",\"timeout_note\":\"5000 ms initial wait; cancellation drain may take longer; no hard real-time guarantee\"";
        }
    } else if (!valid) {
        out << ",\"error\":{\"kind\":\"invalid_response\",\"validator\":\"nnpi_info_response_valid\""
            << ",\"message\":\"Short, unknown or malformed response rejected\""
            << ",\"bytes_returned\":" << result.bytes << ",\"expected_bytes\":576}";
    } else {
        const auto& r = result.response;
        out << ",\"version\":" << r.version << ",\"size\":" << r.size
            << ",\"driver_version\":" << r.driver_version << ",\"flags\":" << r.flags
            << ",\"prepared\":" << ((r.flags & NNPI_INFO_FLAG_PREPARED) ? "true" : "false")
            << ",\"d0\":" << ((r.flags & NNPI_INFO_FLAG_D0) ? "true" : "false")
            << ",\"vendor\":" << r.vendor << ",\"device\":" << r.device
            << ",\"identity_source\":" << r.identity_source
            << ",\"identity_source_name\":\"PnP hardware IDs\""
            // String preserves all 64 bits for JSON consumers using IEEE-754 numbers.
            << ",\"generation\":" << jsonString(std::to_string(r.generation))
            << ",\"resource_count\":" << r.resource_count << ",\"resources\":[";
        for (uint32_t i = 0; i < r.resource_count; ++i) {
            if (i) out << ',';
            const auto& resource = r.resources[i];
            out << "{\"translated_resource_index\":" << resource.translated_resource_index
                << ",\"type\":" << resource.type
                << ",\"type_name\":" << jsonString(resourceType(resource.type))
                << ",\"flags\":" << resource.flags << ",\"valid\":" << resource.valid
                << ",\"start\":" << jsonString(hex64(resource.start))
                << ",\"length\":" << jsonString(hex64(resource.length)) << '}';
        }
        out << ']';
    }
    out << '}';
    return emit(out.str(), valid ? 0 : 1);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        return run(argc, argv);
    } catch (const Win32Failure& failure) {
        try {
            std::ostringstream out;
            out.imbue(std::locale::classic());
            out << "{\"ok\":false,\"error\":";
            appendWin32(out, failure.operation, failure.code);
            out << ",\"hardware_boot_verified\":false,\"compute_verified\":false";
            out << '}';
            return emit(out.str(), 1);
        } catch (...) {
            // ASCII-only fallback also preserves the original numeric Win32 error.
            std::fprintf(stderr, "{\"ok\":false,\"error\":{\"kind\":\"win32\","
                "\"win32_code\":%lu,\"message\":\"Unable to format Win32 error\"},"
                "\"data_scope\":\"no-valid-sample-in-this-output\",\"hardware_boot_verified\":false,"
                "\"compute_verified\":false}\n", static_cast<unsigned long>(failure.code));
            return 1;
        }
    } catch (...) {
        // Do not assume exception::what() uses UTF-8 on Windows.
        std::fputs("{\"ok\":false,\"error\":{\"kind\":\"internal\","
            "\"message\":\"Allocation, encoding or internal failure\"},"
            "\"data_scope\":\"no-valid-sample-in-this-output\",\"hardware_boot_verified\":false,"
            "\"compute_verified\":false}\n", stderr);
        return 1;
    }
}
