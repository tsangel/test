#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include <dicom.h>

using namespace std::chrono;

namespace {

constexpr std::size_t kElementCount = 3000;
constexpr int kIterations = 200;

class DataSetNoPool {
public:
	dicom::DataElement* add_dataelement(dicom::Tag tag, dicom::VR vr,
	    std::size_t offset, std::size_t length) {
		auto element = std::make_unique<dicom::DataElement>(tag, vr, length, offset, nullptr);
		auto* raw = element.get();
		elements_.emplace_back(tag, std::move(element));
		return raw;
	}

private:
	std::vector<std::pair<dicom::Tag, std::unique_ptr<dicom::DataElement>>> elements_;
};

template <typename Fn>
auto measure_ns(Fn&& fn) {
	const auto start = steady_clock::now();
	fn();
	return duration_cast<nanoseconds>(steady_clock::now() - start).count();
}

void run_benchmark() {
	std::int64_t pooled_total = 0;
	std::int64_t plain_total = 0;

	for (int i = 0; i < kIterations; ++i) {
		pooled_total += measure_ns([] {
			dicom::DataSet data_set;
			for (std::size_t idx = 0; idx < kElementCount; ++idx) {
				const auto tag = dicom::Tag(static_cast<std::uint16_t>(0x0010 + idx), 0x0001);
				data_set.add_dataelement(tag, dicom::VR::SQ, idx, 0);
			}
		});

		plain_total += measure_ns([] {
			DataSetNoPool data_set;
			for (std::size_t idx = 0; idx < kElementCount; ++idx) {
				const auto tag = dicom::Tag(static_cast<std::uint16_t>(0x0010 + idx), 0x0001);
				data_set.add_dataelement(tag, dicom::VR::SQ, idx, 0);
			}
		});
	}

	const double pooled_avg_us = static_cast<double>(pooled_total) / kIterations / 1000.0;
	const double plain_avg_us = static_cast<double>(plain_total) / kIterations / 1000.0;

	std::cout << "Benchmark results for adding " << kElementCount << " elements\n";
	std::cout << "Iterations\t: " << kIterations << "\n";
	std::cout << "DataSet (pmr)\tavg: " << pooled_avg_us << " us\n";
	std::cout << "DataSetNoPool\tavg: " << plain_avg_us << " us\n";
}

} // namespace

int main() {
	run_benchmark();
	return 0;
}
