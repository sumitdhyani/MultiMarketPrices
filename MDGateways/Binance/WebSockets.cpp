//
// Copyright (c) 2016-2019 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

//------------------------------------------------------------------------------
//
// Example: WebSocket SSL client, asynchronous
//
//------------------------------------------------------------------------------

//#include "example/common/root_certificates.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/json.hpp>
#include <cstdlib>
#include <thread>
#include <functional>
#include <stdint.h>
#include <queue>
#include <iostream>
#include <memory>
#include <string>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl;       // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

//------------------------------------------------------------------------------

class session;

// Report a failure
void fail(beast::error_code ec,
          char const* what,
          std::shared_ptr<session> sess,
          std::chrono::seconds retryDelay);

// Sends a WebSocket message and prints the response
class session : public std::enable_shared_from_this<session>
{
    using PriceCallback = std::function<void(const std::string&, const std::string&)>; 
    tcp::resolver m_resolver;
    websocket::stream<
    beast::ssl_stream<beast::tcp_stream>> m_ws;
    beast::flat_buffer m_buffer;
    std::string m_host;
    std::string m_port;
    std::string m_path;
    const PriceCallback m_tradeCallback;
    const PriceCallback m_depthCallback;
    std::queue<std::string> m_commandQueue;
    bool m_inFlight;
    int m_msgNo;

    void on_resolve(beast::error_code ec, tcp::resolver::results_type results)
    {
        if(ec)
            return fail(ec, "resolve", shared_from_this(), std::chrono::seconds(5));

        // Set a timeout on the operation
        beast::get_lowest_layer(m_ws).expires_after(std::chrono::seconds(60));

        std::cout << "[Binance WS] on_resolve" << std::endl;
        // Make the connection on the IP address we get from a lookup
        beast::get_lowest_layer(m_ws).async_connect(
            results,
            beast::bind_front_handler(
                &session::on_connect,
                shared_from_this()));
    }

    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep)
    {
        if(ec)
            return fail(ec, "connect", shared_from_this(), std::chrono::seconds(5));

        // Update the host_ string. This will provide the value of the
        // Host HTTP header during the WebSocket handshake.
        // See https://tools.ietf.org/html/rfc7230#section-5.4
        m_host += ':' + std::to_string(ep.port());

        // Set a timeout on the operation
        beast::get_lowest_layer(m_ws).expires_after(std::chrono::seconds(30));

        // Set SNI Hostname (many hosts need this to handshake successfully)
        if(! SSL_set_tlsext_host_name(
                m_ws.next_layer().native_handle(),
                m_host.c_str()))
        {
            ec = beast::error_code(static_cast<int>(::ERR_get_error()),
                net::error::get_ssl_category());
            return fail(ec, "connect", shared_from_this(), std::chrono::seconds(5));
        }

        std::cout << "[Binance WS] on_connect" << std::endl;
        // Perform the SSL handshake
        m_ws.next_layer().async_handshake(
            ssl::stream_base::client,
            beast::bind_front_handler(
                &session::on_ssl_handshake,
                shared_from_this()));
    }

    void
    on_ssl_handshake(beast::error_code ec)
    {
        if(ec)
            return fail(ec, "ssl_handshake", shared_from_this(), std::chrono::seconds(5));

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

        std::cout << "[Binance WS] on_ssl_handshake" << std::endl;
        // Perform the websocket handshake
        m_ws.async_handshake(m_host, m_path,
            beast::bind_front_handler(
                &session::on_handshake,
                shared_from_this()));
    }

    void
    on_handshake(beast::error_code ec)
    {
        if(ec) return fail(ec, "handshake", shared_from_this(), std::chrono::seconds(5));

        std::cout << "[Binance WS] on_handshake" << std::endl;
        //subscribeDepth("btcusdt");
        subscribeTrade("btcusdt");
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
        //boost::ignore_unused(bytes_transferred);
        if(ec) return fail(ec, "read", shared_from_this(), std::chrono::seconds(5));
        
        std::string payload = beast::buffers_to_string(m_buffer.data());
        m_buffer.consume(bytes_transferred);
        try
        {
            if (payload.find("@trade") != std::string::npos)
            {
                 std::cout << "Received trade message: " << payload << std::endl;
            }
            else if (payload.find("@depth") != std::string::npos)
            {
                std::cout << "Received depth message: " << payload << std::endl;
            }
        }
        catch (std::exception& ex)
        {
            std::cerr << "Received message with unexpected format: " << ex.what() << std::endl;
        }

        read();
    }

    void on_write(beast::error_code ec,
        std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if(ec)
            return fail(ec, "write", shared_from_this(), std::chrono::seconds(5));
        
        if (m_commandQueue.empty()) {
            m_inFlight = false;
            return;
            
        }
        
        std::string cmd = m_commandQueue.front();
        m_commandQueue.pop();
        m_ws.async_write(net::buffer(cmd),
                         beast::bind_front_handler(&session::on_write, shared_from_this())
        );

        m_inFlight = true;
    }

    void on_close(beast::error_code ec)
    {
        if(ec)
            return fail(ec, "close", shared_from_this(), std::chrono::seconds(5));

        // If we get here then the connection is closed gracefully

        // The make_printable() function helps print a ConstBufferSequence
        std::cout << beast::make_printable(m_buffer.data()) << std::endl;
    }

    void fail(beast::error_code ec,
        char const* what,
        std::shared_ptr<session> sess,
        std::chrono::seconds retryDelay)
    {
        std::cerr << what << ": " << ec.message() << "\n";
        std::this_thread::sleep_for(retryDelay);
        sess->run();
    }


