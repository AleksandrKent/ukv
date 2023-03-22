
#include <fcntl.h>    // `open` files
#include <sys/stat.h> // `stat` to obtain file metadata
#include <sys/mman.h> // `mmap` to read datasets faster
#include <unistd.h>   // `close` files

#include <ctime>
#include <vector>
#include <cstring>
#include <numeric>
#include <algorithm>
#include <filesystem>

#include <arrow/api.h>
#include <arrow/array.h>
#include <arrow/table.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/io/api.h>
#include <arrow/csv/api.h>
#include <arrow/io/file.h>
#include <arrow/csv/writer.h>
#include <arrow/memory_pool.h>
#include <parquet/arrow/reader.h>
#include <parquet/stream_writer.h>
#include <arrow/compute/api_aggregate.h>

#include <simdjson.h>
#include <fmt/format.h>

#include <ukv/ukv.hpp>

#include "dataset.h"
#include "../benchmarks/mixed.hpp"
#include "helpers/linked_memory.hpp" // `linked_memory_lock_t`

#include <ukv/cpp/ranges.hpp>
#include <ukv/cpp/blobs_range.hpp> // `keys_stream_t`

using namespace unum::ukv::bench;
using namespace unum::ukv;

// Count of symbols to make json ('"', '"', ':', ',')
constexpr ukv_size_t symbols_count_k = 4;
// Json object open brackets for json and parquet
constexpr ukv_str_view_t prefix_k = "{";

std::mutex gen_mtx;

using tape_t = ptr_range_gt<ukv_char_t>;
using fields_t = strided_iterator_gt<ukv_str_view_t const>;
using lengths_t = strided_iterator_gt<ukv_length_t const>;
using keys_length_t = std::pair<ukv_key_t*, ukv_size_t>;
using val_t = std::pair<ukv_bytes_ptr_t, ukv_size_t>;
using counts_t = ptr_range_gt<ukv_size_t>;
using chunked_array_t = std::shared_ptr<arrow::ChunkedArray>;
using array_t = std::shared_ptr<arrow::Array>;
using int_builder_t = arrow::NumericBuilder<arrow::Int64Type>;

enum ukv_dataset_ext_t {
    parquet_k = 0,
    csv_k,
    ndjson_k,
    unknown_k,
};
using ext_t = ukv_dataset_ext_t;

#pragma region - Helpers

template <typename at>
class arena_allocator_gt {
  public:
    using value_type = at;
    using pointer = value_type*;
    using const_pointer = value_type const*;
    using reference = value_type&;
    using const_reference = value_type const&;
    using size_type = ukv_size_t;
    using difference_type = std::ptrdiff_t;

    template <typename other_at>
    struct rebind {
        using other = arena_allocator_gt<other_at>;
    };

    inline explicit arena_allocator_gt()
        : arena_(linked_memory(nullptr, ukv_options_default_k, nullptr)), error_(nullptr) {}
    inline ~arena_allocator_gt() {}
    inline explicit arena_allocator_gt(arena_allocator_gt const& other) : arena_(other.arena_), error_(other.error_) {}
    template <typename other_at>
    inline explicit arena_allocator_gt(arena_allocator_gt<other_at> const& other)
        : arena_(other.arena_), error_(other.error_) {}
    inline explicit arena_allocator_gt(linked_memory_lock_t const& arena, ukv_error_t* error)
        : arena_(arena), error_(error) {}

    inline const_pointer address(const_reference r) { return &r; }
    inline pointer address(reference r) { return &r; }

    inline pointer allocate(size_type sz, typename std::allocator<void>::const_pointer = nullptr) {
        return arena_.alloc<value_type>(sz, error_).begin();
    }
    inline void deallocate(pointer p, size_type) { p->~value_type(); }
    inline size_type max_size() const { return std::numeric_limits<size_type>::max() / sizeof(value_type); }
    inline void construct(pointer p, value_type const& t) { new (p) value_type(t); }
    inline void destroy(pointer p) { p->~value_type(); }
    inline bool operator==(arena_allocator_gt const&) { return true; }
    inline bool operator!=(arena_allocator_gt const& a) { return !operator==(a); }

  private:
    linked_memory_lock_t arena_;
    ukv_error_t* error_;
};

template <typename at>
using alloc_t = arena_allocator_gt<at>;

template <typename at>
using stl_vector_t = std::vector<at, alloc_t<at>>;
using docs_t = stl_vector_t<value_view_t>;

class arrow_visitor_t {
  public:
    arrow_visitor_t(std::string& json) : json(json) {}

