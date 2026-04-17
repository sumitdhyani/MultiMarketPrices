#include <thread>
#include <ranges>
#include "Binance.h"
#include "RestApi.h"
#include <Logging.h>

using namespace NanoLog::LogLevels;

// ====================== IMPLEMENTATION ======================

BinanceRestClient::BinanceRestClient(net::strand<net::io_context::executor_type>& strand,
        ssl::context& ctx,
        const std::function<void(const beast::error_code&)>& readyHandler,
        const uint16_t& retryIntervalSec)
    : m_strand(strand)
    , m_ctx(ctx)
    , m_stream(std::make_unique<beast::ssl_stream<beast::tcp_stream>>(strand, ctx))
    , m_resolver(strand)
    , m_timer(strand)
    , m_connected(false)
    , m_connectedOnce(false)
    , m_readyCallback(readyHandler)
    , m_retryIntervalSec(retryIntervalSec)
{
}

void BinanceRestClient::run()
{
    NANO_LOG(DEBUG, "Starting Binance REST client...");
    m_resolver.async_resolve(m_host, m_port,
            beast::bind_front_handler(&BinanceRestClient::on_resolve, shared_from_this()));
    // This function can be used to start the IO context or any other necessary setup
}

void BinanceRestClient::launch_request(const std::string& path,
                                       http::verb method,
                                       const std::string& query,
                                       const Callback& cb)
{
    std::string target = m_api_version + path;
    if (!query.empty()) target += "?" + query;

    net::post(m_strand, [self = shared_from_this(), this, target, method, query, cb]() {
        m_queue.push({target, method, query, cb});
        if (m_queue.size() == 1 && m_connected)
        {
            start_next_request();
        }
    });
    
}

void BinanceRestClient::start_next_request()
{
    auto const& current = m_queue.front();
    m_request = {current.method, current.target, 11};
    m_request.set(http::field::host, m_host);
    m_request.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

    //std::cout << "[Binance REST] Sending request: " << m_request.method_string() << " " << m_request.target() << std::endl;
    http::async_write(*m_stream, m_request,
        beast::bind_front_handler(&BinanceRestClient::on_write, shared_from_this()));
    //std::cout << "[Binance REST] sent request: " << std::endl;
}

void BinanceRestClient::on_resolve(beast::error_code ec, tcp::resolver::results_type results)
{
    if (ec) return fail(ec, "resolve");

    NANO_LOG(DEBUG, "[Binance REST] DNS resolution successful, connecting...");
    beast::get_lowest_layer(*m_stream).expires_after(std::chrono::seconds(30));

    beast::get_lowest_layer(*m_stream).async_connect(results,
        beast::bind_front_handler(&BinanceRestClient::on_connect, shared_from_this()));
}

void BinanceRestClient::on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type)
{
    if (ec) return fail(ec, "connect");

    NANO_LOG(DEBUG, "[Binance REST] Connected to server, performing SSL handshake...");
    // SNI Hostname
    if (!SSL_set_tlsext_host_name((*m_stream).native_handle(), m_host.c_str()))
    {
        ec = beast::error_code(static_cast<int>(::ERR_get_error()),
                               net::error::get_ssl_category());
        return fail(ec, "SNI");
    }

    (*m_stream).async_handshake(ssl::stream_base::client,
        beast::bind_front_handler(&BinanceRestClient::on_handshake, shared_from_this()));
}

void BinanceRestClient::on_handshake(beast::error_code ec)
{
    if (ec) return fail(ec, "handshake");

    NANO_LOG(DEBUG, "[Binance REST] SSL handshake successful, connection established!");

    beast::get_lowest_layer(*m_stream).expires_never();
    m_connected = true;
    NANO_LOG(DEBUG, "[Binance REST] Connected successfully!");

    m_timer.cancel();
    scheduleKeepAlive();
    if (!m_queue.empty())
    {
        start_next_request();
    }

    if(!m_connectedOnce)
    {
        m_connectedOnce = true;
        // Reply with a null error
        m_readyCallback(beast::error_code());
    }
}

void BinanceRestClient::on_write(beast::error_code ec, std::size_t)
{
    if (ec) return fail(ec, "write");
    do_read();
}

void BinanceRestClient::do_read()
{
    //std::cout << "[Binance REST] awaiting next response..." << std::endl;
    m_buffer.clear();
    http::async_read(*m_stream, m_buffer, m_response,
        beast::bind_front_handler(&BinanceRestClient::on_read, shared_from_this()));
}

void BinanceRestClient::on_read(beast::error_code ec, std::size_t bytesRead)
{
    if (ec) return fail(ec, "read");

    boost::json::object result;
    try {
        if (!m_response.body().empty())
            NANO_LOG(DEBUG, "%s", m_response.body().c_str());
            NANO_LOG(DEBUG, "========================================");
            result[*BinanceTag::data()] = boost::json::parse(m_response.body());
    } catch (const std::exception& e) {
        NANO_LOG(DEBUG, "[Binance REST] JSON parse error: %s", e.what());
    }

    m_response = {};  // Full reset — clear() only clears headers, not body
    // Call user callback
    auto const& cb = m_queue.front().callback;
    cb(result, ec);
    m_queue.pop();

    // Reset and process next
    if (!m_queue.empty())
    {
        start_next_request();
    }
}

