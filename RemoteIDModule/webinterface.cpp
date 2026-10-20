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

static WebServer server(80);

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
        server.send(200, "application/json", status_json());
        return true;
    }

} AJAX_Handler;

/*
  init web server
 */
void WebInterface::init(void)
{
    Serial.printf("WAP start %s %s\n", g.wifi_ssid, g.wifi_password);
    IPAddress myIP = WiFi.softAPIP();

    server.addHandler( &AJAX_Handler );
    server.addHandler( &ROMFS_Handler );

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
    if (!initialised) {
        init();
        initialised = true;
    }
    server.handleClient();
}
