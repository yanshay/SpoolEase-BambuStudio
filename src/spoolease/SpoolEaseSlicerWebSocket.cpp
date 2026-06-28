#include "SpoolEaseSlicerWebSocket.hpp"

#include "SpoolEaseConfig.hpp"
#include "SpoolEaseLog.hpp"
#include "SpoolEaseStatus.hpp"

#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>
#include <openssl/err.h>

#include <wx/app.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Slic3r { namespace SpoolEase {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = boost::beast::websocket;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;
using SlicerWebSocketStream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

constexpr auto reconnect_interval = std::chrono::seconds(5);
constexpr auto connect_timeout = std::chrono::seconds(5);
constexpr auto heartbeat_interval_base = std::chrono::milliseconds(12 * 1000);
constexpr unsigned heartbeat_interval_jitter_ms = 2 * 1000;
constexpr unsigned max_missed_heartbeats = 1;
constexpr const char* ws_status_key = "slicer_ws";
constexpr const char* proxy_status_key = "slicer_proxy";
constexpr const char* slicer_ws_path = "/api/internal/slicer/ws";

struct Endpoint
{
    std::string host;
    std::string port{"443"};
    std::string host_header;
};

std::string trim_copy(std::string value)
{
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char ch) { return !is_space(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char ch) { return !is_space(ch); }).base(), value.end());
    return value;
}

std::string normalized_address(std::string address)
{
    address = trim_copy(std::move(address));
    const std::string https_prefix = "https://";
    const std::string http_prefix = "http://";
    if (address.rfind(https_prefix, 0) == 0)
        address.erase(0, https_prefix.size());
    else if (address.rfind(http_prefix, 0) == 0)
        address.erase(0, http_prefix.size());

    const size_t slash = address.find('/');
    if (slash != std::string::npos)
        address.erase(slash);

    while (!address.empty() && address.back() == '/')
        address.pop_back();
    return address;
}

Endpoint endpoint_from_config(const ConsoleConfig& config)
{
    Endpoint endpoint;
    std::string address = normalized_address(config.address);

    if (!address.empty() && address.front() == '[') {
        const size_t close = address.find(']');
        if (close != std::string::npos) {
            endpoint.host = address.substr(1, close - 1);
            if (close + 1 < address.size() && address[close + 1] == ':')
                endpoint.port = address.substr(close + 2);
        }
    }

    if (endpoint.host.empty()) {
        const size_t colon = address.rfind(':');
        if (colon != std::string::npos && address.find(':') == colon) {
            endpoint.host = address.substr(0, colon);
            endpoint.port = address.substr(colon + 1);
        } else {
            endpoint.host = address;
        }
    }

    endpoint.host_header = endpoint.host;
    if (!endpoint.port.empty() && endpoint.port != "443")
        endpoint.host_header += ":" + endpoint.port;
    return endpoint;
}

std::string connection_key(const ConsoleConfig& config)
{
    return normalized_address(config.address) + '\x1f' + config.api_token + '\x1f' + config.ca_cert_pem;
}

std::chrono::milliseconds heartbeat_interval()
{
    if (heartbeat_interval_jitter_ms == 0)
        return heartbeat_interval_base;

    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<unsigned> distribution(0, heartbeat_interval_jitter_ms - 1);
    return heartbeat_interval_base + std::chrono::milliseconds(distribution(rng));
}

websocket::ping_data heartbeat_ping_data()
{
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    websocket::ping_data data;
    for (size_t i = 0; i < sizeof(ticks); ++i)
        data.push_back(static_cast<char>((ticks >> (8 * i)) & 0xff));
    return data;
}

std::string unsupported_state_key(const ConsoleConfig& config)
{
    return connection_key(config) + '\x1f' + (config.proxy_printer_messages_configured ? "1" : "0")
        + '\x1f' + (config.proxy_printer_messages ? "1" : "0");
}

class UnsupportedSlicerWebSocket : public std::runtime_error
{
public:
    explicit UnsupportedSlicerWebSocket(const std::string& message) : std::runtime_error(message) {}
};

std::string json_field_string(const json& object, const char* key)
{
    const auto it = object.find(key);
    if (it == object.end() || it->is_null())
        return "-";
    if (it->is_string())
        return it->get<std::string>();
    return it->dump();
}

std::string payload_print_field(const json& payload, const char* key)
{
    const auto print_it = payload.find("print");
    if (print_it == payload.end() || !print_it->is_object())
        return "-";
    return json_field_string(*print_it, key);
}

