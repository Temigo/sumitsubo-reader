#pragma once
// The files the study core reads and writes, supplied by its host: the card on a device,
// a folder in the tests. Paths are absolute on the card, e.g. "/sumitsubo/deck/cards.tsv".
#include <cstdint>
#include <functional>
#include <string>

namespace sumitsubo {
struct Store {
  virtual ~Store() = default;
  virtual bool read(const std::string& path, std::string& out) = 0;  // a whole small file
  virtual bool append(const std::string& path, const std::string& text) = 0;
  virtual bool write(const std::string& path, const std::string& text) = 0;  // replaces the file
  virtual uint32_t size(const std::string& path) = 0;                        // 0 when there is no such file
  // Every line of a file from `from` on, with the offset it starts at; `each` returns false to stop.
  virtual bool lines(const std::string& path, const std::function<bool(uint32_t, const std::string&)>& each,
                     uint32_t from = 0) = 0;
  virtual bool lineAt(const std::string& path, uint32_t offset, std::string& out) = 0;
  // Exactly n bytes from an offset; false when the file ends before that.
  virtual bool readAt(const std::string& path, uint32_t offset, size_t n, std::string& out) = 0;
};
}  // namespace sumitsubo
