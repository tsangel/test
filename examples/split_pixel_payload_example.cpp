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

/*
This example demonstrates the DicomSDL split PixelData convention.

There are two separate workflows:

1. Attach and read an already split instance:
   - main P10 DICOM contains (7FE0,0010) PixelData as a 4-byte "DXP1" marker.
   - pixel-payload.bin contains the complete PixelData value bytes.
   - For encapsulated transfer syntaxes, that value starts with the Basic Offset
     Table item and includes all fragment items plus the sequence delimiter.

2. Split a normal DICOM instance:
   - write_bytes_split_pixel_payload() serializes normal metadata into the main
     DICOM bytes and moves PixelData value bytes into a separate buffer.
   - write_with_transfer_syntax_split_pixel_payload() does the same while
     serializing through a requested target transfer syntax.

The external pixel payload memory is caller-owned. Detach before freeing it.
detach_pixel_payload() keeps only the lightweight DXP1 marker; pass true if you
want the detached marker to retain PixelData dump text for diagnostics.
*/

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

void write_all_bytes(const std::string& path,
    const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to create: " + path);
    }
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    if (!out) {
        throw std::runtime_error("failed to write: " + path);
    }
}

template <typename ByteRange>
std::string hex_prefix(const ByteRange& bytes, std::size_t max_count = 16) {
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

bool is_placeholder_pixel_data(const dicom::DataElement& pixel_data) {
    const auto value = pixel_data.value_span();
    return value.size() == dicom::kPixelPayloadPlaceholderMagic.size() &&
        std::equal(value.begin(), value.end(),
            dicom::kPixelPayloadPlaceholderMagic.begin());
}

void print_decode_summary(dicom::DicomFile& file, std::size_t frame_index) {
    try {
        const auto decoded = file.pixel_data(frame_index);
        std::cout << "Decoded frame " << frame_index << ": "
                  << decoded.size() << " bytes";
        if (!decoded.empty()) {
            std::cout << " [" << hex_prefix(decoded) << "]";
        }
        std::cout << "\n";
    } catch (const std::exception& ex) {
        std::cout << "Decoded frame " << frame_index
                  << ": unavailable (" << ex.what() << ")\n";
    }
}

void print_encoded_summary_if_available(dicom::DicomFile& file,
    std::size_t frame_index) {
    auto& pixel_data = file.get_dataelement("PixelData"_tag);
    if (!pixel_data.vr().is_pixel_sequence()) {
        return;
    }
    try {
        const auto encoded = file.encoded_pixel_frame_view(frame_index);
        std::cout << "Encoded frame " << frame_index << ": "
                  << encoded.size() << " bytes";
        if (!encoded.empty()) {
            std::cout << " [" << hex_prefix(encoded) << "]";
        }
        std::cout << "\n";
    } catch (const std::exception& ex) {
        std::cout << "Encoded frame " << frame_index
                  << ": unavailable (" << ex.what() << ")\n";
    }
}

void print_attached_summary(dicom::DicomFile& file, std::size_t frame_index) {
    auto& dataset = file.dataset();
    auto& pixel_data = file.get_dataelement("PixelData"_tag);

    std::cout << "TransferSyntaxUID: "
              << (file.transfer_syntax_uid().valid()
                     ? file.transfer_syntax_uid().value()
                     : "<missing>")
              << "\n";
    std::cout << "Rows x Columns: "
              << dataset["Rows"_tag].to_long().value_or(0) << " x "
              << dataset["Columns"_tag].to_long().value_or(0) << "\n";
    std::cout << "PixelData VR after attach: " << pixel_data.vr().str()
              << "\n";
    std::cout << "Attached payload: "
              << (file.has_attached_pixel_payload() ? "yes" : "no") << "\n";

    print_encoded_summary_if_available(file, frame_index);
    print_decode_summary(file, frame_index);
}

int run_read_split_example(const std::string& name,
    std::vector<std::uint8_t>& main_p10,
    std::vector<std::uint8_t>& pixel_payload, std::size_t frame_index) {
    auto file = dicom::read_bytes_with_pixel_payload(name,
        main_p10.data(), main_p10.size(),
        pixel_payload.data(), pixel_payload.size());

    std::cout << "Loaded split DICOM: " << file->path() << "\n";
    print_attached_summary(*file, frame_index);

    // After decode, the caller can detach and release the external payload.
    // Pass true to keep PixelData dump text inside the detached marker.
    file->detach_pixel_payload();
    pixel_payload.clear();
    pixel_payload.shrink_to_fit();

    std::cout << "Attached payload after detach: "
              << (file->has_attached_pixel_payload() ? "yes" : "no") << "\n";
    std::cout << "Rows still available after detach: "
              << file->dataset()["Rows"_tag].to_long().value_or(0) << "\n";
    std::cout << "After detach, do not call pixel decode APIs until a payload "
              << "is attached again.\n";

    return 0;
}

int run_split_source_example(const std::string& source_path,
    const std::string& main_out_path, const std::string& payload_out_path,
    const std::string& transfer_syntax_text, std::size_t frame_index) {
    auto source = dicom::read_file(source_path);
    dicom::SplitPixelPayloadWriteResult split;

    if (transfer_syntax_text.empty()) {
        split = source->write_bytes_split_pixel_payload();
        std::cout << "Split source with its current transfer syntax.\n";
    } else {
        const auto transfer_syntax = dicom::uid::lookup_or_throw(transfer_syntax_text);
        split = source->write_with_transfer_syntax_split_pixel_payload(
            transfer_syntax);
        std::cout << "Split source through target transfer syntax: "
                  << transfer_syntax.value() << "\n";
    }

    write_all_bytes(main_out_path, split.dicom_bytes);
    write_all_bytes(payload_out_path, split.pixel_payload_bytes);

    std::cout << "Wrote main DICOM: " << main_out_path << " ("
              << split.dicom_bytes.size() << " bytes)\n";
    std::cout << "Wrote PixelData payload: " << payload_out_path << " ("
              << split.pixel_payload_bytes.size() << " bytes)\n";

    auto placeholder_only = dicom::read_bytes("split-main-placeholder-check",
        split.dicom_bytes.data(), split.dicom_bytes.size());
    if (!is_placeholder_pixel_data(
            placeholder_only->get_dataelement("PixelData"_tag))) {
        throw std::runtime_error(
            "split main DICOM does not contain the DXP1 PixelData placeholder");
    }
    std::cout << "Verified main DICOM has the DXP1 PixelData placeholder.\n";

    auto rejoined = dicom::read_bytes_with_pixel_payload(
        "split-roundtrip-check", split.dicom_bytes.data(),
        split.dicom_bytes.size(), split.pixel_payload_bytes.data(),
        split.pixel_payload_bytes.size());
    std::cout << "Reattached split payload for a roundtrip check.\n";
    print_attached_summary(*rejoined, frame_index);

    rejoined->detach_pixel_payload();
    std::cout << "Detached roundtrip payload; metadata remains available.\n";
    return 0;
}

void print_usage(const char* program) {
    std::cerr
        << "Usage:\n"
        << "  " << program << "\n"
        << "  " << program << " <main-p10.dcm> <pixel-payload.bin> [frame]\n"
        << "  " << program << " --split <source.dcm> <main-out.dcm>"
        << " <pixel-payload-out.bin> [frame]\n"
        << "  " << program << " --split-ts <source.dcm> <transfer-syntax>"
        << " <main-out.dcm> <pixel-payload-out.bin> [frame]\n\n"
        << "No arguments:\n"
        << "  Run a tiny built-in native PixelData attach/detach demo.\n\n"
        << "Read split inputs:\n"
        << "  main-p10.dcm must contain (7FE0,0010) PixelData as the fixed\n"
        << "  4-byte DXP1 placeholder. pixel-payload.bin must contain the\n"
        << "  complete PixelData value bytes. For encapsulated payloads, that\n"
        << "  means Basic Offset Table item, fragment items, and sequence\n"
        << "  delimiter.\n\n"
        << "Create split outputs:\n"
        << "  --split keeps the source transfer syntax. --split-ts serializes\n"
        << "  through a target transfer syntax before splitting. Use a UID value\n"
        << "  or keyword such as ExplicitVRLittleEndian, ExplicitVRBigEndian,\n"
        << "  or RLELossless.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 1) {
            auto main_p10 = build_demo_main_p10();
            std::vector<std::uint8_t> pixel_payload{
                0x34u, 0x12u, 0x56u, 0x78u, 0x9Au, 0xBCu};
            return run_read_split_example(
                "built-in-split-pixel-payload-demo", main_p10, pixel_payload, 0);
        }

        const std::string command = argv[1];
        if (command == "--split") {
            if (argc < 5 || argc > 6) {
                print_usage(argv[0]);
                return 1;
            }
            const auto frame_index = argc == 6
                ? static_cast<std::size_t>(std::stoull(argv[5]))
                : std::size_t{0};
            return run_split_source_example(
                argv[2], argv[3], argv[4], {}, frame_index);
        }

        if (command == "--split-ts") {
            if (argc < 6 || argc > 7) {
                print_usage(argv[0]);
                return 1;
            }
            const auto frame_index = argc == 7
                ? static_cast<std::size_t>(std::stoull(argv[6]))
                : std::size_t{0};
            return run_split_source_example(
                argv[2], argv[4], argv[5], argv[3], frame_index);
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

        return run_read_split_example(argv[1], main_p10, pixel_payload, frame_index);
    } catch (const std::exception& ex) {
        std::cerr << "split_pixel_payload_example: " << ex.what() << "\n";
        return 1;
    }
}
