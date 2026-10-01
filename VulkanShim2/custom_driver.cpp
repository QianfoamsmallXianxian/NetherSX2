#include "custom_driver.h"
#include <string>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <android/log.h>

namespace custom_driver {

static const char* TAG = "NetherSX2Turnip";
static const char* PKG = "xyz.aethersx2.tturnip";
static const char* CONF_NAME = "driver.conf";
static const char* PRIVATE_SO_NAME = "custom_driver.so";
static const char* DEFAULT_DIR_NAME = "drivers";
static const char* DEFAULT_SO_NAME = "libvulkan_freedreno.so";

static std::string external_files_dir() {
    return std::string("/sdcard/Android/data/") + PKG + "/files";
}
static std::string private_files_dir() {
    return std::string("/data/data/") + PKG + "/files";
}
static std::string config_path() {
    return external_files_dir() + "/" + CONF_NAME;
}
static std::string default_driver_dir() {
    return external_files_dir() + "/" + DEFAULT_DIR_NAME;
}
static std::string private_so_path() {
    return private_files_dir() + "/" + PRIVATE_SO_NAME;
}
static void log_line(const std::string& msg) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", msg.c_str());
    std::string p = external_files_dir() + "/vulkan_shim.log";
    FILE* fp = fopen(p.c_str(), "a");
    if (!fp) return;
    fprintf(fp, "%s\n", msg.c_str());
    fclose(fp);
}
static bool ensure_dir(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur == "/" || cur.empty()) continue;
            struct stat st;
            if (stat(cur.c_str(), &st) == 0) {
                if (!S_ISDIR(st.st_mode)) return false;
                continue;
            }
            if (mkdir(cur.c_str(), 0770) != 0) return false;
        }
    }
    return true;
}
static std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}
static bool write_default_config() {
    if (!ensure_dir(external_files_dir())) return false;
    std::string p = config_path();
    if (access(p.c_str(), F_OK) == 0) return true;
    std::ofstream f(p);
    if (!f) return false;
    f << "enable_custom_driver=0\n";
    f << "driver_dir=" << default_driver_dir() << "\n";
    f << "driver_name=" << DEFAULT_SO_NAME << "\n";
    f.close();
    ensure_dir(default_driver_dir());
    return true;
}
struct Config {
    bool enable = false;
    std::string dir;
    std::string name;
};
static Config read_config() {
    Config c;
    std::ifstream f(config_path());
    if (!f) return c;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string k = trim(line.substr(0, pos));
        std::string v = trim(line.substr(pos + 1));
        if (k == "enable_custom_driver") c.enable = (v == "1" || v == "true" || v == "TRUE");
        else if (k == "driver_dir") c.dir = v;
        else if (k == "driver_name") c.name = v;
    }
    return c;
}
static bool is_regular_file(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    return S_ISREG(st.st_mode) && st.st_size > 0;
}
static bool copy_file(const std::string& src, const std::string& dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    char buf[65536];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
        out.write(buf, in.gcount());
        if (!out) return false;
    }
    out.close();
    chmod(dst.c_str(), 0700);
    return is_regular_file(dst);
}
std::string resolve_custom_driver() {
    ensure_dir(external_files_dir());
    write_default_config();
    Config c = read_config();
    if (!c.enable) {
        log_line("custom driver disabled");
        return "";
    }
    if (c.dir.empty() || c.name.empty()) {
        log_line("custom driver config incomplete");
        return "";
    }
    std::string full = c.dir + "/" + c.name;
    log_line("custom driver source: " + full);
    if (!is_regular_file(full)) {
        log_line("custom driver source not found");
        return "";
    }
    if (!ensure_dir(private_files_dir())) {
        log_line("cannot create private files dir");
        return "";
    }
    std::string dst = private_so_path();
    if (!copy_file(full, dst)) {
        log_line("copy custom driver failed");
        return "";
    }
    log_line("custom driver copied to: " + dst);
    return dst;
}
}
