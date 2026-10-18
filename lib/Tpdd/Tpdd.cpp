#include "Tpdd.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tpdd {
namespace {
int fileStat(const char* path, struct stat* st) {
#ifdef ARDUINO
    // ESP-IDF's FAT VFS has no symlinks and does not expose lstat.
    return stat(path, st);
#else
    return lstat(path, st);
#endif
}
std::vector<uint8_t> status(uint8_t code) { return packet(0x12, &code, 1); }
std::string join(const std::string& a, const std::string& b) {
    return a + (a == "/" ? "" : "/") + b;
}
std::string wireName(const uint8_t* data) {
    std::string s(reinterpret_cast<const char*>(data), 24);
    auto end = s.find('\0');
    if (end != std::string::npos) s.resize(end);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    auto dot = s.find('.');
    if (dot != std::string::npos) {
        auto base = s.substr(0, dot), ext = s.substr(dot + 1);
        while (!base.empty() && base.back() == ' ') base.pop_back();
        s = base + "." + ext;
    }
    std::string out;
    return Drive::clientName(s, out) ? out : "";
}
}
std::vector<uint8_t> packet(uint8_t type, const uint8_t* data, size_t length) {
    std::vector<uint8_t> out{type, static_cast<uint8_t>(length)};
    uint8_t sum = type + static_cast<uint8_t>(length);
    for (size_t i = 0; i < length; ++i) { out.push_back(data[i]); sum += data[i]; }
    out.push_back(static_cast<uint8_t>(~sum));
    return out;
}
bool Drive::safePath(const std::string& p) {
    if (p.empty() || p.front() != '/' || p.size() > 190 || p.find('\0') != std::string::npos)
        return false;
    if (p == "/") return true;
    if (p.back() == '/' || p.find('\\') != std::string::npos) return false;
    size_t pos = 1;
    while (pos < p.size()) {
        auto end = p.find('/', pos);
        auto part = p.substr(pos, end == std::string::npos ? end : end - pos);
        if (part.empty() || part == "." || part == "..") return false;
        pos = end == std::string::npos ? p.size() : end + 1;
    }
    return true;
}
bool Drive::clientName(const std::string& s, std::string& result) {
    auto dot = s.find('.');
    size_t base = dot == std::string::npos ? s.size() : dot;
    size_t ext = dot == std::string::npos ? 0 : s.size() - dot - 1;
    if (!base || base > 6 || ext > 2 || (dot != std::string::npos && !ext)) return false;
    result.clear();
    for (unsigned char c : s) {
        if (!(std::isalnum(c) && c < 128) && c != '_' && c != '-' && c != '#' && c != '.')
            return false;
        result += static_cast<char>(std::toupper(c));
    }
    return result.find('.', dot == std::string::npos ? result.size() : dot + 1) == std::string::npos;
}
const Entry* Drive::find(unsigned b, const std::string& name) const {
    for (const auto& f : banks[b].files) if (f.name == name) return &f;
    return nullptr;
}
uint8_t Drive::freeSectors(unsigned b) const {
    uint32_t used = 0;
    for (const auto& e : banks[b].files) used += (e.size + SectorSize - 1) / SectorSize;
    return used < 80 ? 80 - used : 0;
}
bool Drive::refresh(unsigned b) {
    if (b > 1) { error = "Invalid bank"; return false; }
    Bank& bank = banks[b];
    DIR* dir = opendir(bank.path.c_str());
    if (!dir) { error = "Folder unavailable"; return false; }
    std::vector<Entry> entries;
    size_t skipped = 0;
    bool ok = true;
    errno = 0;
    while (auto* item = readdir(dir)) {
        std::string name = item->d_name, client;
        if (name == "." || name == "..") continue;
        struct stat st{};
        if (fileStat(join(bank.path, name).c_str(), &st) != 0) {
            error = "Cannot inspect file"; ok = false; break;
        }
        if (S_ISDIR(st.st_mode)) continue;
        if (!S_ISREG(st.st_mode) || !clientName(name, client)) { ++skipped; continue; }
        if (st.st_size < 0 || static_cast<uint64_t>(st.st_size) > MaxFileSize) {
            error = "File exceeds 65534 bytes"; ok = false; break;
        }
        if (entries.size() == MaxFiles) { error = "More than 40 supported files"; ok = false; break; }
        for (const auto& e : entries) if (e.name == client) {
            error = "Client filename collision"; ok = false; break;
        }
        if (!ok) break;
        entries.push_back({name, client, static_cast<uint32_t>(st.st_size)});
        errno = 0;
    }
    if (ok && errno) { error = "Directory read failed"; ok = false; }
    closedir(dir);
    if (!ok) return false;
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& c) { return a.name < c.name; });
    bank.files = std::move(entries); bank.skipped = skipped;
    return true;
}
bool Drive::mount(unsigned b, const std::string& path, bool ro) {
    if (b > 1) return false;
    if (busy()) { error = "Close client file first"; return false; }
    if (!safePath(path)) { error = "Invalid folder path"; return false; }
    struct stat st{};
    // Inspect every component so native tests cannot escape through symlinks.
    for (size_t end = 1; end <= path.size(); ++end) {
        if (end != path.size() && path[end] != '/') continue;
        if (fileStat(path.substr(0,end).c_str(), &st) || !S_ISDIR(st.st_mode)) {
            error = "Folder not found"; return false;
        }
    }
    auto other = banks[1-b].path, candidate = path;
#ifdef ARDUINO
    // FAT paths are case-insensitive even if a saved spelling changed on a PC.
    std::transform(other.begin(),other.end(),other.begin(),[](unsigned char c){return std::tolower(c);});
    std::transform(candidate.begin(),candidate.end(),candidate.begin(),[](unsigned char c){return std::tolower(c);});
#endif
    if (other == candidate) { error = "Folder already in other bank"; return false; }
    Bank old = banks[b]; banks[b] = Bank{}; banks[b].path = path; banks[b].readOnly = ro;
    if (!refresh(b)) { banks[b] = std::move(old); return false; }
    activity = "Folder mounted"; error.clear(); return true;
}
bool Drive::unmount(unsigned b) {
    if (b > 1 || busy()) { error = "Close client file first"; return false; }
    banks[b] = Bank{}; error.clear(); return true;
}
bool Drive::setReadOnly(unsigned b, bool value) {
    if (b > 1 || busy()) { error = "Close client file first"; return false; }
    banks[b].readOnly = value; error.clear(); return true;
}
bool Drive::setServing(bool value) {
    if (busy()) { error = "Close client file first"; return false; }
    serving = value; activity = value ? "Waiting for requests" : "Stopped"; error.clear(); return true;
}
bool Drive::closeFile() {
    if (fd_ < 0) return true;
    bool ok = mode_ == 3 || fsync(fd_) == 0;
    if (close(fd_) != 0) ok = false;
    fd_ = -1; openBank_ = -1; mode_ = 0; readRemaining_ = 0; openPath_.clear();
    return ok;
}
Drive::~Drive() { closeFile(); }
void Drive::abortSession(const std::string& reason) {
    const bool closed = closeFile();
    serving = false;
    error = reason + (closed ? "" : "; close failed");
    activity = "Stopped; partial files may remain";
}
std::vector<uint8_t> Drive::dirReply(unsigned b, const Entry* e) const {
    uint8_t data[28]{};
    if (e) {
        std::fill(data, data + 24, ' ');
        auto dot = e->name.find('.');
        auto base = e->name.substr(0, dot);
        std::copy(base.begin(), base.end(), data);
        if (dot != std::string::npos) {
            data[6] = '.';
            std::copy(e->name.begin() + dot + 1, e->name.end(), data + 7);
        }
        data[24] = 'F'; data[25] = e->size >> 8; data[26] = e->size & 255;
    }
    data[27] = freeSectors(b);
    return packet(0x11, data, sizeof(data));
}
std::vector<uint8_t> Drive::request(uint8_t command, const uint8_t* data, size_t n) {
    ++requests;
    if (!serving) return status(0x70);
    const unsigned b = (command & 0x40) ? 1 : 0;
    const uint8_t cmd = command & ~0x40;
    if (command & 0x80) return status(0x36);
    const size_t expected = cmd == 0 ? 26 : cmd == 1 ? 1 : cmd == 0x0d ? 25 : 0;
    if ((cmd == 4 && (n == 0 || n > 128)) || (cmd != 4 && n != expected)) return status(0x36);
    if (cmd == 0x23 && !b) {
        // TPDD2's documented version/geometry record; sector commands remain unsupported.
        const uint8_t version[] = {0x41,0x10,1,0,0x50,5,0,2,0,0x28,0,0xe1,0,0,0};
        return packet(0x14, version, sizeof(version));
    }
    if (cmd == 7) return status(0);
    Bank& bank = banks[b];
    if (cmd == 0x0c && !b) {
        uint8_t flags = (banks[0].path.empty() ? 4 : 0) | (banks[0].readOnly ? 2 : 0);
        return packet(0x15, &flags, 1);
    }
    if (bank.path.empty()) return status(0x70);
    if (cmd == 6 || cmd == 8 || cmd >= 0x23) return status(cmd == 6 ? 0x50 : 0x36);
    if (cmd == 0) {
        uint8_t action = data[25];
        if (action == 4) { bank.cursor = -1; return {}; } // TPDD2 sends no reply.
        if (action > 4) return status(0x36);
        if (action == 0 || action == 1) {
            if (!refresh(b)) return status(0x31);
        }
        if (action == 0) {
            bank.selected = wireName(data);
            if (bank.selected.empty()) return status(0x30);
            return dirReply(b, find(b, bank.selected));
        }
        if (action == 1) bank.cursor = 0;
        if (action == 2 && bank.cursor < static_cast<int>(bank.files.size())) ++bank.cursor;
        if (action == 3 && bank.cursor >= 0) --bank.cursor;
        return dirReply(b, bank.cursor >= 0 && static_cast<size_t>(bank.cursor) < bank.files.size()
                            ? &bank.files[bank.cursor] : nullptr);
    }
    if (cmd == 2) {
        if (busy() && openBank_ != static_cast<int>(b)) return status(0x35);
        bool ok = closeFile();
        if (!refresh(b)) ok = false;
        activity = ok ? "File closed" : "Close failed";
        return status(ok ? 0 : 0x49);
    }
    if (cmd == 1) {
        if (data[0] < 1 || data[0] > 3) return status(0x36);
        if (busy()) return status(0x37);
        if (bank.selected.empty()) return status(0x30);
        if (!refresh(b)) return status(0x31);
        auto* entry = find(b, bank.selected);
        if (data[0] != 3 && bank.readOnly) return status(0x50);
        if (data[0] == 1 && entry) return status(0x11);
        if (data[0] != 1 && !entry) return status(0x10);
        if (data[0] == 1 && bank.files.size() >= MaxFiles) return status(0x60);
        const auto name = entry ? entry->local : bank.selected;
        int flags = data[0] == 3 ? O_RDONLY : data[0] == 2 ? O_WRONLY | O_APPEND : O_WRONLY | O_CREAT | O_EXCL;
        openPath_ = join(bank.path, name);
        fd_ = open(openPath_.c_str(), flags, 0666);
        if (fd_ < 0) return status(0x37);
        openBank_ = b; mode_ = data[0];
        readRemaining_ = mode_ == 3 ? entry->size : 0;
        activity = "Opened " + bank.selected;
        return status(0);
    }
    if (cmd == 3 || cmd == 4) {
        if (!busy()) return status(0x37);
        if (openBank_ != static_cast<int>(b)) return status(0x35);
        if (cmd == 3) {
            if (mode_ != 3) return status(0x37);
            uint8_t buffer[128]; auto count = read(fd_, buffer, sizeof(buffer));
            if (count < 0 || static_cast<uint32_t>(count) != std::min<uint32_t>(readRemaining_,128)) {
                abortSession("SD read failed or file changed"); return status(0x49);
            }
            readRemaining_ -= count;
            bytesRead += count; activity = "Reading " + bank.selected;
            return packet(0x10, buffer, count);
        }
        if (mode_ == 3) return status(0x37);
        if (bank.readOnly) return status(0x50);
        struct stat st{};
        if (fstat(fd_, &st)) return status(0x49);
        if (static_cast<uint64_t>(st.st_size) + n > MaxFileSize) return status(0x6e);
        if (!refresh(b)) return status(0x31);
        uint32_t used = 0;
        for (const auto& e : bank.files) if (join(bank.path, e.local) != openPath_)
            used += (e.size + SectorSize - 1) / SectorSize;
        if (used + (st.st_size + n + SectorSize - 1) / SectorSize > 80) return status(0x61);
        if (write(fd_, data, n) != static_cast<ssize_t>(n) || fsync(fd_) != 0) {
            abortSession("SD write failed"); return status(0x49);
        }
        bytesWritten += n; activity = "Writing " + bank.selected;
        return status(0);
    }
    if (cmd == 5 || cmd == 0x0d) {
        if (busy()) return status(0x37);
        if (bank.readOnly) return status(0x50);
        if (!refresh(b)) return status(0x31);
        auto* entry = find(b, bank.selected);
        if (!entry) return status(0x10);
        auto path = join(bank.path, entry->local);
        if (cmd == 5) {
            if (unlink(path.c_str())) return status(0x49);
        } else {
            auto target = wireName(data);
            if (target.empty()) return status(0x30);
            if (find(b, target)) return status(0x11);
            if (rename(path.c_str(), join(bank.path, target).c_str())) return status(0x49);
            bank.selected = target;
        }
        if (!refresh(b)) return status(0x31);
        activity = cmd == 5 ? "File deleted" : "File renamed";
        return status(0);
    }
    return status(0x36);
}
void Parser::expire(uint32_t now) {
    if (partial() && static_cast<uint32_t>(now - last_) > 1000) { ++drive_.badFrames; reset(); }
}
void Parser::feed(uint8_t byte, uint32_t now) {
    expire(now); last_ = now;
    if (state_ < 2) {
        if (byte == 0x5a) ++state_; else state_ = 0;
        return;
    }
    frame_[used_++] = byte;
    if (used_ == 2) expected_ = static_cast<size_t>(frame_[1]) + 3;
    if (!expected_ || used_ < expected_) return;
    uint8_t sum = 0;
    for (size_t i = 0; i < used_; ++i) sum += frame_[i];
    if (sum == 0xff) {
        auto response = drive_.request(frame_[0], frame_.data() + 2, frame_[1]);
        if (!response.empty()) send_(response);
    } else ++drive_.badFrames;
    reset();
}
}