    arrow::Status Visit(arrow::NullArray const& arr) {
        fmt::format_to(std::back_inserter(json), "\"\",");
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::BooleanArray const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::Int8Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::Int16Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::Int32Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::Int64Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::UInt8Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::UInt16Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::UInt32Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::UInt64Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::HalfFloatArray const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::FloatArray const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::DoubleArray const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::StringArray const& arr) { return format_bin_str(arr, idx); }
    arrow::Status Visit(arrow::BinaryArray const& arr) { return format_bin_str(arr, idx); }
    arrow::Status Visit(arrow::LargeStringArray const& arr) { return format_bin_str(arr, idx); }
    arrow::Status Visit(arrow::LargeBinaryArray const& arr) { return format_bin_str(arr, idx); }
    arrow::Status Visit(arrow::FixedSizeBinaryArray const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::Date32Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::Date64Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::Time32Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::Time64Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::TimestampArray const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::DayTimeIntervalArray const& arr) {
        auto ds = arr.Value(idx);
        fmt::format_to(std::back_inserter(json), "{{\"days\":{},\"ms-s\":{}}},", ds.days, ds.milliseconds);
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::MonthDayNanoIntervalArray const& arr) {
        auto mdn = arr.Value(idx);
        fmt::format_to(std::back_inserter(json),
                       "{{\"months\":{},\"days\":{},\"us-s\":{}}},",
                       mdn.months,
                       mdn.days,
                       mdn.nanoseconds);
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::MonthIntervalArray const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::DurationArray const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::Decimal128Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::Decimal256Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::ListArray const& arr) {
        arrow::VisitArrayInline(*arr.values().get(), this);
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::LargeListArray const& arr) {
        arrow::VisitArrayInline(*arr.values().get(), this);
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::MapArray const& arr) {
        arrow::VisitArrayInline(*arr.values().get(), this);
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::FixedSizeListArray const& arr) {
        arrow::VisitArrayInline(*arr.values().get(), this);
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::DictionaryArray const& arr) {
        fmt::format_to(std::back_inserter(json), "{},", arr.GetValueIndex(idx));
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::ExtensionArray const& arr) {
        arrow::VisitArrayInline(*arr.storage().get(), this);
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::StructArray const& arr) {
        return arrow::Status(arrow::StatusCode::TypeError, "Not supported type");
    }
    arrow::Status Visit(arrow::SparseUnionArray const& arr) {
        return arrow::Status(arrow::StatusCode::TypeError, "Not supported type");
    }
    arrow::Status Visit(arrow::DenseUnionArray const& arr) {
        return arrow::Status(arrow::StatusCode::TypeError, "Not supported type");
    }

    std::string& json;
    ukv_size_t idx = 0;

