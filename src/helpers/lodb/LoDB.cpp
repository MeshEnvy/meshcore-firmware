#include <lodb/LoDB.h>

#include <memory>
#include <Arduino.h>
#include <SHA256.h>
#include <algorithm>
#include <cstring>
#include <pb_decode.h>
#include <pb_encode.h>

namespace {

constexpr char kB64url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

void lodb_uuid_to_fn11(lodb_uuid_t u, char out[12])
{
    for (int i = 10; i >= 0; --i) {
        out[i] = kB64url[u & 63];
        u >>= 6;
    }
    out[11] = '\0';
}

bool lodb_fn11_to_uuid(const char s[11], lodb_uuid_t *out)
{
    lodb_uuid_t v = 0;
    for (int i = 0; i < 11; ++i) {
        char c = s[i];
        uint32_t x = 256;
        if (c >= 'A' && c <= 'Z')
            x = (uint32_t)(c - 'A');
        else if (c >= 'a' && c <= 'z')
            x = (uint32_t)(c - 'a' + 26);
        else if (c >= '0' && c <= '9')
            x = (uint32_t)(c - '0' + 52);
        else if (c == '-')
            x = 62;
        else if (c == '_')
            x = 63;
        if (x >= 64) return false;
        v = (v << 6) | (lodb_uuid_t)x;
    }
    *out = v;
    return true;
}

void fill_ns_hex(const std::string &db, const char *table, char ns_out[7])
{
    uint32_t h = 2166136261u;
    for (unsigned char c : db) {
        h ^= c;
        h *= 16777619u;
    }
    h ^= ':';
    h *= 16777619u;
    for (const char *p = table; *p; ++p) {
        h ^= (unsigned char)*p;
        h *= 16777619u;
    }
    snprintf(ns_out, 7, "%06x", (unsigned)(h & 0xffffffu));
}

void record_file_path(const char *table_path, const char *ns_hex, lodb_uuid_t uuid, char *path, size_t cap)
{
    char fn11[12];
    lodb_uuid_to_fn11(uuid, fn11);
    snprintf(path, cap, "%s/%s%s.pr", table_path, ns_hex, fn11);
}

bool parse_record_filename(const std::string &filename, const char *ns_hex, lodb_uuid_t *uuid_out)
{
    if (filename.size() != 20) return false;
    if (strncmp(filename.c_str(), ns_hex, 6) != 0) return false;
    if (filename.compare(17, 3, ".pr") != 0) return false;
    return lodb_fn11_to_uuid(filename.c_str() + 6, uuid_out);
}

bool record_filename_belongs_table(const std::string &filename, const char *ns_hex)
{
    if (filename.size() != 20) return false;
    if (strncmp(filename.c_str(), ns_hex, 6) != 0) return false;
    return filename.compare(17, 3, ".pr") == 0;
}

}  // namespace

__attribute__((weak)) uint32_t lodb_now_ms(void)
{
    return static_cast<uint32_t>(millis());
}

void lodb_uuid_to_hex(lodb_uuid_t uuid, char hex_out[17])
{
    snprintf(hex_out, 17, LODB_UUID_FMT, LODB_UUID_ARGS(uuid));
}

lodb_uuid_t lodb_new_uuid(const char *str, uint64_t salt)
{
    char generated_str[32];
    const char *input_str = str;

    if (str == nullptr) {
        uint32_t timestamp = lodb_now_ms();
        uint32_t random_val = (uint32_t)random(0x7fffffff) ^ ((uint32_t)random(0x7fffffff) << 1);
        snprintf(generated_str, sizeof(generated_str), "%u:%u", timestamp, random_val);
        input_str = generated_str;
    }

    SHA256 sha256;
    uint8_t hash[32];

    sha256.reset();
    sha256.update(input_str, strlen(input_str));

    uint8_t salt_bytes[8];
    memcpy(salt_bytes, &salt, 8);
    sha256.update(salt_bytes, 8);

    sha256.finalize(hash, 32);

    lodb_uuid_t uuid;
    memcpy(&uuid, hash, sizeof(lodb_uuid_t));
    return uuid;
}

LoDb::LoDb(const char *db_name, const char *mount) : db_name(db_name)
{
    const char *m = mount && mount[0] ? mount : "/__ext__";
    int n = snprintf(db_path, sizeof(db_path), "%s/l", m);
    if (n <= 0 || (size_t)n >= sizeof(db_path)) {
        db_path[0] = '\0';
        LODB_LOG_ERROR("LoDB: db_path overflow");
        return;
    }

    if (!LoFS::mkdir(m)) {
        LODB_LOG_DEBUG("LoDB: mount dir may exist: %s", m);
    }

    if (!LoFS::mkdir(db_path)) {
        LODB_LOG_DEBUG("LoDB: records dir may already exist: %s", db_path);
    }

    LODB_LOG_INFO("Initialized LoDB database: %s", db_path);
}