void schedule_message(std::string message)
{
    if (!wxTheApp) {
        SPOOLEASE_LOG(warning) << "SpoolEase: slicer WebSocket message dropped: wxTheApp unavailable";
        return;
    }

    wxTheApp->CallAfter([message = std::move(message)]() mutable {
        if (!wxTheApp)
            return;

        try {
            json envelope = json::parse(message);
            const std::string type = envelope.value("type", std::string());
            if (type != "printer_send_json") {
                SPOOLEASE_LOG(debug) << "SpoolEase: slicer WebSocket ignored message type: type=" << type;
                return;
            }

            const std::optional<ConsoleConfig> config = console_config(false, "slicer_ws_message");
            if (!config.has_value() || !config->proxy_printer_messages_enabled()) {
                clear_live_status(proxy_status_key);
                SPOOLEASE_LOG(info) << "SpoolEase: slicer printer proxy message ignored: proxy disabled";
                return;
            }

            const std::string printer_serial = envelope.value("printer_serial", std::string());
            if (printer_serial.empty()) {
                set_live_status_error(proxy_status_key, "Slicer proxy message missing printer serial.");
                SPOOLEASE_LOG(warning) << "SpoolEase: slicer proxy message missing printer_serial";
                return;
            }

            const auto payload_it = envelope.find("payload");
            if (payload_it == envelope.end() || !payload_it->is_object()) {
                set_live_status_error(proxy_status_key, "Slicer proxy message has invalid payload.");
                SPOOLEASE_LOG(warning) << "SpoolEase: slicer proxy message has invalid payload";
                return;
            }
            json payload = *payload_it;
            const std::string command = payload_print_field(payload, "command");

            Slic3r::GUI::GUI_App& app = Slic3r::GUI::wxGetApp();
            DeviceManager* device_manager = app.getDeviceManager();
            if (!device_manager) {
                set_live_status_error(proxy_status_key, "Bambu Studio device manager is unavailable.");
                SPOOLEASE_LOG(warning) << "SpoolEase: slicer proxy failed: DeviceManager unavailable";
                return;
            }

            MachineObject* target = device_manager->get_my_machine(printer_serial);
            if (!target) {
                set_live_status_error(proxy_status_key, "Printer is not known to Bambu Studio.");
                SPOOLEASE_LOG(warning) << "SpoolEase: slicer proxy failed: unknown printer=" << printer_serial;
                return;
            }

            if (target->is_lan_mode_printer()) {
                set_live_status_warning(proxy_status_key, "Printer is not configured for cloud sending in Bambu Studio.");
                SPOOLEASE_LOG(warning) << "SpoolEase: slicer proxy skipped LAN printer=" << printer_serial;
                return;
            }

            NetworkAgent* agent = app.getAgent();
            if (!agent || !agent->is_user_login()) {
                set_live_status_error(proxy_status_key, "Bambu account is not logged in for cloud sending.");
                SPOOLEASE_LOG(warning) << "SpoolEase: slicer proxy failed: Bambu account not logged in";
                return;
            }
            if (!agent->is_server_connected()) {
                set_live_status_error(proxy_status_key, "Bambu cloud connection is not available.");
                SPOOLEASE_LOG(warning) << "SpoolEase: slicer proxy failed: cloud server not connected";
                return;
            }

            auto replace_sequence_ids = [](json& item, const std::string& sequence_id, const auto& self) -> size_t {
                size_t replaced = 0;
                if (item.is_object()) {
                    for (auto& entry : item.items()) {
                        if (entry.key() == "sequence_id") {
                            entry.value() = sequence_id;
                            ++replaced;
                        } else {
                            replaced += self(entry.value(), sequence_id, self);
                        }
                    }
                } else if (item.is_array()) {
                    for (auto& entry : item)
                        replaced += self(entry, sequence_id, self);
                }
                return replaced;
            };

            const std::string sequence_id = std::to_string(MachineObject::m_sequence_id++);
            const size_t replaced = replace_sequence_ids(payload, sequence_id, replace_sequence_ids);
            if (replaced == 0) {
                auto print_it = payload.find("print");
                if (print_it == payload.end() || !print_it->is_object()) {
                    set_live_status_error(proxy_status_key, "Slicer proxy payload has no sequence_id location.");
                    SPOOLEASE_LOG(warning) << "SpoolEase: slicer proxy failed: no sequence_id location";
                    return;
                }
                (*print_it)["sequence_id"] = sequence_id;
            }

            const std::string outgoing_payload = payload.dump();
            const int result = target->cloud_publish_json(outgoing_payload, 0, 0);
            if (result != 0) {
                set_live_status_error(proxy_status_key, "Bambu cloud send failed for proxied printer message.");
                SPOOLEASE_LOG(warning) << "SpoolEase: slicer proxy cloud send failed: printer=" << printer_serial
                                      << " command=" << command
                                      << " studio_sequence_id=" << sequence_id
                                      << " code=" << result;
                return;
            }

            clear_live_status(proxy_status_key);
        } catch (const std::exception& e) {
            set_live_status_error(proxy_status_key, "Slicer proxy message is invalid JSON.");
            SPOOLEASE_LOG(warning) << "SpoolEase: slicer WebSocket message handling failed: error=\"" << e.what() << "\"";
        } catch (...) {
            set_live_status_error(proxy_status_key, "Slicer proxy message handling failed.");
            SPOOLEASE_LOG(warning) << "SpoolEase: slicer WebSocket message handling failed: error=unknown";
        }
    });
}

