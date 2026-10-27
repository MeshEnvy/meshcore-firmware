#pragma once
#include "ReaderLookup.h"

namespace mesh { namespace bible {

// Independent bounded-history streams trade cold Reader decode time for flash.
// The existing ESP32 block corpus and its decoder remain unchanged.
struct StreamTokenCorpus {
  const char* translation;
  const char* attribution;
  const uint8_t* data;
  uint32_t data_size;
  uint32_t encoded_size;
  const uint8_t* words;
  uint16_t words_size;
  uint16_t word_plain_size;
  uint16_t word_count;
  uint16_t verse_count;
  uint16_t chunk_count;  // Zero retains the single-stream fixture format.
};

inline LookupResult lookup(const StreamTokenCorpus& corpus, Reference ref,
                           char* scratch, size_t capacity, const char*& text) {
  text = nullptr;
  uint16_t index;
  if (!verseNumber(ref, index)) return LookupResult::Invalid;
  if (!scratch || capacity < kBlockSize || !corpus.data || !corpus.data_size
      || !corpus.encoded_size || corpus.verse_count != kVerseCount
      || corpus.chunk_count > kVerseCount
      || corpus.word_count > 128 || corpus.word_plain_size > kBlockSize
      || (corpus.word_count && (!corpus.words || !corpus.words_size
          || corpus.word_plain_size < corpus.word_count * 2U))
      || (!corpus.word_count && (corpus.words_size || corpus.word_plain_size)))
    return LookupResult::Corrupt;
  const uint8_t* source = corpus.data;
  uint32_t source_size = corpus.data_size, output_limit = corpus.encoded_size;
  if (corpus.chunk_count) {
    // Each independently compressed stream has a bounded six-byte header:
    // first verse, decoded byte limit and compressed byte length, little-endian.
    const uint8_t* cursor = corpus.data;
    uint32_t remaining = corpus.data_size, total = 0;
    const uint16_t wanted = index;
    uint16_t previous = 0;
    for (uint16_t n = 0; n < corpus.chunk_count; ++n) {
      if (remaining < 6) return LookupResult::Corrupt;
      const uint16_t first = cursor[0] | (uint16_t(cursor[1]) << 8);
      const uint16_t plain = cursor[2] | (uint16_t(cursor[3]) << 8);
      const uint16_t packed = cursor[4] | (uint16_t(cursor[5]) << 8);
      cursor += 6;
      remaining -= 6;
      if (!plain || !packed || packed > remaining || first >= kVerseCount
          || (n ? first <= previous : first != 0)) return LookupResult::Corrupt;
      if (first <= wanted) {
        source = cursor;
        source_size = packed;
        output_limit = plain;
        index = wanted - first;
      }
      previous = first;
      total += plain;
      cursor += packed;
      remaining -= packed;
    }
    if (remaining || total != corpus.encoded_size) return LookupResult::Corrupt;
  }
  unsigned int encoded_size = 0;
  if (tinf_uncompress_record(scratch, kBlockSize, &encoded_size,
      source, source_size, output_limit, index) != TINF_OK
      || encoded_size >= kBlockSize) return LookupResult::Corrupt;
  if (!encoded_size) return LookupResult::Missing;
  bool tokenized = false;
  for (unsigned int i = 0; i < encoded_size; ++i) {
    const uint8_t byte = static_cast<uint8_t>(scratch[i]);
    if (byte >= 128) {
      if (byte - 128U >= corpus.word_count) return LookupResult::Corrupt;
      tokenized = true;
    } else if (byte < 32 || byte >= 127) return LookupResult::Corrupt;
  }
  if (!tokenized) { text = scratch; return LookupResult::Found; }

  // Reuse the history after inflation. Keep the selected encoded record at
  // the far end, the decoded vocabulary at the beginning, and expand into the
  // gap. The host packer uses literal fallback for a long verse that would
  // not fit these three regions, preserving the original 4095-byte limit.
  uint8_t* encoded = reinterpret_cast<uint8_t*>(scratch) + kBlockSize - encoded_size - 1;
  if (encoded <= reinterpret_cast<uint8_t*>(scratch) + corpus.word_plain_size)
    return LookupResult::Corrupt;
  memmove(encoded, scratch, encoded_size + 1);
  unsigned int dictionary_size = corpus.word_plain_size;
  if (tinf_uncompress_exact(scratch, &dictionary_size, corpus.words, corpus.words_size) != TINF_OK
      || dictionary_size != corpus.word_plain_size) return LookupResult::Corrupt;
  const char* dictionary_end = scratch + dictionary_size;
  const char* check = scratch;
  for (unsigned int n = 0; n < corpus.word_count; ++n) {
    const char* zero = static_cast<const char*>(memchr(check, 0, dictionary_end - check));
    if (!zero || zero == check) return LookupResult::Corrupt;
    for (const char* p = check; p != zero; ++p)
      if (*p < 32 || static_cast<uint8_t>(*p) >= 127) return LookupResult::Corrupt;
    check = zero + 1;
  }
  if (check != dictionary_end) return LookupResult::Corrupt;

  char* dest = scratch + dictionary_size;
  const char* const output = dest;
  const char* const end = reinterpret_cast<char*>(encoded) - 1;
  while (*encoded) {
    const uint8_t byte = *encoded++;
    const uint8_t* word = &byte;
    size_t length = 1;
    if (byte >= 128) {
      const unsigned int token = byte - 128;
      const char* value = scratch;
      for (unsigned int n = 0; n < token; ++n) value += strlen(value) + 1;
      word = reinterpret_cast<const uint8_t*>(value);
      length = strlen(value);
    }
    if (length > static_cast<size_t>(end - dest)) return LookupResult::Corrupt;
    memcpy(dest, word, length);
    dest += length;
  }
  *dest = 0;
  text = output;
  return LookupResult::Found;
}

}} // namespace mesh::bible
