#pragma once

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <fstream>
#include <vector>
#include <mutex>
#include <format>

namespace dicom {
namespace diag {

// Logging principles (concise):
// - Reporter adds [LEVEL]; message body starts with context: "context KEY=VALUE ... reason".
// - Parsing errors must include file, offset (hex), tag, vr (if known), length.
// - Error helpers that throw should log once, names end with _or_throw/throw_.
// - Warn/Error keywords: INVALID_TAG, UNEXPECTED_EOF, BAD_VR, LENGTH_MISMATCH,
//   CHARSET_UNSUPPORTED, UID_UNKNOWN.
// - Info logs are compiled out unless DICOMSDL_ENABLE_INFO_LOGS=1.

enum class LogLevel { Info, Warning, Error };

#ifndef DICOMSDL_ENABLE_INFO_LOGS
// Info logs are compiled out by default. Enable them by defining
// DICOMSDL_ENABLE_INFO_LOGS=1 in your build flags.
#define DICOMSDL_ENABLE_INFO_LOGS 0
#endif

// Unified exception type
class DicomException : public std::runtime_error {
public:
	explicit DicomException(std::string msg) : std::runtime_error(std::move(msg)) {}
};

class Reporter {
public:
	virtual ~Reporter() = default;

	virtual void report(LogLevel level, std::string_view message) = 0;

	void info(std::string_view msg) { report(LogLevel::Info, msg); }
	void warn(std::string_view msg) { report(LogLevel::Warning, msg); }
	void error(std::string_view msg) { report(LogLevel::Error, msg); }

	[[noreturn]] void error_and_throw(std::string_view msg) {
		report(LogLevel::Error, msg);
		throw DicomException(std::string(msg));
	}
};

// Default reporter that writes to stderr
class StderrReporter : public Reporter {
public:
	void report(LogLevel level, std::string_view message) override {
		const char* lvl = nullptr;
		switch (level) {
			case LogLevel::Info: lvl = "INFO"; break;
			case LogLevel::Warning: lvl = "WARN"; break;
			case LogLevel::Error: lvl = "ERROR"; break;
		}
		std::cerr << '[' << lvl << "] " << message << '\n';
	}
};

// Reporter that appends plain-text logs to a file (one line per message).
// Example:
//   auto file_rep = std::make_shared<dicom::diag::FileReporter>("dicom.log");
//   dicom::diag::set_default_reporter(file_rep);
//   dicom::diag::info("written to dicom.log");
class FileReporter : public Reporter {
public:
	explicit FileReporter(std::string path) : out_(std::move(path), std::ios::app) {}

	void report(LogLevel level, std::string_view message) override {
		if (!out_) return;
		out_ << '[' << level_to_str(level) << "] " << message << '\n';
		out_.flush();
	}

private:
	static const char* level_to_str(LogLevel level) {
		switch (level) {
			case LogLevel::Info: return "INFO";
			case LogLevel::Warning: return "WARN";
			case LogLevel::Error: return "ERROR";
		}
		return "UNKNOWN";
	}

	std::ofstream out_;
};

// Reporter that buffers messages in memory; handy for tests or batch flushing.
// Example:
//   auto buf = std::make_shared<dicom::diag::BufferingReporter>(512); // up to 512 messages buffered (ring)
//   dicom::diag::set_thread_reporter(buf);
//   dicom::diag::info("queued message");
//   auto msgs = buf->take_messages();  // clears buffer, returns strings
//   for (auto& m : msgs) std::cerr << m << '\n';
class BufferingReporter : public Reporter {
public:
	explicit BufferingReporter(size_t max_messages = 0) : max_(max_messages) {}

	void report(LogLevel level, std::string_view message) override {
		std::lock_guard<std::mutex> lock(mu_);
		if (max_ && messages_.size() >= max_) {
			// drop oldest to keep size bounded
			messages_.erase(messages_.begin());
		}
		messages_.emplace_back(level, std::string(message));
	}

