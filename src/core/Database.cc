#include "core/Database.h"

#include <algorithm>
#include <filesystem>
#include <sqlite3.h>

#include "core/Logger.h"

namespace pm {
namespace {

namespace fs = std::filesystem;

const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS products (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    platform           TEXT    NOT NULL,
    product_id         TEXT    NOT NULL,
    url                TEXT    NOT NULL,
    title              TEXT    NOT NULL DEFAULT '',
    shop_name          TEXT    NOT NULL DEFAULT '',
    image_url          TEXT    NOT NULL DEFAULT '',
    target_price       REAL,
    note               TEXT    NOT NULL DEFAULT '',
    enabled            INTEGER NOT NULL DEFAULT 1,
    created_at         INTEGER NOT NULL,
    updated_at         INTEGER NOT NULL,
    last_checked_at    INTEGER,
    last_auto_checked_at INTEGER,
    interval_minutes   INTEGER NOT NULL DEFAULT 0,
    last_error         TEXT    NOT NULL DEFAULT '',
    last_notified_price REAL,
    UNIQUE(platform, product_id)
);

CREATE TABLE IF NOT EXISTS price_records (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    product_id     INTEGER NOT NULL,
    price          REAL    NOT NULL,
    original_price REAL,
    in_stock       INTEGER NOT NULL DEFAULT 1,
    captured_at    INTEGER NOT NULL,
    raw            TEXT    NOT NULL DEFAULT '',
    FOREIGN KEY(product_id) REFERENCES products(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_price_product_time
    ON price_records(product_id, captured_at DESC);

CREATE INDEX IF NOT EXISTS idx_products_enabled
    ON products(enabled, last_checked_at);

CREATE TABLE IF NOT EXISTS crawl_logs (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    product_id INTEGER,
    ok         INTEGER NOT NULL,
    message    TEXT    NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL
);
)SQL";

const char* kProductSelect = R"SQL(
SELECT p.id, p.platform, p.product_id, p.url, p.title, p.shop_name, p.image_url,
       p.target_price, p.note, p.enabled, p.created_at, p.updated_at,
       p.last_checked_at, p.last_error,
       (SELECT r.price FROM price_records r WHERE r.product_id = p.id
            ORDER BY r.captured_at DESC, r.id DESC LIMIT 1),
       (SELECT r.price FROM price_records r WHERE r.product_id = p.id
            ORDER BY r.captured_at DESC, r.id DESC LIMIT 1 OFFSET 1),
       (SELECT r.in_stock FROM price_records r WHERE r.product_id = p.id
            ORDER BY r.captured_at DESC, r.id DESC LIMIT 1),
       (SELECT r.captured_at FROM price_records r WHERE r.product_id = p.id
            ORDER BY r.captured_at DESC, r.id DESC LIMIT 1),
       p.interval_minutes, p.last_auto_checked_at
FROM products p
)SQL";

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            stmt_ = nullptr;
        }
    }
    ~Stmt() {
        if (stmt_) sqlite3_finalize(stmt_);
    }

    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    bool valid() const { return stmt_ != nullptr; }

    void bindText(int index, const std::string& value) {
        sqlite3_bind_text(stmt_, index, value.c_str(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT);
    }
    void bindInt64(int index, long long value) {
        sqlite3_bind_int64(stmt_, index, value);
    }
    void bindInt(int index, int value) { sqlite3_bind_int(stmt_, index, value); }
    void bindDouble(int index, double value) { sqlite3_bind_double(stmt_, index, value); }
    void bindNull(int index) { sqlite3_bind_null(stmt_, index); }

    int step() { return sqlite3_step(stmt_); }
    sqlite3_stmt* raw() { return stmt_; }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

std::string colText(sqlite3_stmt* st, int index) {
    const unsigned char* text = sqlite3_column_text(st, index);
    if (!text) return "";
    int bytes = sqlite3_column_bytes(st, index);
    return std::string(reinterpret_cast<const char*>(text), static_cast<size_t>(bytes));
}

bool colIsNull(sqlite3_stmt* st, int index) {
    return sqlite3_column_type(st, index) == SQLITE_NULL;
}

Product mapProduct(sqlite3_stmt* st) {
    Product p;
    p.id = sqlite3_column_int64(st, 0);
    p.platform = colText(st, 1);
    p.productId = colText(st, 2);
    p.url = colText(st, 3);
    p.title = colText(st, 4);
    p.shopName = colText(st, 5);
    p.imageUrl = colText(st, 6);
    if (!colIsNull(st, 7)) {
        p.hasTarget = true;
        p.targetPrice = sqlite3_column_double(st, 7);
    }
    p.note = colText(st, 8);
    p.enabled = sqlite3_column_int(st, 9) != 0;
    p.createdAt = sqlite3_column_int64(st, 10);
    p.updatedAt = sqlite3_column_int64(st, 11);
    if (!colIsNull(st, 12)) {
        p.hasLastChecked = true;
        p.lastCheckedAt = sqlite3_column_int64(st, 12);
    }
    p.lastError = colText(st, 13);
    if (!colIsNull(st, 14)) {
        p.hasLastPrice = true;
        p.lastPrice = sqlite3_column_double(st, 14);
    }
    if (!colIsNull(st, 15)) {
        p.hasPrevPrice = true;
        p.prevPrice = sqlite3_column_double(st, 15);
    }
    if (!colIsNull(st, 16)) {
        p.hasLastStock = true;
        p.lastInStock = sqlite3_column_int(st, 16) != 0;
    }
    if (!colIsNull(st, 17)) {
        p.hasLastCapture = true;
        p.lastCaptureAt = sqlite3_column_int64(st, 17);
    }
    p.intervalMinutes = sqlite3_column_int(st, 18);
    if (!colIsNull(st, 19)) p.lastAutoCheckedAt = sqlite3_column_int64(st, 19);
    return p;
}

PriceRecord mapPrice(sqlite3_stmt* st) {
    PriceRecord r;
    r.id = sqlite3_column_int64(st, 0);
    r.productId = sqlite3_column_int64(st, 1);
    r.price = sqlite3_column_double(st, 2);
    if (!colIsNull(st, 3)) {
        r.hasOriginal = true;
        r.originalPrice = sqlite3_column_double(st, 3);
    }
    r.inStock = sqlite3_column_int(st, 4) != 0;
    r.capturedAt = sqlite3_column_int64(st, 5);
    r.raw = colText(st, 6);
    return r;
}

}  // namespace

