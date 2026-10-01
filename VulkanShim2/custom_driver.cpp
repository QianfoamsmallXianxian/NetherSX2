#include "custom_driver.h"
#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace custom_driver {
namespace {

const char* PKG = "xyz.aethersx2.tturnip";
const char* DEF = "libvulkan_freedreno.so";
const int   MAX_DEPTH = 6;
const size_t CHUNK = 65536;

std::string ext()  { return "/sdcard/Android/data/" + std::string(PKG) + "/files"; }
std::string priv() { return "/data/data/" + std::string(PKG) + "/files"; }
std::string conf() { return ext() + "/driver.conf"; }
std::string dirs() { return ext() + "/drivers"; }

struct Found {
    std::string dir;
    std::string name;
    long long   size = 0;
    time_t      mtime = 0;
};

void mkdirs(const std::string& p) {
    std::string c;
    for (size_t i = 0; i < p.size(); ++i) {
        c += p[i];
        if (p[i] == '/' || i + 1 == p.size()) {
            if (c != "/") mkdir(c.c_str(), 0770);
        }
    }
}

bool file_ok(const std::string& p) {
    struct stat s{};
    return stat(p.c_str(), &s) == 0 && S_ISREG(s.st_mode) && s.st_size > 0;
}

long long fsize(const std::string& p) {
    struct stat s{};
    return stat(p.c_str(), &s) == 0 ? (long long)s.st_size : 0;
}

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

const void* find_bytes(const void* hay, size_t hlen, const void* needle, size_t nlen) {
    if (nlen == 0 || hlen < nlen) return nullptr;
    const char* h = (const char*)hay;
    const char* n = (const char*)needle;
    for (size_t i = 0; i + nlen <= hlen; ++i) {
        if (h[i] == n[0] && memcmp(h + i, n, nlen) == 0) return h + i;
    }
    return nullptr;
}

bool has_key(const char* buf, size_t len) {
    static const char* keys[] = { "freedreno", "turnip", "vkCreateInstance" };
    for (const char* k : keys) {
        if (find_bytes(buf, len, k, strlen(k)) != nullptr) return true;
    }
    return false;
}

bool is_driver(const std::string& p, long long size) {
    if (size < 1024 * 1024) return false;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    unsigned char h[20]{};
    size_t n = fread(h, 1, 20, f);
    if (n < 20 || h[0] != 0x7F || h[1] != 'E' || h[2] != 'L' || h[3] != 'F'
        || h[4] != 2 || h[5] != 1 || h[18] != 0xB7 || h[19] != 0x00) {
        fclose(f);
        return false;
    }
    char* buf = (char*)malloc(CHUNK + 1);
    if (!buf) { fclose(f); return false; }
    bool ok = false;
    size_t r = fread(buf, 1, CHUNK, f);
    buf[r] = 0;
    if (has_key(buf, r)) ok = true;
    if (!ok && size > (long long)CHUNK) {
        fseek(f, -(long long)CHUNK, SEEK_END);
        r = fread(buf, 1, CHUNK, f);
        buf[r] = 0;
        if (has_key(buf, r)) ok = true;
    }
    free(buf);
    fclose(f);
    return ok;
}

bool skip_dir(const std::string& n) {
    if (n.empty() || n[0] == '.') return true;
    static const char* bad[] = {
        "Android", "obb", "cache", "tmp", "temp",
        "node_modules", "backup", "logs", "log", "thumbnails"
    };
    for (const char* b : bad) if (n == b) return true;
    return false;
}

void walk(const std::string& root, int depth, Found& best) {
    if (depth > MAX_DEPTH) return;
    DIR* dp = opendir(root.c_str());
    if (!dp) return;
    dirent* e;
    while ((e = readdir(dp))) {
        if (e->d_name[0] == '.') continue;
        std::string full = root + "/" + e->d_name;
        struct stat st{};
        if (lstat(full.c_str(), &st) != 0 || S_ISLNK(st.st_mode)) continue;
        if (S_ISDIR(st.st_mode)) {
            if (skip_dir(e->d_name)) continue;
            walk(full, depth + 1, best);
        } else if (S_ISREG(st.st_mode) && st.st_size > 0) {
            std::string n = e->d_name;
            if (n.size() < 4 || n.compare(n.size() - 3, 3, ".so") != 0) continue;
            if (st.st_size < 1024 * 1024) continue;
            if (!is_driver(full, st.st_size)) continue;
            if (st.st_mtime > best.mtime ||
                (st.st_mtime == best.mtime && !best.name.empty() && n > best.name)) {
                best.mtime = st.st_mtime;
                best.size  = (long long)st.st_size;
                best.name  = n;
                best.dir   = root;
            }
        }
    }
    closedir(dp);
}

Found scan_all() {
    Found best;
    static const char* roots[] = {
        "/storage/emulated/0/PS2",
        "/storage/emulated/0/Download",
        "/storage/emulated/0/Documents",
        "/storage/emulated/0/NetherSX2",
        "/storage/emulated/0/驱动",
    };
    for (const char* r : roots) {
        walk(r, 0, best);
        if (!best.name.empty()) return best;
    }
    walk("/storage/emulated/0", 0, best);
    return best;
}

void log_ready(const std::string& name, const std::string& path,
               const std::string& ver, long long size) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "custom driver: name=%s size=%lld version=%s path=%s",
             name.c_str(), size, ver.empty() ? "unknown" : ver.c_str(), path.c_str());
    __android_log_print(ANDROID_LOG_INFO, "NetherSX2-Turnip", "%s", buf);
    FILE* f = fopen((ext() + "/vulkan_shim.log").c_str(), "ae");
    if (f) { fprintf(f, "%s\n", buf); fclose(f); }
}