	// Flush buffered messages to another reporter, clearing the buffer.
	void flush_to(Reporter& target) {
		std::vector<std::pair<LogLevel, std::string>> copy;
		{
			std::lock_guard<std::mutex> lock(mu_);
			copy.swap(messages_);
		}
		for (auto& msg : copy) target.report(msg.first, msg.second);
	}

	// Visit buffered messages without clearing.
	template <typename Fn>
	void for_each(Fn&& fn) const {
		std::lock_guard<std::mutex> lock(mu_);
		for (auto& msg : messages_) fn(msg.first, msg.second);
	}

	// Take buffered messages (clears buffer) and return as strings.
	// Example:
	//   auto msgs = buf->take_messages();  // buffer emptied
	//   for (auto& m : msgs) std::cerr << m << '\n';
	std::vector<std::string> take_messages(bool include_log_level = true) {
		std::vector<std::pair<LogLevel, std::string>> copy;
		{
			std::lock_guard<std::mutex> lock(mu_);
			copy = messages_;
			messages_.clear();
		}
		std::vector<std::string> lines;
		lines.reserve(copy.size());
		for (auto& msg : copy) {
			if (include_log_level) {
				lines.emplace_back('[' + std::string(level_to_str(msg.first)) + "] " + msg.second);
			} else {
				lines.emplace_back(msg.second);
			}
		}
		return lines;
	}

private:
	size_t max_;
	mutable std::mutex mu_;
	std::vector<std::pair<LogLevel, std::string>> messages_;