Database::Database(std::string path) : path_(std::move(path)) {}

Database::~Database() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void Database::setError(const std::string& error) {
    lastError_ = error;
}

std::string Database::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

bool Database::open() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }

    std::error_code ec;
    fs::path p(path_);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
        if (ec) {
            setError("无法创建数据库目录: " + p.parent_path().string() + " (" + ec.message() + ")");
            return false;
        }
    }

    int rc = sqlite3_open_v2(path_.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                 SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        setError(std::string("打开数据库失败: ") + (db_ ? sqlite3_errmsg(db_) : "unknown"));
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }

    sqlite3_busy_timeout(db_, 5000);
    const char* pragmas =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA foreign_keys=ON;";
    char* errMsg = nullptr;
    if (sqlite3_exec(db_, pragmas, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        setError(std::string("设置数据库参数失败: ") + (errMsg ? errMsg : ""));
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }

    if (!initSchema()) return false;

    lastError_.clear();
    return true;
}

bool Database::initSchema() {
    char* errMsg = nullptr;
    if (sqlite3_exec(db_, kSchema, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        setError(std::string("初始化数据表失败: ") + (errMsg ? errMsg : ""));
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }

    // 旧库升级：补齐后加的列。last_auto_checked_at 用 last_checked_at 回填，
    // 避免升级后所有商品立刻被判为到期而集中采集。
    auto hasColumn = [this](const std::string& name) {
        Stmt st(db_, "PRAGMA table_info(products)");
        if (!st.valid()) return false;
        while (st.step() == SQLITE_ROW) {
            if (colText(st.raw(), 1) == name) return true;
        }
        return false;
    };
    auto execSql = [this](const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            setError(std::string("升级数据表失败: ") + (err ? err : ""));
            if (err) sqlite3_free(err);
            return false;
        }
        return true;
    };

    if (!hasColumn("last_auto_checked_at")) {
        if (!execSql("ALTER TABLE products ADD COLUMN last_auto_checked_at INTEGER")) {
            return false;
        }
        if (!execSql("UPDATE products SET last_auto_checked_at = last_checked_at"
                     " WHERE last_checked_at IS NOT NULL")) {
            return false;
        }
    }
    if (!hasColumn("interval_minutes")) {
        if (!execSql("ALTER TABLE products ADD COLUMN interval_minutes INTEGER NOT NULL"
                     " DEFAULT 0")) {
            return false;
        }
    }
    return true;
}

