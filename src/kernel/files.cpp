#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

#include "kernel/disc.h"
#include "kernel/kernel.h"
#include "kernel/exports.h"

namespace fs = std::filesystem;

namespace kernel {

namespace {

std::mutex g_links_lock;
std::map<std::string, std::string> g_links;    // upper case "\??\D:" -> "\Device\CdRom0"
std::map<std::string, fs::path> g_devices;     // upper case device -> host directory
constexpr char kCdRom[] = "\\DEVICE\\CDROM0";

std::string Upper(std::string s) {
    for (char& c : s) c = char(std::toupper(uint8_t(c)));
    return s;
}

std::string Normalize(std::string p) {
    std::replace(p.begin(), p.end(), '/', '\\');
    std::string out;
    for (char c : p) {
        if (c == '\\' && !out.empty() && out.back() == '\\') continue;
        out += c;
    }
    while (out.size() > 1 && out.back() == '\\') out.pop_back();
    return out;
}

// host lookup that ignores case on file systems that do not
fs::path MatchCase(const fs::path& dir, const std::vector<std::string>& parts) {
    fs::path cur = dir;
    for (auto& part : parts) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            cur = cur.parent_path();
            continue;
        }
        fs::path direct = cur / part;
        std::error_code ec;
        if (fs::exists(direct, ec)) {
            cur = direct;
            continue;
        }
        bool found = false;
        std::string want = Upper(part);
        for (auto& e : fs::directory_iterator(cur, ec)) {
            if (Upper(e.path().filename().string()) == want) {
                cur = e.path();
                found = true;
                break;
            }
        }
        if (!found) cur = direct;
    }
    return cur;
}

struct File;
// the host files open now: a size asked for by path is taken through the open file, as the Switch keeps files open
// for writing from being opened again
std::mutex g_open_mu;
std::vector<File*> g_open;

struct File : Object {
    fs::path host;
    std::string xbox;
    FILE* fp = nullptr;
    const Disc::Entry* entry = nullptr;  // on the disc image instead of a host path
    bool dir = false;
    uint64_t pos = 0;
    std::vector<fs::directory_entry> entries;
    std::vector<const Disc::Entry*> disc_entries;
    size_t next = 0;
    bool listed = false;
    bool delete_on_close = false;
    File() : Object(ObjType::File) {}
    ~File() override {
        if (fp) {
            std::lock_guard lock(g_open_mu);
            g_open.erase(std::remove(g_open.begin(), g_open.end(), this), g_open.end());
            std::fclose(fp);
        }
        if (delete_on_close) {
            std::error_code ec;
            fs::remove(host, ec);
        }
    }
};

struct Link : Object {
    std::string target;
    Link() : Object(ObjType::SymLink) {}
};

uint64_t ToFileTime(fs::file_time_type t) {
    using namespace std::chrono;
    auto sys = time_point_cast<nanoseconds>(t - fs::file_time_type::clock::now() + system_clock::now());
    return uint64_t(sys.time_since_epoch().count() / 100 + 116444736000000000ll);
}

void TrackOpen(File* f) {
    if (!f->fp) return;
    std::lock_guard lock(g_open_mu);
    g_open.push_back(f);
}