LoDb::~LoDb() {}

LoDbError LoDb::registerTable(const char *table_name, const pb_msgdesc_t *pb_descriptor, size_t record_size)
{
    if (!table_name || !pb_descriptor || record_size == 0) {
        return LODB_ERR_INVALID;
    }

    TableMetadata metadata;
    metadata.table_name = table_name;
    metadata.pb_descriptor = pb_descriptor;
    metadata.record_size = record_size;
    fill_ns_hex(db_name, table_name, metadata.ns_hex);

    snprintf(metadata.table_path, sizeof(metadata.table_path), "%s", db_path);

    tables[table_name] = metadata;
    LODB_LOG_INFO("Registered table: %s at %s", table_name, metadata.table_path);
    return LODB_OK;
}

LoDb::TableMetadata *LoDb::getTable(const char *table_name)
{
    auto it = tables.find(table_name);
    if (it == tables.end()) {
        LODB_LOG_ERROR("Table not registered: %s", table_name);
        return nullptr;
    }
    return &it->second;
}

LoDbError LoDb::insert(const char *table_name, lodb_uuid_t uuid, const void *record)
{
    if (!table_name || !record) {
        return LODB_ERR_INVALID;
    }

    TableMetadata *table = getTable(table_name);
    if (!table) {
        return LODB_ERR_INVALID;
    }

    char file_path[192];
    record_file_path(table->table_path, table->ns_hex, uuid, file_path, sizeof(file_path));

    {
        auto existing = LoFS::open(file_path, FILE_O_READ);
        if (existing) {
            existing.close();
            return LODB_ERR_INVALID;
        }
    }

    constexpr size_t kBufSize = 2048;
    std::unique_ptr<uint8_t[]> buffer(new uint8_t[kBufSize]);
    pb_ostream_t stream = pb_ostream_from_buffer(buffer.get(), kBufSize);

    if (!pb_encode(&stream, table->pb_descriptor, record)) {
        LODB_LOG_ERROR("Failed to encode protobuf for insert");
        return LODB_ERR_ENCODE;
    }

    size_t encoded_size = stream.bytes_written;

    auto file = LoFS::open(file_path, FILE_O_WRITE);
    if (!file) {
        LODB_LOG_ERROR("Failed to open file for writing: %s", file_path);
        return LODB_ERR_IO;
    }

    size_t written = file.write(buffer.get(), encoded_size);
    if (written != encoded_size) {
        LODB_LOG_ERROR("Failed to write file, wrote %u of %u bytes", (unsigned)written, (unsigned)encoded_size);
        file.close();
        return LODB_ERR_IO;
    }

    file.flush();
    file.close();
    return LODB_OK;
}

LoDbError LoDb::get(const char *table_name, lodb_uuid_t uuid, void *record_out)
{
    if (!table_name || !record_out) {
        return LODB_ERR_INVALID;
    }

    TableMetadata *table = getTable(table_name);
    if (!table) {
        return LODB_ERR_INVALID;
    }

    char file_path[192];
    record_file_path(table->table_path, table->ns_hex, uuid, file_path, sizeof(file_path));
    LODB_LOG_DEBUG("file_path: %s", file_path);

    constexpr size_t kBufSize = 2048;
    std::unique_ptr<uint8_t[]> buffer(new uint8_t[kBufSize]);
    size_t file_size = 0;

    auto file = LoFS::open(file_path, FILE_O_READ);
    if (!file) {
        return LODB_ERR_NOT_FOUND;
    }

    file_size = file.read(buffer.get(), kBufSize);
    file.close();

    if (file_size == 0) {
        LODB_LOG_ERROR("Record file is empty: " LODB_UUID_FMT, LODB_UUID_ARGS(uuid));
        return LODB_ERR_IO;
    }

    pb_istream_t stream = pb_istream_from_buffer(buffer.get(), file_size);
    memset(record_out, 0, table->record_size);

    if (!pb_decode(&stream, table->pb_descriptor, record_out)) {
        LODB_LOG_ERROR("Failed to decode protobuf from " LODB_UUID_FMT, LODB_UUID_ARGS(uuid));
        return LODB_ERR_DECODE;
    }

    return LODB_OK;
}

