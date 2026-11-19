#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "container.h"
#include "inflate.h"
#include "inflate_vectors.h"
#include "mapped_file.h"
#include "test_data.h"
#include "test_support.h"

using HNwd::detail::ErrorCode;
using HNwd::detail::InflateStream;
using HNwd::detail::adler32;
using HNwd::detail::inflate_zlib;
using testdata::Bytes;

namespace {

using Checkpoint = InflateStream::Checkpoint;
using Clock = std::chrono::steady_clock;

// Läser resten av strömmen i bitar om `chunk` byte. Ok bara om strömmen tog
// slut med got 0 och Ok. Varje bit utom den sista ska vara hel; en kort bit
// följs av slutet (annars kCount).
ErrorCode read_rest(InflateStream& z, size_t chunk, Bytes& out, std::string* detail = nullptr) {
    out.clear();
    Bytes buf(chunk);
    for (;;) {
        size_t got = 0;
        const ErrorCode e = z.read(buf.data(), chunk, got, detail);
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got));
        if (e != ErrorCode::Ok) return e;
        if (got == 0) return ErrorCode::Ok;
        if (got < chunk) {
            const ErrorCode e2 = z.read(buf.data(), chunk, got, detail);
            if (e2 != ErrorCode::Ok) return e2;
            return got == 0 ? ErrorCode::Ok : ErrorCode::kCount;
        }
    }
}

ErrorCode stream_all(const uint8_t* src, size_t size, uint64_t max, size_t chunk, Bytes& out,
                     size_t* consumed = nullptr, std::string* detail = nullptr) {
    InflateStream z;
    out.clear();
    const ErrorCode e = z.open(src, size, max);
    if (e != ErrorCode::Ok) return e;
    const ErrorCode r = read_rest(z, chunk, out, detail);
    if (r == ErrorCode::Ok && consumed) *consumed = z.consumed();
    return r;
}

