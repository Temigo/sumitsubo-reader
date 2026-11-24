#pragma once
// The study core's files, on the device's card.
#include <HalStorage.h>
#include <Store.h>

#include <string>

class SdStore final : public sumitsubo::Store {
 public:
  bool read(const std::string& path, std::string& out) override {
    HalFile f = Storage.open(path.c_str(), O_RDONLY);
    if (!f) return false;
    out.clear();
    char buf[256];
    int n;
    while ((n = f.read(buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
    f.close();
    return true;
  }
  bool append(const std::string& path, const std::string& text) override {
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) Storage.ensureDirectoryExists(path.substr(0, slash).c_str());
    HalFile f = Storage.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
    if (!f) return false;
    const bool ok = f.write(text.data(), text.size()) == text.size();
    f.flush();
    f.close();
    return ok;
  }
  bool write(const std::string& path, const std::string& text) override {
    Storage.remove(path.c_str());
    return append(path, text);
  }
  uint32_t size(const std::string& path) override {
    HalFile f = Storage.open(path.c_str(), O_RDONLY);
    if (!f) return 0;
    const auto n = static_cast<uint32_t>(f.size());
    f.close();
    return n;
  }
  bool lines(const std::string& path, const std::function<bool(uint32_t, const std::string&)>& each,
             const uint32_t from = 0) override {
    HalFile f = Storage.open(path.c_str(), O_RDONLY);
    if (!f) return false;
    if (from && !f.seekSet(from)) {
      f.close();
      return false;
    }
    std::string line;
    char buf[512];
    uint32_t base = from, lineStart = from;
    int n;
    bool go = true;
    while (go && (n = f.read(buf, sizeof buf)) > 0) {
      for (int i = 0; i < n && go; ++i) {
        if (buf[i] == '\n') {
          go = each(lineStart, line);
          line.clear();
          lineStart = base + static_cast<uint32_t>(i) + 1;
        } else if (buf[i] != '\r') {
          line += buf[i];
        }
      }
      base += static_cast<uint32_t>(n);
    }
    if (go && !line.empty()) each(lineStart, line);
    f.close();
    return true;
  }
  bool lineAt(const std::string& path, const uint32_t offset, std::string& out) override {
    HalFile f = Storage.open(path.c_str(), O_RDONLY);
    if (!f || !f.seekSet(offset)) return false;
    out.clear();
    char buf[512];
    int n;
    bool done = false;
    while (!done && (n = f.read(buf, sizeof buf)) > 0) {
      for (int i = 0; i < n; ++i) {
        if (buf[i] == '\n') {
          done = true;
          break;
        }
        if (buf[i] != '\r') out += buf[i];
      }
    }
    f.close();
    return !out.empty();
  }
  bool readAt(const std::string& path, const uint32_t offset, const size_t n, std::string& out) override {
    HalFile f = Storage.open(path.c_str(), O_RDONLY);
    if (!f || !f.seekSet(offset)) return false;
    out.resize(n);
    const int got = n ? f.read(&out[0], n) : 0;
    f.close();
    return got == static_cast<int>(n);
  }
};
