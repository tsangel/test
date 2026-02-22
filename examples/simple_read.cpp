#include <iostream>

#include <dicom.h>

int main(int argc, char* argv[]) {
    using namespace dicom::literals;

    if (argc < 2) {
        std::cerr << "Usage: simple_read <file.dcm>\n";
        return 1;
    }

    const std::string path = argv[1];
    auto file = dicom::read_file(path);
    if (!file) {
        std::cerr << "Failed to read: " << path << "\n";
        return 1;
    }

    std::cout << "Loaded: " << file->path()
              << " (bytes=" << file->stream().attached_size() << ")\n";

    auto& dataset = file->dataset();
    const long row_count = dataset["Rows"_tag].to_long().value_or(0);
    const long col_count = dataset["Columns"_tag].to_long().value_or(0);
    std::cout << "Rows: " << row_count << "\n";
    std::cout << "Columns: " << col_count << "\n";

    return 0;
}