  private:
    inline static constexpr ukv_char_t int_to_hex_k[16] = {
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

    inline void char_to_hex(ukv_byte_t const c, ukv_byte_t* hex) noexcept {
        hex[0] = int_to_hex_k[c >> 4];
        hex[1] = int_to_hex_k[c & 0x0F];
    }

    template <typename at>
    arrow::Status format(at const& cont, ukv_size_t idx) {
        fmt::format_to(std::back_inserter(json), "{},", cont.Value(idx));
        return arrow::Status::OK();
    }

    template <typename at>
    arrow::Status format_bin_str(at const& cont, ukv_size_t idx) {
        auto str = cont.Value(idx);
        json.push_back('\"');
        json.reserve(json.size() + str.size());
        for (ukv_size_t ch_idx = 0; ch_idx != str.size(); ++ch_idx) {
            ukv_byte_t ch = str[ch_idx];
            switch (ch) {
            case 34: json += "\\\""; break;
            case 92: json += "\\\\"; break;
            case 8: json += "\\b"; break;
            case 9: json += "\\t"; break;
            case 10: json += "\\n"; break;
            case 12: json += "\\f"; break;
            case 13: json += "\\r"; break;
            case 0: [[fallthrough]];
            case 1: [[fallthrough]];
            case 2: [[fallthrough]];
            case 3: [[fallthrough]];
            case 4: [[fallthrough]];
            case 5: [[fallthrough]];
            case 6: [[fallthrough]];
            case 7: [[fallthrough]];
            case 11: [[fallthrough]];
            case 14: [[fallthrough]];
            case 15: [[fallthrough]];
            case 16: [[fallthrough]];
            case 17: [[fallthrough]];
            case 18: [[fallthrough]];
            case 19: [[fallthrough]];
            case 20: [[fallthrough]];
            case 21: [[fallthrough]];
            case 22: [[fallthrough]];
            case 23: [[fallthrough]];
            case 24: [[fallthrough]];
            case 25: [[fallthrough]];
            case 26: [[fallthrough]];
            case 27: [[fallthrough]];
            case 28: [[fallthrough]];
            case 29: [[fallthrough]];
            case 30: [[fallthrough]];
            case 31: {
                json += "\\u0000";
                auto target_ptr = reinterpret_cast<ukv_byte_t*>(json.data() + json.size() - 2);
                char_to_hex(ch, target_ptr);
                break;
            }
            default: json += ch; [[fallthrough]];
            }
        }
        if (json.back() == '\n')
            json.pop_back();
        json += "\",";
        return arrow::Status::OK();
    }
};

bool strcmp_(ukv_char_t const* lhs, ukv_char_t const* rhs) {
    return std::strcmp(lhs, rhs) == 0;
}

bool strncmp_(ukv_char_t const* lhs, ukv_char_t const* rhs, ukv_size_t sz) {
    return std::strncmp(lhs, rhs, sz) == 0;
}

bool chrcmp_(const ukv_char_t lhs, const ukv_char_t rhs) {
    return lhs == rhs;
}

bool is_json_ptr(ukv_str_view_t field) {
    return chrcmp_(field[0], '/');
}

auto get_time_since_epoch() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

std::string generate_file_name() {
    time_t now = time(0);
    gen_mtx.lock();
    ukv_char_t* out = std::ctime(&now);
    gen_mtx.unlock();
    for (ukv_size_t idx = 0; idx < strlen(out); ++idx) {
        if ((out[idx] == ' ') | (out[idx] == ':'))
            out[idx] = '_';
    }
    out[strlen(out) - 1] = '\0';
    return fmt::format("{}_{}", out, get_time_since_epoch());
}

template <typename task_at>
void validate_docs_fields(task_at& task) {
    if (!task.fields_count && !task.fields)
        return;
    return_error_if_m(!(!task.fields_count && task.fields),
                      task.error,
                      uninitialized_state_k,
                      "Fields count must be initialized");
    return_error_if_m(!(task.fields_count && !task.fields),
                      task.error,
                      uninitialized_state_k,
                      "Fields must be initialized");
    return_error_if_m(!(task.fields_count && task.fields && !task.fields_stride),
                      task.error,
                      uninitialized_state_k,
                      "Fields stride must be initialized");

    fields_t fields {task.fields, task.fields_stride};
    for (ukv_size_t idx = 0; idx < task.fields_count; ++idx) {
        return_error_if_m(fields[idx] != nullptr, task.error, 0, "Invalid field!");
    }
}

void check_for_id_field(ukv_docs_import_t& imp) {
    if (imp.fields == nullptr)
        return;

    bool state = false;
    fields_t fields {imp.fields, imp.fields_stride};
    for (ukv_size_t idx = 0; idx < imp.fields_count; ++idx) {
        if (strcmp_(fields[idx], imp.id_field))
            state = true;
    }
    return_error_if_m(state, imp.error, 0, "Fields must contain id_field");
}

template <typename ukv_docs_task_t>
fields_t prepare_fields(ukv_docs_task_t& c, linked_memory_lock_t& arena) {

    if (c.fields_count == 1)
        return {c.fields, c.fields_stride};
    fields_t fields {c.fields, c.fields_stride};
    ukv_size_t next_idx = 0;

    auto bitmask = arena.alloc<ukv_octet_t>(c.fields_count, c.error);
    for (auto& bit : bitmask)
        bit = 1;

    for (ukv_size_t idx = 0; idx < c.fields_count;) {
        while (!bitmask[idx])
            ++idx;

        next_idx = idx + 1;
        if (chrcmp_(fields[idx][0], '/')) {
            while (next_idx < c.fields_count && strncmp_(fields[idx], fields[next_idx], strlen(fields[idx])) &&
                   chrcmp_(fields[next_idx][strlen(fields[idx])], '/') && bitmask[next_idx]) {
                bitmask[next_idx] = 0;
                ++next_idx;
            }
        }
        else {
            while (next_idx < c.fields_count && !chrcmp_(fields[next_idx][0], '/') &&
                   strcmp_(fields[idx], fields[next_idx])) {
                bitmask[next_idx] = 0;
                ++next_idx;
            }
            ukv_size_t ptr_idx = next_idx;
            while (ptr_idx < c.fields_count && !chrcmp_(fields[ptr_idx][0], '/'))
                ++ptr_idx;

            while (ptr_idx < c.fields_count) {
                if (strncmp_(fields[idx], &fields[ptr_idx][1], strlen(fields[idx])) &&
                    chrcmp_(fields[ptr_idx][strlen(fields[idx]) + 1], '/'))
                    bitmask[ptr_idx] = 0;
                ++ptr_idx;
            }
        }
        idx = next_idx;
    }

    size_t count = 0;
    for (auto bit : bitmask) {
        if (bit)
            ++count;
    }
    auto prepared_fields = arena.alloc<ukv_str_view_t>(count, c.error);

    for (ukv_size_t idx = 0, pos = 0; idx < c.fields_count; ++idx) {
        if (!bitmask[idx])
            continue;

        ukv_size_t len = strlen(fields[idx]);
        auto field = arena.alloc<ukv_char_t>(len + 1, c.error);
        std::memcpy(field.begin(), fields[idx], len + 1);
        prepared_fields[pos] = field.begin();
        ++pos;
    }
    c.fields_count = count;
    return {prepared_fields.begin(), sizeof(ukv_str_view_t)};
}

simdjson::ondemand::object& rewinded(simdjson::ondemand::object& doc) noexcept {
    doc.reset();
    return doc;
}

void get_value(simdjson::ondemand::object& data, ukv_str_view_t json_field, ukv_str_view_t field, std::string& json) {

    bool state = is_json_ptr(field);

    auto get_result = [&]() {
        return state ? rewinded(data).at_pointer(field) : rewinded(data)[field];
    };

    switch (get_result().type()) {
    case simdjson::ondemand::json_type::array:
        fmt::format_to( //
            std::back_inserter(json),
            "{}{},",
            json_field,
            get_result().get_array().value().raw_json().value());
        break;
    case simdjson::ondemand::json_type::object:
        fmt::format_to( //
            std::back_inserter(json),
            "{}{},",
            json_field,
            get_result().get_object().value().raw_json().value());
        break;
    case simdjson::ondemand::json_type::number:
        fmt::format_to( //
            std::back_inserter(json),
            "{}{},",
            json_field,
            std::string_view(get_result().raw_json_token().value()));
        break;
    case simdjson::ondemand::json_type::string:
        fmt::format_to( //
            std::back_inserter(json),
            "{}{},",
            json_field,
            std::string_view(get_result().raw_json_token().value()));
        break;
    case simdjson::ondemand::json_type::boolean:
        fmt::format_to( //
            std::back_inserter(json),
            "{}{},",
            json_field,
            bool(get_result().value()) ? "true" : "false");
        break;
    case simdjson::ondemand::json_type::null:
        fmt::format_to( //
            std::back_inserter(json),
            "{}{},",
            json_field,
            "null");
        break;
    default: break;
    }
}

void simdjson_object_parser( //
    simdjson::ondemand::object& object,
    counts_t counts,
    fields_t const& fields,
    ukv_size_t fields_count,
    tape_t const& tape,
    std::string& json) {

    strings_tape_iterator_t iter(fields_count * fields_count, tape.begin());
    auto counts_iter = counts.begin();

    auto try_close = [&]() {
        if (!strcmp_(*iter, "}"))
            return;

        do {
            json.insert(json.size() - 1, *iter);
            ++iter;
        } while (strcmp_(*iter, "}"));
    };

    for (ukv_size_t idx = 0; idx < fields_count;) {
        try_close();
        if (is_json_ptr(fields[idx])) {
            if (strchr(fields[idx] + 1, '/') != nullptr) {
                while ((*iter)[strlen(*iter) - 1] == '{') {
                    fmt::format_to(std::back_inserter(json), "{}", *iter);
                    ++iter;
                }
                for (ukv_size_t pos = idx; idx < pos + (*counts_iter); ++idx, ++iter)
                    get_value(object, *iter, fields[idx], json);
                ++counts_iter;
                continue;
            }
        }
        get_value(object, *iter, fields[idx], json);
        ++iter;
        ++idx;
    }
    try_close();
    json.back() = '}';
}

void fields_parser( //
    ukv_error_t* error,
    linked_memory_lock_t& arena,
    ukv_size_t fields_count,
    fields_t const& fields,
    counts_t& counts,
    tape_t& tape) {

    ukv_size_t counts_idx = ULLONG_MAX;
    ukv_size_t back_idx = 0;
    ukv_size_t pre_idx = 0;
    ukv_size_t offset = 0;
    ukv_size_t size = 0;
    ukv_size_t pos = 0;

    for (ukv_size_t idx = 0; idx < fields_count; ++idx) {
        pos = 1;
        if (is_json_ptr(fields[idx])) {
            pos = strchr(fields[idx] + pos, '/') - fields[idx];
            while (pos <= strlen(fields[idx])) {
                ++size;
                pos = strchr(fields[idx] + pos + 1, '/') - fields[idx];
            }
        }
    }

    auto prefixes = arena.alloc<ukv_char_t*>(size, error);

    auto close_bracket = [&]() {
        --back_idx;
        tape[offset] = '}';
        tape[offset + 1] = '\0';
        offset += 2;
    };

    auto fill_prefixes = [&](ukv_str_view_t field) {
        while (pos <= strlen(field)) {
            prefixes[back_idx] = arena.alloc<ukv_char_t>(pos + 2, error).begin();
            ++back_idx;
            std::memcpy(prefixes[back_idx - 1], field, pos + 1);
            prefixes[back_idx - 1][pos + 1] = '\0';
            auto substr = arena.alloc<ukv_char_t>(pos - pre_idx + 2, error);
            std::memcpy(substr.begin(), field + pre_idx, pos - pre_idx);
            substr[substr.size() - 2] = '\0';
            auto str = fmt::format("\"{}\":{{{}", substr.begin(), '\0');
            strncpy(tape.begin() + offset, str.data(), str.size());
            offset += str.size();
            pre_idx = pos + 1;
            pos = strchr(field + pre_idx, '/') - field;
        }
    };

    for (ukv_size_t idx = 0; idx < fields_count;) {
        if (is_json_ptr(fields[idx])) {
            pre_idx = 1;

            pos = strchr(fields[idx] + pre_idx, '/') - fields[idx];
            if (pos <= strlen(fields[idx])) {
                fill_prefixes(fields[idx]);
                while (back_idx) {
                    ++counts_idx;
                    counts[counts_idx] = 0;
                    while (strncmp_(prefixes[back_idx - 1], fields[idx], strlen(prefixes[back_idx - 1]))) {
                        pre_idx = strlen(prefixes[back_idx - 1]) + 1;
                        ukv_size_t length = strlen(fields[idx]) - pre_idx + 2;
                        auto substr = arena.alloc<ukv_char_t>(length, error);
                        std::memcpy(substr.begin(), fields[idx] + pre_idx - 1, length);
                        substr[substr.size() - 1] = '\0';
                        auto str = fmt::format("\"{}\":{}", substr.begin(), '\0');
                        strncpy(tape.begin() + offset, str.data(), str.size());
                        offset += str.size();
                        ++counts[counts_idx];
                        ++idx;
                        if (idx == fields_count) {
                            while (back_idx)
                                close_bracket();
                            return;
                        }
                        ukv_size_t sz = back_idx;
                        while (back_idx && !strncmp_( //
                                               prefixes[back_idx - 1],
                                               fields[idx],
                                               strlen(prefixes[back_idx - 1]))) {
                            close_bracket();
                        }

                        if (back_idx == 0)
                            break;
                        else if (sz > back_idx) {
                            pre_idx = strlen(prefixes[back_idx - 1]) + 1;
                            ++counts_idx;
                            counts[counts_idx] = 0;
                        }

                        pos = strchr(fields[idx] + pre_idx, '/') - fields[idx];
                        if (pos <= strlen(fields[idx])) {
                            --pre_idx;
                            fill_prefixes(fields[idx]);
                            ++counts_idx;
                            counts[counts_idx] = 0;
                        }
                    }
                }
            }
            else {
                auto str = fmt::format("\"{}\":{}", fields[idx] + 1, '\0');
                strncpy(tape.begin() + offset, str.data(), str.size());
                offset += str.size();
                break;
                ++idx;
            }
        }
        else {
            auto str = fmt::format("\"{}\":{}", fields[idx], '\0');
            strncpy(tape.begin() + offset, str.data(), str.size());
            offset += str.size();
            ++idx;
        }
    }
}

#pragma endregion - Helpers

#pragma region - Upserting

void upsert_docs(ukv_docs_import_t& c, docs_t& docs) {

    ukv_docs_write_t docs_write {
        .db = c.db,
        .error = c.error,
        .arena = c.arena,
        .options = ukv_option_dont_discard_memory_k,
        .tasks_count = docs.size(),
        .type = ukv_doc_field_json_k,
        .modification = ukv_doc_modify_upsert_k,
        .collections = &c.collection,
        .lengths = docs.front().member_length(),
        .lengths_stride = sizeof(value_view_t),
        .values = docs.front().member_ptr(),
        .values_stride = sizeof(value_view_t),
        .id_field = c.id_field,
    };

    ukv_docs_write(&docs_write);
}

#pragma endregion - Upserting

#pragma region - Docs

template <typename import_t>
void import_parquet(import_t& c, std::shared_ptr<arrow::Table>& table) {

    arrow::Status status;
    arrow::MemoryPool* pool = arrow::default_memory_pool();

    // Open File
    auto maybe_input = arrow::io::ReadableFile::Open(c.paths_pattern);
    return_error_if_m(maybe_input.ok(), c.error, 0, status.ToString().c_str());
    auto input = *maybe_input;

    std::unique_ptr<parquet::arrow::FileReader> arrow_reader;
    status = parquet::arrow::OpenFile(input, pool, &arrow_reader);
    return_error_if_m(status.ok(), c.error, 0, status.ToString().c_str());

    // Read File into table
    status = arrow_reader->ReadTable(&table);
    return_error_if_m(status.ok(), c.error, 0, status.ToString().c_str());
}

template <typename import_t>
void import_csv(import_t& c, std::shared_ptr<arrow::Table>& table) {

    arrow::io::IOContext io_context = arrow::io::default_io_context();
    auto maybe_input = arrow::io::ReadableFile::Open(c.paths_pattern);
    return_error_if_m(maybe_input.ok(), c.error, 0, "Can't open file");
    std::shared_ptr<arrow::io::InputStream> input = *maybe_input;

    auto read_options = arrow::csv::ReadOptions::Defaults();
    auto parse_options = arrow::csv::ParseOptions::Defaults();
    auto convert_options = arrow::csv::ConvertOptions::Defaults();

    // Instantiate TableReader from input stream and options
    auto maybe_reader = arrow::csv::TableReader::Make(io_context, input, read_options, parse_options, convert_options);
    return_error_if_m(maybe_reader.ok(), c.error, 0, "Can't instantiate reader");
    std::shared_ptr<arrow::csv::TableReader> reader = *maybe_reader;

    // Read table from CSV file
    auto maybe_table = reader->Read();
    return_error_if_m(maybe_table.ok(), c.error, 0, "Can't read file");
    table = *maybe_table;
}

void parse_arrow_table(ukv_docs_import_t& c, std::shared_ptr<arrow::Table> const& table) {

    fields_t fields;
    ukv_size_t used_mem = 0;
    ukv_char_t* field = nullptr;
    std::string json = "{";
    ukv_char_t* json_cstr = nullptr;
    arrow_visitor_t visitor(json);
    auto arena = linked_memory(c.arena, c.options, c.error);

    if (!c.fields) {
        auto clmn_names = table->ColumnNames();
        c.fields_count = clmn_names.size();
        auto names = arena.alloc<ukv_str_view_t>(c.fields_count, c.error);

        for (ukv_size_t idx = 0; idx < c.fields_count; ++idx) {
            field = arena.alloc<ukv_char_t>(clmn_names[idx].size() + 1, c.error).begin();
            std::memcpy(field, clmn_names[idx].data(), clmn_names[idx].size() + 1);
            names[idx] = field;
        }

        c.fields_stride = sizeof(ukv_str_view_t);
        fields = fields_t {names.begin(), c.fields_stride};
    }
    else {
        fields = fields_t {c.fields, c.fields_stride};
        c.fields_count = c.fields_count;
    }

    docs_t values(alloc_t<value_view_t>(arena, c.error));
    auto columns = arena.alloc<chunked_array_t>(c.fields_count, c.error);
    auto chunks = arena.alloc<array_t>(c.fields_count, c.error);

    auto columns_begin = columns.begin();
    auto chunks_begin = chunks.begin();

    for (ukv_size_t idx = 0; idx < c.fields_count; ++idx, ++columns_begin, ++chunks_begin) {
        new (columns_begin) chunked_array_t();
        new (chunks_begin) array_t();
        chunked_array_t column = table->GetColumnByName(fields[idx]);
        return_error_if_m(column, c.error, 0, fmt::format("{} is not exist", fields[idx]).c_str());
        columns[idx] = column;
    }

    ukv_size_t count = columns[0]->num_chunks();
    values.reserve(ukv_size_t(columns[0]->chunk(0)->length()));

    for (ukv_size_t chunk_idx = 0, g_idx = 0; chunk_idx != count; ++chunk_idx, g_idx = 0) {

        for (auto column : columns) {
            chunks[g_idx] = column->chunk(chunk_idx);
            ++g_idx;
        }

        for (ukv_size_t value_idx = 0; value_idx < columns[0]->chunk(chunk_idx)->length(); ++value_idx) {

            g_idx = 0;
            for (auto it = fields; g_idx < c.fields_count; ++g_idx, ++it) {
                fmt::format_to(std::back_inserter(json), "\"{}\":", *it);
                visitor.idx = value_idx;
                arrow::VisitArrayInline(*chunks[g_idx].get(), &visitor);
            }

            json[json.size() - 1] = '}';
            json.push_back('\n');
            json_cstr = arena.alloc<ukv_char_t>(json.size() + 1, c.error).begin();
            std::memcpy(json_cstr, json.data(), json.size() + 1);

            values.push_back(json_cstr);
            used_mem += json.size();
            json = "{";

            if (used_mem >= c.max_batch_size) {
                upsert_docs(c, values);
                values.clear();
                used_mem = 0;
            }
        }
    }
    if (values.size() != 0) {
        upsert_docs(c, values);
        values.clear();
    }
}

void import_whole_ndjson(ukv_docs_import_t& c, simdjson::ondemand::document_stream& docs) {

    auto arena = linked_memory(c.arena, c.options, c.error);
    docs_t values(alloc_t<value_view_t>(arena, c.error));

    ukv_size_t used_mem = 0;

    for (auto doc : docs) {
        simdjson::ondemand::object object = doc.get_object().value();
        values.push_back(rewinded(object).raw_json().value());
        used_mem += values.back().size();
        if (used_mem >= c.max_batch_size) {
            upsert_docs(c, values);
            values.clear();
        }
    }
    if (values.size() != 0)
        upsert_docs(c, values);
}

void import_sub_ndjson(ukv_docs_import_t& c, simdjson::ondemand::document_stream& docs) noexcept {

    auto arena = linked_memory(c.arena, c.options, c.error);
    auto fields = prepare_fields(c, arena);
    ukv_size_t max_size = c.fields_count * symbols_count_k;
    for (ukv_size_t idx = 0; idx < c.fields_count; ++idx)
        max_size += strlen(fields[idx]);

    docs_t values(alloc_t<value_view_t>(arena, c.error));

    ukv_size_t used_mem = 0;
    std::string json = "{";
    ukv_char_t* json_cstr = nullptr;

    counts_t counts = arena.alloc<ukv_size_t>(c.fields_count, c.error);
    return_if_error_m(c.error);

    auto tape = arena.alloc<ukv_char_t>(max_size, c.error);
    return_if_error_m(c.error);
    fields_parser(c.error, arena, c.fields_count, fields, counts, tape);

    for (auto doc : docs) {
        simdjson::ondemand::object object = doc.get_object().value();

        try {
            simdjson_object_parser(object, counts, fields, c.fields_count, tape, json);
        }
        catch (simdjson::simdjson_error const& ex) {
            *c.error = ex.what();
            return_if_error_m(c.error);
        }

        json.push_back('\n');
        json_cstr = arena.alloc<ukv_char_t>(json.size() + 1, c.error).begin();
        std::memcpy(json_cstr, json.data(), json.size() + 1);

        values.push_back(json_cstr);
        used_mem += json.size();
        json = "{";

        if (used_mem >= c.max_batch_size) {
            upsert_docs(c, values);
            values.clear();
        }
    }
    if (values.size() != 0)
        upsert_docs(c, values);
}

void import_ndjson_docs(ukv_docs_import_t& c) {

    auto handle = open(c.paths_pattern, O_RDONLY);
    return_error_if_m(handle != -1, c.error, 0, "Can't open file");

    ukv_size_t file_size = std::filesystem::file_size(std::filesystem::path(c.paths_pattern));
    auto begin = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, handle, 0);
    std::string_view mapped_content = std::string_view(reinterpret_cast<ukv_char_t const*>(begin), file_size);
    madvise(begin, file_size, MADV_SEQUENTIAL);

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document_stream docs = parser.iterate_many( //
        mapped_content.data(),
        mapped_content.size(),
        1000000ul);

