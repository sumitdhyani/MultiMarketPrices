#pragma once
#include <NanoLog.h>
#include <NanoLogCpp17.h>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/error.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/json.hpp>
#include <boost/mp11/list.hpp>
#include <boost/system/detail/error_code.hpp>
#include <cstdlib>
#include <thread>
#include <functional>
#include <stdint.h>
#include <queue>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <Logging.h>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl;       // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

// Sends a WebSocket message and prints the response
class session : public std::enable_shared_from_this<session>
{
    enum class ErrorAction
    {
        Ignore,
        Retry,
        Reconnect,
        Fatal
    };

    enum class ErrorOrigin
    {
        Connect,
        Resolve,
        SSLHandshake,
        Handshake,
        Read,
        Write,
        Close
    };

    using PriceCallback = std::function<void(const std::string&)>; 
    // error, fatal(true means that the connection will not be recovered)
    using ReadyCallback = std::function<void(const beast::error_code&)>;

    tcp::resolver m_resolver;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> m_ws;
    beast::flat_buffer m_buffer;
    const std::string m_host;
    const std::string m_port;
    const std::string m_path;
    const PriceCallback m_priceCallback;
    const ReadyCallback m_readyCallback;
    std::queue<std::string> m_commandQueue;
    bool m_inFlight;
    bool m_connected;
    bool m_connectedOnce;
    int m_msgNo;
    const uint32_t m_retryDelay_sec;
    std::set<std::string> m_depthSubscriptions;
    std::set<std::string> m_tradeSubscriptions;
    net::steady_timer m_timer;