LoDbError LoDb::update(const char *table_name, lodb_uuid_t uuid, const void *record)
{
    if (!table_name || !record) {
        return LODB_ERR_INVALID;
    }

    TableMetadata *table = getTable(table_name);
    if (!table) {
        return LODB_ERR_INVALID;
    }

    char file_path[192];
    record_file_path(table->table_path, table->ns_hex, uuid, file_path, sizeof(file_path));

    {
        auto file = LoFS::open(file_path, FILE_O_READ);
        if (!file) {
            return LODB_ERR_NOT_FOUND;
        }
        file.close();
    }

    constexpr size_t kBufSize = 2048;
    std::unique_ptr<uint8_t[]> buffer(new uint8_t[kBufSize]);
    pb_ostream_t stream = pb_ostream_from_buffer(buffer.get(), kBufSize);

    if (!pb_encode(&stream, table->pb_descriptor, record)) {
        LODB_LOG_ERROR("Failed to encode updated record: " LODB_UUID_FMT, LODB_UUID_ARGS(uuid));
        return LODB_ERR_ENCODE;
    }

    size_t encoded_size = stream.bytes_written;

    LoFS::remove(file_path);
    auto file = LoFS::open(file_path, FILE_O_WRITE);
    if (!file) {
        LODB_LOG_ERROR("Failed to open file for update: %s", file_path);
        return LODB_ERR_IO;
    }

    size_t written = file.write(buffer.get(), encoded_size);
    if (written != encoded_size) {
        LODB_LOG_ERROR("Failed to write updated file");
        file.close();
        return LODB_ERR_IO;
    }

    file.flush();
    file.close();

    LODB_LOG_INFO("Updated record: " LODB_UUID_FMT, LODB_UUID_ARGS(uuid));
    return LODB_OK;
}

LoDbError LoDb::deleteRecord(const char *table_name, lodb_uuid_t uuid)
{
    if (!table_name) {
        return LODB_ERR_INVALID;
    }

    TableMetadata *table = getTable(table_name);
    if (!table) {
        return LODB_ERR_INVALID;
    }

    char file_path[192];
    record_file_path(table->table_path, table->ns_hex, uuid, file_path, sizeof(file_path));

    if (LoFS::remove(file_path)) {
        LODB_LOG_DEBUG("Deleted record: " LODB_UUID_FMT, LODB_UUID_ARGS(uuid));
        return LODB_OK;
    }
    LODB_LOG_WARN("Failed to delete record (may not exist): " LODB_UUID_FMT, LODB_UUID_ARGS(uuid));
    return LODB_ERR_NOT_FOUND;
}

std::vector<void *> LoDb::select(const char *table_name, LoDbFilter filter, LoDbComparator comparator, size_t limit)
{
    std::vector<void *> results;

    if (!table_name) {
        LODB_LOG_ERROR("Invalid table_name");
        return results;
    }

    TableMetadata *table = getTable(table_name);
    if (!table) {
        LODB_LOG_ERROR("Table not found: %s", table_name);
        return results;
    }

    File dir = LoFS::open(table->table_path, FILE_O_READ);
    if (!dir) {
        LODB_LOG_DEBUG("Table directory not found: %s", table->table_path);
        return results;
    }

    if (!dir.isDirectory()) {
        LODB_LOG_ERROR("Table path is not a directory: %s", table->table_path);
        dir.close();
        return results;
    }

    while (true) {
        File file = dir.openNextFile();
        if (!file) {
            break;
        }

        if (file.isDirectory()) {
            file.close();
            continue;
        }

        std::string pathStr = file.name();
        file.close();

        size_t lastSlash = pathStr.rfind('/');
        std::string filename = (lastSlash != std::string::npos) ? pathStr.substr(lastSlash + 1) : pathStr;

        lodb_uuid_t uuid;
        if (!parse_record_filename(filename, table->ns_hex, &uuid)) {
            LODB_LOG_DEBUG("Skipped non-record file: %s", filename.c_str());
            continue;
        }

        uint8_t *record_buffer = new uint8_t[table->record_size];
        if (!record_buffer) {
            LODB_LOG_ERROR("Failed to allocate record buffer");
            continue;
        }

        memset(record_buffer, 0, table->record_size);
        LoDbError err = get(table_name, uuid, record_buffer);

        if (err != LODB_OK) {
            LODB_LOG_WARN("Failed to read record " LODB_UUID_FMT " during select", LODB_UUID_ARGS(uuid));
            delete[] record_buffer;
            continue;
        }

        if (filter && !filter(record_buffer)) {
            LODB_LOG_DEBUG("Record " LODB_UUID_FMT " filtered out", LODB_UUID_ARGS(uuid));
            delete[] record_buffer;
            continue;
        }

        results.push_back(record_buffer);
        LODB_LOG_DEBUG("Added record " LODB_UUID_FMT " to results", LODB_UUID_ARGS(uuid));
    }

    dir.close();

    LODB_LOG_INFO("Select from %s: %u records after filtering", table_name, (unsigned)results.size());

    if (comparator && !results.empty()) {
        std::sort(results.begin(), results.end(),
                  [comparator](const void *a, const void *b) { return comparator(a, b) < 0; });
        LODB_LOG_DEBUG("Sorted %u records", (unsigned)results.size());
    }

    if (limit > 0 && results.size() > limit) {
        for (size_t i = limit; i < results.size(); i++) {
            delete[] (uint8_t *)results[i];
        }
        results.resize(limit);
        LODB_LOG_DEBUG("Limited results to %u records", (unsigned)limit);
    }

    LODB_LOG_INFO("Select from %s complete: %u records returned", table_name, (unsigned)results.size());
    return results;
}

