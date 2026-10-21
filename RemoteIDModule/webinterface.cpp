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
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

static WebServer server(80);
// Only the network task owns server/OTA/upload state. The flight/CAN loop
// publishes a bounded status copy; it never waits for a slow HTTP client.
namespace {
struct StatusPacket { char json[8192]; };
QueueHandle_t status_queue=nullptr;
StatusPacket received_status={"{}"};
uint8_t activation_upload[rid_activation::PACKAGE_SIZE] {};
size_t activation_received=0;
bool activation_failed=true;
bool activation_finished=false;
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
    server.on("/activation",HTTP_POST,[]() {
        server.sendHeader("Connection","close");
        if (activation_failed || !activation_finished ||
            activation_received!=rid_activation::PACKAGE_SIZE) {
            server.send(400,"application/json","{\"error\":\"INVALID_FORMAT\"}");
        } else if (!ActivationService::submit(activation_upload,activation_received)) {
            server.send(503,"application/json","{\"error\":\"BUSY_OR_UNAVAILABLE\"}");
        } else {
            // Accepted is receipt only. Poll for durable PENDING_REBOOT or error.
            server.send(202,"application/json","{\"accepted\":true}");
        }
        activation_failed=true;
        activation_finished=false;
        activation_received=0;
    },[]() {
        HTTPUpload &upload=server.upload();
        if (upload.status==UPLOAD_FILE_START) {
            activation_received=0;
            activation_failed=false;
            activation_finished=false;
        } else if (upload.status==UPLOAD_FILE_WRITE) {
            if (activation_failed || upload.currentSize>sizeof(activation_upload)-activation_received) {
                activation_failed=true;
            } else {
                memcpy(activation_upload+activation_received,upload.buf,upload.currentSize);
                activation_received+=upload.currentSize;
            }
        } else if (upload.status==UPLOAD_FILE_END) {
            activation_finished=!activation_failed && activation_received==sizeof(activation_upload) &&
                                upload.totalSize==sizeof(activation_upload);
        } else if (upload.status==UPLOAD_FILE_ABORTED) {
            activation_failed=true;
            activation_finished=false;
        }
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
    Serial.printf("WAP started\n");
    server.begin();
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
            for (;;) {
                server.handleClient();
                ActivationService::process();
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