    void on_resolve(beast::error_code ec, tcp::resolver::results_type results)
    {
        if(ec && !on_error(ec, ErrorOrigin::Resolve)) return;

        // Set a timeout on the operation
        beast::get_lowest_layer(m_ws).expires_after(std::chrono::seconds(60));

        NANO_LOG(DEBUG, "[Binance WS] on_resolve");
        // Make the connection on the IP address we get from a lookup
        beast::get_lowest_layer(m_ws).async_connect(
            results,
            beast::bind_front_handler(
                &session::on_connect,
                shared_from_this()));
    }

    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep)
    {
        if(ec && !on_error(ec, ErrorOrigin::Connect)) return;

        std::string hostPort = m_host + ':' + std::to_string(ep.port());

        if(!SSL_set_tlsext_host_name(
                m_ws.next_layer().native_handle(),
                m_host.c_str()))  // ⚠️ use host, not host:port
        {
            ec = beast::error_code(static_cast<int>(::ERR_get_error()),
                net::error::get_ssl_category());
            if(!on_error(ec, ErrorOrigin::Connect)) return;
        }

        beast::get_lowest_layer(m_ws).expires_after(std::chrono::seconds(30));

        m_ws.next_layer().async_handshake(
            ssl::stream_base::client,
            beast::bind_front_handler(
                &session::on_ssl_handshake,
                shared_from_this()));
    }

    void on_ssl_handshake(beast::error_code ec)
    {
        if(ec && !on_error(ec, ErrorOrigin::SSLHandshake)) return;

        // Turn off the timeout on the tcp_stream, because
        // the websocket stream has its own timeout system.
        beast::get_lowest_layer(m_ws).expires_never();

        // Set suggested timeout settings for the websocket
        m_ws.set_option(
            websocket::stream_base::timeout::suggested(
                beast::role_type::client));

        // Set a decorator to change the User-Agent of the handshake
        m_ws.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req)
            {
                req.set(http::field::user_agent,
                    std::string(BOOST_BEAST_VERSION_STRING) +
                        " websocket-client-async-ssl");
            }));

        NANO_LOG(DEBUG, "[Binance WS] on_ssl_handshake");
        // Perform the websocket handshake
        m_ws.async_handshake(m_host, m_path,
            beast::bind_front_handler(
                &session::on_handshake,
                shared_from_this()));
    }

    void on_handshake(beast::error_code ec)
    {
        if(ec && !on_error(ec, ErrorOrigin::Handshake)) return;
        m_connected = true;
        beast::get_lowest_layer(m_ws).expires_never();
        NANO_LOG(DEBUG, "[Binance WS] on_handshake");

        for (auto const& symbol : m_tradeSubscriptions) subscribeTrade(symbol);
        for (auto const& symbol : m_depthSubscriptions) subscribeDepth(symbol);
        
        // Notify with null error
        if(!m_connectedOnce)
        {
            m_connectedOnce = true;
            m_readyCallback(beast::error_code());
        }

        read();
    }

    void read()
    {
        m_ws.async_read(m_buffer,
                        beast::bind_front_handler(&session::on_read, shared_from_this())
        );
    }

    void on_read(beast::error_code ec, size_t bytes_transferred)
    {
        if(ec && !on_error(ec, ErrorOrigin::Read)) return;
        
        std::string payload = beast::buffers_to_string(m_buffer.data());
        m_buffer.consume(bytes_transferred);
        if (payload.find("@trade") != std::string::npos ||
            payload.find("@depth") != std::string::npos)
        {
            m_priceCallback(payload);
        }
        else
        {
            // log error about invalid format
        }

        read();
    }

    void closeConnection()
    {
        if (m_ws.is_open())
        {
            beast::error_code ec;
            m_ws.close(websocket::close_code::normal, ec);   // graceful close

            // if graceful close fails, force close the connection
            if (ec) beast::get_lowest_layer(m_ws).close();
        }
    }

    void on_write(beast::error_code ec, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if(ec && !on_error(ec, ErrorOrigin::Write))
        {
            m_inFlight = false;
            return;
        }

        m_commandQueue.pop();
        if (m_commandQueue.empty()) {
            m_inFlight = false;
            return;
        }

        write();
        
    }

    void on_close(beast::error_code ec)
    {
        if(!ec || ec == websocket::error::closed) return;
        on_error(ec, ErrorOrigin::Close);
        // If we get here then the connection is closed gracefully
    }

    const char* errorOriginToString(ErrorOrigin origin)
    {
        switch (origin)
        {
            case ErrorOrigin::Connect: return "Connect";
            case ErrorOrigin::Resolve: return "Resolve";
            case ErrorOrigin::SSLHandshake: return "SSLHandshake";
            case ErrorOrigin::Handshake: return "Handshake";
            case ErrorOrigin::Read: return "Read";
            case ErrorOrigin::Write: return "Write";
            case ErrorOrigin::Close: return "Close";
            default: return "Unknown";
        }
    }

    // True means continue with rest of the function
    bool on_error(beast::error_code ec, const ErrorOrigin& origin)
    {
        auto action = classifyError(ec);

        NANO_LOG(WARNING, "[Binance WS] %s: %s",
                errorOriginToString(origin), ec.message().c_str());

        switch (action)
        {
            case ErrorAction::Ignore:
                return true;

            case ErrorAction::Retry:
            {
                NANO_LOG(WARNING, "[Binance WS] Retrying operation");
                m_timer.cancel();
                handleRetry(origin);
                return false;
            }

            case ErrorAction::Reconnect:
            {
                NANO_LOG(WARNING, "[Binance WS] Reconnecting");

                m_timer.cancel();
                closeConnection();

                // reset state
                m_inFlight = false;
                m_connected = false;

                m_timer.expires_after(std::chrono::seconds(m_retryDelay_sec));

                m_timer.async_wait([self = shared_from_this()](const beast::error_code& ec){
                    if (ec) return;
                    self->run();
                });

                return false;
            }

            case ErrorAction::Fatal:
            {
                NANO_LOG(ERROR, "[Binance WS] Fatal error, shutting down");

                m_timer.cancel();
                closeConnection();

                if (!m_connectedOnce)
                    m_readyCallback(ec);

                return false;
            }
        }

        return false;
    }

    bool hasSubscriptions()
    {
        return !(m_depthSubscriptions.empty() && m_tradeSubscriptions.empty());
    }

    ErrorAction classifyError(const beast::error_code& ec)
    {
        using namespace boost::asio;
        using namespace beast;
        namespace ws = websocket;

        if (ec == net::error::operation_aborted)
            return ErrorAction::Ignore;

        if (ec == ws::error::closed)
            return ErrorAction::Reconnect;

        if (ec == beast::error::timeout)
            return ErrorAction::Reconnect;

        if (ec == ws::condition::handshake_failed)
            return ErrorAction::Fatal;

        if (ec == ws::condition::protocol_violation)
            return ErrorAction::Reconnect;

        if (ec.category() == net::error::get_ssl_category())
            return ErrorAction::Fatal;

        if (ec == net::error::connection_reset)
            return ErrorAction::Reconnect;

        if (ec == net::error::would_block ||
            ec == net::error::try_again)
            return ErrorAction::Retry;

        return ErrorAction::Reconnect;
    }

    void handleRetry(ErrorOrigin origin)
    {
        m_timer.cancel();
        switch (origin)
        {
            case ErrorOrigin::Write:
            if(!m_ws.is_open())
            {
                run();
            }
            else if (!m_commandQueue.empty())
            {
                auto const& cmd = m_commandQueue.front();

                m_ws.async_write(net::buffer(cmd),
                    beast::bind_front_handler(
                        &session::on_write,
                        shared_from_this()));
                m_inFlight = true;
            }
            break;

            case ErrorOrigin::Read:
                read(); // restart read loop
                break;
            default:
                // fallback: reconnect
                run();
                break;
        }
    }