std::string meta_version(const std::string& dir) {
    const char* paths[] = { "/meta.json", "/../meta.json" };
    for (const char* s : paths) {
        std::ifstream f(dir + s);
        if (!f) continue;
        std::stringstream ss;
        ss << f.rdbuf();
        std::string all = ss.str();
        size_t p = all.find("\"driverVersion\"");
        if (p == std::string::npos) continue;
        size_t a = all.find('"', p + 15);
        if (a == std::string::npos) continue;
        size_t b = all.find('"', a + 1);
        if (b == std::string::npos) continue;
        return all.substr(a + 1, b - a - 1);
    }
    return {};
}

void save_fields(const std::string& ddir, const std::string& dname) {
    std::ifstream in(conf());
    if (!in) return;
    std::string body, line;
    bool gd = false, gn = false;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (!gd && !ddir.empty() && t.rfind("driver_dir=", 0) == 0) {
            body += "driver_dir=" + ddir + "\n";
            gd = true;
        } else if (!gn && !dname.empty() && t.rfind("driver_name=", 0) == 0) {
            body += "driver_name=" + dname + "\n";
            gn = true;
        } else {
            body += line + "\n";
        }
    }
    if (!gd && !ddir.empty()) body += "driver_dir=" + ddir + "\n";
    if (!gn && !dname.empty()) body += "driver_name=" + dname + "\n";
    std::string tmp = conf() + ".tmp";
    { std::ofstream o(tmp, std::ios::trunc); if (!o) return; o << body; }
    rename(tmp.c_str(), conf().c_str());
}

bool copy_file(const std::string& src, const std::string& dst) {
    FILE* in = fopen(src.c_str(), "rb");
    if (!in) return false;
    FILE* out = fopen(dst.c_str(), "wb");
    if (!out) { fclose(in); return false; }
    char buf[65536];
    size_t r;
    bool ok = true;
    while ((r = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, r, out) != r) { ok = false; break; }
    }
    fclose(in);
    fclose(out);
    if (!ok) { unlink(dst.c_str()); return false; }
    chmod(dst.c_str(), 0700);
    return true;
}

}

std::string resolve_custom_driver() {
    mkdirs(ext());
    mkdirs(dirs());
    if (!file_ok(conf())) {
        std::ofstream f(conf());
        if (f) f << "enable_custom_driver=0\nauto_detect=1\n"
                 << "driver_dir=" << dirs() << "\n"
                 << "driver_name=" << DEF << "\n";
    }
    bool enabled = false;
    bool autodetect = true;
    std::string dir = dirs();
    std::string name;
    std::ifstream in(conf());
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t p = line.find('=');
        if (p == std::string::npos) continue;
        std::string k = trim(line.substr(0, p));
        std::string v = trim(line.substr(p + 1));
        if (k == "enable_custom_driver") enabled = (v == "1" || v == "true");
        else if (k == "auto_detect")    autodetect = (v == "1" || v == "true");
        else if (k == "driver_dir")     dir = v;
        else if (k == "driver_name")    name = v;
    }
    std::string src = dir + "/" + name;
    bool valid = file_ok(src) && is_driver(src, fsize(src));
    if (!valid) {
        Found f = scan_all();
        if (!f.name.empty()) {
            enabled = true;
            dir = f.dir;
            name = f.name;
            src = dir + "/" + name;
            save_fields(dir, name);
            valid = true;
        }
    }
    if (!enabled || name.empty() || !valid) {
        if (autodetect) save_fields("", DEF);
        return {};
    }
    mkdirs(priv());
    std::string dst = priv() + "/custom_driver.so";
    if (!copy_file(src, dst) || !file_ok(dst) || !is_driver(dst, fsize(dst))) {
        unlink(dst.c_str());
        if (autodetect) save_fields("", DEF);
        return {};
    }
    log_ready(name, dst, meta_version(dir), fsize(dst));
    if (autodetect) save_fields(dir, name);
    return dst;
}

}