    if (!c.fields)
        import_whole_ndjson(c, docs);
    else
        import_sub_ndjson(c, docs);

    munmap((void*)mapped_content.data(), mapped_content.size());
    close(handle);
}

void export_whole_docs( //
    ukv_error_t* error,
    linked_memory_lock_t arena,
    ptr_range_gt<ukv_key_t const>& keys,
    ptr_range_gt<ukv_key_t>* keys_ptr,
    ptr_range_gt<ukv_char_t*>* docs_ptr,
    parquet::StreamWriter* os_ptr,
    val_t const& values,
    int handle,
    ext_t pcn) {

    ptr_range_gt<ukv_char_t*>& docs_vec = *docs_ptr;
    ptr_range_gt<ukv_key_t>& keys_vec = *keys_ptr;
    parquet::StreamWriter& os = *os_ptr;

    ukv_size_t idx = 0;
    auto iter = keys.begin();

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document_stream docs = parser.iterate_many( //
        values.first,
        values.second,
        1000000ul);

    for (auto doc : docs) {
        simdjson::ondemand::object obj = doc.get_object().value();
        auto value = rewinded(obj).raw_json().value();
        auto json = std::string(value.data(), value.size());
        json.pop_back();

        if (pcn == parquet_k) {
            auto val = *iter;
            os << *iter << json.data();
            os << parquet::EndRow;
        }
        else if (pcn == csv_k) {
            keys_vec[idx] = *iter;
            docs_vec[idx] = arena.alloc<ukv_char_t>(json.size() + 1, error).begin();
            std::memcpy(docs_vec[idx], json.data(), json.size() + 1);
            ++idx;
        }
        else {
            auto str = fmt::format("{{\"_id\":{},\"doc\":{}}}\n", *iter, json.data());
            write(handle, str.data(), str.size());
        }
        ++iter;
    }
}