// A host file's size and time, through the open file where there is one. std::filesystem's file_size and
// last_write_time fail with EIO on the Switch's SD card (devkitA64's libstdc++), so elsewhere than Windows they come
// from stat and fstat; the time as a FILETIME.
#ifdef _WIN32
uint64_t HostSize(const fs::path& p, FILE* fp) {
    std::error_code ec;
    if (fp) std::fflush(fp);
    uint64_t n = fs::file_size(p, ec);
    return ec ? 0 : n;
}
uint64_t HostTime(const fs::path& p) {
    std::error_code ec;
    return ToFileTime(fs::last_write_time(p, ec));
}
#else
// The size of an open file. The Switch's SD card has failed stat and fstat with EIO here (other libnx ports fstat
// fine), so it goes down a list: fstat, lseek to the end, and as the last resort a search for the last byte that
// reads. Each way that fails is logged once, and the first file says which one worked.
bool ReadsAt(FILE* fp, uint64_t at) {
    char c;
    return !std::fseek(fp, long(at), SEEK_SET) && std::fread(&c, 1, 1, fp) == 1;
}
uint64_t EndOf(FILE* fp, const fs::path& p) {
    std::fflush(fp);
    int fd = fileno(fp);
    struct stat st{};
    if (!fstat(fd, &st)) {
        LOG_FIRST(1, "files: sizes by fstat (%s, %lld bytes)", p.string().c_str(), (long long)st.st_size);
        return uint64_t(st.st_size);
    }
    LOG_FIRST(8, "files: fstat %s: %s", p.string().c_str(), std::strerror(errno));
    off_t end = lseek(fd, 0, SEEK_END);
    if (end >= 0) {
        LOG_FIRST(1, "files: sizes by lseek (%s, %lld bytes)", p.string().c_str(), (long long)end);
        return uint64_t(end);
    }
    LOG_FIRST(8, "files: lseek %s: %s", p.string().c_str(), std::strerror(errno));
    // the last offset that reads: doubling, then halving (the callers seek before each read and write)
    if (!ReadsAt(fp, 0)) return 0;
    uint64_t lo = 0, hi = 1;
    while (ReadsAt(fp, hi)) lo = hi, hi *= 2;
    while (hi - lo > 1) {
        uint64_t mid = lo + (hi - lo) / 2;
        (ReadsAt(fp, mid) ? lo : hi) = mid;
    }
    std::clearerr(fp);
    LOG_FIRST(1, "files: sizes by probing reads (%s, %llu bytes)", p.string().c_str(), (unsigned long long)hi);
    return hi;
}
uint64_t HostSize(const fs::path& p, FILE* fp) {
    if (fp) return EndOf(fp, p);
    {
        std::lock_guard lock(g_open_mu);
        for (File* f : g_open) {
            if (f->host == p) return EndOf(f->fp, p);
        }
    }
    struct stat st{};
    if (!stat(p.c_str(), &st)) return uint64_t(st.st_size);
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) {
        LOG_FIRST(16, "files: size of %s: %s", p.string().c_str(), std::strerror(errno));
        return 0;
    }
    uint64_t n = EndOf(f, p);
    std::fclose(f);
    return n;
}
uint64_t HostTime(const fs::path& p) {
    struct stat st{};
    if (stat(p.c_str(), &st)) return 116444736000000000ull;  // 1970, as a FILETIME
    return uint64_t(st.st_mtim.tv_sec) * 10000000ull + uint64_t(st.st_mtim.tv_nsec) / 100 + 116444736000000000ull;
}
#endif

void FillNetworkInfo(uint32_t out, uint64_t t, uint64_t size, bool dir) {
    for (int i = 0; i < 4; i++) W64(out + i * 8, t);
    W64(out + 0x20, (size + 0xFFF) & ~0xFFFull);
    W64(out + 0x28, size);
    W32(out + 0x30, dir ? 0x10 : 0x80);  // FILE_ATTRIBUTE_DIRECTORY or NORMAL
}

void FillNetworkInfo(uint32_t out, const fs::path& p) {
    std::error_code ec;
    bool dir = fs::is_directory(p, ec);
    FillNetworkInfo(out, HostTime(p), dir ? 0 : HostSize(p, nullptr), dir);
}

void FillNetworkInfo(uint32_t out, const Disc::Entry& e) {
    FillNetworkInfo(out, MountedDisc()->Time(), e.dir ? 0 : e.size, e.dir);
}

bool WildMatch(const std::string& pat, const std::string& s) {
    size_t p = 0, i = 0, star = std::string::npos, mark = 0;
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == '?' || std::toupper(uint8_t(pat[p])) == std::toupper(uint8_t(s[i])))) {
            p++;
            i++;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = i;
        } else if (star != std::string::npos) {
            p = star + 1;
            i = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
}

void IoStatus(uint32_t iosb, uint32_t status, uint32_t info) {
    if (!iosb) return;
    W32(iosb, status);
    W32(iosb + 4, info);
}

// Opened for writing only when the title asks to write: Horizon keeps a file open for writing to that handle alone.
bool WantsWrite(uint32_t access) {
    // GENERIC_WRITE, GENERIC_ALL, MAXIMUM_ALLOWED, FILE_WRITE_DATA, FILE_APPEND_DATA
    return access & (0x40000000 | 0x10000000 | 0x02000000 | 0x2 | 0x4);
}

std::shared_ptr<File> FileFromHandle(uint32_t h) {
    ObjectRef o = FromHandle(h);
    if (!o || o->type != ObjType::File) return nullptr;
    return std::static_pointer_cast<File>(o);
}