bool Database::insertProduct(Product& product) {
    std::lock_guard<std::mutex> lock(mutex_);

    const char* sql =
        "INSERT INTO products(platform, product_id, url, title, shop_name, image_url,"
        " target_price, note, enabled, created_at, updated_at, last_checked_at,"
        " last_auto_checked_at, interval_minutes, last_error)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    Stmt st(db_, sql);
    if (!st.valid()) {
        setError(std::string("插入商品失败: ") + sqlite3_errmsg(db_));
        return false;
    }

    int i = 1;
    st.bindText(i++, product.platform);
    st.bindText(i++, product.productId);
    st.bindText(i++, product.url);
    st.bindText(i++, product.title);
    st.bindText(i++, product.shopName);
    st.bindText(i++, product.imageUrl);
    if (product.hasTarget)
        st.bindDouble(i++, product.targetPrice);
    else
        st.bindNull(i++);
    st.bindText(i++, product.note);
    st.bindInt(i++, product.enabled ? 1 : 0);
    st.bindInt64(i++, product.createdAt);
    st.bindInt64(i++, product.updatedAt);
    if (product.hasLastChecked)
        st.bindInt64(i++, product.lastCheckedAt);
    else
        st.bindNull(i++);
    if (product.lastAutoCheckedAt > 0)
        st.bindInt64(i++, product.lastAutoCheckedAt);
    else
        st.bindNull(i++);
    st.bindInt(i++, product.intervalMinutes);
    st.bindText(i++, product.lastError);

    if (st.step() != SQLITE_DONE) {
        setError(std::string("插入商品失败: ") + sqlite3_errmsg(db_));
        return false;
    }
    product.id = sqlite3_last_insert_rowid(db_);
    lastError_.clear();
    return true;
}

std::optional<Product> Database::getProduct(long long id) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string sql = std::string(kProductSelect) + " WHERE p.id = ?";
    Stmt st(db_, sql.c_str());
    if (!st.valid()) {
        setError(std::string("查询商品失败: ") + sqlite3_errmsg(db_));
        return std::nullopt;
    }
    st.bindInt64(1, id);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    return mapProduct(st.raw());
}

std::optional<Product> Database::findProduct(const std::string& platform,
                                             const std::string& productId) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string sql = std::string(kProductSelect) +
                      " WHERE p.platform = ? AND p.product_id = ?";
    Stmt st(db_, sql.c_str());
    if (!st.valid()) {
        setError(std::string("查询商品失败: ") + sqlite3_errmsg(db_));
        return std::nullopt;
    }
    st.bindText(1, platform);
    st.bindText(2, productId);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    return mapProduct(st.raw());
}

std::vector<Product> Database::listProducts() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Product> products;
    std::string sql = std::string(kProductSelect) + " ORDER BY p.id DESC";
    Stmt st(db_, sql.c_str());
    if (!st.valid()) {
        setError(std::string("查询商品列表失败: ") + sqlite3_errmsg(db_));
        return products;
    }
    while (st.step() == SQLITE_ROW) {
        products.push_back(mapProduct(st.raw()));
    }
    return products;
}

std::vector<long long> Database::listDueProducts(long long now,
                                                int defaultIntervalMinutes) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<long long> ids;
    // 到期判断只看 last_auto_checked_at（自动采集锚点），手动检索不会推迟自动计划。
    const char* sql =
        "SELECT id FROM products WHERE enabled = 1"
        " AND (COALESCE(last_auto_checked_at, 0) = 0"
        "      OR last_auto_checked_at +"
        "         (CASE WHEN interval_minutes > 0 THEN interval_minutes ELSE ? END) * 60 <= ?)"
        " ORDER BY COALESCE(last_auto_checked_at, 0) ASC LIMIT 200";
    Stmt st(db_, sql);
    if (!st.valid()) {
        setError(std::string("查询待采集商品失败: ") + sqlite3_errmsg(db_));
        return ids;
    }
    st.bindInt(1, defaultIntervalMinutes > 0 ? defaultIntervalMinutes : 60);
    st.bindInt64(2, now);
    while (st.step() == SQLITE_ROW) {
        ids.push_back(sqlite3_column_int64(st.raw(), 0));
    }
    return ids;
}