void export_sub_docs( //
    ukv_docs_export_t& c,
    linked_memory_lock_t arena,
    parquet::StreamWriter* os_ptr,
    ptr_range_gt<ukv_char_t*>* docs_ptr,
    ptr_range_gt<ukv_key_t>* keys_ptr,
    ptr_range_gt<ukv_key_t const>& keys,
    ptr_range_gt<ukv_char_t> const& tape,
    fields_t const& fields,
    counts_t const& counts,
    val_t const& values,
    int handle,
    ext_t pcn) {

    ptr_range_gt<ukv_char_t*>& docs_vec = *docs_ptr;
    ptr_range_gt<ukv_key_t>& keys_vec = *keys_ptr;
    parquet::StreamWriter& os = *os_ptr;

    ukv_size_t idx = 0;
    auto iter = keys.begin();
    std::string json = prefix_k;

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document_stream docs = parser.iterate_many( //
        values.first,
        values.second,
        1000000ul);

    for (auto doc : docs) {
        simdjson::ondemand::object obj = doc.get_object().value();
        try {
            simdjson_object_parser(obj, counts, fields, c.fields_count, tape, json);
        }
        catch (simdjson::simdjson_error const& ex) {
            *c.error = ex.what();
            return_if_error_m(c.error);
        }
        if (pcn == parquet_k) {
            os << *iter << json.data();
            os << parquet::EndRow;
        }
        else if (pcn == csv_k) {
            keys_vec[idx] = *iter;
            docs_vec[idx] = arena.alloc<ukv_char_t>(json.size() + 1, c.error).begin();
            std::memcpy(docs_vec[idx], json.data(), json.size() + 1);
            ++idx;
        }
        else {
            auto dat = *iter;
            auto str = fmt::format("{{\"_id\":{},\"doc\":{}}}\n", *iter, json.data());
            write(handle, str.data(), str.size());
        }
        json = prefix_k;
        ++iter;
    }
}

