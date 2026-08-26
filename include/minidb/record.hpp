#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "minidb/row.hpp"

namespace minidb {

// A row on disk is a flat byte string: one type tag per column, followed by
// that column's payload. Nothing is aligned and nothing is padded, so the
// encoding is identical on every platform and a row costs exactly what it needs.
//
//   tag 0  NULL     no payload
//   tag 1  INTEGER  8 bytes, little-endian two's complement
//   tag 2  REAL     8 bytes, little-endian IEEE-754
//   tag 3  TEXT     4-byte length, then that many bytes
//
// The column count is not stored: the caller knows the schema, and a short row
// (written before a column was added) simply decodes to fewer values.

std::size_t encoded_size(const Row& row);

void encode_row(const Row& row, std::vector<std::uint8_t>& out);

std::vector<std::uint8_t> encode_row(const Row& row);

// Decodes until `size` bytes are consumed. Throws DbError on a malformed record.
Row decode_row(const std::uint8_t* data, std::size_t size);

}  // namespace minidb
