#include "custom_driver.h"
#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace custom_driver {
namespace {

const char* kTag       = "NetherSX2-Turnip";
const char* kPkg       = "xyz.aethersx2.tturnip";
const char* kConfName  = "driver.conf";
const char* kPrivateSo = "custom_driver.so";
const char* kDefaultSo = "libvulkan_freedreno.so";

std::string ext_dir()     { return std::string("/sdcard/Android/data/") + kPkg + "/files"; }
std::string priv_dir()    { return std::string("/data/data/") + kPkg + "/files"; }
std::string conf_path()   { return ext_dir() + "/" + kConfName; }
std::string drivers_dir() { return ext_dir() + "/drivers"; }

void log_ready(const std::string& p) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "custom driver ready: %s", p.c_str());
    FILE* f = fopen((ext_dir() + "/vulkan_shim.log").c_str(), "ae");
    if (!f) return;
    fprintf(f, "custom driver ready: %s\n", p.c_str());
    fclose(f);
}

bool is_file(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

bool mkdirs(const std::string& p) {
    std::string cur;
    for (size_t i = 0; i < p.size(); ++i) {
        cur += p[i];
        if (p[i] != '/' && i + 1 != p.size()) continue;
        if (cur == "/") continue;
        mkdir(cur.c_str(), 0770);
    }
    return true;
}

bool elf_ok(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    unsigned char h[20]{};
    size_t n = fread(h, 1, 20, f);
    fclose(f);
    if (n < 20) return false;
    if (h[0] != 0x7F || h[1] != 'E' || h[2] != 'L' || h[3] != 'F') return false;
    if (h[4] != 2 || h[5] != 1) return false;
    return h[18] == 0xB7 && h[19] == 0x00;
}

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool ends_with(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

struct Config {
    bool        enabled    = false;
    bool        autoDetect = true;
    std::string dir;
    std::string name;
};

void write_config_if_missing() {
    mkdirs(ext_dir());
    mkdirs(drivers_dir());
    if (is_file(conf_path())) return;

    std::ofstream f(conf_path());
    if (!f) return;
    f << "enable_custom_driver=0\n"
      << "auto_detect=1\n"
      << "driver_dir=" << drivers_dir() << "\n"
      << "driver_name=" << kDefaultSo << "\n";
}

Config read_config() {
    Config c;
    std::ifstream f(conf_path());
    if (!f) return c;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string k = trim(line.substr(0, pos));
        std::string v = trim(line.substr(pos + 1));
        if (k == "enable_custom_driver")
            c.enabled = (v == "1" || v == "true" || v == "TRUE");
        else if (k == "auto_detect")
            c.autoDetect = (v == "1" || v == "true" || v == "TRUE");
        else if (k == "driver_dir")
            c.dir = v;
        else if (k == "driver_name")
            c.name = v;
    }
    return c;
}

std::string scan_single_so(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return {};

    std::string found;
    int count = 0;
    dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        std::string n = e->d_name;
        if (!ends_with(n, ".so")) continue;
        if (!is_file(dir + "/" + n)) continue;
        found = n;
        if (++count > 1) break;
    }
    closedir(d);
    return count == 1 ? found : std::string{};
}

void auto_detect(Config& c) {
    if (c.enabled || !c.autoDetect) return;

    std::string dir  = c.dir.empty() ? drivers_dir() : c.dir;
    std::string name = scan_single_so(dir);
    if (name.empty()) return;

    c.enabled = true;
    c.dir     = dir;
    c.name    = name;
}

bool copy_file(const std::string& src, const std::string& dst) {
    FILE* in = fopen(src.c_str(), "rb");
    if (!in) return false;
    FILE* out = fopen(dst.c_str(), "wb");
    if (!out) { fclose(in); return false; }

    bool ok = true;
    char buf[65536];
    size_t r;
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
    write_config_if_missing();

    Config c = read_config();
    auto_detect(c);

    if (!c.enabled || c.dir.empty() || c.name.empty()) return {};

    std::string src = c.dir + "/" + c.name;
    if (!is_file(src)) return {};
    if (!elf_ok(src)) return {};

    mkdirs(priv_dir());
    std::string dst = priv_dir() + "/" + kPrivateSo;

    if (!copy_file(src, dst) || !is_file(dst) || !elf_ok(dst)) {
        unlink(dst.c_str());
        return {};
    }

    log_ready(dst);
    return dst;
}

}