void make_parquet(ukv_docs_export_t& c, parquet::StreamWriter& os) {

    parquet::schema::NodeVector nodes;
    nodes.push_back(parquet::schema::PrimitiveNode::Make( //
        "_id",
        parquet::Repetition::REQUIRED,
        parquet::Type::INT64,
        parquet::ConvertedType::INT_64));

    nodes.push_back(parquet::schema::PrimitiveNode::Make( //
        "doc",
        parquet::Repetition::REQUIRED,
        parquet::Type::BYTE_ARRAY,
        parquet::ConvertedType::UTF8));

    std::shared_ptr<parquet::schema::GroupNode> schema = std::static_pointer_cast<parquet::schema::GroupNode>(
        parquet::schema::GroupNode::Make("schema", parquet::Repetition::REQUIRED, nodes));

    auto maybe_outfile =
        arrow::io::FileOutputStream::Open(fmt::format("{}{}", generate_file_name(), c.paths_extension));
    return_error_if_m(maybe_outfile.ok(), c.error, 0, "Can't open file");
    auto outfile = *maybe_outfile;

    parquet::WriterProperties::Builder builder;
    builder.memory_pool(arrow::default_memory_pool());
    builder.write_batch_size(c.max_batch_size);

    os = parquet::StreamWriter {parquet::ParquetFileWriter::Open(outfile, schema, builder.build())};
}

