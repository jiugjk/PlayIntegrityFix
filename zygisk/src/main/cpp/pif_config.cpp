#include "pif_config.hpp"
#include "ipc.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace pif {
    namespace {
        std::string trim(std::string_view value) {
            const auto start = value.find_first_not_of(" \t\r\n");
            if (start == std::string_view::npos) {
                return {};
            }

            const auto end = value.find_last_not_of(" \t\r\n");
            return std::string(value.substr(start, end - start + 1));
        }

        bool parseBool(std::string_view value) {
            return value == "1" || value == "true";
        }

        std::vector<std::string> splitFingerprint(std::string_view fingerprint) {
            std::vector<std::string> parts;
            std::string current;
            current.reserve(fingerprint.size());

            for (const char ch : fingerprint) {
                if (ch == '/' || ch == ':') {
                    parts.emplace_back(current);
                    current.clear();
                    continue;
                }

                current.push_back(ch);
            }

            parts.emplace_back(current);
            return parts;
        }

        void expandFingerprint(Config &config, std::string_view fingerprint) {
            const auto parts = splitFingerprint(fingerprint);
            static constexpr std::array<std::string_view, 8> keys = {
                "BRAND",
                "PRODUCT",
                "DEVICE",
                "RELEASE",
                "ID",
                "INCREMENTAL",
                "TYPE",
                "TAGS",
            };

            for (size_t i = 0; i < keys.size(); ++i) {
                config.propMap[std::string(keys[i])] = i < parts.size() ? parts[i] : "";
            }
        }
    }

    Config parseConfig(std::string_view content) {
        Config config;
        std::unordered_map<std::string, std::string> rawMap;

        size_t lineStart = 0;
        while (lineStart <= content.size()) {
            const auto lineEnd = content.find('\n', lineStart);
            const auto rawLine = content.substr(lineStart, lineEnd == std::string_view::npos
                                                            ? content.size() - lineStart
                                                            : lineEnd - lineStart);

            auto line = rawLine;
            if (const auto comment = line.find('#'); comment != std::string_view::npos) {
                line = line.substr(0, comment);
            }

            const auto trimmed = trim(line);
            if (!trimmed.empty()) {
                const auto eq = trimmed.find('=');
                if (eq != std::string::npos) {
                    rawMap.emplace(trim(trimmed.substr(0, eq)), trim(trimmed.substr(eq + 1)));
                }
            }

            if (lineEnd == std::string_view::npos) {
                break;
            }
            lineStart = lineEnd + 1;
        }

        if (const auto it = rawMap.find("spoofVendingSdk"); it != rawMap.end()) {
            config.spoofVendingSdk = parseBool(it->second);
            rawMap.erase(it);
        }
        if (const auto it = rawMap.find("spoofVendingBuild"); it != rawMap.end()) {
            config.spoofVendingBuild = parseBool(it->second);
            rawMap.erase(it);
        }
        if (const auto it = rawMap.find("DEVICE_INITIAL_SDK_INT"); it != rawMap.end()) {
            config.deviceInitialSdkInt = it->second;
            rawMap.erase(it);
        }
        if (const auto it = rawMap.find("spoofBuild"); it != rawMap.end()) {
            config.spoofBuild = parseBool(it->second);
            rawMap.erase(it);
        }
        if (const auto it = rawMap.find("spoofProvider"); it != rawMap.end()) {
            config.spoofProvider = parseBool(it->second);
            rawMap.erase(it);
        }
        if (const auto it = rawMap.find("spoofProps"); it != rawMap.end()) {
            config.spoofProps = parseBool(it->second);
            rawMap.erase(it);
        }
        if (const auto it = rawMap.find("spoofSignature"); it != rawMap.end()) {
            config.spoofSignature = parseBool(it->second);
            rawMap.erase(it);
        }
        if (const auto it = rawMap.find("DEBUG"); it != rawMap.end()) {
            config.debug = parseBool(it->second);
            rawMap.erase(it);
        }
        if (const auto it = rawMap.find("SECURITY_PATCH"); it != rawMap.end()) {
            config.securityPatch = it->second;
        }
        if (const auto it = rawMap.find("ID"); it != rawMap.end()) {
            config.buildId = it->second;
        }

        config.propMap = std::move(rawMap);
        if (const auto it = config.propMap.find("FINGERPRINT"); it != config.propMap.end()) {
            expandFingerprint(config, it->second);
        }
        if (config.buildId.empty()) {
            if (const auto it = config.propMap.find("ID"); it != config.propMap.end()) {
                config.buildId = it->second;
            }
        }

        return config;
    }

    bool writeConfig(int fd, const Config &config) {
        bool ok = ipc::writeBool(fd, config.spoofBuild);
        ok = ok && ipc::writeBool(fd, config.spoofProps);
        ok = ok && ipc::writeBool(fd, config.spoofProvider);
        ok = ok && ipc::writeBool(fd, config.spoofSignature);
        ok = ok && ipc::writeBool(fd, config.debug);
        ok = ok && ipc::writeString(fd, config.deviceInitialSdkInt);
        ok = ok && ipc::writeString(fd, config.securityPatch);
        ok = ok && ipc::writeString(fd, config.buildId);
        ok = ok && ipc::writeBool(fd, config.spoofVendingSdk);
        ok = ok && ipc::writeBool(fd, config.spoofVendingBuild);

        const uint32_t propCount = static_cast<uint32_t>(config.propMap.size());
        ok = ok && ipc::writeExact(fd, &propCount, sizeof(propCount));
        for (const auto &[key, value] : config.propMap) {
            ok = ok && ipc::writeString(fd, key);
            ok = ok && ipc::writeString(fd, value);
        }

        return ok;
    }

    bool readConfig(int fd, Config &config) {
        Config parsed;
        bool ok = ipc::readBool(fd, parsed.spoofBuild);
        ok = ok && ipc::readBool(fd, parsed.spoofProps);
        ok = ok && ipc::readBool(fd, parsed.spoofProvider);
        ok = ok && ipc::readBool(fd, parsed.spoofSignature);
        ok = ok && ipc::readBool(fd, parsed.debug);
        ok = ok && ipc::readString(fd, parsed.deviceInitialSdkInt);
        ok = ok && ipc::readString(fd, parsed.securityPatch);
        ok = ok && ipc::readString(fd, parsed.buildId);
        ok = ok && ipc::readBool(fd, parsed.spoofVendingSdk);
        ok = ok && ipc::readBool(fd, parsed.spoofVendingBuild);

        uint32_t propCount = 0;
        ok = ok && ipc::readExact(fd, &propCount, sizeof(propCount));
        for (uint32_t i = 0; ok && i < propCount; ++i) {
            std::string key;
            std::string value;
            ok = ipc::readString(fd, key);
            ok = ok && ipc::readString(fd, value);
            if (ok) {
                parsed.propMap.emplace(std::move(key), std::move(value));
            }
        }

        if (!ok) {
            return false;
        }

        config = std::move(parsed);
        return true;
    }
}
