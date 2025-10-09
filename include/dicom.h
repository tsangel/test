#include <memory>
#include <string>

class DicomFile {
public:
	explicit DicomFile(const std::string& path);
	DicomFile(const DicomFile&) = delete;
	DicomFile& operator=(const DicomFile&) = delete;
	DicomFile(DicomFile&&) noexcept = default;
	DicomFile& operator=(DicomFile&&) noexcept = default;

	static std::unique_ptr<DicomFile> attach(const std::string& path);
	const std::string& path() const;

private:
	std::string path_;
};
