#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <dicom.h>

using namespace dicom::literals;

namespace {

void append_u16_le(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void append_u32_le(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

void append_bytes(std::vector<std::uint8_t>& out,
    const std::vector<std::uint8_t>& bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

std::vector<std::uint8_t> padded_ui(std::string uid) {
    if (uid.empty() || uid.back() != '\0') {
        uid.push_back('\0');
    }
    if ((uid.size() & 1u) != 0u) {
        uid.push_back('\0');
    }
    return std::vector<std::uint8_t>(uid.begin(), uid.end());
}

void append_explicit_vr_le_16(std::vector<std::uint8_t>& out,
    dicom::Tag tag, char vr0, char vr1,
    const std::vector<std::uint8_t>& value) {
    append_u16_le(out, tag.group());
    append_u16_le(out, tag.element());
    out.push_back(static_cast<std::uint8_t>(vr0));
    out.push_back(static_cast<std::uint8_t>(vr1));
    append_u16_le(out, static_cast<std::uint16_t>(value.size()));
    append_bytes(out, value);
}

void append_explicit_vr_le_32(std::vector<std::uint8_t>& out,
    dicom::Tag tag, char vr0, char vr1,
    const std::vector<std::uint8_t>& value) {
    append_u16_le(out, tag.group());
    append_u16_le(out, tag.element());
    out.push_back(static_cast<std::uint8_t>(vr0));
    out.push_back(static_cast<std::uint8_t>(vr1));
    append_u16_le(out, 0);
    append_u32_le(out, static_cast<std::uint32_t>(value.size()));
    append_bytes(out, value);
}

std::vector<std::uint8_t> build_part10(std::string transfer_syntax_uid,
    const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> meta_ts;
    append_explicit_vr_le_16(meta_ts, dicom::Tag(0x0002u, 0x0010u),
        'U', 'I', padded_ui(std::move(transfer_syntax_uid)));

    std::vector<std::uint8_t> meta_length_value;
    append_u32_le(meta_length_value, static_cast<std::uint32_t>(meta_ts.size()));

    std::vector<std::uint8_t> meta_length;
    append_explicit_vr_le_16(meta_length, dicom::Tag(0x0002u, 0x0000u),
        'U', 'L', meta_length_value);

    std::vector<std::uint8_t> out(128, 0);
    out.insert(out.end(), {'D', 'I', 'C', 'M'});
    append_bytes(out, meta_length);
    append_bytes(out, meta_ts);
    append_bytes(out, body);
    return out;
}

std::vector<std::uint8_t> placeholder_magic() {
    return std::vector<std::uint8_t>(
        dicom::kPixelPayloadPlaceholderMagic.begin(),
        dicom::kPixelPayloadPlaceholderMagic.end());
}

std::vector<std::uint8_t> build_demo_main_p10() {
    std::vector<std::uint8_t> body;
    append_explicit_vr_le_16(body, "SamplesPerPixel"_tag, 'U', 'S',
        {0x01u, 0x00u});
    append_explicit_vr_le_16(body, "PhotometricInterpretation"_tag, 'C', 'S',
        {'M', 'O', 'N', 'O', 'C', 'H', 'R', 'O', 'M', 'E', '2', ' '});
    append_explicit_vr_le_16(body, "NumberOfFrames"_tag, 'I', 'S', {'1', ' '});
    append_explicit_vr_le_16(body, "Rows"_tag, 'U', 'S', {0x01u, 0x00u});
    append_explicit_vr_le_16(body, "Columns"_tag, 'U', 'S', {0x03u, 0x00u});
    append_explicit_vr_le_16(body, "BitsAllocated"_tag, 'U', 'S',
        {0x10u, 0x00u});
    append_explicit_vr_le_16(body, "BitsStored"_tag, 'U', 'S',
        {0x10u, 0x00u});
    append_explicit_vr_le_16(body, "HighBit"_tag, 'U', 'S', {0x0Fu, 0x00u});
    append_explicit_vr_le_16(body, "PixelRepresentation"_tag, 'U', 'S',
        {0x00u, 0x00u});

    // The main P10 keeps only the fixed DXP1 placeholder in PixelData.
    append_explicit_vr_le_32(body, "PixelData"_tag, 'O', 'B',
        placeholder_magic());

    return build_part10("1.2.840.10008.1.2.1", body);
}

std::vector<std::uint8_t> read_all_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open: " + path);
    }
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string hex_prefix(const std::vector<std::uint8_t>& bytes,
    std::size_t max_count = 16) {
    std::ostringstream out;
    const auto count = std::min(bytes.size(), max_count);
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) {
            out << ' ';
        }
        out << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned>(bytes[i]);
    }
    if (bytes.size() > count) {
        out << " ...";
    }
    return out.str();
}

int run_example(const std::string& name, std::vector<std::uint8_t>& main_p10,
    std::vector<std::uint8_t>& pixel_payload, std::size_t frame_index) {
    auto file = dicom::read_bytes_with_pixel_payload(name,
        main_p10.data(), main_p10.size(),
        pixel_payload.data(), pixel_payload.size());

    auto& dataset = file->dataset();
    auto& pixel_data = file->get_dataelement("PixelData"_tag);

    std::cout << "Loaded split DICOM: " << file->path() << "\n";
    std::cout << "TransferSyntaxUID: "
              << (file->transfer_syntax_uid().valid()
                     ? file->transfer_syntax_uid().value()
                     : "<missing>")
              << "\n";
    std::cout << "Rows x Columns: "
              << dataset["Rows"_tag].to_long().value_or(0) << " x "
              << dataset["Columns"_tag].to_long().value_or(0) << "\n";
    std::cout << "PixelData VR after attach: " << pixel_data.vr().str()
              << "\n";
    std::cout << "Attached payload: "
              << (file->has_attached_pixel_payload() ? "yes" : "no") << "\n";

    const auto decoded = file->pixel_data(frame_index);
    std::cout << "Decoded frame " << frame_index << ": "
              << decoded.size() << " bytes";
    if (!decoded.empty()) {
        std::cout << " [" << hex_prefix(decoded) << "]";
    }
    std::cout << "\n";

    // After decode, the caller can detach and release the external payload.
    file->detach_pixel_payload();
    pixel_payload.clear();
    pixel_payload.shrink_to_fit();

    std::cout << "Attached payload after detach: "
              << (file->has_attached_pixel_payload() ? "yes" : "no") << "\n";
    std::cout << "Rows still available after detach: "
              << dataset["Rows"_tag].to_long().value_or(0) << "\n";
    std::cout << "After detach, do not call pixel decode APIs until a payload "
              << "is attached again.\n";

    return 0;
}

void print_usage(const char* program) {
    std::cerr
        << "Usage:\n"
        << "  " << program << "\n"
        << "  " << program << " <main-p10.dcm> <pixel-payload.bin> [frame]\n\n"
        << "With no arguments, runs a tiny built-in native PixelData demo.\n"
        << "For encapsulated payloads, pixel-payload.bin must contain the full\n"
        << "PixelData value field: Basic Offset Table item, fragments, and\n"
        << "sequence delimiter.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 1) {
            auto main_p10 = build_demo_main_p10();
            std::vector<std::uint8_t> pixel_payload{
                0x34u, 0x12u, 0x56u, 0x78u, 0x9Au, 0xBCu};
            return run_example(
                "built-in-split-pixel-payload-demo", main_p10, pixel_payload, 0);
        }

        if (argc < 3 || argc > 4) {
            print_usage(argv[0]);
            return 1;
        }

        auto main_p10 = read_all_bytes(argv[1]);
        auto pixel_payload = read_all_bytes(argv[2]);
        const auto frame_index = argc == 4
            ? static_cast<std::size_t>(std::stoull(argv[3]))
            : std::size_t{0};

        return run_example(argv[1], main_p10, pixel_payload, frame_index);
    } catch (const std::exception& ex) {
        std::cerr << "split_pixel_payload_example: " << ex.what() << "\n";
        return 1;
    }
}