double ms(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

// Läser hela strömmen med kontrollpunkter och fortsätter sedan från var och
// en (högst `max_resumes`, jämnt spridda): de följande `tail_bytes` byten, eller
// hela svansen, ska vara lika med facit. Efter resume sparas inga nya.
int check_checkpoints(const uint8_t* src, size_t size, const Bytes& expect, uint64_t every,
                      size_t chunk, size_t max_resumes, size_t tail_bytes, size_t* count) {
    int fails = 0;
    InflateStream z;
    std::vector<Checkpoint> cps;
    CHECK(z.open(src, size, expect.size()) == ErrorCode::Ok);
    z.record_checkpoints(every, &cps);
    Bytes all;
    CHECK(read_rest(z, chunk, all) == ErrorCode::Ok);
    CHECK(all == expect);
    CHECK(z.finished());
    if (count) *count = cps.size();

    uint64_t last = 0;
    for (const Checkpoint& cp : cps) {
        CHECK(cp.out_pos > last && cp.out_pos - last >= every);
        CHECK(cp.out_pos < expect.size());
        CHECK(cp.window.size() == std::min<uint64_t>(cp.out_pos, 32768));
        CHECK(std::equal(cp.window.begin(), cp.window.end(),
                         expect.begin() + static_cast<std::ptrdiff_t>(cp.out_pos - cp.window.size())));
        last = cp.out_pos;
    }

    const size_t step = cps.size() > max_resumes ? cps.size() / max_resumes : 1;
    const size_t n_before = cps.size();
    for (size_t i = 0; i < cps.size(); i += step) {
        const Checkpoint& cp = cps[i];
        CHECK(z.resume(cp) == ErrorCode::Ok);
        CHECK(z.position() == cp.out_pos);
        const size_t left = static_cast<size_t>(expect.size() - cp.out_pos);
        Bytes tail;
        if (tail_bytes >= left) {
            CHECK(read_rest(z, chunk, tail) == ErrorCode::Ok);
            CHECK(z.finished() && z.consumed() <= size);
        } else {
            tail.resize(tail_bytes);
            size_t got = 0;
            CHECK(z.read(tail.data(), tail_bytes, got) == ErrorCode::Ok && got == tail_bytes);
        }
        const size_t want = std::min(tail_bytes, left);
        CHECK(tail.size() == want &&
              std::memcmp(tail.data(), expect.data() + cp.out_pos, want) == 0);
    }
    CHECK(cps.size() == n_before);
    return fails;
}

// Varje paket av zlib-strömmar i exempelfilerna, ström för ström: InflateStream
// ger samma byte och samma längd som inflate_zlib. Sedan hastigheten för båda
// på den största strömmen, och kontrollpunkter i den.
int check_samples() {
    using namespace HNwd::detail;
    int fails = 0;
    struct Largest {
        const char* path = nullptr;
        uint64_t offset = 0;
        size_t size = 0;
        size_t out = 0;
    } largest;

    std::vector<const char*> paths(std::begin(testdata::kSampleNwc), std::end(testdata::kSampleNwc));
    paths.push_back(testdata::kSampleNwd);
    size_t files = 0, streams = 0;
    uint64_t total_out = 0;
    Bytes ref, got;
    for (const char* path : paths) {
        if (!testdata::exists(path)) {
            std::printf("  SKIP — %s saknas\n", path);
            ++g_skipped;
            continue;
        }
        MappedFile file;
        Container c;
        CHECK(file.open(path) == ErrorCode::Ok);
        if (!file.is_open()) continue;
        CHECK(read_container(file.data(), file.size(), c) == ErrorCode::Ok);
        ++files;
        for (const DbCacheEntry& e : c.entries) {
            if (e.flags != 1) continue;
            CHECK((e.a == 0 && e.b == 0) || (e.a == 4 && e.b > 0));
            uint64_t at = e.offset + (e.a == 4 ? 4 : 0);
            const uint64_t end = e.offset + e.length;
            while (at < end) {
                const uint8_t* src = file.data() + at;
                const size_t avail = static_cast<size_t>(end - at);
                size_t used_ref = 0, used = 0;
                const ErrorCode er =
                    inflate_zlib(src, avail, ref, kDefaultMaxStreamBytes, &used_ref);
                if (er != ErrorCode::Ok) {
                    std::printf("  FAIL — %s: %s, inflate_zlib at %llu\n", path,
                                std::string(e.leaf()).c_str(), static_cast<unsigned long long>(at));
                    ++fails;
                    break;
                }
                // Bitarna har olika storlek från ström till ström.
                const size_t chunk = streams % 3 == 0 ? 4093 : streams % 3 == 1 ? 65536 : 1 << 20;
                const ErrorCode es =
                    stream_all(src, avail, kDefaultMaxStreamBytes, chunk, got, &used);
                if (es != ErrorCode::Ok || got != ref || used != used_ref) {
                    std::printf("  FAIL — %s: %s, stream at %llu differs\n", path,
                                std::string(e.leaf()).c_str(), static_cast<unsigned long long>(at));
                    ++fails;
                    break;
                }
                ++streams;
                total_out += ref.size();
                if (ref.size() > largest.out) largest = {path, at, avail, ref.size()};
                at += used_ref;
            }
            CHECK(at == end);
        }
    }
    std::printf("  %zu files, %zu streams, %.1f MB inflated, InflateStream equal to inflate_zlib\n",
                files, streams, static_cast<double>(total_out) / 1e6);

    if (!largest.path) return fails;
    MappedFile file;
    CHECK(file.open(largest.path) == ErrorCode::Ok);
    if (!file.is_open()) return fails;
    const uint8_t* src = file.data() + largest.offset;
    double best_ref = 1e30, best_stream = 1e30;
    for (int round = 0; round < 3; ++round) {
        const auto t0 = Clock::now();
        CHECK(inflate_zlib(src, largest.size, ref) == ErrorCode::Ok);
        const auto t1 = Clock::now();
        CHECK(stream_all(src, largest.size, kDefaultMaxStreamBytes, 1 << 20, got) == ErrorCode::Ok);
        const auto t2 = Clock::now();
        best_ref = std::min(best_ref, ms(t0, t1));
        best_stream = std::min(best_stream, ms(t1, t2));
    }
    CHECK(got == ref);
    const double mb = static_cast<double>(ref.size()) / 1e6;
    std::printf("  largest stream: %s, %.1f MB from %.1f MB; inflate_zlib %.0f MB/s, "
                "InflateStream %.0f MB/s\n",
                largest.path, mb, static_cast<double>(largest.size) / 1e6,
                mb / (best_ref / 1000.0), mb / (best_stream / 1000.0));

    size_t count = 0;
    fails += check_checkpoints(src, largest.size, ref, 1 << 20, 65536, size_t(-1), 1 << 20, &count);
    CHECK(count > 0);
    std::printf("  checkpoints every 1 MB in it: %zu, each resumed\n", count);
    return fails;
}

} // namespace