public:
    // Resolver and socket require an io_context
    explicit
    session(net::io_context& ioc, 
        ssl::context& ctx,
        const PriceCallback& tradeCallback,
        const PriceCallback& depthCallback,
        std::string host,
        std::string port,
        std::string path)
        : m_resolver(net::make_strand(ioc))
        , m_ws(net::make_strand(ioc), ctx)
        , m_tradeCallback(tradeCallback)
        , m_depthCallback(depthCallback)
        , m_inFlight(false)
        , m_host(host)
        , m_port(port)
        , m_path(path)
    {}

    void subscribeTrade(const std::string& symbol)
    {
        std::string stream = symbol + "@trade";
        boost::json::object jsonObj {
            {"method", "SUBSCRIBE"},
            {"id", ++m_msgNo},
            {"params", boost::json::array{stream}}
        };

        sendOrQueue(boost::json::serialize(jsonObj));
    }

    void subscribeDepth(const std::string& symbol)
    {
        std::string stream = symbol + "@depth5";
        boost::json::object jsonObj {
            {"method", "SUBSCRIBE"},
            {"id", ++m_msgNo},
            {"params", boost::json::array{stream}}
        };

        sendOrQueue(boost::json::serialize(jsonObj));
    }

    void unsubscribeTrade(const std::string& symbol)
    {
        std::string stream = symbol + "@trade";
        boost::json::object jsonObj {
            {"method", "UNSUBSCRIBE"},
            {"id", ++m_msgNo},
            {"params", boost::json::array{stream}}
        };

        sendOrQueue(boost::json::serialize(jsonObj));
    }

    void unsubscribeDepth(const std::string& symbol)
    {
        std::string stream = symbol + "@depth5";
        boost::json::object jsonObj {
            {"method", "UNSUBSCRIBE"},
            {"id", ++m_msgNo},
            {"params", boost::json::array{stream}}
        };

        sendOrQueue(boost::json::serialize(jsonObj));
    }

    void sendOrQueue(const std::string& jsonCmd)
    {
        if (m_inFlight) {
            m_commandQueue.push(jsonCmd);
        } else {
            std::cout << "Sending command: " << jsonCmd << std::endl;
            m_ws.async_write(net::buffer(jsonCmd),
                             beast::bind_front_handler(&session::on_write, shared_from_this())
            );
            m_inFlight = true;
        }
    }

    // Start the asynchronous operation
    void run()
    {
        // Save these for later
        std::cout << "[Binance WS] run" << std::endl;
        // Look up the domain name
        m_resolver.async_resolve(
            m_host,
            m_port,
            beast::bind_front_handler(
                &session::on_resolve,
                shared_from_this()));
    }

};

//------------------------------------------------------------------------------

void fail(beast::error_code ec,
          char const* what,
          std::shared_ptr<session> sess,
          std::chrono::seconds retryDelay)
{
    std::cerr << what << ": " << ec.message() << "\n";
    std::this_thread::sleep_for(retryDelay);
    sess->run();
}

int main(int argc, char** argv)
{
    // Check command line arguments.
    // if(argc != 4)
    // {
    //     std::cerr <<
    //         "Usage: websocket-client-async-ssl <host> <port> <text>\n" <<
    //         "Example:\n" <<
    //         "    websocket-client-async-ssl echo.websocket.org 443 \"Hello, world!\"\n";
    //     return EXIT_FAILURE;
    // }

    char const* host = "stream.binance.com";
    char const* port = "9443";
    auto const path = "/stream";

    // The io_context is required for all I/O
    net::io_context ioc;

    // The SSL context is required, and holds certificates
    ssl::context ctx{ssl::context::tlsv12_client};

    // This holds the root certificate used for verification
    //load_root_certificates(ctx);

    // Launch the asynchronous operation
    auto sess = std::make_shared<session>(ioc,
        ctx, 
        [](const std::string& symbol, const std::string& price) {
            std::cout << "Trade: " << symbol << " - " << price << std::endl;
        },
        [](const std::string& symbol, const std::string& depth) {
            std::cout << "Depth: " << symbol << " - " << depth << std::endl;
        },
        host,
        port,
        path
    );

    sess->run();
    std::this_thread::sleep_for(std::chrono::seconds(10)); // wait for connection to establish
    

    // Run the I/O service. The call will return when
    // the socket is closed.
    ioc.run();

    return EXIT_SUCCESS;
}