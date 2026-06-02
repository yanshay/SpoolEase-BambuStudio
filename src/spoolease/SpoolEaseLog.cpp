#include "SpoolEaseLog.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Slic3r {
const std::string& data_dir();
}

namespace Slic3r { namespace SpoolEase {

namespace fs = boost::filesystem;

namespace {

constexpr std::uintmax_t max_log_file_size = 5u * 1024u * 1024u;
constexpr std::size_t    max_log_files     = 10;
constexpr long           max_log_age_days  = 30;

struct LogFileInfo
{
    std::time_t time{};
    fs::path    path;
};

std::mutex                         s_log_mutex;
bool                               s_initialized{false};
bool                               s_enabled{false};
fs::path                           s_log_dir;
std::string                        s_session_stamp;
unsigned                           s_pid{0};
unsigned                           s_rotation_index{0};
std::uintmax_t                     s_current_size{0};
std::unique_ptr<boost::nowide::ofstream> s_stream;

std::tm local_time(std::time_t value)
{
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &value);
#else
    localtime_r(&value, &tm);
#endif
    return tm;
}

std::string timestamp_for_filename(std::time_t value)
{
    const std::tm tm = local_time(value);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S");
    return out.str();
}

std::string timestamp_for_log_line()
{
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    const std::tm tm = local_time(time);

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << millis;
    return out.str();
}

const char* level_name(LogLevel level)
{
    switch (level) {
    case LogLevel::trace: return "trace";
    case LogLevel::debug: return "debug";
    case LogLevel::info: return "info";
    case LogLevel::warning: return "warning";
    case LogLevel::error: return "error";
    case LogLevel::fatal: return "fatal";
    }

    return "unknown";
}

bool starts_with(const std::string& value, const std::string& prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_spoolease_log_file(const fs::path& path)
{
    const std::string name = path.filename().string();
    return starts_with(name, "spoolease_") && ends_with(name, ".log");
}

unsigned current_pid()
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned>(::getpid());
#endif
}

fs::path current_log_path()
{
    std::ostringstream name;
    name << "spoolease_" << s_session_stamp << '_' << s_pid;
    if (s_rotation_index > 0)
        name << '_' << s_rotation_index;
    name << ".log";
    return s_log_dir / name.str();
}

void cleanup_logs_locked()
{
    try {
        if (s_log_dir.empty() || !fs::exists(s_log_dir))
            return;

        const std::time_t now = std::time(nullptr);
        std::vector<LogFileInfo> files;
        for (const auto& entry : fs::directory_iterator(s_log_dir)) {
            if (!fs::is_regular_file(entry.status()) || !is_spoolease_log_file(entry.path()))
                continue;

            const std::time_t modified = fs::last_write_time(entry.path());
            if (modified > 0 && now - modified > max_log_age_days * 24L * 60L * 60L) {
                boost::system::error_code ec;
                fs::remove(entry.path(), ec);
                continue;
            }

            files.push_back(LogFileInfo{modified, entry.path()});
        }

        std::sort(files.begin(), files.end(), [](const LogFileInfo& lhs, const LogFileInfo& rhs) { return lhs.time > rhs.time; });
        while (files.size() > max_log_files) {
            boost::system::error_code ec;
            fs::remove(files.back().path, ec);
            files.pop_back();
        }
    } catch (...) {
    }
}

void open_current_file_locked()
{
    const fs::path path = current_log_path();
    s_stream.reset(new boost::nowide::ofstream(path.string(), std::ios::out | std::ios::app | std::ios::binary));
    if (!*s_stream) {
        s_stream.reset();
        s_enabled = false;
        return;
    }

    try {
        s_current_size = fs::exists(path) ? fs::file_size(path) : 0;
    } catch (...) {
        s_current_size = 0;
    }
}

void rotate_locked()
{
    if (s_stream)
        s_stream->close();
    s_stream.reset();
    ++s_rotation_index;
    open_current_file_locked();
    cleanup_logs_locked();
}

void initialize_locked()
{
    if (s_initialized)
        return;

    if (Slic3r::data_dir().empty())
        return;

    s_initialized = true;

    try {
        s_log_dir = fs::path(Slic3r::data_dir()) / "spoolease" / "log";
        fs::create_directories(s_log_dir);
        s_session_stamp = timestamp_for_filename(std::time(nullptr));
        s_pid = current_pid();
        s_rotation_index = 0;
        s_enabled = true;
        cleanup_logs_locked();
        open_current_file_locked();
    } catch (...) {
        s_enabled = false;
    }
}

} // namespace

LogLine::LogLine(LogLevel level)
    : m_level(level)
{
}

LogLine::~LogLine()
{
    write_log_line(m_level, m_stream.str());
}

void write_log_line(LogLevel level, const std::string& message) noexcept
{
    if (message.empty())
        return;

    try {
        std::ostringstream line;
        line << timestamp_for_log_line()
             << " [Thread " << std::this_thread::get_id() << ']'
             << ' ' << level_name(level) << ": " << message << '\n';
        const std::string text = line.str();

        std::lock_guard<std::mutex> lock(s_log_mutex);
        initialize_locked();
        if (!s_enabled || !s_stream)
            return;

        if (s_current_size > 0 && s_current_size + text.size() > max_log_file_size)
            rotate_locked();
        if (!s_stream)
            return;

        *s_stream << text;
        s_stream->flush();
        s_current_size += text.size();
    } catch (...) {
    }
}

}} // namespace Slic3r::SpoolEase
