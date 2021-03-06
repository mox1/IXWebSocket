/*
 *  ws_send.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2017-2018 Machine Zone, Inc. All rights reserved.
 */

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <ixcrypto/IXBase64.h>
#include <ixcrypto/IXHash.h>
#include <ixcrypto/IXUuid.h>
#include <ixwebsocket/IXSocket.h>
#include <ixwebsocket/IXSocketTLSOptions.h>
#include <ixwebsocket/IXWebSocket.h>
#include <msgpack11/msgpack11.hpp>
#include <mutex>
#include <spdlog/spdlog.h>
#include <sstream>
#include <vector>

using msgpack11::MsgPack;

namespace ix
{
    class WebSocketSender
    {
    public:
        WebSocketSender(const std::string& _url,
                        bool enablePerMessageDeflate,
                        const ix::SocketTLSOptions& tlsOptions);

        void subscribe(const std::string& channel);
        void start();
        void stop();

        void waitForConnection();
        void waitForAck();

        void sendMessage(const std::string& filename, bool throttle);

    private:
        std::string _url;
        std::string _id;
        ix::WebSocket _webSocket;
        bool _enablePerMessageDeflate;

        std::mutex _conditionVariableMutex;
        std::condition_variable _condition;

        void log(const std::string& msg);
    };

    WebSocketSender::WebSocketSender(const std::string& url,
                                     bool enablePerMessageDeflate,
                                     const ix::SocketTLSOptions& tlsOptions)
        : _url(url)
        , _enablePerMessageDeflate(enablePerMessageDeflate)
    {
        _webSocket.disableAutomaticReconnection();
        _webSocket.setTLSOptions(tlsOptions);
    }

    void WebSocketSender::stop()
    {
        _webSocket.stop();
    }

    void WebSocketSender::log(const std::string& msg)
    {
        spdlog::info(msg);
    }

    void WebSocketSender::waitForConnection()
    {
        spdlog::info("{}: Connecting...", "ws_send");

        std::unique_lock<std::mutex> lock(_conditionVariableMutex);
        _condition.wait(lock);
    }

    void WebSocketSender::waitForAck()
    {
        spdlog::info("{}: Waiting for ack...", "ws_send");

        std::unique_lock<std::mutex> lock(_conditionVariableMutex);
        _condition.wait(lock);
    }

    std::vector<uint8_t> load(const std::string& path)
    {
        std::vector<uint8_t> memblock;

        std::ifstream file(path);
        if (!file.is_open()) return memblock;

        file.seekg(0, file.end);
        std::streamoff size = file.tellg();
        file.seekg(0, file.beg);

        memblock.resize((size_t) size);
        file.read((char*) &memblock.front(), static_cast<std::streamsize>(size));

        return memblock;
    }

    void WebSocketSender::start()
    {
        _webSocket.setUrl(_url);

        ix::WebSocketPerMessageDeflateOptions webSocketPerMessageDeflateOptions(
            _enablePerMessageDeflate, false, false, 15, 15);
        _webSocket.setPerMessageDeflateOptions(webSocketPerMessageDeflateOptions);

        std::stringstream ss;
        log(std::string("ws_send: Connecting to url: ") + _url);

        _webSocket.setOnMessageCallback([this](const WebSocketMessagePtr& msg) {
            std::stringstream ss;
            if (msg->type == ix::WebSocketMessageType::Open)
            {
                _condition.notify_one();

                log("ws_send: connected");
                spdlog::info("Uri: {}", msg->openInfo.uri);
                spdlog::info("Headers:");
                for (auto it : msg->openInfo.headers)
                {
                    spdlog::info("{}: {}", it.first, it.second);
                }
            }
            else if (msg->type == ix::WebSocketMessageType::Close)
            {
                ss << "ws_send: connection closed:";
                ss << " code " << msg->closeInfo.code;
                ss << " reason " << msg->closeInfo.reason << std::endl;
                log(ss.str());
            }
            else if (msg->type == ix::WebSocketMessageType::Message)
            {
                _condition.notify_one();

                ss << "ws_send: received message (" << msg->wireSize << " bytes)";
                log(ss.str());

                std::string errMsg;
                MsgPack data = MsgPack::parse(msg->str, errMsg);
                if (!errMsg.empty())
                {
                    spdlog::info("Invalid MsgPack response");
                    return;
                }

                std::string id = data["id"].string_value();
                if (_id != id)
                {
                    spdlog::info("Invalid id");
                }
            }
            else if (msg->type == ix::WebSocketMessageType::Error)
            {
                ss << "ws_send ";
                ss << "Connection error: " << msg->errorInfo.reason << std::endl;
                ss << "#retries: " << msg->errorInfo.retries << std::endl;
                ss << "Wait time(ms): " << msg->errorInfo.wait_time << std::endl;
                ss << "HTTP Status: " << msg->errorInfo.http_status << std::endl;
                log(ss.str());
            }
            else
            {
                ss << "ws_send: Invalid ix::WebSocketMessageType";
                log(ss.str());
            }
        });

        _webSocket.start();
    }

