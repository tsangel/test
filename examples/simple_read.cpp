#include <iostream>

#include <dicom.h>

int main(int argc, char* argv[]) {
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
              << " (bytes=" << file->stream().datasize() << ")\n";
    return 0;
}
