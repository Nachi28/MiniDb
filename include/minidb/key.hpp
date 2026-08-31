#pragma once

#include <cstdint>
#include <string>

#include "minidb/slice.hpp"
#include "minidb/value.hpp"

namespace minidb {

// Order-preserving key encodings: memcmp over the encoded bytes reproduces SQL
// ordering exactly, which is what lets one byte-keyed B+tree serve both the
// rowid-ordered table trees and the value-ordered index trees.
//
// A leading tag byte separates the storage classes, so NULL < numbers < text
// falls straight out of the byte comparison.
//
//   0x00  NULL
//   0x01  number, then 8 bytes of order-preserving IEEE-754
//   0x02  text, escaped and terminated
//
// Integers are encoded through double, so two integers more than 2^53 apart in
// magnitude can share a key. That is a false positive, never a false negative:
// an index seek re-checks the predicate against the row it fetched.

std::string encode_rowid(std::int64_t rowid);
std::int64_t decode_rowid(Slice key);

std::string encode_value(const Value& value);

// An index entry is (column value, rowid) so that duplicate values coexist and
// still sort in a stable, seekable order.
std::string encode_index_key(const Value& value, std::int64_t rowid);

// The shortest key that sorts at or after every entry for `value`, used as the
// exclusive upper bound of an equality seek.
std::string encode_index_upper_bound(const Value& value);

// Splits an index key back into its two halves.
std::int64_t index_key_rowid(Slice key);

}  // namespace minidb
