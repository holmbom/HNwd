#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "container.h"
#include "geometry_segment.h"
#include "mapped_file.h"
#include "test_data.h"
#include "test_support.h"

using namespace HNwd::detail;

namespace {

using Clock = std::chrono::steady_clock;

double millis(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

bool same_data(const RecordData& a, const RecordData& b) {
    return a.indices == b.indices && a.positions == b.positions && a.primitives == b.primitives;
}

const testdata::SampleFacts* facts_for(const char* path) {
    for (const testdata::SampleFacts& f : testdata::kFacts)
        if (std::strcmp(f.path, path) == 0) return &f;
    return nullptr;
}

// Geometrisegmentet i en exempelfil: antalen stämmer, chunkarna mätta genom
// uppackning följer regeln i Fixed (P byte utom den sista, som har storleken
// i kFacts), varje post tolkas i Fixed, och två cursorer över samma layout
// som läser omväxlande ger samma byte som en ensam.
int check_file(const testdata::SampleCounts& s) {
    int fails = 0;
    const bool nwd = std::strcmp(s.path, testdata::kSampleNwd) == 0;
    const testdata::SampleFacts* facts = facts_for(s.path);
    CHECK(facts != nullptr);
    if (!facts) return fails;

    MappedFile file;
    CHECK(file.open(s.path) == ErrorCode::Ok);
    if (!file.is_open()) return fails;
    Container c;
    std::string detail;
    if (read_container(file.data(), file.size(), c, &detail) != ErrorCode::Ok) {
        std::printf("  FAIL — %s: %s\n", s.path, detail.c_str());
        return fails + 1;
    }
    const DbCacheEntry* entry = find_entry(c.entries, "LcOpNwdGeometry");
    CHECK(entry != nullptr);
    if (!entry) return fails;

    // Scan mäter varje chunk genom att packa upp den.
    SegmentLimits scan_limits;
    scan_limits.layout = ChunkLayout::Scan;
    GeometryLayout scanned;
    const auto t0 = Clock::now();
    detail.clear();
    if (scanned.open(file.data(), file.size(), *entry, scan_limits, &detail) != ErrorCode::Ok) {
        std::printf("  FAIL — %s: open with Scan: %s\n", s.path, detail.c_str());
        return fails + 1;
    }
    const auto t1 = Clock::now();
    CHECK(scanned.chunk_count() == s.chunks);
    uint32_t off_rule = 0;
    for (uint32_t i = 0; i + 1 < scanned.chunk_count(); ++i)
        if (scanned.chunk_size(i) != 65536 || scanned.chunk_start(i) != uint64_t(i) * 65536)
            ++off_rule;
    CHECK(off_rule == 0);
    const uint64_t last_chunk = scanned.chunk_size(scanned.chunk_count() - 1);
    CHECK(last_chunk == facts->last_chunk_bytes);

    // Fixed: förvalet. Inget packas upp vid open.
    GeometrySegment seg;
    detail.clear();
    if (seg.open(file.data(), file.size(), *entry, SegmentLimits{}, &detail) != ErrorCode::Ok) {
        std::printf("  FAIL — %s: open: %s\n", s.path, detail.c_str());
        return fails + 1;
    }
    CHECK(seg.layout() && seg.layout()->layout() == ChunkLayout::Fixed);
    CHECK(seg.record_count() == s.records);
    CHECK(seg.chunk_count() == s.chunks);
    CHECK(seg.inflated_total() == 0);
    uint32_t differ_layout = 0;
    for (uint32_t i = 0; i < seg.chunk_count(); ++i)
        if (seg.chunk_size(i) != scanned.chunk_size(i) ||
            seg.chunk_start(i) != scanned.chunk_start(i))
            ++differ_layout;
    CHECK(differ_layout == 0);

    // Varje chunk packas upp en gång och prövas mot Fixed.
    const auto t2 = Clock::now();
    detail.clear();
    const ErrorCode sized = seg.size_all(&detail);
    const auto t3 = Clock::now();
    if (sized != ErrorCode::Ok) {
        std::printf("  FAIL — %s: size_all: %s\n", s.path, detail.c_str());
        return fails + 1;
    }

    // Poster som går över en chunkgräns.
    const GeometryLayout& layout = *seg.layout();
    uint32_t crossing = 0;
    for (uint32_t k = 0; k < seg.record_count(); ++k) {
        const uint64_t rs = layout.record_start(k), re = layout.record_start(k + 1);
        if (re > rs && layout.chunk_of(rs) != layout.chunk_of(re - 1)) ++crossing;
    }
    CHECK(layout.record_start(seg.record_count()) == seg.total_bytes());

    // Alla poster tolkas med den avgjorda bredden.
    uint64_t triangles = 0, segments = 0;
    uint32_t failed = 0;
    const auto t4 = Clock::now();
    RecordData rd;
    for (uint32_t k = 0; k < seg.record_count(); ++k) {
        detail.clear();
        const ErrorCode e = seg.parse(k, rd, &detail);
        if (e != ErrorCode::Ok) {
            if (failed++ < 3)
                std::printf("  FAIL — %s: record %u: %s (%s)\n", s.path, k, error_message(e),
                            detail.c_str());
            continue;
        }
        (rd.lines ? segments : triangles) += rd.primitives;
    }
    const auto t5 = Clock::now();
    CHECK(failed == 0);
    fails += failed ? 1 : 0;
    // I ordning packas varje chunk upp en gång.
    CHECK(seg.inflated_total() == seg.total_bytes());

    // 16 bitar för .nwc och 8 för .nwd.
    CHECK(seg.normal_bits() == (nwd ? 8 : 16));

    std::printf("  %s\n    records %u, chunks %u, last chunk %llu B, crossing %u\n"
                "    open Scan %.1f ms, size_all %.1f ms, parse all %.1f ms, triangles %llu, "
                "segments %llu\n    normal bits %d, decided by record %lld\n",
                s.path, seg.record_count(), seg.chunk_count(),
                static_cast<unsigned long long>(last_chunk), crossing, millis(t0, t1),
                millis(t2, t3), millis(t4, t5), static_cast<unsigned long long>(triangles),
                static_cast<unsigned long long>(segments), seg.normal_bits(),
                static_cast<long long>(seg.decided_by()));

    // Två cursorer med en cache på 1 MB var, omväxlande, mot en ensam: varje
    // post ger samma byte.
    const size_t small = size_t(1) << 20;
    GeometryCursor one(layout, file.data(), file.size(), small);
    GeometryCursor a(layout, file.data(), file.size(), small);
    GeometryCursor b(layout, file.data(), file.size(), small);
    uint32_t differ = 0;
    for (uint32_t k = 0; k < seg.record_count(); ++k) {
        const uint8_t* want = nullptr;
        const uint8_t* got = nullptr;
        size_t nw = 0, ng = 0;
        GeometryCursor& other = (k % 2 == 0) ? a : b;
        if (one.record_bytes(k, want, nw) != ErrorCode::Ok ||
            other.record_bytes(k, got, ng) != ErrorCode::Ok || nw != ng ||
            (nw != 0 && std::memcmp(want, got, nw) != 0))
            ++differ;
    }
    // Och baklänges var 50:e post, tolkad, så att cacharna kastar och tar om.
    RecordData want, got;
    uint32_t last = (seg.record_count() - 1) - (seg.record_count() - 1) % 50;
    for (bool use_a = true;; last -= 50, use_a = !use_a) {
        if (one.parse(last, want, &detail) != ErrorCode::Ok ||
            (use_a ? a : b).parse(last, got, &detail) != ErrorCode::Ok || !same_data(want, got))
            ++differ;
        if (last == 0) break;
    }
    CHECK(differ == 0);
    const uint64_t largest = 65536;
    CHECK(a.cached_bytes() <= small + largest * 2 && b.cached_bytes() <= small + largest * 2);
    std::printf("    two cursors: inflated %.1f + %.1f MB, one cursor %.1f MB\n",
                static_cast<double>(a.inflated_total()) / (1024.0 * 1024.0),
                static_cast<double>(b.inflated_total()) / (1024.0 * 1024.0),
                static_cast<double>(one.inflated_total()) / (1024.0 * 1024.0));
    return fails;
}

} // namespace

int test_samples_geometry() {
    SKIP_IF_MISSING(testdata::kSampleNwd);
    int fails = 0;
    for (const testdata::SampleCounts& s : testdata::kSamples) {
        if (!testdata::exists(s.path)) {
            std::printf("  SKIP — %s saknas\n", s.path);
            ++g_skipped;
            continue;
        }
        fails += check_file(s);
    }
    std::printf("test_samples_geometry: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
