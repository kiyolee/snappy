// Copyright 2005 and onwards Google Inc.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "snappy-test.h"

#include "gtest/gtest.h"

#include "snappy-internal.h"
#include "snappy-sinksource.h"
#include "snappy.h"
#include "snappy_test_data.h"

SNAPPY_FLAG(bool, snappy_dump_decompression_table, false,
            "If true, we print the decompression table during tests.");

namespace snappy {

namespace {

#if HAVE_FUNC_MMAP && HAVE_FUNC_SYSCONF

// To test against code that reads beyond its input, this class copies a
// string to a newly allocated group of pages, the last of which
// is made unreadable via mprotect. Note that we need to allocate the
// memory with mmap(), as POSIX allows mprotect() only on memory allocated
// with mmap(), and some malloc/posix_memalign implementations expect to
// be able to read previously allocated memory while doing heap allocations.
class DataEndingAtUnreadablePage {
 public:
  explicit DataEndingAtUnreadablePage(const std::string& s) {
    const size_t page_size = sysconf(_SC_PAGESIZE);
    const size_t size = s.size();
    // Round up space for string to a multiple of page_size.
    size_t space_for_string = (size + page_size - 1) & ~(page_size - 1);
    alloc_size_ = space_for_string + page_size;
    mem_ = mmap(NULL, alloc_size_,
                PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    CHECK_NE(MAP_FAILED, mem_);
    protected_page_ = reinterpret_cast<char*>(mem_) + space_for_string;
    char* dst = protected_page_ - size;
    std::memcpy(dst, s.data(), size);
    data_ = dst;
    size_ = size;
    // Make guard page unreadable.
    CHECK_EQ(0, mprotect(protected_page_, page_size, PROT_NONE));
  }

  ~DataEndingAtUnreadablePage() {
    const size_t page_size = sysconf(_SC_PAGESIZE);
    // Undo the mprotect.
    CHECK_EQ(0, mprotect(protected_page_, page_size, PROT_READ|PROT_WRITE));
    CHECK_EQ(0, munmap(mem_, alloc_size_));
  }

  const char* data() const { return data_; }
  size_t size() const { return size_; }

 private:
  size_t alloc_size_;
  void* mem_;
  char* protected_page_;
  const char* data_;
  size_t size_;
};

#else  // HAVE_FUNC_MMAP) && HAVE_FUNC_SYSCONF

// Fallback for systems without mmap.
using DataEndingAtUnreadablePage = std::string;

#endif

size_t VerifyString(const std::string& input) {
  std::string compressed;
  DataEndingAtUnreadablePage i(input);
  const size_t written = snappy::Compress(i.data(), i.size(), &compressed);
  CHECK_EQ(written, compressed.size());
  CHECK_LE(compressed.size(),
           snappy::MaxCompressedLength(input.size()));
  CHECK(snappy::IsValidCompressedBuffer(compressed.data(), compressed.size()));

  std::string uncompressed;
  DataEndingAtUnreadablePage c(compressed);
  CHECK(snappy::Uncompress(c.data(), c.size(), &uncompressed));
  CHECK_EQ(uncompressed, input);
  return uncompressed.size();
}

void VerifyStringSink(const std::string& input) {
  std::string compressed;
  DataEndingAtUnreadablePage i(input);
  const size_t written = snappy::Compress(i.data(), i.size(), &compressed);
  CHECK_EQ(written, compressed.size());
  CHECK_LE(compressed.size(),
           snappy::MaxCompressedLength(input.size()));
  CHECK(snappy::IsValidCompressedBuffer(compressed.data(), compressed.size()));

  std::string uncompressed;
  uncompressed.resize(input.size());
  snappy::UncheckedByteArraySink sink(string_as_array(&uncompressed));
  DataEndingAtUnreadablePage c(compressed);
  snappy::ByteArraySource source(c.data(), c.size());
  CHECK(snappy::Uncompress(&source, &sink));
  CHECK_EQ(uncompressed, input);
}

struct iovec* GetIOVec(const std::string& input, char*& buf, size_t& num) {
  std::minstd_rand0 rng(static_cast<std::minstd_rand0::result_type>(input.size()));
  std::uniform_int_distribution<size_t> uniform_1_to_10(1, 10);
  num = uniform_1_to_10(rng);
  if (input.size() < num) {
    num = input.size();
  }
  struct iovec* iov = new iovec[num];
  size_t used_so_far = 0;
  std::bernoulli_distribution one_in_five(1.0 / 5);
  for (size_t i = 0; i < num; ++i) {
    assert(used_so_far < input.size());
    iov[i].iov_base = buf + used_so_far;
    if (i == num - 1) {
      iov[i].iov_len = input.size() - used_so_far;
    } else {
      // Randomly choose to insert a 0 byte entry.
      if (one_in_five(rng)) {
        iov[i].iov_len = 0;
      } else {
        std::uniform_int_distribution<size_t> uniform_not_used_so_far(
            0, input.size() - used_so_far - 1);
        iov[i].iov_len = uniform_not_used_so_far(rng);
      }
    }
    used_so_far += iov[i].iov_len;
  }
  return iov;
}

int VerifyIOVecSource(const std::string& input) {
  std::string compressed;
  std::string copy = input;
  char* buf = const_cast<char*>(copy.data());
  size_t num = 0;
  struct iovec* iov = GetIOVec(input, buf, num);
  const size_t written = snappy::CompressFromIOVec(iov, num, &compressed);
  CHECK_EQ(written, compressed.size());
  CHECK_LE(compressed.size(), snappy::MaxCompressedLength(input.size()));
  CHECK(snappy::IsValidCompressedBuffer(compressed.data(), compressed.size()));

  std::string uncompressed;
  DataEndingAtUnreadablePage c(compressed);
  CHECK(snappy::Uncompress(c.data(), c.size(), &uncompressed));
  CHECK_EQ(uncompressed, input);
  delete[] iov;
  return uncompressed.size();
}

void VerifyIOVecSink(const std::string& input) {
  std::string compressed;
  DataEndingAtUnreadablePage i(input);
  const size_t written = snappy::Compress(i.data(), i.size(), &compressed);
  CHECK_EQ(written, compressed.size());
  CHECK_LE(compressed.size(), snappy::MaxCompressedLength(input.size()));
  CHECK(snappy::IsValidCompressedBuffer(compressed.data(), compressed.size()));
  char* buf = new char[input.size()];
  size_t num = 0;
  struct iovec* iov = GetIOVec(input, buf, num);
  CHECK(snappy::RawUncompressToIOVec(compressed.data(), compressed.size(), iov,
                                     num));
  CHECK(!memcmp(buf, input.data(), input.size()));
  delete[] iov;
  delete[] buf;
}

// Test that data compressed by a compressor that does not
// obey block sizes is uncompressed properly.
void VerifyNonBlockedCompression(const std::string& input) {
  if (input.length() > snappy::kBlockSize) {
    // We cannot test larger blocks than the maximum block size, obviously.
    return;
  }

  std::string prefix;
  Varint::Append32(&prefix, static_cast<uint32_t>(input.size()));

  // Setup compression table
  snappy::internal::WorkingMemory wmem(input.size());
  size_t table_size;
  uint16_t* table = wmem.GetHashTable(input.size(), &table_size);

  // Compress entire input in one shot
  std::string compressed;
  compressed += prefix;
  compressed.resize(prefix.size()+snappy::MaxCompressedLength(input.size()));
  char* dest = string_as_array(&compressed) + prefix.size();
  char* end = snappy::internal::CompressFragment(input.data(), input.size(),
                                                dest, table, table_size);
  compressed.resize(end - compressed.data());

  // Uncompress into std::string
  std::string uncomp_str;
  CHECK(snappy::Uncompress(compressed.data(), compressed.size(), &uncomp_str));
  CHECK_EQ(uncomp_str, input);

  // Uncompress using source/sink
  std::string uncomp_str2;
  uncomp_str2.resize(input.size());
  snappy::UncheckedByteArraySink sink(string_as_array(&uncomp_str2));
  snappy::ByteArraySource source(compressed.data(), compressed.size());
  CHECK(snappy::Uncompress(&source, &sink));
  CHECK_EQ(uncomp_str2, input);

  // Uncompress into iovec
  {
    static const int kNumBlocks = 10;
    struct iovec vec[kNumBlocks];
    const size_t block_size = 1 + input.size() / kNumBlocks;
    std::string iovec_data(block_size * kNumBlocks, 'x');
    for (int i = 0; i < kNumBlocks; ++i) {
      vec[i].iov_base = string_as_array(&iovec_data) + i * block_size;
      vec[i].iov_len = block_size;
    }
    CHECK(snappy::RawUncompressToIOVec(compressed.data(), compressed.size(),
                                       vec, kNumBlocks));
    CHECK_EQ(std::string(iovec_data.data(), input.size()), input);
  }
}

// Expand the input so that it is at least K times as big as block size
std::string Expand(const std::string& input) {
  static const int K = 3;
  std::string data = input;
  while (data.size() < K * snappy::kBlockSize) {
    data += input;
  }
  return data;
}

size_t Verify(const std::string& input) {
  VLOG(1) << "Verifying input of size " << input.size();

  // Compress using string based routines
  const size_t result = VerifyString(input);

  // Compress using `iovec`-based routines.
  CHECK_EQ(VerifyIOVecSource(input), result);

  // Verify using sink based routines
  VerifyStringSink(input);

  VerifyNonBlockedCompression(input);
  VerifyIOVecSink(input);
  if (!input.empty()) {
    const std::string expanded = Expand(input);
    VerifyNonBlockedCompression(expanded);
    VerifyIOVecSink(input);
  }

  return result;
}

bool IsValidCompressedBuffer(const std::string& c) {
  return snappy::IsValidCompressedBuffer(c.data(), c.size());
}
bool Uncompress(const std::string& c, std::string* u) {
  return snappy::Uncompress(c.data(), c.size(), u);
}

// This test checks to ensure that snappy doesn't coredump if it gets
// corrupted data.
TEST(CorruptedTest, VerifyCorrupted) {
  std::string source = "making sure we don't crash with corrupted input";
  VLOG(1) << source;
  std::string dest;
  std::string uncmp;
  snappy::Compress(source.data(), source.size(), &dest);

  // Mess around with the data. It's hard to simulate all possible
  // corruptions; this is just one example ...
  CHECK_GT(dest.size(), 3);
  dest[1]--;
  dest[3]++;
  // this really ought to fail.
  CHECK(!IsValidCompressedBuffer(dest));
  CHECK(!Uncompress(dest, &uncmp));

  // This is testing for a security bug - a buffer that decompresses to 100k
  // but we lie in the snappy header and only reserve 0 bytes of memory :)
  source.resize(100000);
  for (char& source_char : source) {
    source_char = 'A';
  }
  snappy::Compress(source.data(), source.size(), &dest);
  dest[0] = dest[1] = dest[2] = dest[3] = 0;
  CHECK(!IsValidCompressedBuffer(dest));
  CHECK(!Uncompress(dest, &uncmp));

  if (sizeof(void *) == 4) {
    // Another security check; check a crazy big length can't DoS us with an
    // over-allocation.
    // Currently this is done only for 32-bit builds.  On 64-bit builds,
    // where 3 GB might be an acceptable allocation size, Uncompress()
    // attempts to decompress, and sometimes causes the test to run out of
    // memory.
    dest[0] = dest[1] = dest[2] = dest[3] = '\xff';
    // This decodes to a really large size, i.e., about 3 GB.
    dest[4] = 'k';
    CHECK(!IsValidCompressedBuffer(dest));
    CHECK(!Uncompress(dest, &uncmp));
  } else {
    LOG(WARNING) << "Crazy decompression lengths not checked on 64-bit build";
  }

  // This decodes to about 2 MB; much smaller, but should still fail.
  dest[0] = dest[1] = dest[2] = '\xff';
  dest[3] = 0x00;
  CHECK(!IsValidCompressedBuffer(dest));
  CHECK(!Uncompress(dest, &uncmp));

  // try reading stuff in from a bad file.
  for (int i = 1; i <= 3; ++i) {
    std::string data =
        ReadTestDataFile(StrFormat("baddata%d.snappy", i).c_str(), 0);
    std::string uncmp;
    // check that we don't return a crazy length
    size_t ulen;
    CHECK(!snappy::GetUncompressedLength(data.data(), data.size(), &ulen)
          || (ulen < (1<<20)));
    uint32_t ulen2;
    snappy::ByteArraySource source(data.data(), data.size());
    CHECK(!snappy::GetUncompressedLength(&source, &ulen2) ||
          (ulen2 < (1<<20)));
    CHECK(!IsValidCompressedBuffer(data));
    CHECK(!Uncompress(data, &uncmp));
  }
}

// Helper routines to construct arbitrary compressed strings.
// These mirror the compression code in snappy.cc, but are copied
// here so that we can bypass some limitations in the how snappy.cc
// invokes these routines.
void AppendLiteral(std::string* dst, const std::string& literal) {
  if (literal.empty()) return;
  size_t n = literal.size() - 1;
  if (n < 60) {
    // Fit length in tag byte
    dst->push_back(static_cast<char>(0 | (n << 2)));
  } else {
    // Encode in upcoming bytes
    char number[4];
    int count = 0;
    while (n > 0) {
      number[count++] = n & 0xff;
      n >>= 8;
    }
    dst->push_back(0 | ((59+count) << 2));
    *dst += std::string(number, count);
  }
  *dst += literal;
}

void AppendCopy(std::string* dst, size_t offset, size_t length) {
  while (length > 0) {
    // Figure out how much to copy in one shot
    size_t to_copy;
    if (length >= 68) {
      to_copy = 64;
    } else if (length > 64) {
      to_copy = 60;
    } else {
      to_copy = length;
    }
    length -= to_copy;

    if ((to_copy >= 4) && (to_copy < 12) && (offset < 2048)) {
      assert(to_copy-4 < 8);            // Must fit in 3 bits
      dst->push_back(static_cast<char>(1 | ((to_copy-4) << 2) | ((offset >> 8) << 5)));
      dst->push_back(static_cast<char>(offset & 0xff));
    } else if (offset < 65536) {
      dst->push_back(static_cast<char>(2 | ((to_copy-1) << 2)));
      dst->push_back(static_cast<char>(offset & 0xff));
      dst->push_back(static_cast<char>(offset >> 8));
    } else {
      dst->push_back(static_cast<char>(3 | ((to_copy-1) << 2)));
      dst->push_back(offset & 0xff);
      dst->push_back((offset >> 8) & 0xff);
      dst->push_back((offset >> 16) & 0xff);
      dst->push_back((offset >> 24) & 0xff);
    }
  }
}

TEST(Snappy, SimpleTests) {
  Verify("");
  Verify("a");
  Verify("ab");
  Verify("abc");

  Verify("aaaaaaa" + std::string(16, 'b') + std::string("aaaaa") + "abc");
  Verify("aaaaaaa" + std::string(256, 'b') + std::string("aaaaa") + "abc");
  Verify("aaaaaaa" + std::string(2047, 'b') + std::string("aaaaa") + "abc");
  Verify("aaaaaaa" + std::string(65536, 'b') + std::string("aaaaa") + "abc");
  Verify("abcaaaaaaa" + std::string(65536, 'b') + std::string("aaaaa") + "abc");
}

// Regression test for cr/345340892.
TEST(Snappy, AppendSelfPatternExtensionEdgeCases) {
  Verify("abcabcabcabcabcabcab");
  Verify("abcabcabcabcabcabcab0123456789ABCDEF");

  Verify("abcabcabcabcabcabcabcabcabcabcabcabc");
  Verify("abcabcabcabcabcabcabcabcabcabcabcabc0123456789ABCDEF");
}

// Regression test for cr/345340892.
TEST(Snappy, AppendSelfPatternExtensionEdgeCasesExhaustive) {
  std::mt19937 rng;
  std::uniform_int_distribution<int> uniform_byte(0, 255);
  for (int pattern_size = 1; pattern_size <= 18; ++pattern_size) {
    for (int length = 1; length <= 64; ++length) {
      for (int extra_bytes_after_pattern : {0, 1, 15, 16, 128}) {
        const int size = pattern_size + length + extra_bytes_after_pattern;
        std::string input;
        input.resize(size);
        for (int i = 0; i < pattern_size; ++i) {
          input[i] = 'a' + i;
        }
        for (int i = 0; i < length; ++i) {
          input[pattern_size + i] = input[i];
        }
        for (int i = 0; i < extra_bytes_after_pattern; ++i) {
          input[pattern_size + length + i] =
              static_cast<char>(uniform_byte(rng));
        }
        Verify(input);
      }
    }
  }
}

// Verify max blowup (lots of four-byte copies)
TEST(Snappy, MaxBlowup) {
  std::mt19937 rng;
  std::uniform_int_distribution<int> uniform_byte(0, 255);
  std::string input;
  for (int i = 0; i < 80000; ++i)
    input.push_back(static_cast<char>(uniform_byte(rng)));

  for (int i = 0; i < 80000; i += 4) {
    std::string four_bytes(input.end() - i - 4, input.end() - i);
    input.append(four_bytes);
  }
  Verify(input);
}

// Issue #201, when output is more than 4GB, we had a data corruption bug.
// We cannot run this test always because of CI constraints.
TEST(Snappy, DISABLED_MoreThan4GB) {
  std::mt19937 rng;
  std::uniform_int_distribution<int> uniform_byte(0, 255);
  std::string input;
  input.resize((1ull << 32) - 1);
  for (uint64_t i = 0; i < ((1ull << 32) - 1); ++i)
    input[i] = static_cast<char>(uniform_byte(rng));
  Verify(input);
}

TEST(Snappy, RandomData) {
  std::minstd_rand0 rng(snappy::GetFlag(FLAGS_test_random_seed));
  std::uniform_int_distribution<int> uniform_0_to_3(0, 3);
  std::uniform_int_distribution<int> uniform_0_to_8(0, 8);
  std::uniform_int_distribution<int> uniform_byte(0, 255);
  std::uniform_int_distribution<size_t> uniform_4k(0, 4095);
  std::uniform_int_distribution<size_t> uniform_64k(0, 65535);
  std::bernoulli_distribution one_in_ten(1.0 / 10);

  constexpr int num_ops = 20000;
  for (int i = 0; i < num_ops; ++i) {
    if ((i % 1000) == 0) {
      VLOG(0) << "Random op " << i << " of " << num_ops;
    }

    std::string x;
    size_t len = uniform_4k(rng);
    if (i < 100) {
      len = 65536 + uniform_64k(rng);
    }
    while (x.size() < len) {
      int run_len = 1;
      if (one_in_ten(rng)) {
        int skewed_bits = uniform_0_to_8(rng);
        // int is guaranteed to hold at least 16 bits, this uses at most 8 bits.
        std::uniform_int_distribution<int> skewed_low(0,
                                                      (1 << skewed_bits) - 1);
        run_len = skewed_low(rng);
      }
      char c = static_cast<char>(uniform_byte(rng));
      if (i >= 100) {
        int skewed_bits = uniform_0_to_3(rng);
        // int is guaranteed to hold at least 16 bits, this uses at most 3 bits.
        std::uniform_int_distribution<int> skewed_low(0,
                                                      (1 << skewed_bits) - 1);
        c = static_cast<char>(skewed_low(rng));
      }
      while (run_len-- > 0 && x.size() < len) {
        x.push_back(c);
      }
    }

    Verify(x);
  }
}

TEST(Snappy, FourByteOffset) {
  // The new compressor cannot generate four-byte offsets since
  // it chops up the input into 32KB pieces.  So we hand-emit the
  // copy manually.

  // The two fragments that make up the input string.
  std::string fragment1 = "012345689abcdefghijklmnopqrstuvwxyz";
  std::string fragment2 = "some other string";

  // How many times each fragment is emitted.
  const size_t n1 = 2;
  const size_t n2 = 100000 / fragment2.size();
  const size_t length = n1 * fragment1.size() + n2 * fragment2.size();

  std::string compressed;
  Varint::Append32(&compressed, static_cast<uint32_t>(length));

  AppendLiteral(&compressed, fragment1);
  std::string src = fragment1;
  for (size_t i = 0; i < n2; ++i) {
    AppendLiteral(&compressed, fragment2);
    src += fragment2;
  }
  AppendCopy(&compressed, src.size(), fragment1.size());
  src += fragment1;
  CHECK_EQ(length, src.size());

  std::string uncompressed;
  CHECK(snappy::IsValidCompressedBuffer(compressed.data(), compressed.size()));
  CHECK(snappy::Uncompress(compressed.data(), compressed.size(),
                           &uncompressed));
  CHECK_EQ(uncompressed, src);
}

TEST(Snappy, IOVecSourceEdgeCases) {
  // Validate that empty leading, trailing, and in-between iovecs are handled:
  // [] [] ['a'] [] ['b'] [].
  std::string data = "ab";
  char* buf = const_cast<char*>(data.data());
  size_t used_so_far = 0;
  static const int kLengths[] = {0, 0, 1, 0, 1, 0};
  struct iovec iov[ARRAYSIZE(kLengths)];
  for (int i = 0; i < ARRAYSIZE(kLengths); ++i) {
    iov[i].iov_base = buf + used_so_far;
    iov[i].iov_len = kLengths[i];
    used_so_far += kLengths[i];
  }
  std::string compressed;
  snappy::CompressFromIOVec(iov, ARRAYSIZE(kLengths), &compressed);
  std::string uncompressed;
  snappy::Uncompress(compressed.data(), compressed.size(), &uncompressed);
  CHECK_EQ(data, uncompressed);
}

TEST(Snappy, IOVecSinkEdgeCases) {
  // Test some tricky edge cases in the iovec output that are not necessarily
  // exercised by random tests.

  // Our output blocks look like this initially (the last iovec is bigger
  // than depicted):
  // [  ] [ ] [    ] [        ] [        ]
  static const int kLengths[] = { 2, 1, 4, 8, 128 };

  struct iovec iov[ARRAYSIZE(kLengths)];
  for (int i = 0; i < ARRAYSIZE(kLengths); ++i) {
    iov[i].iov_base = new char[kLengths[i]];
    iov[i].iov_len = kLengths[i];
  }

  std::string compressed;
  Varint::Append32(&compressed, 22);

  // A literal whose output crosses three blocks.
  // [ab] [c] [123 ] [        ] [        ]
  AppendLiteral(&compressed, "abc123");

  // A copy whose output crosses two blocks (source and destination
  // segments marked).
  // [ab] [c] [1231] [23      ] [        ]
  //           ^--^   --
  AppendCopy(&compressed, 3, 3);

  // A copy where the input is, at first, in the block before the output:
  //
  // [ab] [c] [1231] [231231  ] [        ]
  //           ^---     ^---
  // Then during the copy, the pointers move such that the input and
  // output pointers are in the same block:
  //
  // [ab] [c] [1231] [23123123] [        ]
  //                  ^-    ^-
  // And then they move again, so that the output pointer is no longer
  // in the same block as the input pointer:
  // [ab] [c] [1231] [23123123] [123     ]
  //                    ^--      ^--
  AppendCopy(&compressed, 6, 9);

  // Finally, a copy where the input is from several blocks back,
  // and it also crosses three blocks:
  //
  // [ab] [c] [1231] [23123123] [123b    ]
  //   ^                            ^
  // [ab] [c] [1231] [23123123] [123bc   ]
  //       ^                         ^
  // [ab] [c] [1231] [23123123] [123bc12 ]
  //           ^-                     ^-
  AppendCopy(&compressed, 17, 4);

  CHECK(snappy::RawUncompressToIOVec(
      compressed.data(), compressed.size(), iov, ARRAYSIZE(iov)));
  CHECK_EQ(0, memcmp(iov[0].iov_base, "ab", 2));
  CHECK_EQ(0, memcmp(iov[1].iov_base, "c", 1));
  CHECK_EQ(0, memcmp(iov[2].iov_base, "1231", 4));
  CHECK_EQ(0, memcmp(iov[3].iov_base, "23123123", 8));
  CHECK_EQ(0, memcmp(iov[4].iov_base, "123bc12", 7));

  for (int i = 0; i < ARRAYSIZE(kLengths); ++i) {
    delete[] reinterpret_cast<char *>(iov[i].iov_base);
  }
}

TEST(Snappy, IOVecLiteralOverflow) {
  static const int kLengths[] = { 3, 4 };

  struct iovec iov[ARRAYSIZE(kLengths)];
  for (int i = 0; i < ARRAYSIZE(kLengths); ++i) {
    iov[i].iov_base = new char[kLengths[i]];
    iov[i].iov_len = kLengths[i];
  }

  std::string compressed;
  Varint::Append32(&compressed, 8);

  AppendLiteral(&compressed, "12345678");

  CHECK(!snappy::RawUncompressToIOVec(
      compressed.data(), compressed.size(), iov, ARRAYSIZE(iov)));

  for (int i = 0; i < ARRAYSIZE(kLengths); ++i) {
    delete[] reinterpret_cast<char *>(iov[i].iov_base);
  }
}

TEST(Snappy, IOVecCopyOverflow) {
  static const int kLengths[] = { 3, 4 };

  struct iovec iov[ARRAYSIZE(kLengths)];
  for (int i = 0; i < ARRAYSIZE(kLengths); ++i) {
    iov[i].iov_base = new char[kLengths[i]];
    iov[i].iov_len = kLengths[i];
  }

  std::string compressed;
  Varint::Append32(&compressed, 8);

  AppendLiteral(&compressed, "123");
  AppendCopy(&compressed, 3, 5);

  CHECK(!snappy::RawUncompressToIOVec(
      compressed.data(), compressed.size(), iov, ARRAYSIZE(iov)));

  for (int i = 0; i < ARRAYSIZE(kLengths); ++i) {
    delete[] reinterpret_cast<char *>(iov[i].iov_base);
  }
}

bool CheckUncompressedLength(const std::string& compressed, size_t* ulength) {
  const bool result1 = snappy::GetUncompressedLength(compressed.data(),
                                                     compressed.size(),
                                                     ulength);

  snappy::ByteArraySource source(compressed.data(), compressed.size());
  uint32_t length;
  const bool result2 = snappy::GetUncompressedLength(&source, &length);
  CHECK_EQ(result1, result2);
  return result1;
}

TEST(SnappyCorruption, TruncatedVarint) {
  std::string compressed, uncompressed;
  size_t ulength;
  compressed.push_back('\xf0');
  CHECK(!CheckUncompressedLength(compressed, &ulength));
  CHECK(!snappy::IsValidCompressedBuffer(compressed.data(), compressed.size()));
  CHECK(!snappy::Uncompress(compressed.data(), compressed.size(),
                            &uncompressed));
}

TEST(SnappyCorruption, UnterminatedVarint) {
  std::string compressed, uncompressed;
  size_t ulength;
  compressed.push_back('\x80');
  compressed.push_back('\x80');
  compressed.push_back('\x80');
  compressed.push_back('\x80');
  compressed.push_back('\x80');
  compressed.push_back(10);
  CHECK(!CheckUncompressedLength(compressed, &ulength));
  CHECK(!snappy::IsValidCompressedBuffer(compressed.data(), compressed.size()));
  CHECK(!snappy::Uncompress(compressed.data(), compressed.size(),
                            &uncompressed));
}

TEST(SnappyCorruption, OverflowingVarint) {
  std::string compressed, uncompressed;
  size_t ulength;
  compressed.push_back('\xfb');
  compressed.push_back('\xff');
  compressed.push_back('\xff');
  compressed.push_back('\xff');
  compressed.push_back('\x7f');
  CHECK(!CheckUncompressedLength(compressed, &ulength));
  CHECK(!snappy::IsValidCompressedBuffer(compressed.data(), compressed.size()));
  CHECK(!snappy::Uncompress(compressed.data(), compressed.size(),
                            &uncompressed));
}

TEST(Snappy, ReadPastEndOfBuffer) {
  // Check that we do not read past end of input

  // Make a compressed string that ends with a single-byte literal
  std::string compressed;
  Varint::Append32(&compressed, 1);
  AppendLiteral(&compressed, "x");

  std::string uncompressed;
  DataEndingAtUnreadablePage c(compressed);
  CHECK(snappy::Uncompress(c.data(), c.size(), &uncompressed));
  CHECK_EQ(uncompressed, std::string("x"));
}

// Check for an infinite loop caused by a copy with offset==0
TEST(Snappy, ZeroOffsetCopy) {
  const char* compressed = "\x40\x12\x00\x00";
  //  \x40              Length (must be > kMaxIncrementCopyOverflow)
  //  \x12\x00\x00      Copy with offset==0, length==5
  char uncompressed[100];
  EXPECT_FALSE(snappy::RawUncompress(compressed, 4, uncompressed));
}

TEST(Snappy, ZeroOffsetCopyValidation) {
  const char* compressed = "\x05\x12\x00\x00";
  //  \x05              Length
  //  \x12\x00\x00      Copy with offset==0, length==5
  EXPECT_FALSE(snappy::IsValidCompressedBuffer(compressed, 4));
}

size_t TestFindMatchLength(const char* s1, const char *s2, size_t length) {
  uint64_t data;
  std::pair<size_t, bool> p =
      snappy::internal::FindMatchLength(s1, s2, s2 + length, &data);
  CHECK_EQ(p.first < 8, p.second);
  return p.first;
}

TEST(Snappy, FindMatchLength) {
  // Exercise all different code paths through the function.
  // 64-bit version:

  // Hit s1_limit in 64-bit loop, hit s1_limit in single-character loop.
  EXPECT_EQ(6, TestFindMatchLength("012345", "012345", 6));
  EXPECT_EQ(11, TestFindMatchLength("01234567abc", "01234567abc", 11));

  // Hit s1_limit in 64-bit loop, find a non-match in single-character loop.
  EXPECT_EQ(9, TestFindMatchLength("01234567abc", "01234567axc", 9));

  // Same, but edge cases.
  EXPECT_EQ(11, TestFindMatchLength("01234567abc!", "01234567abc!", 11));
  EXPECT_EQ(11, TestFindMatchLength("01234567abc!", "01234567abc?", 11));

  // Find non-match at once in first loop.
  EXPECT_EQ(0, TestFindMatchLength("01234567xxxxxxxx", "?1234567xxxxxxxx", 16));
  EXPECT_EQ(1, TestFindMatchLength("01234567xxxxxxxx", "0?234567xxxxxxxx", 16));
  EXPECT_EQ(4, TestFindMatchLength("01234567xxxxxxxx", "01237654xxxxxxxx", 16));
  EXPECT_EQ(7, TestFindMatchLength("01234567xxxxxxxx", "0123456?xxxxxxxx", 16));

  // Find non-match in first loop after one block.
  EXPECT_EQ(8, TestFindMatchLength("abcdefgh01234567xxxxxxxx",
                                   "abcdefgh?1234567xxxxxxxx", 24));
  EXPECT_EQ(9, TestFindMatchLength("abcdefgh01234567xxxxxxxx",
                                   "abcdefgh0?234567xxxxxxxx", 24));
  EXPECT_EQ(12, TestFindMatchLength("abcdefgh01234567xxxxxxxx",
                                    "abcdefgh01237654xxxxxxxx", 24));
  EXPECT_EQ(15, TestFindMatchLength("abcdefgh01234567xxxxxxxx",
                                    "abcdefgh0123456?xxxxxxxx", 24));

  // 32-bit version:

  // Short matches.
  EXPECT_EQ(0, TestFindMatchLength("01234567", "?1234567", 8));
  EXPECT_EQ(1, TestFindMatchLength("01234567", "0?234567", 8));
  EXPECT_EQ(2, TestFindMatchLength("01234567", "01?34567", 8));
  EXPECT_EQ(3, TestFindMatchLength("01234567", "012?4567", 8));
  EXPECT_EQ(4, TestFindMatchLength("01234567", "0123?567", 8));
  EXPECT_EQ(5, TestFindMatchLength("01234567", "01234?67", 8));
  EXPECT_EQ(6, TestFindMatchLength("01234567", "012345?7", 8));
  EXPECT_EQ(7, TestFindMatchLength("01234567", "0123456?", 8));
  EXPECT_EQ(7, TestFindMatchLength("01234567", "0123456?", 7));
  EXPECT_EQ(7, TestFindMatchLength("01234567!", "0123456??", 7));

  // Hit s1_limit in 32-bit loop, hit s1_limit in single-character loop.
  EXPECT_EQ(10, TestFindMatchLength("xxxxxxabcd", "xxxxxxabcd", 10));
  EXPECT_EQ(10, TestFindMatchLength("xxxxxxabcd?", "xxxxxxabcd?", 10));
  EXPECT_EQ(13, TestFindMatchLength("xxxxxxabcdef", "xxxxxxabcdef", 13));

  // Same, but edge cases.
  EXPECT_EQ(12, TestFindMatchLength("xxxxxx0123abc!", "xxxxxx0123abc!", 12));
  EXPECT_EQ(12, TestFindMatchLength("xxxxxx0123abc!", "xxxxxx0123abc?", 12));

  // Hit s1_limit in 32-bit loop, find a non-match in single-character loop.
  EXPECT_EQ(11, TestFindMatchLength("xxxxxx0123abc", "xxxxxx0123axc", 13));

  // Find non-match at once in first loop.
  EXPECT_EQ(6, TestFindMatchLength("xxxxxx0123xxxxxxxx",
                                   "xxxxxx?123xxxxxxxx", 18));
  EXPECT_EQ(7, TestFindMatchLength("xxxxxx0123xxxxxxxx",
                                   "xxxxxx0?23xxxxxxxx", 18));
  EXPECT_EQ(8, TestFindMatchLength("xxxxxx0123xxxxxxxx",
                                   "xxxxxx0132xxxxxxxx", 18));
  EXPECT_EQ(9, TestFindMatchLength("xxxxxx0123xxxxxxxx",
                                   "xxxxxx012?xxxxxxxx", 18));

  // Same, but edge cases.
  EXPECT_EQ(6, TestFindMatchLength("xxxxxx0123", "xxxxxx?123", 10));
  EXPECT_EQ(7, TestFindMatchLength("xxxxxx0123", "xxxxxx0?23", 10));
  EXPECT_EQ(8, TestFindMatchLength("xxxxxx0123", "xxxxxx0132", 10));
  EXPECT_EQ(9, TestFindMatchLength("xxxxxx0123", "xxxxxx012?", 10));

  // Find non-match in first loop after one block.
  EXPECT_EQ(10, TestFindMatchLength("xxxxxxabcd0123xx",
                                    "xxxxxxabcd?123xx", 16));
  EXPECT_EQ(11, TestFindMatchLength("xxxxxxabcd0123xx",
                                    "xxxxxxabcd0?23xx", 16));
  EXPECT_EQ(12, TestFindMatchLength("xxxxxxabcd0123xx",
                                    "xxxxxxabcd0132xx", 16));
  EXPECT_EQ(13, TestFindMatchLength("xxxxxxabcd0123xx",
                                    "xxxxxxabcd012?xx", 16));

  // Same, but edge cases.
  EXPECT_EQ(10, TestFindMatchLength("xxxxxxabcd0123", "xxxxxxabcd?123", 14));
  EXPECT_EQ(11, TestFindMatchLength("xxxxxxabcd0123", "xxxxxxabcd0?23", 14));
  EXPECT_EQ(12, TestFindMatchLength("xxxxxxabcd0123", "xxxxxxabcd0132", 14));
  EXPECT_EQ(13, TestFindMatchLength("xxxxxxabcd0123", "xxxxxxabcd012?", 14));
}

TEST(Snappy, FindMatchLengthRandom) {
  constexpr int kNumTrials = 10000;
  constexpr int kTypicalLength = 10;
  std::minstd_rand0 rng(snappy::GetFlag(FLAGS_test_random_seed));
  std::uniform_int_distribution<int> uniform_byte(0, 255);
  std::bernoulli_distribution one_in_two(1.0 / 2);
  std::bernoulli_distribution one_in_typical_length(1.0 / kTypicalLength);

  for (int i = 0; i < kNumTrials; ++i) {
    std::string s, t;
    char a = static_cast<char>(uniform_byte(rng));
    char b = static_cast<char>(uniform_byte(rng));
    while (!one_in_typical_length(rng)) {
      s.push_back(one_in_two(rng) ? a : b);
      t.push_back(one_in_two(rng) ? a : b);
    }
    DataEndingAtUnreadablePage u(s);
    DataEndingAtUnreadablePage v(t);
    size_t matched = TestFindMatchLength(u.data(), v.data(), t.size());
    if (matched == t.size()) {
      EXPECT_EQ(s, t);
    } else {
      EXPECT_NE(s[matched], t[matched]);
      for (size_t j = 0; j < matched; ++j) {
        EXPECT_EQ(s[j], t[j]);
      }
    }
  }
}

uint16_t MakeEntry(unsigned int extra, unsigned int len,
                   unsigned int copy_offset) {
  // Check that all of the fields fit within the allocated space
  assert(extra       == (extra & 0x7));          // At most 3 bits
  assert(copy_offset == (copy_offset & 0x7));    // At most 3 bits
  assert(len         == (len & 0x7f));           // At most 7 bits
  return len | (copy_offset << 8) | (extra << 11);
}

// Check that the decompression table is correct, and optionally print out
// the computed one.
TEST(Snappy, VerifyCharTable) {
  using snappy::internal::LITERAL;
  using snappy::internal::COPY_1_BYTE_OFFSET;
  using snappy::internal::COPY_2_BYTE_OFFSET;
  using snappy::internal::COPY_4_BYTE_OFFSET;
  using snappy::internal::char_table;

  uint16_t dst[256];

  // Place invalid entries in all places to detect missing initialization
  int assigned = 0;
  for (int i = 0; i < 256; ++i) {
    dst[i] = 0xffff;
  }

  // Small LITERAL entries.  We store (len-1) in the top 6 bits.
  for (uint8_t len = 1; len <= 60; ++len) {
    dst[LITERAL | ((len - 1) << 2)] = MakeEntry(0, len, 0);
    assigned++;
  }

  // Large LITERAL entries.  We use 60..63 in the high 6 bits to
  // encode the number of bytes of length info that follow the opcode.
  for (uint8_t extra_bytes = 1; extra_bytes <= 4; ++extra_bytes) {
    // We set the length field in the lookup table to 1 because extra
    // bytes encode len-1.
    dst[LITERAL | ((extra_bytes + 59) << 2)] = MakeEntry(extra_bytes, 1, 0);
    assigned++;
  }

  // COPY_1_BYTE_OFFSET.
  //
  // The tag byte in the compressed data stores len-4 in 3 bits, and
  // offset/256 in 3 bits.  offset%256 is stored in the next byte.
  //
  // This format is used for length in range [4..11] and offset in
  // range [0..2047]
  for (uint8_t len = 4; len < 12; ++len) {
    for (uint16_t offset = 0; offset < 2048; offset += 256) {
      uint8_t offset_high = static_cast<uint8_t>(offset >> 8);
      dst[COPY_1_BYTE_OFFSET | ((len - 4) << 2) | (offset_high << 5)] =
          MakeEntry(1, len, offset_high);
      assigned++;
    }
  }

  // COPY_2_BYTE_OFFSET.
  // Tag contains len-1 in top 6 bits, and offset in next two bytes.
  for (uint8_t len = 1; len <= 64; ++len) {
    dst[COPY_2_BYTE_OFFSET | ((len - 1) << 2)] = MakeEntry(2, len, 0);
    assigned++;
  }

  // COPY_4_BYTE_OFFSET.
  // Tag contents len-1 in top 6 bits, and offset in next four bytes.
  for (uint8_t len = 1; len <= 64; ++len) {
    dst[COPY_4_BYTE_OFFSET | ((len - 1) << 2)] = MakeEntry(4, len, 0);
    assigned++;
  }

  // Check that each entry was initialized exactly once.
  EXPECT_EQ(256, assigned) << "Assigned only " << assigned << " of 256";
  for (int i = 0; i < 256; ++i) {
    EXPECT_NE(0xffff, dst[i]) << "Did not assign byte " << i;
  }

  if (snappy::GetFlag(FLAGS_snappy_dump_decompression_table)) {
    std::printf("static const uint16_t char_table[256] = {\n  ");
    for (int i = 0; i < 256; ++i) {
      std::printf("0x%04x%s",
                  dst[i],
                  ((i == 255) ? "\n" : (((i % 8) == 7) ? ",\n  " : ", ")));
    }
    std::printf("};\n");
  }

  // Check that computed table matched recorded table.
  for (int i = 0; i < 256; ++i) {
    EXPECT_EQ(dst[i], char_table[i]) << "Mismatch in byte " << i;
  }
}

TEST(Snappy, TestBenchmarkFiles) {
  for (int i = 0; i < ARRAYSIZE(kTestDataFiles); ++i) {
    Verify(ReadTestDataFile(kTestDataFiles[i].filename,
                            kTestDataFiles[i].size_limit));
  }
}

}  // namespace

}  // namespace snappy
