#include "nstu/screen_snapshot.hpp"

#include "nstu/control_messages.hpp"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

namespace nstu::screen {
namespace {

using Microsoft::WRL::ComPtr;

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

class ComScope {
public:
    ComScope() noexcept {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        uninitialize_ = result == S_OK || result == S_FALSE;
        ready_ = SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
    }
    ~ComScope() {
        if (uninitialize_) {
            CoUninitialize();
        }
    }
    [[nodiscard]] bool ready() const noexcept { return ready_; }

private:
    bool ready_ = false;
    bool uninitialize_ = false;
};

class ScreenDc {
public:
    ScreenDc() noexcept : handle_(GetDC(nullptr)) {}
    ~ScreenDc() {
        if (handle_ != nullptr) {
            ReleaseDC(nullptr, handle_);
        }
    }
    [[nodiscard]] HDC get() const noexcept { return handle_; }

private:
    HDC handle_ = nullptr;
};

class MemoryDc {
public:
    explicit MemoryDc(HDC compatible) noexcept
        : handle_(CreateCompatibleDC(compatible)) {}
    ~MemoryDc() {
        if (handle_ != nullptr) {
            DeleteDC(handle_);
        }
    }
    [[nodiscard]] HDC get() const noexcept { return handle_; }

private:
    HDC handle_ = nullptr;
};

class BitmapHandle {
public:
    explicit BitmapHandle(HBITMAP handle = nullptr) noexcept
        : handle_(handle) {}
    ~BitmapHandle() {
        if (handle_ != nullptr) {
            DeleteObject(handle_);
        }
    }
    BitmapHandle(const BitmapHandle&) = delete;
    BitmapHandle& operator=(const BitmapHandle&) = delete;
    [[nodiscard]] HBITMAP get() const noexcept { return handle_; }

private:
    HBITMAP handle_ = nullptr;
};

bool create_factory(ComPtr<IWICImagingFactory>& factory,
                    std::string* error) {
    if (FAILED(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)))) {
        set_error(error, "Windows Imaging Component is unavailable");
        return false;
    }
    return true;
}

bool encode_bitmap(HBITMAP bitmap, std::uint16_t width, std::uint16_t height,
                   std::uint8_t quality, JpegImage& image,
                   std::string* error) {
    ComScope com;
    ComPtr<IWICImagingFactory> factory;
    if (!com.ready() || !create_factory(factory, error)) {
        return false;
    }
    ComPtr<IWICBitmap> source;
    if (FAILED(factory->CreateBitmapFromHBITMAP(
            bitmap, nullptr, WICBitmapIgnoreAlpha, &source))) {
        set_error(error, "snapshot bitmap conversion failed");
        return false;
    }
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
        set_error(error, "snapshot memory stream creation failed");
        return false;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr,
                                      &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        set_error(error, "JPEG encoder initialization failed");
        return false;
    }
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (FAILED(encoder->CreateNewFrame(&frame, &properties))) {
        set_error(error, "JPEG frame creation failed");
        return false;
    }
    PROPBAG2 option{};
    option.pstrName = const_cast<wchar_t*>(L"ImageQuality");
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = static_cast<float>(quality) / 100.0f;
    const HRESULT property_result = properties->Write(1, &option, &value);
    VariantClear(&value);
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(property_result) || FAILED(frame->Initialize(properties.Get())) ||
        FAILED(frame->SetSize(width, height)) ||
        FAILED(frame->SetPixelFormat(&pixel_format)) ||
        FAILED(frame->WriteSource(source.Get(), nullptr)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        set_error(error, "JPEG snapshot encoding failed");
        return false;
    }
    HGLOBAL memory = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &memory)) ||
        memory == nullptr) {
        set_error(error, "JPEG snapshot buffer is unavailable");
        return false;
    }
    STATSTG statistics{};
    if (FAILED(stream->Stat(&statistics, STATFLAG_NONAME)) ||
        statistics.cbSize.QuadPart == 0 ||
        statistics.cbSize.QuadPart >
            static_cast<ULONGLONG>(std::numeric_limits<std::size_t>::max())) {
        set_error(error, "JPEG snapshot stream size is unavailable");
        return false;
    }
    const std::size_t byte_count =
        static_cast<std::size_t>(statistics.cbSize.QuadPart);
    const SIZE_T allocation_size = GlobalSize(memory);
    if (allocation_size < byte_count) {
        set_error(error, "JPEG snapshot stream allocation is truncated");
        return false;
    }
    const auto* bytes = static_cast<const std::byte*>(GlobalLock(memory));
    if (bytes == nullptr || byte_count == 0) {
        set_error(error, "JPEG snapshot buffer lock failed");
        return false;
    }
    std::vector<std::byte> encoded;
    try {
        encoded.assign(bytes, bytes + byte_count);
    } catch (const std::bad_alloc&) {
        GlobalUnlock(memory);
        set_error(error, "JPEG snapshot buffer allocation failed");
        return false;
    }
    GlobalUnlock(memory);
    image.width = width;
    image.height = height;
    image.bytes = std::move(encoded);
    return true;
}

