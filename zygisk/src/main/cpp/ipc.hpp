#pragma once

#include <cstdint>
#include <string>
#include <unistd.h>
#include <vector>

namespace pif::ipc {

inline ssize_t xread(int fd, void *buffer, size_t countToRead) {
    ssize_t totalRead = 0;
    auto *currentBuf = static_cast<char *>(buffer);
    size_t remainingBytes = countToRead;

    while (remainingBytes > 0) {
        const ssize_t ret = TEMP_FAILURE_RETRY(read(fd, currentBuf, remainingBytes));
        if (ret < 0) {
            return -1;
        }
        if (ret == 0) {
            break;
        }

        currentBuf += ret;
        totalRead += ret;
        remainingBytes -= ret;
    }

    return totalRead;
}

inline ssize_t xwrite(int fd, const void *buffer, size_t countToWrite) {
    ssize_t totalWritten = 0;
    const auto *currentBuf = static_cast<const char *>(buffer);
    size_t remainingBytes = countToWrite;

    while (remainingBytes > 0) {
        const ssize_t ret = TEMP_FAILURE_RETRY(write(fd, currentBuf, remainingBytes));
        if (ret < 0) {
            return -1;
        }
        if (ret == 0) {
            break;
        }

        currentBuf += ret;
        totalWritten += ret;
        remainingBytes -= ret;
    }

    return totalWritten;
}

inline bool readExact(int fd, void *buffer, size_t size) {
    return xread(fd, buffer, size) == static_cast<ssize_t>(size);
}

inline bool writeExact(int fd, const void *buffer, size_t size) {
    return xwrite(fd, buffer, size) == static_cast<ssize_t>(size);
}

constexpr uint32_t kMaxVectorSize = 8 * 1024 * 1024;

inline bool writeVector(int fd, const std::vector<uint8_t> &buffer) {
    if (buffer.size() > kMaxVectorSize) {
        return false;
    }
    const uint32_t size = static_cast<uint32_t>(buffer.size());
    if (!writeExact(fd, &size, sizeof(size))) {
        return false;
    }
    return size == 0 || writeExact(fd, buffer.data(), size);
}

inline bool readVector(int fd, std::vector<uint8_t> &buffer) {
    uint32_t size = 0;
    if (!readExact(fd, &size, sizeof(size))) {
        return false;
    }
    if (size > kMaxVectorSize) {
        return false;
    }

    buffer.resize(size);
    return size == 0 || readExact(fd, buffer.data(), size);
}

inline bool writeBool(int fd, bool value) {
    const uint8_t byte = value ? 1 : 0;
    return writeExact(fd, &byte, sizeof(byte));
}

inline bool readBool(int fd, bool &value) {
    uint8_t byte = 0;
    if (!readExact(fd, &byte, sizeof(byte))) {
        return false;
    }
    value = byte != 0;
    return true;
}

inline bool writeString(int fd, const std::string &value) {
    const uint32_t size = static_cast<uint32_t>(value.size());
    if (!writeExact(fd, &size, sizeof(size))) {
        return false;
    }
    return size == 0 || writeExact(fd, value.data(), size);
}

inline bool readString(int fd, std::string &value) {
    std::vector<uint8_t> bytes;
    if (!readVector(fd, bytes)) {
        return false;
    }
    value.assign(bytes.begin(), bytes.end());
    return true;
}

} // namespace pif::ipc
