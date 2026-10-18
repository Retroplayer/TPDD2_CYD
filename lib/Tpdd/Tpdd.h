#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace tpdd {
constexpr size_t MaxFiles = 40, MaxFileSize = 65534, SectorSize = 1280;
struct Entry { std::string local, name; uint32_t size = 0; };
struct Bank {
    std::string path, selected;
    std::vector<Entry> files;
    size_t skipped = 0;
    int cursor = -1;
    bool readOnly = true;
};
// POSIX VFS is shared by the native tests and ESP32 FAT SD mount.
class Drive {
public:
    std::array<Bank, 2> banks;
    std::string error, activity = "Stopped";
    bool serving = false;
    uint32_t requests = 0, badFrames = 0, bytesRead = 0, bytesWritten = 0;
    bool mount(unsigned bank, const std::string& path, bool readOnly);
    bool unmount(unsigned bank);
    bool setReadOnly(unsigned bank, bool value);
    bool setServing(bool value);
    void abortSession(const std::string& reason);
    bool busy() const { return fd_ >= 0; }
    bool refresh(unsigned bank);
    static bool clientName(const std::string& local, std::string& result);
    static bool safePath(const std::string& path);
    std::vector<uint8_t> request(uint8_t command, const uint8_t* data, size_t length);
    ~Drive();
private:
    int fd_ = -1, openBank_ = -1;
    uint8_t mode_ = 0;
    uint32_t readRemaining_ = 0;
    std::string openPath_;
    const Entry* find(unsigned bank, const std::string& name) const;
    uint8_t freeSectors(unsigned bank) const;
    bool closeFile();
    std::vector<uint8_t> dirReply(unsigned bank, const Entry* entry) const;
};
std::vector<uint8_t> packet(uint8_t type, const uint8_t* data, size_t length);
// Bounded byte-stream parser, including inter-byte timeout and checksum validation.
class Parser {
public:
    using Send = std::function<void(const std::vector<uint8_t>&)>;
    Parser(Drive& drive, Send send) : drive_(drive), send_(send) {}
    void feed(uint8_t byte, uint32_t now);
    void expire(uint32_t now);
    bool partial() const { return state_ != 0; }
    void reset() { state_ = 0; used_ = expected_ = 0; }
private:
    Drive& drive_;
    Send send_;
    std::array<uint8_t, 258> frame_{};
    size_t used_ = 0, expected_ = 0;
    unsigned state_ = 0;
    uint32_t last_ = 0;
};
}