    class Bench
    {
    public:
        Bench(const std::string& description)
            : _description(description)
            , _start(std::chrono::system_clock::now())
            , _reported(false)
        {
            ;
        }

        ~Bench()
        {
            if (!_reported)
            {
                report();
            }
        }

        void report()
        {
            auto now = std::chrono::system_clock::now();
            auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now - _start);

            _ms = milliseconds.count();
            spdlog::info("{} completed in {}", _description, _ms);

            _reported = true;
        }

        uint64_t getDuration() const
        {
            return _ms;
        }

    private:
        std::string _description;
        std::chrono::time_point<std::chrono::system_clock> _start;
        uint64_t _ms;
        bool _reported;
    };

    void WebSocketSender::sendMessage(const std::string& filename, bool throttle)
    {
        std::vector<uint8_t> content;
        {
            Bench bench("load file from disk");
            content = load(filename);
        }

        _id = uuid4();

        std::map<MsgPack, MsgPack> pdu;
        pdu["kind"] = "send";
        pdu["id"] = _id;
        pdu["content"] = content;
        auto hash = djb2Hash(content);
        pdu["djb2_hash"] = std::to_string(hash);
        pdu["filename"] = filename;

        MsgPack msg(pdu);

        Bench bench("Sending file through websocket");
        _webSocket.sendBinary(msg.dump(), [throttle](int current, int total) -> bool {
            spdlog::info("ws_send: Step {} out of {}", current, total);

            if (throttle)
            {
                std::chrono::duration<double, std::milli> duration(10);
                std::this_thread::sleep_for(duration);
            }

            return true;
        });

        do
        {
            size_t bufferedAmount = _webSocket.bufferedAmount();
            spdlog::info("ws_send: {} bytes left to be sent", bufferedAmount);

            std::chrono::duration<double, std::milli> duration(10);
            std::this_thread::sleep_for(duration);
        } while (_webSocket.bufferedAmount() != 0);

        bench.report();
        auto duration = bench.getDuration();
        auto transferRate = 1000 * content.size() / duration;
        transferRate /= (1024 * 1024);
        spdlog::info("ws_send: Send transfer rate: {} MB/s", transferRate);
    }

    void wsSend(const std::string& url,
                const std::string& path,
                bool enablePerMessageDeflate,
                bool throttle,
                const ix::SocketTLSOptions& tlsOptions)
    {
        WebSocketSender webSocketSender(url, enablePerMessageDeflate, tlsOptions);
        webSocketSender.start();

        webSocketSender.waitForConnection();

        spdlog::info("ws_send: Sending...");
        webSocketSender.sendMessage(path, throttle);

        webSocketSender.waitForAck();

        spdlog::info("ws_send: Done !");
        webSocketSender.stop();
    }

    int ws_send_main(const std::string& url,
                     const std::string& path,
                     const ix::SocketTLSOptions& tlsOptions)
    {
        bool throttle = false;
        bool enablePerMessageDeflate = false;

        wsSend(url, path, enablePerMessageDeflate, throttle, tlsOptions);
        return 0;
    }
} // namespace ix
