#pragma once
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio.hpp>
#include <boost/asio/strand.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/json.hpp>
#include <queue>
#include <functional>
#include <string>
#include <iostream>
#include <memory>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace ssl   = net::ssl;
namespace json  = boost::json;
using tcp       = net::ip::tcp;

class BinanceRestClient : public std::enable_shared_from_this<BinanceRestClient>
{
public:
    using Callback = std::function<void(boost::json::object, beast::error_code)>;

    explicit BinanceRestClient(net::strand<net::io_context::executor_type>& strand,
        ssl::context& ctx,
        const std::function<void(const beast::error_code&)>& readyHandler,
        const uint16_t& retryIntervalSec);

    // ==================== PUBLIC METHODS ====================
    void ping(const Callback& cb);
    void getServerTime(const Callback& cb);
    void getExchangeInfo(const Callback& cb);

    void getDepth(const std::string& symbol, int limit, const Callback& cb);
    void getRecentTrades(const std::string& symbol, int limit, const Callback& cb);
    void getTickerPrice(const std::string& symbol, const Callback& cb);
    void get24hrTicker(const std::string& symbol, const Callback& cb);
    void getKlines(const std::string& symbol,
                   const std::string& interval,
                   int limit,
                   const Callback& cb);
    
    void run();

private:
    bool isFatalError(const beast::error_code& ec) {
        return
        ec.category() == net::error::get_ssl_category()
        || ec == net::error::operation_aborted
        || ec == http::error::bad_alloc
        || ec == http::error::bad_method
        || ec == http::error::bad_version
        || ec == http::error::bad_status
        || ec == http::error::bad_reason
        || ec == http::error::bad_field
        || ec == http::error::bad_value
        || ec == http::error::bad_content_length
        || ec == http::error::bad_transfer_encoding
        || ec == http::error::bad_chunk
        || ec == http::error::bad_chunk_extension
        || ec == http::error::bad_line_ending
        || ec == http::error::bad_obs_fold;
    }

    net::strand<net::io_context::executor_type>& m_strand;
    ssl::context& m_ctx;

    void scheduleKeepAlive()
    {
        m_timer.expires_after(std::chrono::seconds(30));
        m_timer.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
                if (!ec) self->keepAlive();
        });
    }

    void keepAlive()
    {
        if(!m_connected) return;
        if(m_queue.empty())
        {
            ping([](auto, auto){});
        }
        scheduleKeepAlive();
    }
    
    const std::string m_host = "api.binance.com";
    const std::string m_port = "443";
    const std::string m_api_version = "/api/v3";
    net::steady_timer m_timer;

    // Persistent connection
    std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> m_stream;
    tcp::resolver m_resolver;

    // Queue system
    struct PendingRequest {
        std::string target;
        http::verb method;
        std::string query;
        Callback callback;
    };

    const std::function<void(const beast::error_code&)> m_readyCallback;
    const std::function<void(const beast::error_code&)> m_errCallback;
    const uint16_t m_retryIntervalSec;
    std::queue<PendingRequest> m_queue;
    bool m_connected;
    bool m_connectedOnce;

    beast::flat_buffer m_buffer;
    http::response<http::string_body> m_response;
    http::request<http::string_body> m_request;

    // Internal methods
    void launch_request(const std::string& path,
        http::verb method,
        const std::string& query,
        const Callback& cb);

    void start_next_request();

    // Async handlers
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results);
    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type);
    void on_handshake(beast::error_code ec);
    void on_write(beast::error_code ec, std::size_t);
    void do_read();
    void on_read(beast::error_code ec, std::size_t);
    void fail(beast::error_code ec, const char* what);

    void close_connection();
};