int make_ndjson(ukv_docs_export_t& docs) {
    return open(fmt::format("{}{}", generate_file_name(), docs.paths_extension).data(),
                O_CREAT | O_WRONLY,
                S_IRUSR | S_IWUSR);
}

void write_in_parquet( //
    ukv_docs_export_t& c,
    linked_memory_lock_t& arena,
    parquet::StreamWriter& ostream,
    ptr_range_gt<ukv_key_t const>& keys,
    ptr_range_gt<ukv_char_t> const& tape,
    fields_t const& fields,
    counts_t const& counts,
    val_t const& values) {

    if (c.fields)
        export_sub_docs(c, arena, &ostream, nullptr, nullptr, keys, tape, fields, counts, values, 0, parquet_k);
    else
        export_whole_docs(c.error, arena, keys, nullptr, nullptr, &ostream, values, 0, parquet_k);
}

void write_in_csv( //
    ukv_docs_export_t& c,
    linked_memory_lock_t& arena,
    ptr_range_gt<ukv_char_t*>& docs_vec,
    ptr_range_gt<ukv_key_t>& keys_vec,
    ptr_range_gt<ukv_key_t const>& keys,
    int_builder_t& int_builder,
    arrow::StringBuilder& string_builder,
    ptr_range_gt<ukv_char_t> const& tape,
    fields_t const& fields,
    counts_t const& counts,
    val_t const& values,
    ukv_size_t size) {

    arrow::Status status;
    status = int_builder.Resize(int_builder.capacity() + size);
    return_error_if_m(status.ok(), c.error, 0, "Can't resize builder");
    status = string_builder.Resize(string_builder.capacity() + size);
    return_error_if_m(status.ok(), c.error, 0, "Can't resize builder");

    if (c.fields)
        export_sub_docs(c, arena, nullptr, &docs_vec, &keys_vec, keys, tape, fields, counts, values, 0, csv_k);
    else
        export_whole_docs(c.error, arena, keys, &keys_vec, &docs_vec, nullptr, values, 0, csv_k);

    status = int_builder.AppendValues(keys_vec.begin(), size);
    return_error_if_m(status.ok(), c.error, 0, "Can't append keys");
    status = string_builder.AppendValues((ukv_char_t const**)docs_vec.begin(), size);
    return_error_if_m(status.ok(), c.error, 0, "Can't append docs");
}

void end_csv( //
    ukv_docs_export_t& c,
    arrow::StringBuilder& string_builder,
    int_builder_t& int_builder) {

    array_t keys_array;
    array_t docs_array;
    arrow::Status status;

    status = int_builder.Finish(&keys_array);
    return_error_if_m(status.ok(), c.error, 0, "Can't finish array(keys)");
    status = string_builder.Finish(&docs_array);
    return_error_if_m(status.ok(), c.error, 0, "Can't finish array(docs)");

    arrow::FieldVector fields;
    fields.push_back(arrow::field("_id", arrow::int64()));
    fields.push_back(arrow::field("doc", arrow::large_binary()));
    std::shared_ptr<arrow::Schema> schema = std::make_shared<arrow::Schema>(fields);

    std::shared_ptr<arrow::Table> table;
    table = arrow::Table::Make(schema, {keys_array, docs_array});

    auto maybe_outstream =
        arrow::io::FileOutputStream::Open(fmt::format("{}{}", generate_file_name(), c.paths_extension));
    return_error_if_m(maybe_outstream.ok(), c.error, 0, "Can't open file");
    std::shared_ptr<arrow::io::FileOutputStream> outstream = *maybe_outstream;

    status = arrow::csv::WriteCSV(*table, arrow::csv::WriteOptions::Defaults(), outstream.get());
    return_error_if_m(status.ok(), c.error, 0, "Can't write in file");
}

void end_ndjson(int fd) {
    close(fd);
}

void write_in_ndjson( //
    ukv_docs_export_t& c,
    linked_memory_lock_t& arena,
    ptr_range_gt<ukv_key_t const>& keys,
    ptr_range_gt<ukv_char_t> const& tape,
    fields_t const& fields,
    counts_t const& counts,
    val_t const& values,
    int handle) {

    if (c.fields)
        export_sub_docs(c, arena, nullptr, nullptr, nullptr, keys, tape, fields, counts, values, handle, ndjson_k);
    else
        export_whole_docs(c.error, arena, keys, nullptr, nullptr, nullptr, values, handle, ndjson_k);
}