class SlicerWebSocketClient
{
public:
    ~SlicerWebSocketClient()
    {
        m_stop = true;
        refresh_now();
        if (m_thread.joinable())
            m_thread.join();
    }

    void start()
    {
        std::lock_guard<std::mutex> lock(m_thread_mutex);
        if (m_started)
            return;

        m_started = true;
        m_thread = std::thread([this]() { run(); });
    }

    void refresh_now()
    {
        m_wake = true;
        m_cv.notify_all();
        cancel_active_io();
    }

private:
    struct ActiveIo
    {
        std::mutex           mutex;
        bool                 valid{true};
        bool                 cancel_requested{false};
        asio::io_context*    io_context{nullptr};
        SlicerWebSocketStream* ws{nullptr};
        asio::steady_timer*  heartbeat_timer{nullptr};
    };

    std::shared_ptr<ActiveIo> activate_io(asio::io_context& io_context, SlicerWebSocketStream& ws, asio::steady_timer& heartbeat_timer)
    {
        auto active = std::make_shared<ActiveIo>();
        active->io_context = &io_context;
        active->ws = &ws;
        active->heartbeat_timer = &heartbeat_timer;

        std::lock_guard<std::mutex> lock(m_active_io_mutex);
        m_active_io = active;
        return active;
    }

    bool deactivate_io(const std::shared_ptr<ActiveIo>& active)
    {
        bool cancel_requested = false;
        {
            std::lock_guard<std::mutex> lock(active->mutex);
            cancel_requested = active->cancel_requested;
            active->valid = false;
            active->io_context = nullptr;
            active->ws = nullptr;
            active->heartbeat_timer = nullptr;
        }

        std::lock_guard<std::mutex> lock(m_active_io_mutex);
        if (m_active_io.lock() == active)
            m_active_io.reset();
        return cancel_requested;
    }

    void cancel_active_io()
    {
        std::shared_ptr<ActiveIo> active;
        {
            std::lock_guard<std::mutex> lock(m_active_io_mutex);
            active = m_active_io.lock();
        }
        if (!active)
            return;

        std::lock_guard<std::mutex> lock(active->mutex);
        if (!active->valid)
            return;

        active->cancel_requested = true;
        boost::system::error_code ignored;
        if (active->heartbeat_timer)
            active->heartbeat_timer->cancel(ignored);
        if (active->ws)
            beast::get_lowest_layer(*active->ws).socket().cancel(ignored);
        if (active->io_context)
            active->io_context->stop();
    }

    void run()
    {
        SPOOLEASE_LOG(info) << "SpoolEase: slicer WebSocket loop started";
        while (!m_stop) {
            const std::optional<ConsoleConfig> config = console_config(false, "slicer_ws");
            if (!config.has_value()) {
                clear_live_status(ws_status_key);
                clear_live_status(proxy_status_key);
                wait_or_wake(reconnect_interval);
                continue;
            }
            if (!config->proxy_printer_messages_configured) {
                clear_live_status(ws_status_key);
                clear_live_status(proxy_status_key);
                wait_or_wake(reconnect_interval);
                continue;
            }

            try {
                connect_and_read(*config);
            } catch (const UnsupportedSlicerWebSocket& e) {
                if (!m_stop) {
                    if (config->proxy_printer_messages_enabled()) {
                        set_live_status_warning(ws_status_key, "Console version does not support slicer proxy.");
                        SPOOLEASE_LOG(warning) << "SpoolEase: slicer WebSocket unsupported by console: error=\"" << e.what() << "\"";
                    } else {
                        clear_live_status(ws_status_key);
                        SPOOLEASE_LOG(info) << "SpoolEase: slicer WebSocket unsupported by console; proxy disabled";
                    }
                    wait_for_config_change(unsupported_state_key(*config));
                }
                continue;
            } catch (const std::exception& e) {
                if (!m_stop) {
                    set_live_status_warning(ws_status_key, "Slicer WebSocket disconnected. Retrying.");
                    SPOOLEASE_LOG(warning) << "SpoolEase: slicer WebSocket connection failed: error=\"" << e.what() << "\"";
                }
            } catch (...) {
                if (!m_stop) {
                    set_live_status_warning(ws_status_key, "Slicer WebSocket disconnected. Retrying.");
                    SPOOLEASE_LOG(warning) << "SpoolEase: slicer WebSocket connection failed: error=unknown";
                }
            }

            wait_or_wake(reconnect_interval);
        }
        clear_live_status(ws_status_key);
        SPOOLEASE_LOG(info) << "SpoolEase: slicer WebSocket loop stopped";
    }

