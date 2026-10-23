#include "webinterface.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <WiFiAP.h>
#include <ESPmDNS.h>
#include <Update.h>
#include "parameters.h"
#include "romfs.h"
#include "check_firmware.h"
#include "status.h"
#include "activation_service.h"
#include "management_service.h"
#include "wifi_network.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// Only the network task owns server/OTA/upload state. The flight/CAN loop
// publishes a bounded status copy; it never waits for a slow HTTP client.
namespace {
class ManagementWebServer : public WebServer {
public:
    ManagementWebServer() : WebServer(80) {}
    bool listening() { return bool(_server); }
};
ManagementWebServer server;
struct StatusPacket { char json[8192]; };
QueueHandle_t status_queue=nullptr;
StatusPacket received_status={"{}"};
}


class ESPOTAUploadBackend : public OTAUpload::Backend {
public:
    bool begin(size_t &capacity) override
    {
        partition = esp_ota_get_next_update_partition(nullptr);
        if (partition == nullptr) {
            return false;
        }
        capacity = partition->size;
        return Update.begin(UPDATE_SIZE_UNKNOWN);
    }

    size_t write(const uint8_t *data, size_t size) override
    {
        return Update.write(const_cast<uint8_t *>(data), size);
    }

    bool verify(const uint8_t *lead, size_t size) override
    {
        return partition != nullptr && !Update.hasError() &&
            CheckFirmware::check_OTA_next(partition, lead, size);
    }

    bool end() override { return !Update.hasError() && Update.end(true); }
    void abort() override { Update.abort(); }

private:
    const esp_partition_t *partition = nullptr;
};

static ESPOTAUploadBackend ota_backend;

/*
  serve files from ROMFS
 */
class ROMFS_Handler : public RequestHandler
{
    bool canHandle(HTTPMethod method, String uri) {
        if (uri == "/") {
            uri = "/index.html";
        }
        uri = "web" + uri;
        if (ROMFS::exists(uri.c_str())) {
            return true;
        }
        return false;
    }

    bool handle(WebServer& server, HTTPMethod requestMethod, String requestUri) {
        if (requestUri == "/") {
            requestUri = "/index.html";
        }
        String uri = "web" + requestUri;
        Serial.printf("handle: '%s'\n", requestUri.c_str());

        // work out content type
        const char *content_type = "text/html";
        const struct {
            const char *extension;
            const char *content_type;
        } extensions[] = {
            { ".js", "text/javascript" },
            { ".jpg", "image/jpeg" },
            { ".png", "image/png" },
            { ".css", "text/css" },
        };
        for (const auto &e : extensions) {
            if (uri.endsWith(e.extension)) {
                content_type = e.content_type;
                break;
            }
        }

        auto *f = ROMFS::find_stream(uri.c_str());
        if (f != nullptr) {
            server.sendHeader("Content-Encoding", "gzip");
            server.streamFile(*f, content_type);
            delete f;
            return true;
        }
        return false;
    }

} ROMFS_Handler;

/*
  serve files from ROMFS
 */
class AJAX_Handler : public RequestHandler
{
    bool canHandle(HTTPMethod method, String uri) {
        return uri == "/ajax/status.json";
    }

    bool handle(WebServer& server, HTTPMethod requestMethod, String requestUri) {
        if (requestUri != "/ajax/status.json") {
            return false;
        }
        if (status_queue) xQueueReceive(status_queue,&received_status,0);
        server.send(200, "application/json", received_status.json);
        return true;
    }

} AJAX_Handler;

/*
  init web server
 */
void WebInterface::init(void)
{
    Serial.printf("WAP management started\n");

    server.addHandler( &AJAX_Handler );
    server.addHandler( &ROMFS_Handler );

    server.on("/ajax/activation.json",HTTP_GET,[]() {
        server.sendHeader("Cache-Control","no-store");
        server.send(200,"application/json",ActivationService::status_json());
    });
    /*handling uploading firmware file */
    server.on("/update", HTTP_POST, [this]() {
        server.sendHeader("Connection", "close");
        if (!ota_upload.complete(ota_backend)) {
            server.send(500, "text/plain", "FAIL");
            Serial.printf("Update failed; current firmware remains active\n");
            return;
        }
        server.send(200, "text/plain", "OK");
        Serial.printf("Update success: rebooting\n");
        delay(1000);
        ESP.restart();
    }, [this]() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf("Update: %s\n", upload.filename.c_str());
            // Preserve the explicit LOCK_LEVEL=-1 development exception for
            // raw images; normal uploads must carry the complete descriptor.
            ota_upload.start(ota_backend, g.lock_level >= 0);
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            ota_upload.write(ota_backend, upload.buf, upload.currentSize);
        } else if (upload.status == UPLOAD_FILE_END) {
            ota_upload.finish(ota_backend, upload.totalSize);
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
            ota_upload.fail(ota_backend);
        }
    });
}

void WebInterface::update()
{
    static StatusPacket outgoing;
    static uint32_t last_status_ms=0;
    if (!initialised) {
        status_queue=xQueueCreate(1,sizeof(StatusPacket));
        if (status_queue==nullptr) return;
        const BaseType_t created=xTaskCreate([](void *context) {
            auto *self=static_cast<WebInterface *>(context);
            self->init();
            bool network_ready=false;
            bool network_checked=false;
            uint32_t last_network_check_ms=0;
            for (;;) {
                const uint32_t now=millis();
                // Address checks synchronize with TCP/IP; poll once per second.
                if (!network_checked || now-last_network_check_ms>=1000) {
                    network_checked=true;
                    last_network_check_ms=now;
                    // Keep a static-address diagnostic path even if DHCP is down.
                    const bool ready=WiFiNetwork::ap_up() && WiFiNetwork::address_ready();
                    if (!ready && network_ready) {
                        server.client().stop();
                        server.stop();
                    }
                    network_ready=ready;
                    if (network_ready && !server.listening()) {
                        // begin() closes a socket left by a failed bind/listen.
                        server.begin();
                        Serial.printf(server.listening() ? "WAP HTTP listener started\n" :
                                      "WAP HTTP listener failed; retrying\n");
                    }
                }
                if (network_ready && server.listening()) server.handleClient();
                ManagementService::process();
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        },"rid-web",12288,this,1,nullptr);
        if (created!=pdPASS) { vQueueDelete(status_queue); status_queue=nullptr; return; }
        initialised=true;
    }
    const uint32_t now=millis();
    if (now-last_status_ms<500) return;
    last_status_ms=now;
    const String json=status_json();
    if (json.length()>=sizeof(outgoing.json)) {
        strcpy(outgoing.json,"{}");
    } else {
        memcpy(outgoing.json,json.c_str(),json.length()+1);
    }
    xQueueOverwrite(status_queue,&outgoing);
}