#pragma region - Main Functions(Docs)

void ukv_docs_import(ukv_docs_import_t* c_ptr) noexcept(false) {

    ukv_docs_import_t& c = *c_ptr;

    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    validate_docs_fields(c);
    return_if_error_m(c.error);
    return_error_if_m(c.id_field, c.error, uninitialized_state_k, "id_field must be initialized");
    check_for_id_field(c);
    return_if_error_m(c.error);
    return_error_if_m(c.max_batch_size, c.error, uninitialized_state_k, "Max batch size is 0");
    return_error_if_m(c.paths_pattern, c.error, uninitialized_state_k, "Paths pattern is uninitialized");

    if (!c.arena)
        c.arena = arena_t(c.db).member_ptr();

    auto ext = std::filesystem::path(c.paths_pattern).extension();
    if (ext == ".ndjson")
        import_ndjson_docs(c);
    else {
        std::shared_ptr<arrow::Table> table;
        if (ext == ".parquet")
            import_parquet(c, table);
        else if (ext == ".csv")
            import_csv(c, table);
        return_if_error_m(c.error);
        parse_arrow_table(c, table);
    }
}

void ukv_docs_export(ukv_docs_export_t* c_ptr) noexcept(false) {

    ukv_docs_export_t& c = *c_ptr;

    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    validate_docs_fields(c);
    return_if_error_m(c.error);
    return_error_if_m(c.paths_extension, c.error, uninitialized_state_k, "Paths extension is uninitialized");
    return_error_if_m(c.max_batch_size, c.error, uninitialized_state_k, "Max batch size is 0");

    auto ext = c.paths_extension;
    ext_t pcn = strcmp_(ext, ".parquet")  ? parquet_k
                : strcmp_(ext, ".csv")    ? csv_k
                : strcmp_(ext, ".ndjson") ? ndjson_k
                                          : unknown_k;
    return_error_if_m(!(pcn == unknown_k), c.error, 0, "Not supported format");

    if (!c.arena)
        c.arena = arena_t(c.db).member_ptr();

    int handle = 0;
    parquet::StreamWriter os;
    ptr_range_gt<ukv_char_t*> docs_vec;
    ptr_range_gt<ukv_key_t> keys_vec;
    arrow::StringBuilder string_builder;
    int_builder_t int_builder;

    ukv_size_t task_count = 1'000'000;
    keys_stream_t stream(c.db, c.collection, task_count);
    auto arena = linked_memory(c.arena, c.options, c.error);

    fields_t fields;
    ptr_range_gt<ukv_char_t> tape;
    auto counts = arena.alloc<ukv_size_t>(c.fields_count, c.error);

    if (pcn == parquet_k)
        make_parquet(c, os);
    else if (pcn == csv_k) {
        keys_vec = arena.alloc<ukv_key_t>(task_count, c.error);
        docs_vec = arena.alloc<ukv_char_t*>(task_count, c.error);
    }
    else
        handle = make_ndjson(c);

    if (c.fields) {
        fields = prepare_fields(c, arena);
        ukv_size_t max_size = c.fields_count * symbols_count_k;
        for (ukv_size_t idx = 0; idx < c.fields_count; ++idx)
            max_size += strlen(fields[idx]);

        tape = arena.alloc<ukv_char_t>(max_size, c.error);
        fields_parser(c.error, arena, c.fields_count, fields, counts, tape);
    }

    auto status = stream.seek_to_first();
    return_error_if_m(status, c.error, 0, "No batches in stream");

    while (!stream.is_end()) {
        val_t values {nullptr, 0};
        ukv_length_t* offsets = nullptr;
        ukv_length_t* lengths = nullptr;
        auto keys = stream.keys_batch();

        ukv_docs_read_t docs_read {
            .db = c.db,
            .error = c.error,
            .arena = c.arena,
            .options = ukv_option_dont_discard_memory_k,
            .tasks_count = keys.size(),
            .collections = &c.collection,
            .keys = keys.begin(),
            .keys_stride = sizeof(ukv_key_t),
            .lengths = &lengths,
        };
        ukv_docs_read(&docs_read);

        for (ukv_size_t idx = 0; idx < keys.size(); ++idx) {
            ukv_size_t pre_idx = idx;
            ukv_size_t size = 0;

            do {
                size += lengths[idx];
                ++idx;
            } while (size < c.max_batch_size && idx < keys.size());

            ukv_docs_read_t docs_read {
                .db = c.db,
                .error = c.error,
                .arena = c.arena,
                .options = ukv_option_dont_discard_memory_k,
                .tasks_count = idx - pre_idx,
                .collections = &c.collection,
                .keys = keys.begin() + pre_idx,
                .keys_stride = sizeof(ukv_key_t),
                .offsets = &offsets,
                .values = &values.first,
            };
            ukv_docs_read(&docs_read);
            values.second = offsets[idx - pre_idx - 1] + lengths[idx - 1];

            if (pcn == parquet_k)
                write_in_parquet(c, arena, os, keys, tape, fields, counts, values);
            else if (pcn == csv_k)
                write_in_csv(c,
                             arena,
                             docs_vec,
                             keys_vec,
                             keys,
                             int_builder,
                             string_builder,
                             tape,
                             fields,
                             counts,
                             values,
                             idx - pre_idx);
            else
                write_in_ndjson(c, arena, keys, tape, fields, counts, values, handle);
        }
        status = stream.seek_to_next_batch();
        return_error_if_m(status, c.error, 0, "Invalid batch");
    }

    if (pcn == csv_k)
        end_csv(c, string_builder, int_builder);
    else if (pcn == ndjson_k)
        end_ndjson(handle);
}

#pragma endregion - Main Functions(Docs)
#pragma endregion - Docs