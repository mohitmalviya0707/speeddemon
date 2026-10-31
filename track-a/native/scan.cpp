// Track A's line scanner, in C++. The Rust driver calls scan_part once per part of the file, in parallel. Each line
// becomes one record in its hour's stream for that part: its latency, and its endpoint's slot with 0x8000 set when
// the status is 5xx.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr size_t HOURS = 72;
constexpr size_t SLOTS = 4096;
constexpr uint32_t CHUNK = 680;  // records per chunk, so a chunk fills one 4 KB page
constexpr uint64_t K = 0x9e3779b97f4a7c15ull;

uint64_t load64(const uint8_t* p) {
    uint64_t w;
    std::memcpy(&w, p, 8);  // little-endian, as on the x86 machines this builds for
    return w;
}

// How many of the 8 bytes w holds come before its first space (8 if there is none).
size_t word_len(uint64_t w) {
    uint64_t x = w ^ 0x2020202020202020ull;
    uint64_t space = (x - 0x0101010101010101ull) & ~x & 0x8080808080808080ull;
    return space == 0 ? 8 : static_cast<size_t>(__builtin_ctzll(space) / 8);
}

uint64_t num(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t k = 0; k < n; ++k) v = v * 10 + (p[k] - '0');
    return v;
}

// Days since the Unix epoch of the "YYYY-MM-DD" at t.
uint64_t day(const uint8_t* t) {
    uint64_t m = num(t + 5, 2), d = num(t + 8, 2);
    uint64_t y = num(t, 4) - (m <= 2);
    uint64_t doy = (153 * ((m + 9) % 12) + 2) / 5 + d - 1;
    return y / 400 * 146097 + y % 400 * 365 + y % 400 / 4 - y % 400 / 100 + doy - 719468;
}

// The slot of an endpoint in the shared table, interned on first sight. spans holds offset << 8 | length
// of the endpoint's first occurrence in the file.
size_t intern(std::atomic<uint64_t>* keys, std::atomic<uint64_t>* spans, size_t pos, size_t len, uint64_t hash) {
    uint64_t key = hash | 1;
    size_t s = static_cast<size_t>((hash * K) >> 52);
    for (;;) {
        uint64_t v = keys[s].load(std::memory_order_relaxed);
        if (v == key) return s;
        if (v == 0) {
            uint64_t expected = 0;
            if (keys[s].compare_exchange_strong(expected, key, std::memory_order_relaxed)) {
                spans[s].store((static_cast<uint64_t>(pos) << 8) | len, std::memory_order_relaxed);
                return s;
            }
            if (expected == key) return s;
            continue;
        }
        s = (s + 1) & (SLOTS - 1);
    }
}

// Calls f(hour, slot, latency, is_5xx) for every line in b[from..to].
template <class F>
void scan(const uint8_t* b, size_t from, size_t to, uint64_t base, std::atomic<uint64_t>* keys,
          std::atomic<uint64_t>* spans, F f) {
    size_t i = from, h = 0;
    uint64_t stamp = 0;
    while (i < to) {
        // Bytes 5..13 of a timestamp are "MM-DDTHH": one word that changes only with the hour.
        uint64_t w = load64(b + i + 5);
        if (w != stamp) {
            stamp = w;
            h = static_cast<size_t>(day(b + i) * 24 + num(b + i + 11, 2) - base);
        }
        // The method, then the endpoint, 8 bytes at a time. At least " 200 0\n" follows the endpoint.
        size_t at = i + 29 + word_len(load64(b + i + 28));
        size_t j = at;
        uint64_t hash = 0;
        for (;;) {
            uint64_t x = load64(b + j);
            size_t n = word_len(x);
            uint64_t keep = n == 8 ? ~0ull : (1ull << (8 * n)) - 1;
            hash = (hash ^ (x & keep)) * K;
            hash = (hash << 23) | (hash >> 41);
            j += n;
            if (n < 8) break;
        }
        size_t s = intern(keys, spans, at, j - at, hash);
        bool error = b[j + 1] == '5';
        j += 5;
        uint32_t lat = 0;
        while (b[j] != '\n') {
            lat = lat * 10 + static_cast<uint32_t>(b[j] - '0');
            ++j;
        }
        f(h, s, lat, error);
        i = j + 1;
    }
}

}  // namespace

// One part's records for one hour: a linked list of chunks, in arrival order.
struct Chunk {
    Chunk* next;
    uint32_t n;
    uint32_t lat[CHUNK];
    uint16_t slot[CHUNK];  // the endpoint's slot, with 0x8000 set for a 5xx
};
static_assert(sizeof(Chunk) == 4096, "a chunk is one page");

struct Stream {
    Chunk* head;
    Chunk* tail;
};

namespace {

// A new chunk is the next page of the part's arena.
void append(Stream& s, Chunk*& arena, uint32_t lat, uint16_t slot) {
    if (!s.tail || s.tail->n == CHUNK) {
        Chunk* c = arena++;
        c->next = nullptr;
        c->n = 0;
        if (s.tail) s.tail->next = c; else s.head = c;
        s.tail = c;
    }
    s.tail->lat[s.tail->n] = lat;
    s.tail->slot[s.tail->n++] = slot;
}

}  // namespace

extern "C" {

void scan_part(const uint8_t* b, size_t from, size_t to, uint64_t base, std::atomic<uint64_t>* keys,
               std::atomic<uint64_t>* spans, Stream* streams, Chunk* arena) {
    scan(b, from, to, base, keys, spans, [&](size_t h, size_t s, uint32_t lat, bool error) {
        append(streams[h], arena, lat, static_cast<uint16_t>(s | (error ? 0x8000 : 0)));
    });
}

}  // extern "C"