std::string ObjectName(uint32_t attrs, uint32_t* root = nullptr) {
    if (root) *root = R32(attrs);
    return ReadAnsiString(R32(attrs + 4));
}

void Complete(uint32_t event, uint32_t apc, uint32_t apc_ctx, uint32_t iosb) {
    if (event)
        if (ObjectRef e = FromHandle(event)) SetEvent(*e);
    if (apc && Current()) QueueApc(*Current(), apc, apc_ctx, iosb, 0);
}

}  // namespace

std::string ReadAnsiString(uint32_t str) {
    if (!str) return {};
    uint16_t len = R16(str);
    uint32_t buf = R32(str + 4);
    if (!buf) return {};
    return std::string(reinterpret_cast<const char*>(HostPtr(buf)), len);
}

std::string ReadCString(uint32_t va, size_t max) {
    std::string s;
    if (!va) return s;
    for (size_t i = 0; i < max; i++) {
        char c = char(G<uint8_t>(va + uint32_t(i)));
        if (!c) break;
        s += c;
    }
    return s;
}

void MountDefaults(const std::string& game_dir, const std::string& data_dir) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    fs::path d = data_dir;
    g_devices[kCdRom] = game_dir;
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION1"] = d / "E";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION2"] = d / "C";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION3"] = d / "X";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION4"] = d / "Y";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION5"] = d / "Z";
    for (auto& [k, v] : g_devices) {
        std::error_code ec;
        fs::create_directories(v, ec);
    }
    g_links["\\??\\D:"] = "\\Device\\CdRom0";
    const Disc* disc = MountedDisc();
    LOGI("files: D: is %s, hard disk under %s", disc ? disc->Path().string().c_str() : game_dir.c_str(), data_dir.c_str());
}

bool CreateSymbolicLink(const std::string& link, const std::string& target) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    g_links[Upper(Normalize(link))] = Normalize(target);
    LOGI("files: link %s -> %s", link.c_str(), target.c_str());
    return true;
}

bool DeleteSymbolicLink(const std::string& link) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    return g_links.erase(Upper(Normalize(link))) != 0;
}

std::string QuerySymbolicLink(const std::string& link) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    auto it = g_links.find(Upper(Normalize(link)));
    return it == g_links.end() ? std::string() : it->second;
}

namespace {

struct Target {
    std::string host;                    // empty on the disc image or unmapped
    bool disc = false;                   // on the mounted disc image
    const Disc::Entry* entry = nullptr;  // there: the file or folder, null when missing
};

Target Resolve(const std::string& xbox_path, uint32_t root_handle) {
    std::string p = Normalize(xbox_path);
    if (root_handle) {
        if (auto f = FileFromHandle(root_handle)) p = Normalize(f->xbox + "\\" + p);
    }
    if (p.size() >= 2 && p[1] == ':') p = "\\??\\" + p;
    std::lock_guard<std::mutex> lock(g_links_lock);
    for (int depth = 0; depth < 8; depth++) {
        std::string up = Upper(p);
        bool changed = false;
        for (auto& [k, v] : g_links) {
            if (up.compare(0, k.size(), k) == 0 && (up.size() == k.size() || up[k.size()] == '\\')) {
                p = v + p.substr(k.size());
                changed = true;
                break;
            }
        }
        if (!changed) break;
    }
    std::string up = Upper(p);
    const std::pair<const std::string, fs::path>* best = nullptr;
    for (auto& e : g_devices)
        if (up.compare(0, e.first.size(), e.first) == 0 && (up.size() == e.first.size() || up[e.first.size()] == '\\'))
            if (!best || e.first.size() > best->first.size()) best = &e;
    if (!best) return {};
    std::vector<std::string> parts;
    std::string rest = p.substr(best->first.size());
    size_t s = 0;
    while (s < rest.size()) {
        size_t e = rest.find('\\', s);
        if (e == std::string::npos) e = rest.size();
        if (e > s) parts.push_back(rest.substr(s, e - s));
        s = e + 1;
    }
    // no Xbox name holds ':', and fs::path would take "D:" as a host drive
    for (auto& part : parts)
        if (part.find(':') != std::string::npos) return {};
    if (best->first == kCdRom)
        if (const Disc* disc = MountedDisc()) return {{}, true, disc->Find(parts)};
    return {MatchCase(best->second, parts).string()};
}

// the disc image is read only: nothing on it is created, replaced or written
uint32_t OpenOnDisc(const Disc::Entry* e, const std::string& name, uint32_t handle_out, uint32_t iosb, uint32_t disposition,
                    uint32_t options) {
    if (!e && (disposition == 1 || disposition == 4)) {  // FILE_OPEN or FILE_OVERWRITE need an existing file
        LOG_FIRST(200, "missing %s", name.c_str());
        IoStatus(iosb, kObjectNameNotFound, 5);
        return kObjectNameNotFound;
    }
    if (!e || (disposition != 1 && disposition != 3)) {
        IoStatus(iosb, kAccessDenied, 0);
        return kAccessDenied;
    }
    if (e->dir && !(options & 1) && (options & 0x40)) {  // options 1 FILE_DIRECTORY_FILE, 0x40 FILE_NON_DIRECTORY_FILE
        IoStatus(iosb, kFileIsADirectory, 0);
        return kFileIsADirectory;
    }
    auto f = std::make_shared<File>();
    f->xbox = Normalize(name);
    f->entry = e;
    f->dir = e->dir;
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, 1);
    LOG_FIRST(400, "open %s", name.c_str());
    return kSuccess;
}

}  // namespace

