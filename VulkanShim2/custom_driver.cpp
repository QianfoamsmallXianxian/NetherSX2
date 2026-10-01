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

std::string ext()  { return "/sdcard/Android/data/" + std::string(PKG) + "/files"; }
std::string priv() { return "/data/data/" + std::string(PKG) + "/files"; }
std::string conf() { return ext() + "/driver.conf"; }
std::string dirs() { return ext() + "/drivers"; }

bool file_ok(const std::string& p) {
    struct stat s{};
    return stat(p.c_str(), &s) == 0 && S_ISREG(s.st_mode) && s.st_size > 0;
}

long long fsize(const std::string& p) {
    struct stat s{};
    return stat(p.c_str(), &s) == 0 ? (long long)s.st_size : 0;
}

void mkdirs(const std::string& p) {
    std::string c;
    for (size_t i = 0; i < p.size(); ++i) {
        c += p[i];
        if (p[i] == '/' || i + 1 == p.size()) {
            if (c != "/") mkdir(c.c_str(), 0770);
        }
    }
}

bool elf_ok(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    unsigned char h[20]{};
    size_t n = fread(h, 1, 20, f);
    fclose(f);
    return n >= 20 && h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F'
        && h[4] == 2 && h[5] == 1 && h[18] == 0xB7 && h[19] == 0x00;
}

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
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

std::string read_field(const char* key) {
    std::ifstream in(conf());
    if (!in) return {};
    size_t klen = strlen(key);
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.rfind(key, 0) != 0 || t.size() <= klen) continue;
        return trim(t.substr(klen));
    }
    return {};
}

void save_driver_name(const std::string& newname) {
    if (read_field("driver_name=") == newname) return;
    std::ifstream in(conf());
    if (!in) return;
    std::string body, line;
    bool replaced = false;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (!replaced && t.rfind("driver_name=", 0) == 0) {
            body += "driver_name=" + newname + "\n";
            replaced = true;
        } else {
            body += line + "\n";
        }
    }
    if (!replaced) body += "driver_name=" + newname + "\n";
    std::string tmp = conf() + ".tmp";
    {
        std::ofstream o(tmp, std::ios::trunc);
        if (!o) return;
        o << body;
    }
    rename(tmp.c_str(), conf().c_str());
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

std::string newest_so(const std::string& dir) {
    DIR* dp = opendir(dir.c_str());
    if (!dp) return {};
    std::string best;
    time_t bt = 0;
    dirent* e;
    while ((e = readdir(dp))) {
        if (e->d_name[0] == '.') continue;
        std::string n = e->d_name;
        if (n.size() < 4 || n.compare(n.size() - 3, 3, ".so") != 0) continue;
        std::string full = dir + "/" + n;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
        if (st.st_mtime > bt || (st.st_mtime == bt && !best.empty() && n > best)) {
            bt = st.st_mtime;
            best = n;
        }
    }
    closedir(dp);
    return best;
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

}  // namespace

std::string resolve_custom_driver() {
    mkdirs(ext());
    mkdirs(dirs());
    if (!file_ok(conf())) {
        std::ofstream f(conf());
        if (f) f << "enable_custom_driver=0\n"
                 << "auto_detect=1\n"
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

    if (!enabled && autodetect) {
        std::string found = newest_so(dir);
        if (!found.empty()) {
            enabled = true;
            name = found;
        }
    }

    std::string src = dir + "/" + name;
    if (!enabled || name.empty() || !file_ok(src) || !elf_ok(src)) {
        if (autodetect) save_driver_name(DEF);
        return {};
    }

    mkdirs(priv());
    std::string dst = priv() + "/custom_driver.so";
    if (!copy_file(src, dst) || !file_ok(dst) || !elf_ok(dst)) {
        unlink(dst.c_str());
        if (autodetect) save_driver_name(DEF);
        return {};
    }

    log_ready(name, dst, meta_version(dir), fsize(dst));
    if (autodetect) save_driver_name(name);
    return dst;
}

}  // namespace custom_driver