bool Database::updateProductSettings(long long id, bool hasTarget,
                                     double targetPrice,
                                     const std::string& note, bool enabled,
                                     int intervalMinutes) {
    std::lock_guard<std::mutex> lock(mutex_);

    const char* sql =
        "UPDATE products SET target_price = ?, note = ?, enabled = ?,"
        " interval_minutes = ?, updated_at = ?"
        " WHERE id = ?";
    Stmt st(db_, sql);
    if (!st.valid()) {
        setError(std::string("更新商品失败: ") + sqlite3_errmsg(db_));
        return false;
    }
    if (hasTarget)
        st.bindDouble(1, targetPrice);
    else
        st.bindNull(1);
    st.bindText(2, note);
    st.bindInt(3, enabled ? 1 : 0);
    st.bindInt(4, intervalMinutes > 0 ? intervalMinutes : 0);
    st.bindInt64(5, nowSeconds());
    st.bindInt64(6, id);

    if (st.step() != SQLITE_DONE) {
        setError(std::string("更新商品失败: ") + sqlite3_errmsg(db_));
        return false;
    }
    lastError_.clear();
    return true;
}

bool Database::updateProductMeta(long long id, const std::string& title,
                                 const std::string& shopName,
                                 const std::string& imageUrl) {
    std::lock_guard<std::mutex> lock(mutex_);

    const char* sql =
        "UPDATE products SET"
        " title = CASE WHEN ?1 <> '' THEN ?1 ELSE title END,"
        " shop_name = CASE WHEN ?2 <> '' THEN ?2 ELSE shop_name END,"
        " image_url = CASE WHEN ?3 <> '' THEN ?3 ELSE image_url END,"
        " updated_at = ?4"
        " WHERE id = ?5";
    Stmt st(db_, sql);
    if (!st.valid()) {
        setError(std::string("更新商品信息失败: ") + sqlite3_errmsg(db_));
        return false;
    }
    st.bindText(1, title);
    st.bindText(2, shopName);
    st.bindText(3, imageUrl);
    st.bindInt64(4, nowSeconds());
    st.bindInt64(5, id);

    if (st.step() != SQLITE_DONE) {
        setError(std::string("更新商品信息失败: ") + sqlite3_errmsg(db_));
        return false;
    }
    lastError_.clear();
    return true;
}

bool Database::updateCheckStatus(long long id, long long checkedAt,
                                 const std::string& error, bool autoCrawl) {
    std::lock_guard<std::mutex> lock(mutex_);

    const char* sql = autoCrawl
        ? "UPDATE products SET last_checked_at = ?, last_error = ?, updated_at = ?,"
          " last_auto_checked_at = ? WHERE id = ?"
        : "UPDATE products SET last_checked_at = ?, last_error = ?, updated_at = ?"
          " WHERE id = ?";
    Stmt st(db_, sql);
    if (!st.valid()) {
        setError(std::string("更新采集状态失败: ") + sqlite3_errmsg(db_));
        return false;
    }
    int i = 1;
    st.bindInt64(i++, checkedAt);
    st.bindText(i++, error);
    st.bindInt64(i++, checkedAt);
    if (autoCrawl) st.bindInt64(i++, checkedAt);
    st.bindInt64(i++, id);

    if (st.step() != SQLITE_DONE) {
        setError(std::string("更新采集状态失败: ") + sqlite3_errmsg(db_));
        return false;
    }
    lastError_.clear();
    return true;
}

bool Database::deleteProduct(long long id) {
    std::lock_guard<std::mutex> lock(mutex_);

    Stmt st(db_, "DELETE FROM products WHERE id = ?");
    if (!st.valid()) {
        setError(std::string("删除商品失败: ") + sqlite3_errmsg(db_));
        return false;
    }
    st.bindInt64(1, id);
    if (st.step() != SQLITE_DONE) {
        setError(std::string("删除商品失败: ") + sqlite3_errmsg(db_));
        return false;
    }
    lastError_.clear();
    return true;
}

bool Database::addPriceRecord(const PriceRecord& record, long long* outId) {
    std::lock_guard<std::mutex> lock(mutex_);

    const char* sql =
        "INSERT INTO price_records(product_id, price, original_price, in_stock,"
        " captured_at, raw) VALUES(?,?,?,?,?,?)";
    Stmt st(db_, sql);
    if (!st.valid()) {
        setError(std::string("保存价格记录失败: ") + sqlite3_errmsg(db_));
        return false;
    }
    st.bindInt64(1, record.productId);
    st.bindDouble(2, record.price);
    if (record.hasOriginal)
        st.bindDouble(3, record.originalPrice);
    else
        st.bindNull(3);
    st.bindInt(4, record.inStock ? 1 : 0);
    st.bindInt64(5, record.capturedAt);
    st.bindText(6, record.raw);

    if (st.step() != SQLITE_DONE) {
        setError(std::string("保存价格记录失败: ") + sqlite3_errmsg(db_));
        return false;
    }
    if (outId) *outId = sqlite3_last_insert_rowid(db_);
    lastError_.clear();
    return true;
}