int test_inflate() {
    int fails = 0;

    // Adler-32: känt värde, tom indata, och att summan går att fortsätta.
    const char* word = "Wikipedia";
    const uint8_t* wp = reinterpret_cast<const uint8_t*>(word);
    CHECK(adler32(wp, 9) == 0x11E60398u);
    CHECK(adler32(wp, 0) == 1u);
    CHECK(adler32(wp + 4, 5, adler32(wp, 4)) == 0x11E60398u);
    // Längre än ett reduktionsblock: summan får inte spilla över.
    {
        const Bytes big(100000, 0xFF);
        uint32_t a = 1, b = 0;
        for (size_t i = 0; i < big.size(); ++i) {
            a = (a + 0xFF) % 65521;
            b = (b + a) % 65521;
        }
        CHECK(adler32(big.data(), big.size()) == ((b << 16) | a));
    }

    Bytes out;
    size_t consumed = 0;

    // Fasta Huffman-koder.
    CHECK(inflate_zlib(kFixedZlib, sizeof kFixedZlib, out, 1 << 20, &consumed) ==
          ErrorCode::Ok);
    CHECK(out == testdata::pattern(600));
    CHECK(consumed == sizeof kFixedZlib);

    // Dynamiska Huffman-koder.
    CHECK(inflate_zlib(kDynamicZlib, sizeof kDynamicZlib, out, 1 << 20, &consumed) ==
          ErrorCode::Ok);
    CHECK(out == testdata::pattern(20000));
    CHECK(consumed == sizeof kDynamicZlib);

    // Lagrade block: ett, flera, och ett tomt.
    for (const size_t block : {size_t(65535), size_t(1000), size_t(1)}) {
        const Bytes src = testdata::pattern(5000);
        const Bytes z = testdata::zlib_store(src, block);
        CHECK(inflate_zlib(z.data(), z.size(), out, 1 << 20, &consumed) == ErrorCode::Ok);
        CHECK(out == src);
        CHECK(consumed == z.size());
    }
    {
        const Bytes z = testdata::zlib_store(Bytes());
        CHECK(inflate_zlib(z.data(), z.size(), out, 1 << 20, &consumed) == ErrorCode::Ok);
        CHECK(out.empty());
        CHECK(consumed == z.size());
    }

    // Strömmar ligger tätt efter varandra: det som följer efter kontrollsumman
    // hör inte till strömmen och får inte räknas in i dess längd.
    for (const size_t extra : {size_t(1), size_t(3), size_t(8), size_t(100)}) {
        Bytes z(kDynamicZlib, kDynamicZlib + sizeof kDynamicZlib);
        z.insert(z.end(), extra, 0x78);
        CHECK(inflate_zlib(z.data(), z.size(), out, 1 << 20, &consumed) == ErrorCode::Ok);
        CHECK(consumed == sizeof kDynamicZlib);
        CHECK(out.size() == 20000);
    }

    // Taket: precis lagom går, en byte för lite ger StreamTooLarge.
    CHECK(inflate_zlib(kDynamicZlib, sizeof kDynamicZlib, out, 20000) == ErrorCode::Ok);
    CHECK(inflate_zlib(kDynamicZlib, sizeof kDynamicZlib, out, 19999) ==
          ErrorCode::StreamTooLarge);
    CHECK(inflate_zlib(kFixedZlib, sizeof kFixedZlib, out, 599) ==
          ErrorCode::StreamTooLarge);
    {
        const Bytes z = testdata::zlib_store(testdata::pattern(5000), 1000);
        CHECK(inflate_zlib(z.data(), z.size(), out, 4999) == ErrorCode::StreamTooLarge);
    }

    // Avhuggen ström: varje kortare längd ska ge fel, aldrig Ok.
    for (size_t n = 0; n < sizeof kDynamicZlib; n += 7)
        CHECK(inflate_zlib(kDynamicZlib, n, out, 1 << 20) == ErrorCode::CorruptStream);
    CHECK(inflate_zlib(kDynamicZlib, sizeof kDynamicZlib - 1, out, 1 << 20) ==
          ErrorCode::CorruptStream);

    // Fel kontrollsumma.
    {
        Bytes z(kFixedZlib, kFixedZlib + sizeof kFixedZlib);
        z.back() ^= 1;
        CHECK(inflate_zlib(z.data(), z.size(), out, 1 << 20) == ErrorCode::CorruptStream);
    }

    // Fel i huvudet: annan metod, fel kontrollbitar, förinställd ordlista.
    {
        Bytes z(kFixedZlib, kFixedZlib + sizeof kFixedZlib);
        Bytes bad = z;
        bad[0] = 0x79;
        CHECK(inflate_zlib(bad.data(), bad.size(), out, 1 << 20) ==
              ErrorCode::CorruptStream);
        bad = z;
        bad[1] ^= 1;
        CHECK(inflate_zlib(bad.data(), bad.size(), out, 1 << 20) ==
              ErrorCode::CorruptStream);
        bad = z;
        bad[0] = 0x78;
        bad[1] = 0xBB;   // giltiga kontrollbitar, men FDICT satt
        CHECK(inflate_zlib(bad.data(), bad.size(), out, 1 << 20) ==
              ErrorCode::CorruptStream);
    }

    // Trasiga block.
    {
        // Blocktyp 3 finns inte.
        const Bytes z = {0x78, 0x01, 0x07, 0, 0, 0, 0, 0};
        CHECK(inflate_zlib(z.data(), z.size(), out, 1 << 20) == ErrorCode::CorruptStream);
    }
    {
        // Lagrat block där längden och dess komplement inte stämmer.
        Bytes z = testdata::zlib_store(testdata::pattern(10));
        z[5] ^= 0x10;
        CHECK(inflate_zlib(z.data(), z.size(), out, 1 << 20) == ErrorCode::CorruptStream);
    }
    {
        // Fast block som börjar med en referens bakåt, före någon data:
        // BFINAL=1, BTYPE=01, längdkod 257 (0000001), avståndskod 0 (00000).
        const Bytes z = {0x78, 0x01, 0x03, 0x02, 0x00, 0, 0, 0, 0, 0};
        CHECK(inflate_zlib(z.data(), z.size(), out, 1 << 20) == ErrorCode::CorruptStream);
    }

    // Varje enskild bitflip i en giltig ström ska ge ett fel, eller Ok med
    // exakt samma data — aldrig en krasch, och aldrig Ok med annan data,
    // eftersom kontrollsumman då skulle ha släppt igenom en ändrad ström.
    {
        const Bytes good = testdata::pattern(20000);
        Bytes z(kDynamicZlib, kDynamicZlib + sizeof kDynamicZlib);
        for (size_t i = 2; i < z.size(); ++i) {
            z[i] ^= 0x04;
            const ErrorCode e = inflate_zlib(z.data(), z.size(), out, 1 << 20);
            if (e == ErrorCode::Ok && out != good) {
                std::printf("  FAIL — bitflip i byte %zu gav Ok med fel data\n", i);
                ++fails;
            }
            z[i] ^= 0x04;
        }
    }

    // ---------------------------------------------------------------------
    // InflateStream.

    // Facit i bitar om 1, 7 och 4096 byte, med samma längd som inflate_zlib.
    {
        struct Vec {
            Bytes z;
            Bytes expect;
        };
        const std::vector<Vec> vecs = {
            {Bytes(kFixedZlib, kFixedZlib + sizeof kFixedZlib), testdata::pattern(600)},
            {Bytes(kDynamicZlib, kDynamicZlib + sizeof kDynamicZlib), testdata::pattern(20000)},
            {testdata::zlib_store(testdata::pattern(5000), 65535), testdata::pattern(5000)},
            {testdata::zlib_store(testdata::pattern(5000), 1000), testdata::pattern(5000)},
            {testdata::zlib_store(testdata::pattern(5000), 1), testdata::pattern(5000)},
            {testdata::zlib_store(Bytes()), Bytes()},
            {testdata::zlib_store(testdata::pattern(300000), 7000), testdata::pattern(300000)},
        };
        for (const Vec& v : vecs) {
            for (const size_t chunk : {size_t(1), size_t(7), size_t(4096)}) {
                size_t used = 0;
                CHECK(stream_all(v.z.data(), v.z.size(), 1 << 20, chunk, out, &used) ==
                      ErrorCode::Ok);
                CHECK(out == v.expect);
                CHECK(used == v.z.size());
            }
            // Det som följer efter strömmen hör inte till den.
            Bytes extra = v.z;
            extra.insert(extra.end(), 9, 0x78);
            size_t used = 0;
            CHECK(stream_all(extra.data(), extra.size(), 1 << 20, 4096, out, &used) ==
                  ErrorCode::Ok);
            CHECK(used == v.z.size() && out == v.expect);
        }
    }

    // Före open, vid slutet, och open som börjar om.
    {
        InflateStream z;
        uint8_t b[4];
        size_t got = 7;
        std::string detail;
        CHECK(z.read(b, 4, got, &detail) == ErrorCode::CorruptStream && got == 0);
        CHECK(!detail.empty());
        CHECK(z.resume(Checkpoint{}) == ErrorCode::CorruptStream);
        CHECK(!z.finished());

        CHECK(z.open(kFixedZlib, sizeof kFixedZlib, 1 << 20) == ErrorCode::Ok);
        CHECK(z.position() == 0 && z.max_output() == (1u << 20));
        Bytes all;
        CHECK(read_rest(z, 100, all) == ErrorCode::Ok && all == testdata::pattern(600));
        CHECK(z.finished() && z.position() == 600 && z.consumed() == sizeof kFixedZlib);
        CHECK(z.read(b, 4, got) == ErrorCode::Ok && got == 0);   // slutet består

        CHECK(z.open(kDynamicZlib, sizeof kDynamicZlib, 1 << 20) == ErrorCode::Ok);
        CHECK(!z.finished() && z.position() == 0);
        CHECK(read_rest(z, 999, all) == ErrorCode::Ok && all == testdata::pattern(20000));
    }

    // Kontrollpunkter: varje kontrollpunkt ger svansen. Lagrade block om 1004
    // byte, så att gränserna inte ligger på jämna tal.
    {
        const Bytes src = testdata::pattern(100000);
        const Bytes z = testdata::zlib_store(src, 1004);
        const size_t blocks = (src.size() + 1003) / 1004;
        for (const uint64_t every : {uint64_t(0), uint64_t(1), uint64_t(5000), uint64_t(40000)}) {
            size_t count = 0;
            fails += check_checkpoints(z.data(), z.size(), src, every, 333, size_t(-1),
                                       size_t(-1), &count);
            if (every <= 1004) {
                CHECK(count == blocks - 1);
            } else {
                CHECK(count == (src.size() - 1) / (((every + 1003) / 1004) * 1004));
            }
        }
        // Ett enda block har ingen gräns mitt i strömmen.
        size_t count = 7;
        fails += check_checkpoints(kDynamicZlib, sizeof kDynamicZlib, testdata::pattern(20000), 0,
                                   64, size_t(-1), size_t(-1), &count);
        CHECK(count == 0);

        // Kontrollpunkter som inte hör till strömmen.
        InflateStream s;
        std::vector<Checkpoint> cps;
        CHECK(s.open(z.data(), z.size(), src.size()) == ErrorCode::Ok);
        s.record_checkpoints(0, &cps);
        CHECK(s.skip(src.size()) == ErrorCode::Ok && s.finished());
        CHECK(cps.size() == blocks - 1);
        Checkpoint bad = cps[40];
        bad.window.pop_back();
        CHECK(s.resume(bad) == ErrorCode::OutOfRange);
        bad = cps[40];
        bad.in_bit = uint64_t(z.size()) * 8 + 8;
        CHECK(s.resume(bad) == ErrorCode::OutOfRange);
        bad = cps[40];
        bad.in_bit = 8;
        CHECK(s.resume(bad) == ErrorCode::OutOfRange);
        bad = cps[40];
        bad.out_pos = src.size() + 1;
        CHECK(s.resume(bad) == ErrorCode::OutOfRange);
        // Ett fel bitläge ger ett fel vid läsningen, inte fel data.
        bad = cps[40];
        bad.in_bit += 8;
        CHECK(s.resume(bad) == ErrorCode::Ok);
        Bytes rest;
        CHECK(read_rest(s, 4096, rest) == ErrorCode::CorruptStream);

        // Inga kontrollpunkter efter resume, inte heller med en ny sink.
        std::vector<Checkpoint> after;
        CHECK(s.resume(cps[10]) == ErrorCode::Ok);
        s.record_checkpoints(0, &after);
        CHECK(read_rest(s, 4096, rest) == ErrorCode::Ok);
        CHECK(after.empty());
        CHECK(rest.size() == src.size() - cps[10].out_pos &&
              std::memcmp(rest.data(), src.data() + cps[10].out_pos, rest.size()) == 0);

        // Efter resume prövas inte Adler-32, men dess fyra byte ska finnas.
        Bytes wrong = z;
        if (!wrong.empty()) wrong.back() ^= 1;
        InflateStream w;
        CHECK(w.open(wrong.data(), wrong.size(), src.size()) == ErrorCode::Ok);
        CHECK(w.resume(cps[90]) == ErrorCode::Ok);
        CHECK(read_rest(w, 4096, rest) == ErrorCode::Ok);
        CHECK(w.open(wrong.data(), wrong.size() - 1, src.size()) == ErrorCode::Ok);
        CHECK(w.resume(cps[90]) == ErrorCode::Ok);
        CHECK(read_rest(w, 4096, rest) == ErrorCode::CorruptStream);
    }

    // Taket: precis lagom går, en byte för lite ger StreamTooLarge med detail.
    for (const size_t chunk : {size_t(1), size_t(7), size_t(4096)}) {
        std::string detail;
        CHECK(stream_all(kDynamicZlib, sizeof kDynamicZlib, 20000, chunk, out) == ErrorCode::Ok);
        CHECK(stream_all(kDynamicZlib, sizeof kDynamicZlib, 19999, chunk, out, nullptr,
                         &detail) == ErrorCode::StreamTooLarge);
        CHECK(detail.find("limit") != std::string::npos);
        CHECK(out.size() <= 19999);
        CHECK(stream_all(kFixedZlib, sizeof kFixedZlib, 599, chunk, out) ==
              ErrorCode::StreamTooLarge);
        const Bytes z = testdata::zlib_store(testdata::pattern(5000), 1000);
        CHECK(stream_all(z.data(), z.size(), 4999, chunk, out) == ErrorCode::StreamTooLarge);
        CHECK(stream_all(z.data(), z.size(), 5000, chunk, out) == ErrorCode::Ok);
        CHECK(stream_all(z.data(), z.size(), 0, chunk, out) == ErrorCode::StreamTooLarge);
    }

    // Huvudet och för kort ström: felet kommer från open och fastnar.
    {
        InflateStream z;
        Bytes bad(kFixedZlib, kFixedZlib + sizeof kFixedZlib);
        bad[0] = 0x79;
        CHECK(z.open(bad.data(), bad.size(), 1 << 20) == ErrorCode::CorruptStream);
        uint8_t b[4];
        size_t got;
        CHECK(z.read(b, 4, got) == ErrorCode::CorruptStream);
        CHECK(z.open(kFixedZlib, 5, 1 << 20) == ErrorCode::CorruptStream);
        CHECK(z.open(nullptr, 100, 1 << 20) == ErrorCode::CorruptStream);
    }

    // Avhuggen ström: varje kortare längd ger fel, med små och stora bitar.
    for (size_t n = 0; n < sizeof kDynamicZlib; n += 5)
        for (const size_t chunk : {size_t(1), size_t(4096)})
            CHECK(stream_all(kDynamicZlib, n, 1 << 20, chunk, out) == ErrorCode::CorruptStream);

    // Fel mitt i strömmen: det tredje lagrade blocket är trasigt. Byten före
    // det lämnas, sedan kommer felet, och det fastnar.
    {
        const Bytes src = testdata::pattern(5000);
        Bytes z = testdata::zlib_store(src, 1000);
        // Block i börjar på 2 + i·(5 + 1000); NLEN ligger 3 byte in.
        z[2 + 2 * 1005 + 3] ^= 0x01;
        InflateStream s;
        CHECK(s.open(z.data(), z.size(), 1 << 20) == ErrorCode::Ok);
        Bytes got_all;
        std::string detail;
        CHECK(read_rest(s, 7, got_all, &detail) == ErrorCode::CorruptStream);
        CHECK(got_all.size() == 2000 && std::memcmp(got_all.data(), src.data(), 2000) == 0);
        CHECK(s.position() == 2000);
        CHECK(detail.rfind("zlib stream at input byte ", 0) == 0);
        CHECK(detail.find("stored block") != std::string::npos);
        uint8_t b[4];
        size_t got = 9;
        std::string again;
        CHECK(s.read(b, 4, got, &again) == ErrorCode::CorruptStream && got == 0 &&
              again == detail);
        CHECK(s.skip(1) == ErrorCode::CorruptStream);
        CHECK(!s.finished());
    }

    // Fel kontrollsumma: all data kommer, och slutet ger felet.
    {
        Bytes z(kDynamicZlib, kDynamicZlib + sizeof kDynamicZlib);
        z.back() ^= 1;
        InflateStream s;
        CHECK(s.open(z.data(), z.size(), 1 << 20) == ErrorCode::Ok);
        Bytes all;
        std::string detail;
        CHECK(read_rest(s, 512, all, &detail) == ErrorCode::CorruptStream);
        CHECK(all == testdata::pattern(20000));
        CHECK(detail.find("Adler-32") != std::string::npos);
        CHECK(!s.finished());
    }

    // Varje bitflip: samma utfall som inflate_zlib, och vid Ok samma data.
    {
        Bytes z(kDynamicZlib, kDynamicZlib + sizeof kDynamicZlib);
        Bytes ref;
        for (size_t i = 0; i < z.size(); ++i) {
            z[i] ^= 0x10;
            const ErrorCode er = inflate_zlib(z.data(), z.size(), ref, 1 << 20);
            const ErrorCode es = stream_all(z.data(), z.size(), 1 << 20, 4096, out);
            if (er != es || (er == ErrorCode::Ok && out != ref)) {
                std::printf("  FAIL — bitflip i byte %zu: inflate_zlib %d, InflateStream %d\n", i,
                            static_cast<int>(er), static_cast<int>(es));
                ++fails;
            }
            z[i] ^= 0x10;
        }
    }

    // skip: över delar, över allt, och förbi slutet.
    {
        const Bytes src = testdata::pattern(300000);
        const Bytes z = testdata::zlib_store(src, 7000);
        for (const uint64_t n : {uint64_t(0), uint64_t(1), uint64_t(6996), uint64_t(6997),
                                 uint64_t(123457), uint64_t(299996), uint64_t(299997)}) {
            InflateStream s;
            CHECK(s.open(z.data(), z.size(), 1 << 20) == ErrorCode::Ok);
            uint8_t b[3];
            size_t got = 0;
            CHECK(s.read(b, 3, got) == ErrorCode::Ok && got == 3);
            CHECK(s.skip(n) == ErrorCode::Ok);
            CHECK(s.position() == 3 + n);
            Bytes rest;
            CHECK(read_rest(s, 4096, rest) == ErrorCode::Ok);
            CHECK(rest.size() == src.size() - 3 - n &&
                  std::memcmp(rest.data(), src.data() + 3 + n, rest.size()) == 0);
            CHECK(s.finished());
        }
        InflateStream s;
        CHECK(s.open(z.data(), z.size(), 1 << 20) == ErrorCode::Ok);
        std::string detail;
        CHECK(s.skip(300001, &detail) == ErrorCode::OutOfRange);
        CHECK(s.position() == 300000 && s.finished());
        CHECK(detail.find("skip") != std::string::npos);
        // Adler-32 prövas också när allt hoppas över.
        Bytes wrong = z;
        if (!wrong.empty()) wrong.back() ^= 1;
        CHECK(s.open(wrong.data(), wrong.size(), 1 << 20) == ErrorCode::Ok);
        CHECK(s.skip(300000) == ErrorCode::Ok);
        uint8_t b;
        size_t got;
        CHECK(s.read(&b, 1, got) == ErrorCode::CorruptStream);
    }

    // Exempelfilerna.
    fails += check_samples();

    std::printf("test_inflate: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
