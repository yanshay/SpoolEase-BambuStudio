#ifndef spoolEaseLog_hpp_
#define spoolEaseLog_hpp_

#include <sstream>
#include <string>

namespace Slic3r { namespace SpoolEase {

enum class LogLevel
{
    trace,
    debug,
    info,
    warning,
    error,
    fatal
};

class LogLine
{
public:
    explicit LogLine(LogLevel level);
    ~LogLine();

    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;

    template <class T>
    LogLine& operator<<(const T& value)
    {
        m_stream << value;
        return *this;
    }

private:
    LogLevel          m_level;
    std::ostringstream m_stream;
};

void write_log_line(LogLevel level, const std::string& message) noexcept;

}} // namespace Slic3r::SpoolEase

#define SPOOLEASE_LOG(level) ::Slic3r::SpoolEase::LogLine(::Slic3r::SpoolEase::LogLevel::level)

#endif // spoolEaseLog_hpp_