public:
    // Resolver and socket require an io_context
    explicit
    session(net::strand<net::io_context::executor_type>& strand, 
        ssl::context& ctx,
        const PriceCallback& priceCallback,
        const std::string& host,
        const std::string& port,
        const std::string& path,
        const uint32_t& retryDelay_sec,
        const ReadyCallback& readyCallback)
        : m_resolver(strand)
        , m_ws(strand, ctx)
        , m_priceCallback(priceCallback)
        , m_inFlight(false)
        , m_connected(false)
        , m_connectedOnce(false)
        , m_host(host)
        , m_port(port)
        , m_path(path)
        , m_retryDelay_sec(retryDelay_sec)
        , m_readyCallback(readyCallback)
        , m_timer(strand)
    {}

    bool subscribeTrade(const std::string& symbol)
    {
        if (m_tradeSubscriptions.contains(symbol))
            return false;

        std::string stream = symbol + "@trade";
        boost::json::object jsonObj {
            {"method", "SUBSCRIBE"},
            {"id", ++m_msgNo},
            {"params", boost::json::array{stream}}
        };

        sendOrQueue(boost::json::serialize(jsonObj));
        m_tradeSubscriptions.insert(symbol);
        return true;
    }

    bool subscribeDepth(const std::string& symbol)
    {
        if (m_depthSubscriptions.contains(symbol))
            return false;

        std::string stream = symbol + "@depth5";
        boost::json::object jsonObj {
            {"method", "SUBSCRIBE"},
            {"id", ++m_msgNo},
            {"params", boost::json::array{stream}}
        };

        sendOrQueue(boost::json::serialize(jsonObj));
        m_depthSubscriptions.insert(symbol);
        return true;
    }

    bool unsubscribeTrade(const std::string& symbol)
    {
        if (!m_tradeSubscriptions.contains(symbol))
            return false;

        std::string stream = symbol + "@trade";
        boost::json::object jsonObj {
            {"method", "UNSUBSCRIBE"},
            {"id", ++m_msgNo},
            {"params", boost::json::array{stream}}
        };

        sendOrQueue(boost::json::serialize(jsonObj));
        m_tradeSubscriptions.erase(symbol);
        return true;
    }

    bool unsubscribeDepth(const std::string& symbol)
    {
        if (!m_depthSubscriptions.contains(symbol))
            return false;

        std::string stream = symbol + "@depth5";
        boost::json::object jsonObj {
            {"method", "UNSUBSCRIBE"},
            {"id", ++m_msgNo},
            {"params", boost::json::array{stream}}
        };

        sendOrQueue(boost::json::serialize(jsonObj));
        m_depthSubscriptions.erase(symbol);
        return true;
    }

    void sendOrQueue(const std::string& jsonCmd)
    {
        m_commandQueue.push(jsonCmd);
        if (!m_connected) return;
        else if (!m_inFlight)
        {
            write();
            m_inFlight = true;
        }
    }

    void write()
    {
        auto const &cmd = m_commandQueue.front();
        NANO_LOG(DEBUG, "Sending command: %s", cmd.c_str());
            m_ws.async_write(net::buffer(cmd),
                             beast::bind_front_handler(&session::on_write, shared_from_this())
            );
    }

    // Start the asynchronous operation
    void run()
    {
        // Save these for later
        NANO_LOG(DEBUG, "[Binance WS] run");
        // Look up the domain name
        m_resolver.async_resolve(
            m_host,
            m_port,
            beast::bind_front_handler(
                &session::on_resolve,
                shared_from_this()));
    }

};

// int main(int argc, char** argv)
// {
//     char const* host = "stream.binance.com";
//     char const* port = "9443";
//     auto const path = "/stream";

//     // The io_context is required for all I/O
//     net::io_context ioc;

//     // The SSL context is required, and holds certificates
//     ssl::context ctx{ssl::context::tlsv12_client};

//     // Launch the asynchronous operation
//     // std::shared_ptr<session> sess;
//     std::shared_ptr<session> sess = std::make_shared<session>(ioc,
//         ctx, 
//         [](const std::string& update) {
//             std::cout << "Update: " << update << std::endl;
//         },
//         host,
//         port,
//         path,
//         10,
//         [](const beast::error_code& ec, bool isFatal){},
//         [&sess](){
//             std::cout << "Connection established, subscribing to streams..." << std::endl;
//             sess->subscribeTrade("btcusdt");
//         });

//     sess->run();
//     ioc.run();

//     return EXIT_SUCCESS;
// }