void LoDb::freeRecords(std::vector<void *> &records)
{
    for (auto *recordPtr : records) {
        delete[] (uint8_t *)recordPtr;
    }
    records.clear();
}

int LoDb::count(const char *table_name, LoDbFilter filter)
{
    if (!table_name) {
        LODB_LOG_ERROR("Invalid table_name");
        return -1;
    }

    TableMetadata *table = getTable(table_name);
    if (!table) {
        LODB_LOG_ERROR("Table not found: %s", table_name);
        return -1;
    }

    int cnt = 0;

    if (!filter) {
        File dir = LoFS::open(table->table_path, FILE_O_READ);
        if (!dir) {
            LODB_LOG_DEBUG("Table directory not found: %s", table->table_path);
            return 0;
        }

        if (!dir.isDirectory()) {
            LODB_LOG_ERROR("Table path is not a directory: %s", table->table_path);
            dir.close();
            return -1;
        }

        while (true) {
            File file = dir.openNextFile();
            if (!file) {
                break;
            }

            if (file.isDirectory()) {
                file.close();
                continue;
            }

            std::string pathStr = file.name();
            file.close();

            size_t lastSlash = pathStr.rfind('/');
            std::string filename = (lastSlash != std::string::npos) ? pathStr.substr(lastSlash + 1) : pathStr;

            if (record_filename_belongs_table(filename, table->ns_hex)) {
                cnt++;
            }
        }

        dir.close();
        LODB_LOG_DEBUG("Counted %d records in %s (no filter)", cnt, table_name);
        return cnt;
    }

    auto results = select(table_name, filter, LoDbComparator(), 0);
    cnt = (int)results.size();
    freeRecords(results);

    LODB_LOG_DEBUG("Counted %d records in %s (with filter)", cnt, table_name);
    return cnt;
}

LoDbError LoDb::truncate(const char *table_name)
{
    if (!table_name) {
        LODB_LOG_ERROR("Invalid table_name");
        return LODB_ERR_INVALID;
    }

    TableMetadata *table = getTable(table_name);
    if (!table) {
        LODB_LOG_ERROR("Table not registered: %s", table_name);
        return LODB_ERR_INVALID;
    }

    File dir = LoFS::open(table->table_path, FILE_O_READ);
    if (!dir) {
        LODB_LOG_DEBUG("Table directory not found: %s (already empty)", table->table_path);
        return LODB_OK;
    }

    if (!dir.isDirectory()) {
        LODB_LOG_ERROR("Table path is not a directory: %s", table->table_path);
        dir.close();
        return LODB_ERR_INVALID;
    }

    int deletedCount = 0;
    while (true) {
        File file = dir.openNextFile();
        if (!file) {
            break;
        }

        if (file.isDirectory()) {
            file.close();
            continue;
        }

        std::string pathStr = file.name();
        file.close();

        size_t lastSlash = pathStr.rfind('/');
        std::string filename = (lastSlash != std::string::npos) ? pathStr.substr(lastSlash + 1) : pathStr;

        if (!record_filename_belongs_table(filename, table->ns_hex)) {
            continue;
        }

        char file_path[192];
        snprintf(file_path, sizeof(file_path), "%s/%s", table->table_path, filename.c_str());

        if (LoFS::remove(file_path)) {
            deletedCount++;
        } else {
            LODB_LOG_WARN("Failed to delete file during truncate: %s", file_path);
        }
    }

    dir.close();

    LODB_LOG_INFO("Truncated table %s: deleted %d records", table_name, deletedCount);
    return LODB_OK;
}

LoDbError LoDb::drop(const char *table_name)
{
    if (!table_name) {
        LODB_LOG_ERROR("Invalid table_name");
        return LODB_ERR_INVALID;
    }

    TableMetadata *table = getTable(table_name);
    if (!table) {
        LODB_LOG_ERROR("Table not registered: %s", table_name);
        return LODB_ERR_INVALID;
    }

    LoDbError err = truncate(table_name);
    if (err != LODB_OK) {
        LODB_LOG_WARN("Failed to truncate table before drop: %s", table_name);
    }

    tables.erase(table_name);

    LODB_LOG_INFO("Dropped table: %s", table_name);
    return LODB_OK;
}