std::string ResolvePath(const std::string& xbox_path, uint32_t root_handle) { return Resolve(xbox_path, root_handle).host; }

bool ReadGameFile(const std::string& xbox_path, std::vector<uint8_t>& out) {
    Target t = Resolve(xbox_path, 0);
    if (t.disc) {
        if (!t.entry || t.entry->dir) return false;
        out.resize(t.entry->size);
        return MountedDisc()->Read(*t.entry, 0, out.data(), t.entry->size) == t.entry->size;
    }
    std::ifstream f(t.host, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// exports

KERNEL_EXPORT(190, NtCreateFile, 9) {
    uint32_t handle_out = a[0], access = a[1], attrs = a[2], iosb = a[3], disposition = a[7], options = a[8];
    uint32_t root;
    std::string name = ObjectName(attrs, &root);
    Target t = Resolve(name, root);
    if (t.disc) return OpenOnDisc(t.entry, name, handle_out, iosb, disposition, options);
    std::string host = t.host;
    if (host.empty()) {
        LOGW("NtCreateFile: no mapping for %s", name.c_str());
        IoStatus(iosb, kObjectPathNotFound, 0);
        return kObjectPathNotFound;
    }
    std::error_code ec;
    bool exists = fs::exists(host, ec);
    bool is_dir = exists && fs::is_directory(host, ec);
    bool want_dir = options & 1;
    uint32_t info = 1;  // FILE_OPENED
    if (want_dir || is_dir) {
        if (!exists) {
            if (disposition == 1 || disposition == 4) {
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            fs::create_directories(host, ec);
            info = 2;
        } else if (disposition == 2) {
            IoStatus(iosb, kObjectNameCollision, 4);
            return kObjectNameCollision;
        }
        if (!want_dir && (options & 0x40)) {
            IoStatus(iosb, kFileIsADirectory, 0);
            return kFileIsADirectory;
        }
        auto f = std::make_shared<File>();
        f->host = host;
        f->xbox = Normalize(name);
        f->dir = true;
        W32(handle_out, NewHandle(f));
        IoStatus(iosb, kSuccess, info);
        return kSuccess;
    }
    const char* mode = nullptr;
    switch (disposition) {
        case 0:  // supersede
        case 5:  // overwrite if
            mode = "w+b";
            info = exists ? 3 : 2;
            break;
        case 1:  // open
            if (!exists) {
                LOG_FIRST(200, "missing %s", name.c_str());
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            mode = WantsWrite(access) ? "r+b" : "rb";
            break;
        case 2:  // create
            if (exists) {
                IoStatus(iosb, kObjectNameCollision, 4);
                return kObjectNameCollision;
            }
            mode = "w+b";
            info = 2;
            break;
        case 3:  // open if
            mode = exists ? (WantsWrite(access) ? "r+b" : "rb") : "w+b";
            info = exists ? 1 : 2;
            break;
        case 4:  // overwrite
            if (!exists) {
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            mode = "w+b";
            info = 3;
            break;
        default:
            return kInvalidParameter;
    }
    if (!exists) fs::create_directories(fs::path(host).parent_path(), ec);
    FILE* fp = std::fopen(host.c_str(), mode);
    if (!fp && std::string(mode) == "r+b") fp = std::fopen(host.c_str(), "rb");  // read only media
    if (!fp) {
        LOGW("NtCreateFile: cannot open %s", host.c_str());
        IoStatus(iosb, kAccessDenied, 0);
        return kAccessDenied;
    }
    auto f = std::make_shared<File>();
    f->host = host;
    f->xbox = Normalize(name);
    f->fp = fp;
    TrackOpen(f.get());
    f->delete_on_close = options & 0x1000;  // FILE_DELETE_ON_CLOSE
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, info);
    LOG_FIRST(400, "open %s", name.c_str());
    return kSuccess;
}

KERNEL_EXPORT(202, NtOpenFile, 6) {
    // same as create with FILE_OPEN
    uint32_t handle_out = a[0], attrs = a[2], iosb = a[3], options = a[5];
    uint32_t root;
    std::string name = ObjectName(attrs, &root);
    Target t = Resolve(name, root);
    if (t.disc) return OpenOnDisc(t.entry, name, handle_out, iosb, 1, options);
    std::string host = t.host;
    std::error_code ec;
    if (host.empty() || !fs::exists(host, ec)) {
        LOG_FIRST(200, "missing %s", name.c_str());
        IoStatus(iosb, kObjectNameNotFound, 5);
        return host.empty() ? kObjectPathNotFound : kObjectNameNotFound;
    }
    auto f = std::make_shared<File>();
    f->host = host;
    f->xbox = Normalize(name);
    f->dir = fs::is_directory(host, ec);
    if (!f->dir) {
        if (WantsWrite(a[1])) f->fp = std::fopen(host.c_str(), "r+b");
        if (!f->fp) f->fp = std::fopen(host.c_str(), "rb");
        TrackOpen(f.get());
        if (!f->fp) return kAccessDenied;
    }
    (void)options;
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, 1);
    return kSuccess;
}

// Horizon must never get guest memory as an IPC buffer: it is code memory there, and the fs service froze the whole
// console on the save code's small unaligned reads into it (nfsu2-sw). On the Switch reads and writes go through a
// bounce buffer of this thread's own; elsewhere straight into guest memory.
#ifdef SCDA_SWITCH
constexpr size_t kBounce = 256 << 10;
uint8_t* Bounce() {
    thread_local std::unique_ptr<uint8_t[]> b(new uint8_t[kBounce]);
    return b.get();
}
template <typename Fn>
size_t Bounced(uint8_t* guest, size_t len, bool to_guest, Fn io) {
    size_t done = 0;
    while (done < len) {
        size_t part = std::min(kBounce, len - done);
        if (!to_guest) std::memcpy(Bounce(), guest + done, part);
        size_t n = io(Bounce(), part, done);
        if (to_guest) std::memcpy(guest + done, Bounce(), n);
        done += n;
        if (n < part) break;
    }
    return done;
}
size_t HostRead(FILE* fp, void* guest, size_t len) {
    return Bounced(static_cast<uint8_t*>(guest), len, true,
                   [&](uint8_t* b, size_t n, size_t) { return std::fread(b, 1, n, fp); });
}
size_t HostWrite(FILE* fp, const void* guest, size_t len) {
    return Bounced(static_cast<uint8_t*>(const_cast<void*>(guest)), len, false,
                   [&](uint8_t* b, size_t n, size_t) { return std::fwrite(b, 1, n, fp); });
}
uint32_t DiscRead(const Disc::Entry& e, uint64_t pos, void* guest, uint32_t len) {
    return uint32_t(Bounced(static_cast<uint8_t*>(guest), len, true, [&](uint8_t* b, size_t n, size_t at) {
        return size_t(MountedDisc()->Read(e, pos + at, b, uint32_t(n)));
    }));
}
#else
size_t HostRead(FILE* fp, void* guest, size_t len) { return std::fread(guest, 1, len, fp); }
size_t HostWrite(FILE* fp, const void* guest, size_t len) { return std::fwrite(guest, 1, len, fp); }
uint32_t DiscRead(const Disc::Entry& e, uint64_t pos, void* guest, uint32_t len) {
    return MountedDisc()->Read(e, pos, guest, len);
}
#endif

KERNEL_EXPORT(219, NtReadFile, 8) {
    uint32_t h = a[0], event = a[1], apc = a[2], apc_ctx = a[3], iosb = a[4], buf = a[5], len = a[6], off = a[7];
    auto f = FileFromHandle(h);
    if (!f || (!f->fp && !f->entry)) return kInvalidHandle;
    uint64_t pos = f->pos;
    if (off) {
        int64_t o = int64_t(R64(off));
        if (o >= 0) pos = uint64_t(o);
    }
    size_t n = 0;
    if (f->entry) {
        n = len ? DiscRead(*f->entry, pos, HostPtr(buf), len) : 0;
    } else {
        std::fseek(f->fp, long(pos), SEEK_SET);
        n = len ? HostRead(f->fp, HostPtr(buf), len) : 0;
    }
    f->pos = pos + n;
    if (n < len && !f->entry) LOG_FIRST(32, "NtReadFile: %s at %llu: %zu of %u bytes", f->host.string().c_str(),
                                         (unsigned long long)pos, n, len);
    if (len && !n) {
        IoStatus(iosb, kEndOfFile, 0);
        Complete(event, apc, apc_ctx, iosb);
        return kEndOfFile;
    }
    IoStatus(iosb, kSuccess, uint32_t(n));
    Complete(event, apc, apc_ctx, iosb);
    return kSuccess;
}

KERNEL_EXPORT(236, NtWriteFile, 8) {
    uint32_t h = a[0], event = a[1], apc = a[2], apc_ctx = a[3], iosb = a[4], buf = a[5], len = a[6], off = a[7];
    auto f = FileFromHandle(h);
    if (!f || !f->fp) return kInvalidHandle;
    uint64_t pos = f->pos;
    if (off) {
        int64_t o = int64_t(R64(off));
        if (o >= 0) pos = uint64_t(o);
    }
    std::fseek(f->fp, long(pos), SEEK_SET);
    size_t n = HostWrite(f->fp, HostPtr(buf), len);
    if (n < len) LOGW("NtWriteFile: %s at %llu: %zu of %u bytes", f->host.string().c_str(), (unsigned long long)pos, n, len);
    f->pos = pos + n;
    IoStatus(iosb, kSuccess, uint32_t(n));
    Complete(event, apc, apc_ctx, iosb);
    return kSuccess;
}

KERNEL_EXPORT(211, NtQueryInformationFile, 5) {
    uint32_t h = a[0], iosb = a[1], out = a[2], len = a[3], cls = a[4];
    auto f = FileFromHandle(h);
    if (!f) return kInvalidHandle;
    std::error_code ec;
    uint64_t size = f->entry ? (f->dir ? 0 : f->entry->size) : f->dir ? 0 : HostSize(f->host, f->fp);
    std::memset(HostPtr(out), 0, len);
    switch (cls) {
        case 4: {  // FileBasicInformation
            uint64_t t = f->entry ? MountedDisc()->Time() : HostTime(f->host);
            for (int i = 0; i < 4; i++) W64(out + i * 8, t);
            W32(out + 0x20, f->dir ? 0x10 : 0x80);
            IoStatus(iosb, kSuccess, 0x28);
            return kSuccess;
        }
        case 5:  // FileStandardInformation
            W64(out, (size + 0xFFF) & ~0xFFFull);
            W64(out + 8, size);
            W32(out + 0x10, 1);  // NumberOfLinks
            G<uint8_t>(out + 0x15) = f->dir;  // Directory, after DeletePending
            IoStatus(iosb, kSuccess, 0x18);
            return kSuccess;
        case 14:  // FilePositionInformation
            W64(out, f->pos);
            IoStatus(iosb, kSuccess, 8);
            return kSuccess;
        case 34:  // FileNetworkOpenInformation
            if (f->entry) FillNetworkInfo(out, *f->entry);
            else FillNetworkInfo(out, f->host);
            IoStatus(iosb, kSuccess, 0x38);
            return kSuccess;
        case 22:  // FileModeInformation
        case 16:
            IoStatus(iosb, kSuccess, 4);
            return kSuccess;
        default:
            LOG_FIRST(8, "NtQueryInformationFile: class %u", cls);
            return kInvalidParameter;
    }
}

KERNEL_EXPORT(226, NtSetInformationFile, 5) {
    uint32_t h = a[0], iosb = a[1], in = a[2], cls = a[4];
    auto f = FileFromHandle(h);
    if (!f) return kInvalidHandle;
    if (f->entry && cls != 14 && cls != 4) return kAccessDenied;
    std::error_code ec;
    switch (cls) {
        case 14:  // position
            f->pos = R64(in);
            break;
        case 19:  // allocation
        case 20:  // end of file
            if (f->fp) {
                std::fflush(f->fp);
#ifdef _WIN32
                fs::resize_file(f->host, R64(in), ec);
#else
                // through the open file: libnx's SD card has ftruncate, not the path's truncate resize_file takes
                if (ftruncate(fileno(f->fp), off_t(R64(in)))) ec.assign(errno, std::generic_category());
#endif
                if (ec) LOGW("NtSetInformationFile: %s to %llu bytes: %s", f->host.string().c_str(),
                             (unsigned long long)R64(in), ec.message().c_str());
            }
            break;
        case 13:  // disposition
            f->delete_on_close = G<uint8_t>(in) != 0;
            break;
        case 10: {  // rename: ReplaceIfExists, RootDirectory, FileName (ANSI_STRING inline)
            uint32_t root = R32(in + 4);
            std::string target = ReadAnsiString(in + 8);
            std::string host = ResolvePath(target, root);
            if (host.empty()) return kObjectPathNotFound;
            if (f->fp) std::fflush(f->fp);
            fs::rename(f->host, host, ec);
            if (ec) return kAccessDenied;
            f->host = host;
            break;
        }
        case 4:  // basic, times and attributes are not kept
            break;
        default:
            LOG_FIRST(8, "NtSetInformationFile: class %u", cls);
            return kInvalidParameter;
    }
    IoStatus(iosb, kSuccess, 0);
    return kSuccess;
}

KERNEL_EXPORT(218, NtQueryVolumeInformationFile, 5) {
    uint32_t iosb = a[1], out = a[2], len = a[3], cls = a[4];
    std::memset(HostPtr(out), 0, len);
    switch (cls) {
        case 3:  // FileFsSizeInformation: 16 GiB total, 8 GiB free in 16 KiB units
            W64(out, 0x100000);
            W64(out + 8, 0x80000);
            W32(out + 0x10, 32);
            W32(out + 0x14, 512);
            IoStatus(iosb, kSuccess, 0x18);
            return kSuccess;
        case 7:  // FileFsFullSizeInformation
            W64(out, 0x100000);
            W64(out + 8, 0x80000);
            W64(out + 0x10, 0x80000);
            W32(out + 0x18, 32);
            W32(out + 0x1C, 512);
            IoStatus(iosb, kSuccess, 0x20);
            return kSuccess;
        case 1:  // FileFsVolumeInformation
            W32(out + 8, 0x12345678);  // VolumeSerialNumber
            IoStatus(iosb, kSuccess, 0x18);
            return kSuccess;
        case 5:  // FileFsAttributeInformation: attributes, max name, name length, "FATX"
            W32(out, 0);
            W32(out + 4, 42);
            W32(out + 8, 4);
            std::memcpy(HostPtr(out + 12), "FATX", 4);
            IoStatus(iosb, kSuccess, 16);
            return kSuccess;
        default:
            LOG_FIRST(8, "NtQueryVolumeInformationFile: class %u", cls);
            return kInvalidParameter;
    }
}

KERNEL_EXPORT(207, NtQueryDirectoryFile, 10) {
    uint32_t h = a[0], event = a[1], apc = a[2], apc_ctx = a[3], iosb = a[4], out = a[5], len = a[6], cls = a[7],
             mask = a[8], restart = a[9];
    auto f = FileFromHandle(h);
    if (!f || !f->dir) return kInvalidHandle;
    if (cls != 1) {
        LOG_FIRST(8, "NtQueryDirectoryFile: class %u", cls);
        return kInvalidParameter;
    }
    std::string pat = mask ? ReadAnsiString(mask) : "*";
    if (!f->listed || (restart & 0xFF)) {
        f->entries.clear();
        f->disc_entries.clear();
        if (f->entry) {
            for (const Disc::Entry& e : f->entry->children)
                if (WildMatch(pat, e.name)) f->disc_entries.push_back(&e);
        } else {
            std::error_code ec;
            for (auto& e : fs::directory_iterator(f->host, ec))
                if (WildMatch(pat, e.path().filename().string())) f->entries.push_back(e);
            std::sort(f->entries.begin(), f->entries.end());
        }
        f->next = 0;
        f->listed = true;
    }
    if (f->next >= (f->entry ? f->disc_entries.size() : f->entries.size())) {
        IoStatus(iosb, kNoMoreFiles, 0);
        return f->next == 0 ? kNoSuchFile : kNoMoreFiles;
    }
    size_t i = f->next++;
    std::string fname = f->entry ? f->disc_entries[i]->name : f->entries[i].path().filename().string();
    uint32_t need = 0x40 + uint32_t(fname.size());
    if (len < need) return kBufferTooSmall;
    std::memset(HostPtr(out), 0, need);
    // times, sizes and attributes sit at the same place
    if (f->entry) FillNetworkInfo(out + 8, *f->disc_entries[i]);
    else FillNetworkInfo(out + 8, f->entries[i].path());
    // FILE_DIRECTORY_INFORMATION has EndOfFile before AllocationSize, the reverse of the network info
    W32(out + 0x38, R32(out + 8 + 0x30));
    uint64_t alloc = R64(out + 8 + 0x20), eof = R64(out + 8 + 0x28);
    W64(out + 0x28, eof);
    W64(out + 0x30, alloc);
    W32(out + 0x3C, uint32_t(fname.size()));
    std::memcpy(HostPtr(out + 0x40), fname.data(), fname.size());
    IoStatus(iosb, kSuccess, need);
    Complete(event, apc, apc_ctx, iosb);
    return kSuccess;
}

KERNEL_EXPORT(210, NtQueryFullAttributesFile, 2) {
    std::string name = ReadAnsiString(R32(a[0] + 4));
    Target t = Resolve(name, R32(a[0]));
    std::error_code ec;
    if (t.disc ? !t.entry : t.host.empty() || !fs::exists(t.host, ec)) {
        LOG_FIRST(200, "missing %s", name.c_str());
        return kObjectNameNotFound;
    }
    if (t.disc) FillNetworkInfo(a[1], *t.entry);
    else FillNetworkInfo(a[1], t.host);
    return kSuccess;
}

KERNEL_EXPORT(195, NtDeleteFile, 1) {
    std::string host = ResolvePath(ReadAnsiString(R32(a[0] + 4)), R32(a[0]));
    std::error_code ec;
    if (host.empty() || !fs::remove(host, ec)) return kObjectNameNotFound;
    return kSuccess;
}

KERNEL_EXPORT(198, NtFlushBuffersFile, 2) {
    if (auto f = FileFromHandle(a[0]))
        if (f->fp) std::fflush(f->fp);
    IoStatus(a[1], kSuccess, 0);
    return kSuccess;
}

KERNEL_EXPORT(200, NtFsControlFile, 10) {
    IoStatus(a[4], kSuccess, 0);
    return kSuccess;
}

KERNEL_EXPORT(196, NtDeviceIoControlFile, 10) {
    uint32_t iosb = a[4], code = a[5], out = a[8], out_len = a[9];
    LOG_FIRST(16, "NtDeviceIoControlFile: code %08X", code);
    if (out && out_len) std::memset(HostPtr(out), 0, out_len);
    if (code == 0x70000 && out_len >= 0x18) {  // IOCTL_DISK_GET_DRIVE_GEOMETRY
        W64(out, 1000);
        W32(out + 8, 12);
        W32(out + 12, 255);
        W32(out + 16, 63);
        W32(out + 20, 512);
    }
    IoStatus(iosb, kSuccess, out_len);
    return kSuccess;
}

KERNEL_EXPORT(67, IoCreateSymbolicLink, 2) {
    CreateSymbolicLink(ReadAnsiString(a[0]), ReadAnsiString(a[1]));
    return kSuccess;
}

KERNEL_EXPORT(69, IoDeleteSymbolicLink, 1) {
    return DeleteSymbolicLink(ReadAnsiString(a[0])) ? kSuccess : kObjectNameNotFound;
}

KERNEL_EXPORT(203, NtOpenSymbolicLinkObject, 2) {
    std::string name = ObjectName(a[1]);
    std::string target = QuerySymbolicLink(name);
    if (target.empty()) return kObjectNameNotFound;
    auto l = std::make_shared<Link>();
    l->target = target;
    W32(a[0], NewHandle(l));
    return kSuccess;
}

KERNEL_EXPORT(215, NtQuerySymbolicLinkObject, 3) {
    ObjectRef o = FromHandle(a[0]);
    if (!o || o->type != ObjType::SymLink) return kInvalidHandle;
    auto& t = static_cast<Link&>(*o).target;
    uint32_t str = a[1];
    uint16_t maxlen = R16(str + 2);
    if (a[2]) W32(a[2], uint32_t(t.size()));
    if (t.size() > maxlen) return kBufferTooSmall;
    std::memcpy(HostPtr(R32(str + 4)), t.data(), t.size());
    W16(str, uint16_t(t.size()));
    return kSuccess;
}

}  // namespace kernel
