/////////////////////////////////////////////////////////////////////////////
/** @file
Web Server

\copyright Copyright (c) 2018 Chris Byrne. All rights reserved.
Licensed under the MIT License. Refer to LICENSE file in the project root. */
/////////////////////////////////////////////////////////////////////////////
#ifndef UNIT_TEST

//- includes
#include "web_server.h"
#include "settings.h"
#include "web_server_asset_handler.h"
#include <ArduinoJson.h>

/////////////////////////////////////////////////////////////////////////////
/// constructor
WebServer::WebServer(Settings& settings)
: serverWebSocket_("/api/v1")
, settings_(settings)
{ }

/////////////////////////////////////////////////////////////////////////////
/// begin web server
void WebServer::begin() {
    // API requests
    {
        server_.on("/api/v1/ping", HTTP_GET, [](AsyncWebServerRequest* request) {
            request->send(200, "text/plain", "pong");
        });
        server_.on("/api/v1/state", HTTP_GET, [this](AsyncWebServerRequest* request) {
            auto* response = request->beginResponseStream("application/json");
            if (response) {
                DynamicJsonBuffer buffer;
                settings_.toJson(buffer).printTo(*response);
                request->send(response);
            }
        });

        // async WebSocket Event
        serverWebSocket_.onEvent([this](AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
            this->onWebSocketEvent_(server, client, type, arg, data, len);
        });
        server_.addHandler(&serverWebSocket_);

        // no match
        server_.on("/api", [](AsyncWebServerRequest* request) {
            request->send(404);
        });
    }

    // static web asset handler
    server_.addHandler(new WebAssetHandler());

    // 404
    server_.onNotFound([](AsyncWebServerRequest* request) {
        request->send( (request->method() == HTTP_OPTIONS) ? 200 : 404 );
    });

    // dirty property notifications
    settings_.onDirtyProperties([this](const JsonObject& obj, JsonBuffer& buffer) {
        auto& json = buffer.createObject();
        json["jsonrpc"] = "2.0";
        json["method"] = "update";
        json["params"] = obj;
        auto* textBuffer = serverWebSocket_.makeBuffer(json.measureLength());
        if (textBuffer) {
            json.printTo(reinterpret_cast<char*>(textBuffer->get()), textBuffer->length() + 1);
            serverWebSocket_.textAll(textBuffer);
        }
    });

    //
    server_.begin();
}

/////////////////////////////////////////////////////////////////////////////
void WebServer::tick() {
    
}

/////////////////////////////////////////////////////////////////////////////
/// web socket event
void WebServer::onWebSocketEvent_(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
printf("ws[%s][%u] connect\r\n", server->url(), client->id());
// client->printf("Hello Client %u :)", client->id());
// client->ping();
    } else if (type == WS_EVT_DISCONNECT) {
printf("ws[%s][%u] disconnect\r\n", server->url(), client->id());
    } else if (type == WS_EVT_ERROR) {
printf("ws[%s][%u] error(%u): %s\r\n", server->url(), client->id(), *((uint16_t*)arg), (char*)data);
    } else if (type == WS_EVT_PONG) {
printf("ws[%s][%u] pong[%u]: %s\r\n", server->url(), client->id(), len, (len)?(char*)data:"");
    } else if (type == WS_EVT_DATA) {
        // printf("ws[%s][%u] data\r\n", server->url(), client->id());

        auto* info = (AwsFrameInfo*)arg;
        if (info->opcode != WS_TEXT) return; // only interested in text frames

        data[len] = 0; // null terminate

        if (info->final && 0 == info->index && info->len == len) {
            //the whole message is in a single frame and we got all of it's data
            // printf("ws[%s][%u] %s-message[%llu]: ", server->url(), client->id(), (info->opcode == WS_TEXT) ? "text" : "binary", info->len);
            onJsonRpc_(client, (char*)data);

        } else {
            //message is comprised of multiple frames or the frame is split into multiple packets
            if (info->index == 0) {
                if (info->num == 0) {
                    printf("ws[%s][%u] %s-message start\r\n", server->url(), client->id(), (info->message_opcode == WS_TEXT)?"text":"binary");
                }
                printf("ws[%s][%u] frame[%u] start[%llu]\r\n", server->url(), client->id(), info->num, info->len);
            }
            printf("ws[%s][%u] frame[%u] %s[%llu - %llu]: ", server->url(), client->id(), info->num, (info->message_opcode == WS_TEXT)?"text":"binary", info->index, info->index + len);
            if ((info->index + len) == info->len) {
                printf("ws[%s][%u] frame[%u] end[%llu]\r\n", server->url(), client->id(), info->num, info->len);
                if (info->final) {
                    printf("ws[%s][%u] %s-message end\r\n", server->url(), client->id(), (info->message_opcode == WS_TEXT)?"text":"binary");
                    if (info->message_opcode == WS_TEXT) {
                        client->text("I got your text message");
                    } else {
                        client->binary("I got your binary message");
                    }
                }
            }
        }
    }
}

/////////////////////////////////////////////////////////////////////////////
/// on JSON-RPC data
void WebServer::onJsonRpc_(AsyncWebSocketClient* client, char* data) {
    // parse JSON
    DynamicJsonBuffer requestBuffer;
    const auto& request = requestBuffer.parseObject((char*)data);
    if (!request.success()) return; // :(

// request.printTo(Serial);

    // A String specifying the version of the JSON-RPC protocol. MUST be exactly "2.0".
    {
        const char* jsonrpc = request["jsonrpc"];
        if (!jsonrpc || 0 != strcmp(jsonrpc, "2.0")) return;
    }

    // A String containing the name of the method to be invoked
    const char* method = request["method"];
    if (!method) return;
    // A Structured value that holds the parameter values to be used during the invocation of the method. This member MAY be omitted
    const JsonVariant& params = request["params"];
    // An identifier established by the Client that MUST contain a String, Number, or NULL value if included
    const auto id = request["id"];

    // process request
    {
        DynamicJsonBuffer responseBuffer;
        auto result = settings_.onCommand(method, params, responseBuffer);

        // fill in response
        auto& response = responseBuffer.createObject();
        response["jsonrpc"] = "2.0";
        if (id.success()) response["id"] = id;

        if (JsonRpcError::NO_ERROR == result.first) {
            if (!result.second.success()) return;
            response["result"] = result.second;
        } else {
            auto& error = response.createNestedObject("error");
            error["code"] = static_cast<int>(result.first);
            error["message"] = result.second;
        }

        // send response
        auto* buffer = serverWebSocket_.makeBuffer(response.measureLength());
        if (buffer) {
            response.printTo(reinterpret_cast<char*>(buffer->get()), buffer->length() + 1);
            client->text(buffer);
        }
    }
}

#endif // UNIT_TEST