void BinanceRestClient::fail(beast::error_code ec, const char* what)
{
    NANO_LOG(DEBUG, "[Binance REST] %s: %s", what, ec.message().c_str());
    m_connected = false;

    // The session is to be terminated only if the clinet was never connected
    // if the session has once started, it should try to reconsile indefinitely
    if (isFatalError(ec) && !m_connectedOnce)
    {
        NANO_LOG(DEBUG, "[Binance REST] Fatal error encountered. Closing connection.");
        m_timer.cancel();
        close_connection();
        while (!m_queue.empty())
        {
            auto const& cb = m_queue.front().callback;
            cb(json::object{}, ec);
            m_queue.pop();
        }

        m_readyCallback(ec);
        return;
    }
    
    m_resolver  = tcp::resolver(m_strand);
    m_stream    = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(m_strand, m_ctx); 
    m_timer.cancel();
    m_timer.expires_after(std::chrono::seconds(m_retryIntervalSec));
    m_timer.async_wait(
        [self = shared_from_this(), this]
        (const boost::system::error_code& ec)
        {
            if(!ec) run();
            else NANO_LOG(DEBUG, "Problem with timer, details: %s", ec.message().c_str());
        }
    );
}

void BinanceRestClient::close_connection()
{
    beast::error_code ignore;
    (*m_stream).shutdown(ignore);
}

// ====================== PUBLIC METHODS ======================
void BinanceRestClient::ping(const Callback& cb)                    { launch_request("/ping", http::verb::get, "", cb); }
void BinanceRestClient::getServerTime(const Callback& cb)           { launch_request("/time", http::verb::get, "", cb); }
void BinanceRestClient::getExchangeInfo(const Callback& cb)         { launch_request("/exchangeInfo", http::verb::get, "", cb); }

void BinanceRestClient::getDepth(const std::string& symbol, int limit, const Callback& cb)
{
    NANO_LOG(DEBUG, "[Binance REST] Queueing getDepth request for symbol: %s with limit: %d", symbol.c_str(), limit);
    std::string q = "symbol=" + symbol + "&limit=" + std::to_string(limit);
    launch_request("/depth", http::verb::get, q, cb);
}

void BinanceRestClient::getRecentTrades(const std::string& symbol, int limit, const Callback& cb)
{
    std::string q = "symbol=" + symbol + "&limit=" + std::to_string(limit);
    launch_request("/trades", http::verb::get, q, cb);
}

void BinanceRestClient::getTickerPrice(const std::string& symbol, const Callback& cb)
{
    std::string q = symbol.empty() ? "" : "symbol=" + symbol;
    launch_request("/ticker/price", http::verb::get, q, cb);
}

void BinanceRestClient::get24hrTicker(const std::string& symbol, const Callback& cb)
{
    std::string q = symbol.empty() ? "" : "symbol=" + symbol;
    launch_request("/ticker/24hr", http::verb::get, q, cb);
}

void BinanceRestClient::getKlines(const std::string& symbol,
                                  const std::string& interval,
                                  int limit,
                                  const Callback& cb)
{
    std::string q = "symbol=" + symbol +
                    "&interval=" + interval +
                    "&limit=" + std::to_string(limit);
    launch_request("/klines", http::verb::get, q, cb);
}

void processCommand(const std::string& command,
                    std::shared_ptr<BinanceRestClient> client,
                    net::strand<net::io_context::executor_type>& strand)
{
    NANO_LOG(DEBUG, "Processing command: %s", command.c_str());
    auto tokens = command |
                 std::views::split(' ') | 
                 std::ranges::to<std::vector<std::string>>();
    
    if (tokens.size() < 2)
    {
        NANO_LOG(DEBUG, "Invalid command format. Expected: <command> <args...>");
        return;
    }

    if(tokens[0] == "d") {
        net::post(strand, 
            [client, symbol = tokens[1]](){
                client->getDepth(symbol, 5, [](auto json, auto ec){
                    if (ec) {
                        NANO_LOG(DEBUG, "Error in getDepth: %s", ec.message().c_str());
                    } else {
                        NANO_LOG(DEBUG, "Depth response: %s", boost::json::serialize(json).c_str());
                    }
                });
            }
        );
    }
    else if(tokens[0] == "t") {
        net::post(strand, 
            [client, symbol = tokens[1]](){
                client->getRecentTrades(symbol, 1, [](auto json, auto ec){
                    if (ec) {
                        NANO_LOG(DEBUG, "Error in getRecentTrades: %s", ec.message().c_str());
                    } else {
                        NANO_LOG(DEBUG, "RecentTrades response: %s", boost::json::serialize(json).c_str());
                    }
                });
            }
        );
    }
}

// int main()
// {
//     net::io_context ioc;
//     ssl::context ctx(ssl::context::tlsv12_client);
//     auto strand  = net::make_strand<net::io_context::executor_type>(ioc.get_executor());
//     std::shared_ptr<BinanceRestClient> client =
//     std::make_shared<BinanceRestClient>(strand,
//         ctx,
//         [&client, &strand]() {
//             std::thread([&client, &strand](){ 
//                 while(true)
//                 {
//                     std::cout << "Enter command (e.g., 'd BTCUSDT' for depth, 't' for ping): " << std::endl;
//                     std::string command;
//                     std::getline(std::cin, command);
//                     processCommand(command, client, strand);
//                 }
//             }).detach();    
//         },
//         [](const beast::error_code& ec) {
//             std::cout << "Received fatal error from rest client, details: " << ec.message() << std::endl;
//         },
//         10
//     );
    
//     client->run();
//     ioc.run();
//     return 0;
// }