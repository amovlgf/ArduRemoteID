#include "RemoteIDModule/ota_upload.h"

#include <assert.h>
#include <stdio.h>
#include <vector>

class TestBackend : public OTAUpload::Backend {
public:
    size_t limit = 0x1f0000;
    size_t fail_at = size_t(-1);
    bool begin_ok = true;
    bool verify_ok = true;
    bool end_ok = true;
    unsigned begins = 0;
    unsigned verifies = 0;
    unsigned ends = 0;
    unsigned aborts = 0;
    bool active = false;
    std::vector<uint8_t> flash;

    bool begin(size_t &capacity) override
    {
        begins++;
        capacity = limit;
        flash.clear();
        active = begin_ok;
        return begin_ok;
    }
    size_t write(const uint8_t *data, size_t size) override
    {
        if (!active || flash.size() + size > fail_at) {
            return 0;
        }
        flash.insert(flash.end(), data, data + size);
        return size;
    }
    bool verify(const uint8_t *lead, size_t size) override
    {
        verifies++;
        assert(size == 16);
        assert(lead[0] == 0xe9);
        return verify_ok;
    }
    bool end() override { ends++; active = false; return end_ok; }
    void abort() override { aborts++; active = false; }
};

static std::vector<uint8_t> package()
{
    std::vector<uint8_t> data(256, 0x5a);
    data[0] = 0xe9;
    const uint8_t descriptor[] = { 0x43, 0x2a, 0xf1, 0x37, 0x46, 0xe2, 0x75, 0x19,
                                 13, 0, 0, 0, 0, 1, 0, 0 };
    data.insert(data.end(), descriptor, descriptor + sizeof(descriptor));
    data.resize(256 + 80, 0xa5);
    return data;
}

static void send(OTAUpload &upload, TestBackend &backend, const std::vector<uint8_t> &data,
                 size_t chunk_size)
{
    for (size_t offset = 0; offset < data.size();) {
        const size_t count = data.size() - offset < chunk_size ? data.size() - offset : chunk_size;
        assert(upload.write(backend, data.data() + offset, count));
        offset += count;
    }
}

static void valid_upload(OTAUpload &upload, TestBackend &backend, size_t chunk_size = 17)
{
    const auto data = package();
    assert(upload.start(backend, true));
    send(upload, backend, data, chunk_size);
    const unsigned prior_ends = backend.ends;
    assert(upload.finish(backend, data.size()));
    assert(backend.ends == prior_ends); // upload END alone must not activate
    assert(backend.flash.size() == data.size() + OTAUpload::flush_padding);
    assert(upload.complete(backend));
    assert(backend.ends == prior_ends + 1);
    assert(!upload.complete(backend)); // success cannot leak to a later POST
}

int main()
{
    for (const size_t chunk : {size_t(1), size_t(17), size_t(79), size_t(80), size_t(255), size_t(4096)}) {
        OTAUpload upload;
        TestBackend backend;
        valid_upload(upload, backend, chunk);
        valid_upload(upload, backend, chunk); // retry and second OTA transaction
        assert(backend.begins == 2);
    }
    {
        OTAUpload upload;
        TestBackend backend;
        assert(!upload.complete(backend)); // empty POST
        const auto data = package();
        assert(upload.start(backend, true));
        assert(upload.write(backend, data.data(), 20));
        upload.fail(backend); // HTTP disconnect/UPLOAD_FILE_ABORTED
        assert(!upload.complete(backend));
        assert(backend.ends == 0);
        valid_upload(upload, backend);
    }
    for (unsigned failure = 0; failure < 5; failure++) {
        OTAUpload upload;
        TestBackend backend;
        const auto data = package();
        if (failure == 0) backend.begin_ok = false;
        if (failure == 1) backend.fail_at = 15;
        if (failure == 2) backend.fail_at = data.size() + 5; // padding write failure
        if (failure == 3) backend.verify_ok = false; // signature/board verifier rejects
        if (failure == 4) backend.end_ok = false;
        if (upload.start(backend, true) && upload.write(backend, data.data(), data.size())) {
            upload.finish(backend, data.size());
        }
        assert(!upload.complete(backend));
        assert(!backend.active);
        if (failure < 4) assert(backend.ends == 0);
        backend.begin_ok = backend.verify_ok = backend.end_ok = true;
        backend.fail_at = size_t(-1);
        valid_upload(upload, backend);
    }
    for (unsigned malformed = 0; malformed < 5; malformed++) {
        OTAUpload upload;
        TestBackend backend;
        auto data = package();
        if (malformed == 0) data.resize(15);
        if (malformed == 1) data.pop_back();
        if (malformed == 2) data[0] = 0;
        if (malformed == 3) data[256 + 12] = 1;
        assert(upload.start(backend, true));
        send(upload, backend, data, 23);
        assert(!upload.finish(backend, data.size() + (malformed == 4 ? 1 : 0)));
        assert(!upload.complete(backend));
        assert(backend.verifies == 0 && backend.ends == 0);
    }
    {
        OTAUpload upload;
        TestBackend backend;
        auto data = package();
        backend.limit = data.size() + OTAUpload::flush_padding;
        valid_upload(upload, backend); // exact legacy capacity
        backend.limit--;
        assert(upload.start(backend, true));
        assert(!upload.write(backend, data.data(), data.size()));
        assert(!upload.complete(backend));
        backend.limit = OTAUpload::flush_padding;
        assert(!upload.start(backend, true));
    }
    {
        OTAUpload upload;
        TestBackend backend;
        auto raw_image = package();
        raw_image.resize(256);
        assert(upload.start(backend, false)); // existing LOCK_LEVEL=-1 development exception
        send(upload, backend, raw_image, 128);
        assert(upload.finish(backend, raw_image.size()));
        assert(upload.complete(backend));
        assert(upload.start(backend, true)); // same raw image rejected in normal mode
        send(upload, backend, raw_image, 128);
        assert(!upload.finish(backend, raw_image.size()));
        assert(!upload.complete(backend));
    }
    puts("OTA upload transaction tests passed");
}