bool capture_scaled(std::uint16_t maximum_width,
                    std::uint16_t maximum_height, std::uint8_t quality,
                    JpegImage& image, std::string* error) {
    const int source_width = GetSystemMetrics(SM_CXSCREEN);
    const int source_height = GetSystemMetrics(SM_CYSCREEN);
    if (source_width <= 0 || source_height <= 0) {
        set_error(error, "primary screen dimensions are unavailable");
        return false;
    }
    const double scale = std::min(
        static_cast<double>(maximum_width) / source_width,
        static_cast<double>(maximum_height) / source_height);
    const auto width = static_cast<std::uint16_t>(std::clamp(
        static_cast<int>(source_width * std::min(scale, 1.0)), 1,
        static_cast<int>(maximum_width)));
    const auto height = static_cast<std::uint16_t>(std::clamp(
        static_cast<int>(source_height * std::min(scale, 1.0)), 1,
        static_cast<int>(maximum_height)));
    ScreenDc screen;
    MemoryDc memory(screen.get());
    BitmapHandle bitmap(screen.get() == nullptr ? nullptr :
        CreateCompatibleBitmap(screen.get(), width, height));
    if (screen.get() == nullptr || memory.get() == nullptr ||
        bitmap.get() == nullptr) {
        set_error(error, "screen capture resources are unavailable");
        return false;
    }
    const HGDIOBJ previous = SelectObject(memory.get(), bitmap.get());
    SetStretchBltMode(memory.get(), HALFTONE);
    SetBrushOrgEx(memory.get(), 0, 0, nullptr);
    const BOOL copied = StretchBlt(
        memory.get(), 0, 0, width, height, screen.get(), 0, 0, source_width,
        source_height, SRCCOPY | CAPTUREBLT);
    SelectObject(memory.get(), previous);
    if (!copied) {
        set_error(error, "primary screen capture failed");
        return false;
    }
    return encode_bitmap(bitmap.get(), width, height, quality, image, error);
}

} // namespace

bool capture_primary_screen_jpeg(JpegImage& image,
                                 std::uint16_t maximum_width,
                                 std::uint16_t maximum_height,
                                 std::uint8_t quality,
                                 std::size_t maximum_bytes,
                                 std::string* error) {
    image = {};
    if (maximum_width < 160 || maximum_height < 90 || quality < 20 ||
        quality > 90 || maximum_bytes < 4096 ||
        maximum_width > control::kMaximumSnapshotWidth ||
        maximum_height > control::kMaximumSnapshotHeight ||
        maximum_bytes > control::kMaximumSnapshotJpegBytes) {
        set_error(error, "invalid snapshot capture limits");
        return false;
    }
    std::uint16_t width = maximum_width;
    std::uint16_t height = maximum_height;
    std::uint8_t attempt_quality = quality;
    for (int attempt = 0; attempt < 4; ++attempt) {
        JpegImage candidate;
        if (!capture_scaled(width, height, attempt_quality, candidate, error)) {
            return false;
        }
        if (candidate.bytes.size() <= maximum_bytes) {
            image = std::move(candidate);
            return true;
        }
        width = static_cast<std::uint16_t>(std::max(160, width * 4 / 5));
        height = static_cast<std::uint16_t>(std::max(90, height * 4 / 5));
        attempt_quality = static_cast<std::uint8_t>(
            std::max(28, static_cast<int>(attempt_quality) - 8));
    }
    set_error(error, "snapshot could not fit the transport limit");
    return false;
}

bool decode_jpeg(const JpegImage& image, BgraImage& decoded,
                 std::string* error) {
    return decode_jpeg(
        std::span<const std::byte>(image.bytes.data(), image.bytes.size()),
        image.width, image.height, decoded, error);
}

bool decode_jpeg(std::span<const std::byte> bytes,
                 std::uint16_t expected_width,
                 std::uint16_t expected_height, BgraImage& decoded,
                 std::string* error) {
    decoded = {};
    if (bytes.empty() ||
        bytes.size() > control::kMaximumSnapshotJpegBytes ||
        expected_width == 0 || expected_height == 0 ||
        expected_width > control::kMaximumSnapshotWidth ||
        expected_height > control::kMaximumSnapshotHeight) {
        set_error(error, "invalid JPEG snapshot");
        return false;
    }
    ComScope com;
    ComPtr<IWICImagingFactory> factory;
    if (!com.ready() || !create_factory(factory, error)) {
        return false;
    }
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data())),
            static_cast<DWORD>(bytes.size())))) {
        set_error(error, "JPEG input stream creation failed");
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT width = 0;
    UINT height = 0;
    GUID container_format{};
    if (FAILED(factory->CreateDecoderFromStream(
             stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetContainerFormat(&container_format)) ||
        !IsEqualGUID(container_format, GUID_ContainerFormatJpeg) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
        width > control::kMaximumSnapshotWidth ||
        height > control::kMaximumSnapshotHeight ||
        width != expected_width || height != expected_height ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom))) {
        set_error(error, "JPEG snapshot decoding failed");
        return false;
    }
    if (width > std::numeric_limits<std::uint32_t>::max() / 4 ||
        height > std::numeric_limits<std::size_t>::max() / (width * 4u)) {
        set_error(error, "decoded snapshot dimensions overflow");
        return false;
    }
    decoded.width = width;
    decoded.height = height;
    decoded.stride = width * 4u;
    try {
        decoded.pixels.resize(static_cast<std::size_t>(decoded.stride) * height);
    } catch (const std::bad_alloc&) {
        decoded = {};
        set_error(error, "decoded snapshot allocation failed");
        return false;
    }
    if (FAILED(converter->CopyPixels(
            nullptr, decoded.stride,
            static_cast<UINT>(decoded.pixels.size()),
            reinterpret_cast<BYTE*>(decoded.pixels.data())))) {
        decoded = {};
        set_error(error, "decoded snapshot pixel copy failed");
        return false;
    }
    return true;
}

} // namespace nstu::screen