	static const char* level_to_str(LogLevel level) {
		switch (level) {
			case LogLevel::Info: return "INFO";
			case LogLevel::Warning: return "WARN";
			case LogLevel::Error: return "ERROR";
		}
		return "UNKNOWN";
	}
};

// Select the active reporter globally or per-thread
// Returns a reference to the process-wide reporter pointer.
// Lazily initializes to StderrReporter so logging always works.
// Example:
//   auto& rep = dicom::diag::default_reporter();
//   rep = std::make_shared<MyReporter>();  // swap in globally
inline std::shared_ptr<Reporter>& default_reporter() {
	static std::shared_ptr<Reporter> rep = std::make_shared<StderrReporter>();
	return rep;
}

// Install a process-wide reporter; pass nullptr to reset to StderrReporter.
// Example:
//   dicom::diag::set_default_reporter(std::make_shared<MyReporter>());
//   dicom::diag::info("uses MyReporter everywhere");
//   dicom::diag::set_default_reporter(nullptr);  // revert to stderr
inline void set_default_reporter(std::shared_ptr<Reporter> rep) {
	default_reporter() = rep ? std::move(rep) : std::make_shared<StderrReporter>();
}

inline thread_local std::shared_ptr<Reporter> tls_reporter{nullptr};

// Install a reporter that applies only to the current thread.
// Example:
//   std::thread t([] {
//     dicom::diag::set_thread_reporter(std::make_shared<MyReporter>());
//     dicom::diag::warn("thread-specific");  // goes to MyReporter
//   });
//   t.join();
//   dicom::diag::warn("main thread");  // uses default reporter
inline void set_thread_reporter(std::shared_ptr<Reporter> rep) { tls_reporter = std::move(rep); }

// Log level management (drop logs below the current level).
inline LogLevel& default_log_level() {
	static LogLevel level = LogLevel::Info;
	return level;
}

// Set process-wide log level (messages below this are dropped fast).
inline void set_log_level(LogLevel level) { default_log_level() = level; }

inline LogLevel current_log_level() {
	return default_log_level();
}

inline bool meets_log_level(LogLevel level) {
	return static_cast<int>(level) >= static_cast<int>(current_log_level());
}

// Resolve which reporter to use: explicit candidate -> thread-local -> global default.
// Example:
//   dicom::diag::get().report(dicom::diag::LogLevel::Info, "manual call");
//   auto custom = std::make_shared<MyReporter>();
//   dicom::diag::get(custom).error("custom for this call only");
inline Reporter& get(const std::shared_ptr<Reporter>& candidate = nullptr) {
	if (candidate) return *candidate;
	if (tls_reporter) return *tls_reporter;
	return *default_reporter();
}

// Convenience namespace-level helpers
// These pick a reporter via get() and write a single message.
// Example:
//   dicom::diag::info("ok");
//   auto custom = std::make_shared<MyReporter>();
//   dicom::diag::error("with custom", custom);
//   try {
//     dicom::diag::error_and_throw("fatal");
//   } catch (const dicom::diag::DicomException&) {
//     /* handle */
//   }
inline void info(std::string_view msg, const std::shared_ptr<Reporter>& r = nullptr) {
#if DICOMSDL_ENABLE_INFO_LOGS
	if (meets_log_level(LogLevel::Info)) get(r).info(msg);
#else
	(void)r;
	(void)msg;
#endif
}
inline void warn(std::string_view msg, const std::shared_ptr<Reporter>& r = nullptr) {
	if (meets_log_level(LogLevel::Warning)) get(r).warn(msg);
}
inline void error(std::string_view msg, const std::shared_ptr<Reporter>& r = nullptr) {
	if (meets_log_level(LogLevel::Error)) get(r).error(msg);
}
// Logs an error then throws DicomException with the same message.
[[noreturn]] inline void error_and_throw(std::string_view msg, const std::shared_ptr<Reporter>& r = nullptr) {
	get(r).error_and_throw(msg);
}

// fmt-style helpers: diag::info("tag={} vr={}", tag, vr)
template <typename... Args>
inline void info(std::format_string<Args...> fmt, Args&&... args) {
#if DICOMSDL_ENABLE_INFO_LOGS
	if (meets_log_level(LogLevel::Info)) {
		get().info(std::format(fmt, std::forward<Args>(args)...));
	}
#else
	(void)fmt; (void)sizeof...(args);
#endif
}

template <typename... Args>
inline void info(const std::shared_ptr<Reporter>& r, std::format_string<Args...> fmt, Args&&... args) {
#if DICOMSDL_ENABLE_INFO_LOGS
	if (meets_log_level(LogLevel::Info)) {
		get(r).info(std::format(fmt, std::forward<Args>(args)...));
	}
#else
	(void)fmt; (void)r; (void)sizeof...(args);
#endif
}

template <typename... Args>
inline void warn(std::format_string<Args...> fmt, Args&&... args) {
	if (meets_log_level(LogLevel::Warning)) {
		get().warn(std::format(fmt, std::forward<Args>(args)...));
	}
}

template <typename... Args>
inline void warn(const std::shared_ptr<Reporter>& r, std::format_string<Args...> fmt, Args&&... args) {
	if (meets_log_level(LogLevel::Warning)) {
		get(r).warn(std::format(fmt, std::forward<Args>(args)...));
	}
}

template <typename... Args>
inline void error(std::format_string<Args...> fmt, Args&&... args) {
	if (meets_log_level(LogLevel::Error)) {
		get().error(std::format(fmt, std::forward<Args>(args)...));
	}
}

template <typename... Args>
inline void error(const std::shared_ptr<Reporter>& r, std::format_string<Args...> fmt, Args&&... args) {
	if (meets_log_level(LogLevel::Error)) {
		get(r).error(std::format(fmt, std::forward<Args>(args)...));
	}
}

template <typename... Args>
[[noreturn]] inline void error_and_throw(std::format_string<Args...> fmt, Args&&... args) {
	get().error_and_throw(std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
[[noreturn]] inline void error_and_throw(const std::shared_ptr<Reporter>& r, std::format_string<Args...> fmt, Args&&... args) {
	get(r).error_and_throw(std::format(fmt, std::forward<Args>(args)...));
}

} // namespace diag
} // namespace dicom