std::optional<PriceRecord> Database::latestPrice(long long productId) {
    std::lock_guard<std::mutex> lock(mutex_);

    const char* sql =
        "SELECT id, product_id, price, original_price, in_stock, captured_at, raw"
        " FROM price_records WHERE product_id = ?"
        " ORDER BY captured_at DESC, id DESC LIMIT 1";
    Stmt st(db_, sql);
    if (!st.valid()) return std::nullopt;
    st.bindInt64(1, productId);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    return mapPrice(st.raw());
}

std::vector<PriceRecord> Database::priceHistory(long long productId, long long from,
                                                long long to, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<PriceRecord> records;
    if (limit <= 0) limit = 1000;
    if (limit > 5000) limit = 5000;

    std::string sql =
        "SELECT id, product_id, price, original_price, in_stock, captured_at, raw"
        " FROM price_records WHERE product_id = ?";
    if (from > 0) sql += " AND captured_at >= ?";
    if (to > 0) sql += " AND captured_at <= ?";
    sql += " ORDER BY captured_at DESC, id DESC LIMIT ?";

    Stmt st(db_, sql.c_str());
    if (!st.valid()) {
        setError(std::string("查询价格历史失败: ") + sqlite3_errmsg(db_));
        return records;
    }

    int index = 1;
    st.bindInt64(index++, productId);
    if (from > 0) st.bindInt64(index++, from);
    if (to > 0) st.bindInt64(index++, to);
    st.bindInt(index++, limit);

    while (st.step() == SQLITE_ROW) {
        records.push_back(mapPrice(st.raw()));
    }
    std::reverse(records.begin(), records.end());
    return records;
}

std::optional<double> Database::lastNotifiedPrice(long long productId) {
    std::lock_guard<std::mutex> lock(mutex_);

    Stmt st(db_, "SELECT last_notified_price FROM products WHERE id = ?");
    if (!st.valid()) return std::nullopt;
    st.bindInt64(1, productId);
    if (st.step() != SQLITE_ROW || colIsNull(st.raw(), 0)) return std::nullopt;
    return sqlite3_column_double(st.raw(), 0);
}

bool Database::setLastNotifiedPrice(long long productId, double price) {
    std::lock_guard<std::mutex> lock(mutex_);

    Stmt st(db_, "UPDATE products SET last_notified_price = ? WHERE id = ?");
    if (!st.valid()) {
        setError(std::string("更新通知状态失败: ") + sqlite3_errmsg(db_));
        return false;
    }
    st.bindDouble(1, price);
    st.bindInt64(2, productId);
    if (st.step() != SQLITE_DONE) {
        setError(std::string("更新通知状态失败: ") + sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool Database::addCrawlLog(long long productId, bool ok, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);

    Stmt st(db_,
            "INSERT INTO crawl_logs(product_id, ok, message, created_at)"
            " VALUES(?,?,?,?)");
    if (!st.valid()) return false;
    st.bindInt64(1, productId);
    st.bindInt(2, ok ? 1 : 0);
    st.bindText(3, truncate(message, 500));
    st.bindInt64(4, nowSeconds());
    return st.step() == SQLITE_DONE;
}

Stats Database::getStats() {
    std::lock_guard<std::mutex> lock(mutex_);

    Stats stats;
    const char* sql =
        "SELECT (SELECT COUNT(*) FROM products),"
        "       (SELECT COUNT(*) FROM products WHERE enabled = 1),"
        "       (SELECT COUNT(*) FROM price_records),"
        "       (SELECT COUNT(*) FROM price_records WHERE captured_at >= ?)";
    Stmt st(db_, sql);
    if (!st.valid()) return stats;
    st.bindInt64(1, nowSeconds() - 86400);
    if (st.step() != SQLITE_ROW) return stats;
    stats.products = sqlite3_column_int64(st.raw(), 0);
    stats.enabled = sqlite3_column_int64(st.raw(), 1);
    stats.records = sqlite3_column_int64(st.raw(), 2);
    stats.records24h = sqlite3_column_int64(st.raw(), 3);
    return stats;
}

}  // namespace pm