    void connect_and_read(const ConsoleConfig& config)
    {
        const Endpoint endpoint = endpoint_from_config(config);
        if (endpoint.host.empty())
            throw std::runtime_error("empty console host");

        const std::string key = connection_key(config);
        asio::io_context io_context;
        ssl::context ssl_context(ssl::context::tls_client);
        if (!config.ca_cert_pem.empty()) {
            boost::system::error_code ca_ec;
            ssl_context.add_certificate_authority(asio::buffer(config.ca_cert_pem.data(), config.ca_cert_pem.size()), ca_ec);
            if (ca_ec)
                throw boost::system::system_error(ca_ec, "failed to load console CA certificate");
            ssl_context.set_verify_mode(ssl::verify_peer);
        } else {
            ssl_context.set_verify_mode(ssl::verify_none);
        }

        tcp::resolver resolver(io_context);
        SlicerWebSocketStream ws(io_context, ssl_context);

        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), endpoint.host.c_str())) {
            boost::system::error_code ec(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category());
            throw boost::system::system_error(ec, "failed to set SNI host name");
        }

        beast::get_lowest_layer(ws).expires_after(connect_timeout);
        const auto results = resolver.resolve(endpoint.host, endpoint.port);
        beast::get_lowest_layer(ws).connect(results);
        ws.next_layer().handshake(ssl::stream_base::client);
        beast::get_lowest_layer(ws).expires_never();

        ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        const std::string authorization = "Bearer " + config.api_token;
        ws.set_option(websocket::stream_base::decorator([authorization](websocket::request_type& request) {
            request.set(boost::beast::http::field::authorization, authorization);
            request.set(boost::beast::http::field::user_agent, "SpoolEase-BambuStudio");
        }));

        websocket::response_type handshake_response;
        boost::system::error_code handshake_ec;
        ws.handshake(handshake_response, endpoint.host_header, slicer_ws_path, handshake_ec);
        if (handshake_ec) {
            if (handshake_ec == websocket::error::upgrade_declined && handshake_response.result() == boost::beast::http::status::not_found)
                throw UnsupportedSlicerWebSocket("endpoint returned 404");
            throw boost::system::system_error(handshake_ec, "WebSocket handshake failed");
        }
        clear_live_status(ws_status_key);
        SPOOLEASE_LOG(info) << "SpoolEase: slicer WebSocket connected: host=" << endpoint.host_header << " path=" << slicer_ws_path;

        beast::flat_buffer buffer;
        asio::steady_timer heartbeat_timer(io_context);
        const std::shared_ptr<ActiveIo> active_io = activate_io(io_context, ws, heartbeat_timer);
        boost::system::error_code final_ec;
        std::string final_message;
        bool finished = false;
        bool config_changed = false;
        bool graceful_finish = false;
        bool waiting_for_pong = false;
        unsigned missed_heartbeats = 0;
        std::chrono::steady_clock::time_point ping_sent_at;

        auto finish = [&](boost::system::error_code ec, std::string message, bool graceful = false) {
            if (finished)
                return;

            finished = true;
            final_ec = ec;
            final_message = std::move(message);
            graceful_finish = graceful;

            boost::system::error_code ignored;
            heartbeat_timer.cancel(ignored);
            beast::get_lowest_layer(ws).socket().cancel(ignored);
        };

        ws.control_callback([&](websocket::frame_type kind, beast::string_view) {
            if (kind == websocket::frame_type::pong) {
                waiting_for_pong = false;
                missed_heartbeats = 0;
            } else if (kind == websocket::frame_type::close) {
                SPOOLEASE_LOG(info) << "SpoolEase: slicer WebSocket received close frame";
            }
        });

        std::function<void()> start_read;
        std::function<void()> schedule_heartbeat;

        start_read = [&]() {
            if (finished)
                return;

            buffer.consume(buffer.size());
            ws.async_read(buffer, [&](boost::system::error_code ec, std::size_t) {
                if (finished)
                    return;
                if (m_stop) {
                    finish(asio::error::operation_aborted, "slicer WebSocket stopped", true);
                    return;
                }
                if (ec) {
                    finish(ec, "slicer WebSocket read failed");
                    return;
                }

                if (ws.got_text()) {
                    std::string message = beast::buffers_to_string(buffer.data());
                    schedule_message(std::move(message));
                }
                start_read();
            });
        };

        schedule_heartbeat = [&]() {
            if (finished)
                return;

            heartbeat_timer.expires_after(heartbeat_interval());
            heartbeat_timer.async_wait([&](boost::system::error_code ec) {
                if (finished || ec == asio::error::operation_aborted)
                    return;
                if (ec) {
                    finish(ec, "slicer WebSocket heartbeat timer failed");
                    return;
                }
                if (m_stop) {
                    finish(asio::error::operation_aborted, "slicer WebSocket stopped", true);
                    return;
                }

                const std::optional<ConsoleConfig> current_config = console_config(false, "slicer_ws_check");
                if (!current_config.has_value() || connection_key(*current_config) != key) {
                    config_changed = true;
                    SPOOLEASE_LOG(info) << "SpoolEase: slicer WebSocket reconnecting after config change";
                    finish(asio::error::operation_aborted, "slicer WebSocket config changed", true);
                    return;
                }

                if (waiting_for_pong) {
                    ++missed_heartbeats;
                    SPOOLEASE_LOG(warning) << "SpoolEase: slicer WebSocket missed heartbeat: missed=" << missed_heartbeats
                                           << " max=" << max_missed_heartbeats;
                    if (missed_heartbeats >= max_missed_heartbeats) {
                        finish(asio::error::timed_out, "slicer WebSocket heartbeat timeout");
                        return;
                    }

                    schedule_heartbeat();
                    return;
                }

                websocket::ping_data ping_data = heartbeat_ping_data();
                ping_sent_at = std::chrono::steady_clock::now();
                waiting_for_pong = true;
                ws.async_ping(ping_data, [&](boost::system::error_code ping_ec) {
                    if (finished)
                        return;
                    if (ping_ec) {
                        finish(ping_ec, "slicer WebSocket ping failed");
                        return;
                    }
                });

                schedule_heartbeat();
            });
        };

        start_read();
        schedule_heartbeat();
        io_context.run();
        const bool externally_cancelled = deactivate_io(active_io);

        boost::system::error_code close_ec;
        if (!m_stop && !externally_cancelled)
            ws.close(websocket::close_code::normal, close_ec);
        if (close_ec)
            SPOOLEASE_LOG(debug) << "SpoolEase: slicer WebSocket close returned: " << close_ec.message();

        if (m_stop || externally_cancelled || config_changed || graceful_finish)
            return;
        if (final_ec)
            throw boost::system::system_error(final_ec, final_message);
    }

    void wait_or_wake(std::chrono::steady_clock::duration duration)
    {
        std::unique_lock<std::mutex> lock(m_wait_mutex);
        m_cv.wait_for(lock, duration, [this]() { return m_stop.load() || m_wake.exchange(false); });
    }

    void wait_for_config_change(const std::string& key)
    {
        while (!m_stop) {
            wait_or_wake(reconnect_interval);
            const std::optional<ConsoleConfig> config = console_config(false, "slicer_ws_unsupported_check");
            if (!config.has_value() || unsupported_state_key(*config) != key)
                return;
        }
    }

private:
    std::mutex              m_thread_mutex;
    std::mutex              m_wait_mutex;
    std::mutex              m_active_io_mutex;
    std::condition_variable m_cv;
    std::thread             m_thread;
    std::atomic_bool        m_stop{false};
    std::atomic_bool        m_wake{false};
    std::weak_ptr<ActiveIo> m_active_io;
    bool                    m_started{false};
};

SlicerWebSocketClient& slicer_websocket_client()
{
    static SlicerWebSocketClient client;
    return client;
}

} // namespace

void start_slicer_websocket()
{
    slicer_websocket_client().start();
}

void refresh_slicer_websocket_now()
{
    slicer_websocket_client().refresh_now();
}

}} // namespace Slic3r::SpoolEase
