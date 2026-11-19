#include "geometry_segment.h"

#include <algorithm>
#include <cstring>

namespace HNwd::detail {

namespace {

constexpr int32_t kExpectedPrefix = 65536;
constexpr const char* kPackage = "LcOpNwdGeometry";
constexpr uint32_t kProgressStep = 1024;   // poster mellan två anrop till förloppet

// Sätter `detail` om den finns, och ger felkoden vidare.
ErrorCode fail(std::string* detail, ErrorCode code, const std::string& text) {
    if (detail) *detail = text;
    return code;
}

// "LcOpNwdGeometry at byte 1234: <regel>".
std::string at_byte(uint64_t offset, const std::string& rule) {
    return std::string(kPackage) + " at byte " + std::to_string(offset) + ": " + rule;
}

// "LcOpNwdGeometry chunk 3 at byte 1234: <regel>", med chunkens början i filen.
std::string chunk_at(uint32_t i, uint64_t offset, const std::string& rule) {
    return std::string(kPackage) + " chunk " + std::to_string(i) + " at byte " +
           std::to_string(offset) + ": " + rule;
}

// "LcOpNwdGeometry record 3: <regel>".
std::string record_at(uint64_t k, const std::string& rule) {
    return std::string(kPackage) + " record " + std::to_string(k) + ": " + rule;
}

// Ett fel från en annan tolk, med paketets namn framför.
std::string wrap(const std::string& inner) { return std::string(kPackage) + ": " + inner; }

} // namespace

// ---------------------------------------------------------------------------
// GeometryLayout

ErrorCode GeometryLayout::open(const uint8_t* file, size_t size, const DbCacheEntry& entry,
                               const SegmentLimits& limits, std::string* detail) {
    *this = GeometryLayout{};
    const auto bail = [&](ErrorCode code, const std::string& text) {
        *this = GeometryLayout{};
        return fail(detail, code, text);
    };

    if (limits.normal_bits != 0 && limits.normal_bits != 8 && limits.normal_bits != 16)
        return bail(ErrorCode::OutOfRange,
                    std::string(kPackage) + ": limits.normal_bits is not 0, 8 or 16");

    // Paketet ska vara av formen (4, b) och ligga helt i filen.
    StreamSpan toc_span;
    if (!file || entry.a != 4 || last_stream(entry, size, toc_span) != ErrorCode::Ok)
        return bail(ErrorCode::CorruptPackage,
                    at_byte(entry.offset, "package is not of the form (4, b) inside the file"));

    int32_t prefix = 0;
    if (!read_prefix(file, size, entry, prefix))
        return bail(ErrorCode::CorruptPackage,
                    at_byte(entry.offset, "package prefix is outside the file"));
    if (prefix != kExpectedPrefix)
        return bail(ErrorCode::CorruptPackage,
                    at_byte(entry.offset, "prefix is " + std::to_string(prefix) + ", not 65536"));

    std::string inner;
    std::vector<uint8_t> toc_bytes;
    ErrorCode ec = inflate_span(file, size, toc_span, limits.max_stream, toc_bytes, &inner);
    if (ec != ErrorCode::Ok) return bail(ec, wrap(inner));

    GeometryToc toc;
    ec = parse_geometry_toc(toc_bytes.data(), toc_bytes.size(), toc, &inner);
    if (ec != ErrorCode::Ok) return bail(ec, wrap(inner));
    std::vector<uint8_t>().swap(toc_bytes);

    // Chunkarnas komprimerade längder ska fylla paketet mellan prefixet och
    // innehållsförteckningen.
    ec = split_prefixed(entry, size, toc.chunk_sizes, spans_, &inner);
    if (ec != ErrorCode::Ok) return bail(ec, inner);

    // Prefixsummorna av postlängderna, i 64 bitar: ett värde är högst 2^31 - 1
    // och antalet ryms i minnet, så summan kan inte slå runt.
    record_start_.assign(toc.record_lengths.size() + 1, 0);
    for (size_t k = 0; k < toc.record_lengths.size(); ++k)
        record_start_[k + 1] = record_start_[k] + toc.record_lengths[k];

    file_ = file;
    file_size_ = size;
    offset_ = entry.offset;
    limits_ = limits;
    chunk_bytes_ = static_cast<uint64_t>(prefix);

    if (limits.layout == ChunkLayout::Fixed) {
        // Alla chunkar utom den sista är P byte och den sista 1..P, så
        // (ch − 1)·P < total ≤ ch·P. Utan chunkar ska total vara 0.
        const uint64_t ch = spans_.size();
        const uint64_t total = total_bytes();
        const bool fits = ch == 0 ? total == 0
                                  : (ch - 1) * chunk_bytes_ < total && total <= ch * chunk_bytes_;
        if (!fits)
            return bail(ErrorCode::CorruptPackage,
                        at_byte(entry.offset, std::to_string(ch) + " chunks of " +
                                                  std::to_string(chunk_bytes_) +
                                                  " bytes do not match " + std::to_string(total) +
                                                  " record bytes; open with ChunkLayout::Scan"));
        return ErrorCode::Ok;
    }

    ec = measure_chunks(&inner);
    if (ec != ErrorCode::Ok) return bail(ec, inner);
    return ErrorCode::Ok;
}

// Packar upp chunkarna i ordning och mäter dem. Varje chunk får högst det som
// återstår av posterna; den som ger mer har mer data än posterna har plats för.
ErrorCode GeometryLayout::measure_chunks(std::string* detail) {
    const uint64_t total = total_bytes();
    chunk_start_.assign(spans_.size() + 1, 0);
    std::vector<uint8_t> buffer;
    uint64_t known = 0;
    for (uint32_t i = 0; i < spans_.size(); ++i) {
        const uint64_t remaining = total - known;
        const uint64_t cap = std::min<uint64_t>(limits_.max_stream, remaining);
        std::string inner;
        const ErrorCode ec = inflate_span(file_, file_size_, spans_[i],
                                          static_cast<size_t>(cap), buffer, &inner);
        if (ec != ErrorCode::Ok) {
            // Slog uppackningen i det som återstår och inte i max_stream är
            // chunken större än posterna har plats för.
            if (ec == ErrorCode::StreamTooLarge && remaining <= limits_.max_stream)
                return fail(detail, ErrorCode::CorruptPackage,
                            chunk_at(i, spans_[i].offset,
                                     "inflates to more data than the records account for"));
            return fail(detail, ec, chunk_at(i, spans_[i].offset, inner));
        }
        chunk_start_[i] = known;
        known += buffer.size();
    }
    chunk_start_[spans_.size()] = known;
    if (known != total)
        return fail(detail, ErrorCode::CorruptPackage,
                    at_byte(offset_, "the chunks inflate to " + std::to_string(known) +
                                         " bytes and end before the " + std::to_string(total) +
                                         " record bytes do"));
    return ErrorCode::Ok;
}

ErrorCode GeometryLayout::decide_normal_bits(
    const std::function<bool(uint64_t, uint64_t)>& progress, std::string* detail) {
    if (normal_bits_ != 0) return ErrorCode::Ok;
    if (limits_.normal_bits != 0) {
        normal_bits_ = limits_.normal_bits;
        decided_by_ = -1;
        return ErrorCode::Ok;
    }

    const uint32_t count = record_count();
    const auto cancelled = [&] {
        return fail(detail, ErrorCode::Cancelled, wrap("deciding the normal width was cancelled"));
    };

    // Posterna läses i ordning, så en cursor utan cache packar upp varje
    // chunk en gång: chunkarna den senaste posten behöver hålls kvar.
    GeometryCursor cursor(*this, file_, file_size_, 0);
    RecordData scratch;
    int bits = 16;
    int64_t by = -1;
    for (uint32_t k = 0; k < count; ++k) {
        if (progress && k % kProgressStep == 0 && !progress(k, count)) return cancelled();

        const uint8_t* bytes = nullptr;
        size_t length = 0;
        const ErrorCode ec = cursor.record_bytes(k, bytes, length, detail);
        // En post över taket avgör ingenting; den ger sitt fel när den efterfrågas.
        if (ec == ErrorCode::LimitExceeded) continue;
        if (ec != ErrorCode::Ok) return ec;

        const ErrorCode wide = parse_record(bytes, length, 16, scratch, nullptr);
        const ErrorCode narrow = parse_record(bytes, length, 8, scratch, nullptr);
        // En post som inte är avkodad, som går med båda bredderna eller som
        // inte går med någon av dem avgör ingenting.
        if (wide == ErrorCode::Unsupported || narrow == ErrorCode::Unsupported) continue;
        if ((wide == ErrorCode::Ok) == (narrow == ErrorCode::Ok)) continue;
        bits = wide == ErrorCode::Ok ? 16 : 8;
        by = k;
        break;
    }
    if (progress && !progress(count, count)) return cancelled();
    normal_bits_ = bits;
    decided_by_ = by;
    return ErrorCode::Ok;
}

uint32_t GeometryLayout::record_count() const {
    return record_start_.empty() ? 0 : static_cast<uint32_t>(record_start_.size() - 1);
}

uint32_t GeometryLayout::chunk_count() const { return static_cast<uint32_t>(spans_.size()); }

uint64_t GeometryLayout::total_bytes() const {
    return record_start_.empty() ? 0 : record_start_.back();
}

ChunkLayout GeometryLayout::layout() const { return limits_.layout; }

const SegmentLimits& GeometryLayout::limits() const { return limits_; }

uint64_t GeometryLayout::chunk_start(uint32_t i) const {
    if (i >= spans_.size()) return 0;
    return limits_.layout == ChunkLayout::Fixed ? i * chunk_bytes_ : chunk_start_[i];
}

uint64_t GeometryLayout::chunk_size(uint32_t i) const {
    if (i >= spans_.size()) return 0;
    if (limits_.layout == ChunkLayout::Scan) return chunk_start_[i + 1] - chunk_start_[i];
    if (i + 1 < spans_.size()) return chunk_bytes_;
    return total_bytes() - i * chunk_bytes_;
}

uint64_t GeometryLayout::record_start(uint32_t k) const {
    return k < record_start_.size() ? record_start_[k] : total_bytes();
}

uint32_t GeometryLayout::chunk_of(uint64_t pos) const {
    if (spans_.empty()) return 0;
    const uint32_t last = static_cast<uint32_t>(spans_.size() - 1);
    if (limits_.layout == ChunkLayout::Fixed)
        return static_cast<uint32_t>(std::min<uint64_t>(pos / chunk_bytes_, last));
    // Den sista chunken som börjar på eller före pos; bland tomma chunkar med
    // samma början är det den som har byten.
    const auto it = std::upper_bound(chunk_start_.begin(), chunk_start_.end(), pos);
    const uint64_t i = static_cast<uint64_t>(it - chunk_start_.begin());
    return static_cast<uint32_t>(std::min<uint64_t>(i == 0 ? 0 : i - 1, last));
}

int GeometryLayout::normal_bits() const { return normal_bits_; }

int64_t GeometryLayout::decided_by() const { return decided_by_; }

size_t GeometryLayout::memory_bytes() const {
    return sizeof(*this) + spans_.capacity() * sizeof(StreamSpan) +
           chunk_start_.capacity() * sizeof(uint64_t) + record_start_.capacity() * sizeof(uint64_t);
}

ErrorCode GeometryLayout::inflate_chunk(const uint8_t* file, size_t size, uint32_t i,
                                        std::vector<uint8_t>& out, std::string* detail) const {
    if (i >= spans_.size())
        return fail(detail, ErrorCode::OutOfRange,
                    std::string(kPackage) + " chunk " + std::to_string(i) +
                        ": index is not below the chunk count");

    const uint64_t expected = chunk_size(i);
    const bool capped = expected > limits_.max_stream;
    const size_t cap = capped ? limits_.max_stream : static_cast<size_t>(expected);
    std::string inner;
    const ErrorCode ec = inflate_span(file, size, spans_[i], cap, out, &inner);
    if (ec != ErrorCode::Ok) {
        std::vector<uint8_t>().swap(out);
        if (ec == ErrorCode::StreamTooLarge && !capped)
            return fail(detail, ErrorCode::CorruptPackage,
                        chunk_at(i, spans_[i].offset,
                                 "inflates to a different size than the layout gives: more than " +
                                     std::to_string(expected) + " bytes"));
        return fail(detail, ec, chunk_at(i, spans_[i].offset, inner));
    }
    if (out.size() != expected) {
        const size_t got = out.size();
        std::vector<uint8_t>().swap(out);
        return fail(detail, ErrorCode::CorruptPackage,
                    chunk_at(i, spans_[i].offset,
                             "inflates to a different size than the layout gives: " +
                                 std::to_string(got) + " bytes, not " + std::to_string(expected)));
    }
    return ErrorCode::Ok;
}

// ---------------------------------------------------------------------------
// GeometryCursor

GeometryCursor::GeometryCursor(const GeometryLayout& layout, const uint8_t* file, size_t size,
                               size_t cache_bytes)
    : layout_(&layout), file_(file), file_size_(size), cache_bytes_(cache_bytes),
      slot_of_(layout.chunk_count(), kNoSlot) {}

size_t GeometryCursor::cached_bytes() const { return cached_bytes_; }

uint64_t GeometryCursor::inflated_total() const { return inflated_total_; }

// Ser till att chunken ligger uppackad i cachen, sist i dess ordning.
ErrorCode GeometryCursor::load(uint32_t chunk, std::string* detail) {
    if (slot_of_[chunk] != kNoSlot) {
        touch(slot_of_[chunk]);
        return ErrorCode::Ok;
    }

    std::vector<uint8_t> data;
    const ErrorCode ec = layout_->inflate_chunk(file_, file_size_, chunk, data, detail);
    if (ec != ErrorCode::Ok) return ec;
    // Bufferten kortas till det den håller, och cachen räknar det minne den
    // faktiskt tar: annars kan många små chunkar ta mångfalt mer än taket.
    data.shrink_to_fit();

    uint32_t s;
    if (!free_.empty()) {
        s = free_.back();
        free_.pop_back();
    } else {
        slots_.emplace_back();
        s = static_cast<uint32_t>(slots_.size() - 1);
    }
    Slot& slot = slots_[s];
    slot.chunk = chunk;
    inflated_total_ += data.size();
    cached_bytes_ += data.capacity();
    slot.data = std::move(data);
    slot_of_[chunk] = s;
    touch(s);
    return ErrorCode::Ok;
}

// Cachens ordning är en länkad lista genom platserna, så att den som användes
// längst sedan hittas utan sökning.
void GeometryCursor::unlink(uint32_t s) {
    Slot& slot = slots_[s];
    if (slot.older != kNoSlot) slots_[slot.older].newer = slot.newer;
    else if (oldest_ == s) oldest_ = slot.newer;
    if (slot.newer != kNoSlot) slots_[slot.newer].older = slot.older;
    else if (newest_ == s) newest_ = slot.older;
    slot.older = kNoSlot;
    slot.newer = kNoSlot;
}

void GeometryCursor::touch(uint32_t s) {
    if (newest_ == s) return;
    unlink(s);
    Slot& slot = slots_[s];
    slot.older = newest_;
    if (newest_ != kNoSlot) slots_[newest_].newer = s;
    newest_ = s;
    if (oldest_ == kNoSlot) oldest_ = s;
}

// Kastar de chunkar som användes längst sedan tills cachen ryms under taket,
// utom dem i [keep_from, keep_to].
void GeometryCursor::evict(uint32_t keep_from, uint32_t keep_to) {
    uint32_t s = oldest_;
    while (cached_bytes_ > cache_bytes_ && s != kNoSlot) {
        const uint32_t next = slots_[s].newer;
        Slot& slot = slots_[s];
        if (slot.chunk < keep_from || slot.chunk > keep_to) {
            unlink(s);
            cached_bytes_ -= slot.data.capacity();
            std::vector<uint8_t>().swap(slot.data);
            slot_of_[slot.chunk] = kNoSlot;
            slot.chunk = kNoSlot;
            free_.push_back(s);
        }
        s = next;
    }
}

ErrorCode GeometryCursor::record_bytes(uint32_t k, const uint8_t*& data, size_t& size,
                                       std::string* detail) {
    if (!layout_ || k >= layout_->record_count())
        return fail(detail, ErrorCode::OutOfRange,
                    record_at(k, "index is not below the record count"));

    const uint64_t rs = layout_->record_start(k);
    const uint64_t re = layout_->record_start(k + 1);
    const uint64_t length = re - rs;
    if (length > layout_->limits().max_record)
        return fail(detail, ErrorCode::LimitExceeded,
                    record_at(k, "is longer than the record limit"));
    if (length == 0) {
        data = scratch_.data();
        size = 0;
        return ErrorCode::Ok;
    }

    // Layouten säger i vilka chunkar posten ligger; ingen annan chunk packas upp.
    const uint32_t c0 = layout_->chunk_of(rs);
    const uint32_t c1 = layout_->chunk_of(re - 1);
    for (uint32_t i = c0; i <= c1; ++i) {
        if (layout_->chunk_size(i) == 0) continue;
        const ErrorCode ec = load(i, detail);
        if (ec != ErrorCode::Ok) return ec;
        evict(c0, c1);
    }

    if (c0 == c1) {
        const Slot& slot = slots_[slot_of_[c0]];
        data = slot.data.data() + static_cast<size_t>(rs - layout_->chunk_start(c0));
        size = static_cast<size_t>(length);
        return ErrorCode::Ok;
    }

    // Posten går över en eller flera chunkgränser: sätt ihop den.
    scratch_.resize(static_cast<size_t>(length));
    for (uint32_t i = c0; i <= c1; ++i) {
        const uint64_t start = layout_->chunk_start(i);
        const uint64_t from = std::max(rs, start);
        const uint64_t to = std::min(re, start + layout_->chunk_size(i));
        if (to <= from) continue;
        const Slot& slot = slots_[slot_of_[i]];
        std::memcpy(scratch_.data() + static_cast<size_t>(from - rs),
                    slot.data.data() + static_cast<size_t>(from - start),
                    static_cast<size_t>(to - from));
    }
    data = scratch_.data();
    size = scratch_.size();
    return ErrorCode::Ok;
}

ErrorCode GeometryCursor::parse(uint32_t k, RecordData& out, std::string* detail) {
    if (!layout_ || k >= layout_->record_count())
        return fail(detail, ErrorCode::OutOfRange,
                    record_at(k, "index is not below the record count"));
    const int bits = layout_->normal_bits();
    if (bits == 0)
        return fail(detail, ErrorCode::NotLoaded, wrap("the normal width is not decided"));

    const uint8_t* bytes = nullptr;
    size_t length = 0;
    ErrorCode ec = record_bytes(k, bytes, length, detail);
    if (ec != ErrorCode::Ok) return ec;

    std::string inner;
    ec = parse_record(bytes, length, bits, out, &inner);
    if (ec == ErrorCode::Ok) return ec;

    // Med en bredd som avgjorts ur posterna kan en post som bara går med den
    // andra bredden inte ha setts vid avgörandet.
    if (ec != ErrorCode::Unsupported && layout_->limits().normal_bits == 0) {
        RecordData other;
        if (parse_record(bytes, length, bits == 16 ? 8 : 16, other, nullptr) == ErrorCode::Ok)
            return fail(detail, ErrorCode::CorruptGeometry,
                        record_at(k, "parses only under the other normal width"));
    }
    return fail(detail, ec, record_at(k, inner));
}

// ---------------------------------------------------------------------------
// GeometrySegment

ErrorCode GeometrySegment::open(const uint8_t* file, size_t size, const DbCacheEntry& entry,
                                const SegmentLimits& limits, std::string* detail) {
    cursor_ = GeometryCursor{};
    layout_.reset();
    file_ = nullptr;
    file_size_ = 0;

    auto layout = std::make_unique<GeometryLayout>();
    const ErrorCode ec = layout->open(file, size, entry, limits, detail);
    if (ec != ErrorCode::Ok) return ec;
    layout_ = std::move(layout);
    cursor_ = GeometryCursor(*layout_, file, size, limits.cache_bytes);
    file_ = file;
    file_size_ = size;
    return ErrorCode::Ok;
}

uint32_t GeometrySegment::record_count() const { return layout_ ? layout_->record_count() : 0; }

uint32_t GeometrySegment::chunk_count() const { return layout_ ? layout_->chunk_count() : 0; }

uint64_t GeometrySegment::total_bytes() const { return layout_ ? layout_->total_bytes() : 0; }

ErrorCode GeometrySegment::record_bytes(uint32_t k, const uint8_t*& data, size_t& size,
                                        std::string* detail) {
    return cursor_.record_bytes(k, data, size, detail);
}

ErrorCode GeometrySegment::resolve_normal_bits(std::string* detail) {
    if (!layout_)
        return fail(detail, ErrorCode::NotLoaded, wrap("the package is not opened"));
    return layout_->decide_normal_bits({}, detail);
}

int GeometrySegment::normal_bits() const { return layout_ ? layout_->normal_bits() : 0; }

int64_t GeometrySegment::decided_by() const { return layout_ ? layout_->decided_by() : -1; }

ErrorCode GeometrySegment::parse(uint32_t k, RecordData& out, std::string* detail) {
    if (layout_ && k < layout_->record_count()) {
        const ErrorCode ec = resolve_normal_bits(detail);
        if (ec != ErrorCode::Ok) return ec;
    }
    return cursor_.parse(k, out, detail);
}

ErrorCode GeometrySegment::size_all(std::string* detail) {
    if (!layout_) return ErrorCode::Ok;
    std::vector<uint8_t> buffer;
    for (uint32_t i = 0; i < layout_->chunk_count(); ++i) {
        const ErrorCode ec = layout_->inflate_chunk(file_, file_size_, i, buffer, detail);
        if (ec != ErrorCode::Ok) return ec;
    }
    return ErrorCode::Ok;
}

uint32_t GeometrySegment::sized_chunks() const { return chunk_count(); }

uint64_t GeometrySegment::chunk_size(uint32_t i) const { return layout_ ? layout_->chunk_size(i) : 0; }

uint64_t GeometrySegment::chunk_start(uint32_t i) const {
    return layout_ ? layout_->chunk_start(i) : 0;
}

size_t GeometrySegment::cached_bytes() const { return cursor_.cached_bytes(); }

uint64_t GeometrySegment::inflated_total() const { return cursor_.inflated_total(); }

const GeometryLayout* GeometrySegment::layout() const { return layout_.get(); }

} // namespace HNwd::detail
