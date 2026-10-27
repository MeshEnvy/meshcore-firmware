/* MeshCore extension (2026): read a NUL-delimited record through a caller-owned
 * 4 KiB DEFLATE history. It shares tinf's bounded bit reader and Huffman trees.
 * No heap, permanent cache, or second output/history buffer is used. */
struct tinf_record_output {
  unsigned char *window;
  unsigned int size, produced, limit, remaining, length;
};

static int tinf_record_emit(struct tinf_record_output *out, unsigned char byte)
{
  if (out->produced >= out->limit) return TINF_DATA_ERROR;
  out->window[out->produced++ & (out->size - 1)] = byte;
  if (byte == 0) {
    if (!out->remaining) return 1;
    --out->remaining;
    out->length = 0;
  } else if (++out->length >= out->size) {
    return TINF_BUF_ERROR;
  }
  return TINF_OK;
}

static int tinf_record_block(struct tinf_data *d, struct tinf_record_output *out)
{
  static const unsigned char length_bits[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
  };
  static const unsigned short length_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
  };
  static const unsigned char dist_bits[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
  };
  static const unsigned short dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
  };
  for (;;) {
    int sym = tinf_decode_symbol(d, &d->ltree);
    if (d->overflow) return TINF_DATA_ERROR;
    if (sym == 256) return TINF_OK;
    if (sym < 256) {
      const int result = tinf_record_emit(out, (unsigned char) sym);
      if (result) return result;
    } else {
      int length, distance, offset;
      if (sym > d->ltree.max_sym || sym - 257 > 28 || d->dtree.max_sym == -1)
        return TINF_DATA_ERROR;
      sym -= 257;
      length = tinf_getbits_base(d, length_bits[sym], length_base[sym]);
      distance = tinf_decode_symbol(d, &d->dtree);
      if (distance > d->dtree.max_sym || distance > 29) return TINF_DATA_ERROR;
      offset = tinf_getbits_base(d, dist_bits[distance], dist_base[distance]);
      if (d->overflow || (unsigned int) offset > out->produced
          || (unsigned int) offset > out->size) return TINF_DATA_ERROR;
      while (length--) {
        const unsigned char byte = out->window[(out->produced - offset) & (out->size - 1)];
        const int result = tinf_record_emit(out, byte);
        if (result) return result;
      }
    }
  }
}

static void tinf_record_reverse(unsigned char *begin, unsigned char *end)
{
  while (begin < end) {
    unsigned char byte;
    --end;
    if (begin == end) break;
    byte = *begin;
    *begin++ = *end;
    *end = byte;
  }
}

int tinf_uncompress_record(void *window, unsigned int window_size,
    unsigned int *record_length, const void *source, unsigned int source_len,
    unsigned int output_limit, unsigned int record_index)
{
  struct tinf_data d;
  struct tinf_record_output out;
  int final = 0;
  if (!window || !record_length || !source || !source_len || !output_limit
      || window_size != 4096) return TINF_DATA_ERROR;
  *record_length = 0;
  d.source = (const unsigned char *) source;
  d.source_end = d.source + source_len;
  d.tag = 0;
  d.bitcount = d.overflow = 0;
  out.window = (unsigned char *) window;
  out.size = window_size;
  out.produced = out.length = 0;
  out.limit = output_limit;
  out.remaining = record_index;
  do {
    unsigned int type;
    int result;
    final = tinf_getbits(&d, 1);
    type = tinf_getbits(&d, 2);
    if (d.overflow) return TINF_DATA_ERROR;
    if (type == 0) {
      unsigned int length;
      if (d.source_end - d.source < 4) return TINF_DATA_ERROR;
      length = read_le16(d.source);
      if (length != (~read_le16(d.source + 2) & 0xffff)) return TINF_DATA_ERROR;
      d.source += 4;
      if ((unsigned int) (d.source_end - d.source) < length) return TINF_DATA_ERROR;
      d.tag = 0;
      d.bitcount = 0;
      result = TINF_OK;
      while (length--) {
        result = tinf_record_emit(&out, *d.source++);
        if (result) break;
      }
    } else if (type == 1 || type == 2) {
      if (type == 1) {
        tinf_build_fixed_trees(&d.ltree, &d.dtree);
        result = TINF_OK;
      } else result = tinf_decode_trees(&d, &d.ltree, &d.dtree);
      if (result == TINF_OK) result = tinf_record_block(&d, &out);
    } else return TINF_DATA_ERROR;
    if (result == 1) {
      /* Rotate in place: the selected record may cross the circular boundary.
       * Include its NUL and use only initialized history bytes. */
      const unsigned int used = out.produced < out.size ? out.produced : out.size;
      const unsigned int begin = (out.produced - out.length - 1) & (out.size - 1);
      tinf_record_reverse(out.window, out.window + begin);
      tinf_record_reverse(out.window + begin, out.window + used);
      tinf_record_reverse(out.window, out.window + used);
      *record_length = out.length;
      return TINF_OK;
    }
    if (result != TINF_OK) return result;
  } while (!final);
  return TINF_DATA_ERROR; /* Selected record or its terminator is missing. */
}
