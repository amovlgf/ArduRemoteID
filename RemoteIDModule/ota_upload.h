#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Keep the upload transaction independent of HTTP and the ESP32 Update library
// so failed/interrupted uploads can be exercised without flashing hardware.
class OTAUpload {
public:
    static constexpr size_t descriptor_size = 80;
    static constexpr size_t flush_padding = 4097;
    static constexpr size_t lead_size = 16;

    class Backend {
    public:
        virtual bool begin(size_t &capacity) = 0;
        virtual size_t write(const uint8_t *data, size_t size) = 0;
        virtual bool verify(const uint8_t *lead, size_t size) = 0;
        virtual bool end() = 0;
        virtual void abort() = 0;
        virtual ~Backend() = default;
    };

    bool start(Backend &backend, bool require_descriptor)
    {
        backend.abort();
        state = State::FAILED;
        received = 0;
        capacity = 0;
        lead_length = 0;
        tail_length = 0;
        signed_format = require_descriptor;
        if (!backend.begin(capacity) || capacity <= flush_padding) {
            backend.abort();
            return false;
        }
        state = State::RECEIVING;
        return true;
    }

    bool write(Backend &backend, const uint8_t *data, size_t size)
    {
        if (state != State::RECEIVING) {
            return false;
        }
        if (size > capacity - flush_padding - received) {
            return fail(backend);
        }
        const size_t head_count = minimum(size, lead_size - lead_length);
        memcpy(lead + lead_length, data, head_count);
        lead_length += head_count;
        if (size >= descriptor_size) {
            memcpy(tail, data + size - descriptor_size, descriptor_size);
            tail_length = descriptor_size;
        } else if (size > 0) {
            const size_t keep = minimum(tail_length, descriptor_size - size);
            memmove(tail, tail + tail_length - keep, keep);
            memcpy(tail + keep, data, size);
            tail_length = keep + size;
        }
        if (backend.write(data, size) != size) {
            return fail(backend);
        }
        received += size;
        return true;
    }

    bool finish(Backend &backend, size_t reported_size)
    {
        if (state != State::RECEIVING) {
            return false;
        }
        if (reported_size != received || lead_length != lead_size || lead[0] != 0xe9) {
            return fail(backend);
        }
        if (signed_format && !complete_descriptor()) {
            return fail(backend);
        }
        // Arduino Update withholds the image header and buffers the last flash
        // sector. Preserve the old uploader's padding before checking flash.
        const uint8_t ff = 0xff;
        for (size_t i = 0; i < flush_padding; i++) {
            if (backend.write(&ff, 1) != 1) {
                return fail(backend);
            }
        }
        if (!backend.verify(lead, lead_length)) {
            return fail(backend);
        }
        state = State::VERIFIED;
        return true;
    }

    // Only the final HTTP completion callback may activate the new image.
    // Consuming the result also prevents an empty later POST reusing success.
    bool complete(Backend &backend)
    {
        if (state != State::VERIFIED || !backend.end()) {
            return fail(backend);
        }
        state = State::IDLE;
        return true;
    }

    bool fail(Backend &backend)
    {
        backend.abort();
        state = State::FAILED;
        return false;
    }

private:
    enum class State { IDLE, RECEIVING, VERIFIED, FAILED };
    State state = State::IDLE;
    bool signed_format = true;
    size_t capacity = 0;
    size_t received = 0;
    size_t lead_length = 0;
    size_t tail_length = 0;
    uint8_t lead[lead_size] {};
    uint8_t tail[descriptor_size] {};

    static size_t minimum(size_t a, size_t b) { return a < b ? a : b; }

    bool complete_descriptor() const
    {
        // The legacy verifier finds the FIRST descriptor magic in the image.
        // Never embed the forward magic in firmware code or read-only data.
        // Volatile prevents optimization from folding this into that sequence.
        static volatile const uint8_t magic_rev[] = { 0x19, 0x75, 0xe2, 0x46, 0x37, 0xf1, 0x2a, 0x43 };
        if (received < lead_size + descriptor_size || tail_length != descriptor_size) {
            return false;
        }
        for (size_t i = 0; i < sizeof(magic_rev); i++) {
            if (tail[i] != magic_rev[sizeof(magic_rev) - 1 - i]) return false;
        }
        const uint32_t image_size = uint32_t(tail[12]) | (uint32_t(tail[13]) << 8) |
            (uint32_t(tail[14]) << 16) | (uint32_t(tail[15]) << 24);
        return image_size == received - descriptor_size;
    }
};
