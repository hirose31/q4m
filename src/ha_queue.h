/*
 * Copyright (C) 2007,2008 Cybozu Labs, Inc.
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef HA_QUEUE_H
#define HA_QUEUE_H

#include "queue_cond.h"

// error numbers should be less than HA_ERR_FIRST
#define QUEUE_ERR_RECORD_EXISTS (1)

#define QUEUE_MAX_SOURCES (64)

class queue_share_t;

class queue_row_t {
  /* size is stored in the lower 30 bits, while upper 2 bits are used for
   * attributes.  if type == type_checksum, lower 30 bits adler32 is stored
   * in _size, and size of the checksum is stored in the body in type my_off_t.
   */
  char  _size[4];
  uchar _bytes[1];
public:
  enum {
    type_mask         = 0xe0000000,
    type_row          = 0x00000000,
    type_row_received = 0x20000000,
    type_checksum     = 0x40000000,
    type_flag_removed = 0x80000000,
    size_mask         = ~type_mask,
    max_size          = ~type_mask
  };
  queue_row_t() {} // build uninitialized
  queue_row_t(unsigned size, unsigned type) {
    assert((size & type_mask) == 0);
    int4store(_size, size | type);
  }
  unsigned size() const {
    // NOTE: does not check if the row isn't checksum
    return uint4korr(_size) & size_mask;
  }
  unsigned checksum() const {
    return size();
  }
  unsigned type() const {
    return uint4korr(_size) & type_mask;
  }
  void set_type(unsigned type) {
    assert((type & size_mask) == 0);
    int4store(_size, (uint4korr(_size) & size_mask) | type);
  }
  uchar *bytes() { return _bytes; }
  static size_t header_size() {
    return my_offsetof(queue_row_t, _bytes[0]);
  }
  static size_t checksum_size() {
    return header_size() + sizeof(my_off_t);
  }
  my_off_t next(my_off_t off) {
    return off + header_size()
      + (type() != type_checksum ? size() : sizeof(my_off_t));
  }
  my_off_t validate_checksum(int fd, my_off_t off);
  // my_free should be used on deallocation
  static queue_row_t *create_checksum(const iovec* iov, int iovcnt);
private:
  queue_row_t(const queue_row_t&);
  queue_row_t& operator=(const queue_row_t&);
};

class queue_file_header_t {
public:
  enum {
    MAGIC_V1      = 0x304d3451, // 'Q4M0' in little endian
    MAGIC_V2      = 0x314d3451, // 'Q4M1' in little endian, w. conditional wait
    attr_is_dirty = 0x1
  };
private:
  char _magic[4];
  char _attr[4];
  char _end[8];
  char _begin[8];
  char _compaction_offset[8];
  char _last_received_offsets[QUEUE_MAX_SOURCES][8];
  unsigned _padding[1024 - (4 + 4 + 8 + 8 + 8 + QUEUE_MAX_SOURCES * 8)];
public:
  queue_file_header_t();
  unsigned magic() const { return uint4korr(_magic); }
  unsigned attr() const { return uint4korr(_attr); }
  void set_attr(unsigned a) { int4store(_attr, a); }
  my_off_t end() const { return uint8korr(_end); }
  void set_end(my_off_t e) { int8store(_end, e); }
  my_off_t begin() const { return uint8korr(_begin); }
  void set_begin(my_off_t b) { int8store(_begin, b); }
  my_off_t compaction_offset() const { return uint8korr(_compaction_offset); }
  void set_compaction_offset(my_off_t co) { int8store(_compaction_offset, co); }
  my_off_t last_received_offset(unsigned i) const {
    return uint8korr(_last_received_offsets[i]);
  }
  void set_last_received_offset(unsigned i, my_off_t o) {
    int8store(_last_received_offsets[i], o);
  }
  void write(int fd);
};

struct queue_source_t {
  char _offset[8];
  unsigned char _sender;
  unsigned sender() const { return _sender; }
  void set_sender(unsigned s) { _sender = s; }
  my_off_t offset() const { return uint8korr(_offset); }
  void set_offset(my_off_t o) { int8store(_offset, o); }
  queue_source_t(unsigned s, my_off_t o) {
    set_sender(s);
    set_offset(o);
  }
};

class queue_fixed_field_t {
protected:
  char *nam;
  size_t sz;
  size_t null_off;
  uchar null_bit;
public:
  queue_fixed_field_t(const TABLE *t, const Field *f, size_t s)
  : nam(new char [strlen(f->field_name) + 1]), sz(s),
    null_off(f->null_ptr != NULL ? f->null_ptr - t->record[0] : 0),
    null_bit(f->null_ptr != NULL ? f->null_bit : 0) {
    strcpy(nam, f->field_name);
  }
  virtual ~queue_fixed_field_t() { delete [] nam; }
  virtual bool is_convertible() const { return false; }
  virtual queue_cond_t::value_t get_value(const uchar *buf, size_t off) const {
    return queue_cond_t::value_t::null_value();
  }
  bool is_null(const uchar *buf) const {
    return (buf[null_off] & null_bit) != 0;
  }
  const char *name() const { return nam; }
  size_t size() const { return sz; }
};

template<size_t N> class queue_int_field_t : public queue_fixed_field_t {
protected:
  bool is_unsigned;
public:
  queue_int_field_t(const TABLE *t, const Field *f)
  : queue_fixed_field_t(t, f, N),
    is_unsigned(f->key_type() == HA_KEYTYPE_BINARY) {}
  virtual ~queue_int_field_t() {}
  virtual bool is_convertible() const { return true; }
  virtual queue_cond_t::value_t get_value(const uchar *buf, size_t off) const {
    long long v;
    switch (N) {
#define TYPEREAD(sz, rd) \
    case sz: \
      v = rd; \
      if (! is_unsigned && (v & (LLONG_MIN >> (64 - sz * 8))) != 0) \
	v |= LLONG_MIN >> (64 - sz * 8); \
      break
      TYPEREAD(1, buf[off]);
      TYPEREAD(2, uint2korr(buf + off));
      TYPEREAD(3, uint3korr(buf + off));
      TYPEREAD(4, uint4korr(buf + off));
      TYPEREAD(8, uint8korr(buf + off));
    default:
      assert(0);
    }
    return queue_cond_t::value_t::int_value(v);
  }
};

typedef std::list<std::pair<pthread_t, my_off_t> > queue_rows_owned_t;

class queue_share_t {
  
 public:
  struct append_t {
    const void *rows;
    size_t rows_size;
    const queue_source_t *source;
    int err; /* -1 if not completed, otherwise HA_ERR_XXX or 0 */
    append_t(const void *r, size_t rs, const queue_source_t *s)
    : rows(r), rows_size(rs), source(s), err(-1) {
    }
  private:
    append_t(const append_t&);
    append_t& operator=(const append_t&);
  };
  typedef std::vector<append_t*> append_list_t;
  
  struct remove_t {
    my_off_t *offsets;
    int cnt;
    int err; /* -1 if not completed, otherwise HA_ERR_XXX or 0 */
    remove_t(my_off_t *o, int c)
    : offsets(o), cnt(c), err(-1) {
    }
  };
  typedef std::vector<remove_t*> remove_list_t;
  
  struct cond_expr_data_t {
    queue_share_t *share;
    queue_cond_t::node_t *node;
    char *expr;
    size_t expr_len;
    size_t ref_cnt;
    my_off_t pos;
    cond_expr_data_t(queue_share_t *s, queue_cond_t::node_t *n, const char *e,
		     size_t el)
    : share(s), node(n), expr(new char [el]), expr_len(el), ref_cnt(0), pos(0)
    {
      std::copy(e, e + el, expr);
    }
    void release() {
      delete expr;
      delete node;
    }
  };
  typedef std::list<cond_expr_data_t> cond_expr_data_list_t;
  
  class cond_expr_t {
    cond_expr_data_t *d;
  public:
    cond_expr_t(cond_expr_data_t *_d) : d(_d) {
      if (d != NULL) {
	d->ref_cnt++;
      }
    }
    cond_expr_t(const cond_expr_t &x) : d(x.d) {
      if (d != NULL) {
	d->ref_cnt++;
      }
    }
    ~cond_expr_t() {
      if (d != NULL) {
	if (--d->ref_cnt == 0) {
	  d->share->release_cond_expr(d);
	}
      }
    }
    cond_expr_t& operator=(const cond_expr_t& x) {
      if (this != &x) {
	if (d != NULL) {
	  if (--d->ref_cnt == 0) {
	    d->share->release_cond_expr(d);
	  }
	}
	d = x.d;
	if (d != NULL) {
	  d->ref_cnt++;
	}
      }
      return *this;
    }
    bool is_valid() const { return d != NULL; }
    const queue_cond_t::node_t *node() const { return d->node; }
    my_off_t pos() const { return d->pos; }
    void set_pos(my_off_t pos) { d->pos = pos; }
  };
  
  struct listener_t {
    pthread_cond_t cond;
    queue_share_t *signalled_by;
    pthread_t listener;
    listener_t(const pthread_t& t) : signalled_by(NULL), listener(t) {
      pthread_cond_init(&cond, NULL);
    }
    ~listener_t() {
      pthread_cond_destroy(&cond);
    }
  };
  typedef std::list<std::pair<listener_t*, cond_expr_t*> > listener_list_t;
  
 private:
  uint use_count;
  char *table_name;
  uint table_name_length;
  
  pthread_mutex_t mutex, append_mutex;
  THR_LOCK store_lock;
  
  int fd;
  queue_file_header_t _header;
  
  struct {
    my_off_t off;
    char buf[4096];
  } cache;
  
  queue_rows_owned_t rows_owned;
  
  listener_list_t listener_list; /* access serialized using g_mutex */
  
  int num_readers;
  
  pthread_t writer_thread;
  pthread_cond_t to_writer_cond;
  pthread_cond_t *from_writer_cond;
  pthread_cond_t _from_writer_conds[2];
  bool writer_exit;
  append_list_t *append_list;
  remove_list_t *remove_list;
  queue_cond_t cond_eval;
  cond_expr_data_list_t active_cond_expr_list;
  cond_expr_data_list_t inactive_cond_expr_list;
  /* following fields are for V2 type table only */
  queue_fixed_field_t **fixed_fields;
  size_t null_bytes;
  size_t fields;
  uchar *fixed_buf;
  size_t fixed_buf_size;
  
public:
  void fixup_header();
  static uchar *get_share_key(queue_share_t *share, size_t *length,
			      my_bool not_used);
  static queue_share_t *get_share(TABLE *table, const char* table_name);
  void release();
  void lock() { pthread_mutex_lock(&mutex); }
  void unlock() { pthread_mutex_unlock(&mutex); }
  void lock_reader() { lock(); ++num_readers; unlock(); }
  void unlock_reader();
  void register_listener(listener_t *l, cond_expr_t *c) {
    listener_list.push_back(std::make_pair(l, c));
  }
  void unregister_listener(listener_t *l);
  void wake_listeners();
  static int wait_multi(const std::list<queue_share_t*> &shares,
			pthread_cond_t *c, time_t t);
  
  const char *get_table_name() const { return table_name; }
  THR_LOCK *get_store_lock() { return &store_lock; }
  const queue_file_header_t *header() const { return &_header; }
  queue_fixed_field_t * const *get_fixed_fields() const { return fixed_fields; }
  my_off_t reset_owner(pthread_t owner);
  int write_rows(const void *rows, size_t rows_size);
  /* functions below requires lock */
  const void *read_cache(my_off_t off, ssize_t size, bool populate_cache);
  ssize_t read(void *data, my_off_t off, ssize_t size, bool populate_cache);
  void update_cache(const void *data, my_off_t off, size_t size) {
    if (cache.off == 0
	|| cache.off + sizeof(cache.buf) <= off || off + size <= cache.off) {
      // nothing to do
    } else if (cache.off <= off) {
      memcpy(cache.buf + off - cache.off,
	     data,
	     min(size, sizeof(cache.buf) - (off - cache.off)));
    } else {
      memcpy(cache.buf,
	     static_cast<const char*>(data) + cache.off - off,
	     min(size - (cache.off - off), sizeof(cache.buf)));
    }
  }
  int next(my_off_t *off);
  my_off_t get_owned_row(pthread_t owner, bool remove = false);
  int remove_rows(my_off_t *offsets, int cnt);
  pthread_t find_owner(my_off_t off);
  my_off_t assign_owner(pthread_t owner, cond_expr_t *cond_expr);
  int setup_cond_eval(my_off_t pos);
  cond_expr_t compile_cond_expr(const char *expr, size_t len);
  void release_cond_expr(cond_expr_data_t *d);
private:
  int writer_do_append(append_list_t *l);
  void writer_do_remove(remove_list_t *l);
  void *writer_start();
  static void *_writer_start(void* self) {
    return static_cast<queue_share_t*>(self)->writer_start();
  }
  int compact();
  queue_share_t();
  ~queue_share_t();
  queue_share_t(const queue_share_t&);
  queue_share_t& operator=(const queue_share_t&);
};

struct queue_connection_t {
  bool owner_mode;
  queue_share_t *share_owned;
  queue_source_t source;
  bool reset_source;
  void erase_owned();
  static size_t cnt;
  static queue_connection_t *current(bool create_if_empty = false);
  static int close(handlerton *hton, THD *thd);
private:
  queue_connection_t()
  : owner_mode(false), share_owned(NULL), source(0, 0), reset_source(false) {}
  ~queue_connection_t() {}
};

class ha_queue: public handler
{
  THR_LOCK_DATA lock;
  queue_share_t *share;
  
  my_off_t pos;
  uchar *rows;
  size_t rows_size, rows_reserved;
  size_t bulk_insert_rows; /* should be -1 unless bulk_insertion */
  std::vector<my_off_t> *bulk_delete_rows;
  
 public:
  ha_queue(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_queue();
  
  const char *table_type() const {
    return "QUEUE";
  }
  const char *index_type(uint) {
    return "NONE";
  }
  const char **bas_ext() const;
  ulonglong table_flags() const {
    return HA_NO_TRANSACTIONS | HA_REC_NOT_IN_SEQ | HA_CAN_GEOMETRY
      | HA_CAN_BIT_FIELD | HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE
      | HA_FILE_BASED | HA_NO_AUTO_INCREMENT;
  }

  ulong index_flags(uint, uint, bool) const {
    return 0;
  }
  
  int open(const char *name, int mode, uint test_if_locked);
  int close();
  int rnd_init(bool scan);
  int rnd_end();
  int rnd_next(uchar *buf);
  int rnd_pos(uchar *buf, uchar *_pos);
  void position(const uchar *record);
  
  int info(uint);
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info);

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type);     ///< required
  uint8 table_cache_type() { return HA_CACHE_TBL_NOCACHE; }
  
  void start_bulk_insert(ha_rows rows);
  int end_bulk_insert();
  
  bool start_bulk_delete();
  int end_bulk_delete();
  
  int write_row(uchar *buf);
  int update_row(const uchar *old_data, uchar *new_data);
  int delete_row(const uchar *buf);
 private:
  int prepare_rows_buffer(size_t sz);
  void free_rows_buffer();
  void unpack_row(uchar *buf);
  size_t pack_row(uchar *buf, queue_source_t *source);
};

#undef queue_end

extern "C" {
  my_bool queue_wait_init(UDF_INIT *initid, UDF_ARGS *args, char *message);
  void queue_wait_deinit(UDF_INIT *initid);
  long long queue_wait(UDF_INIT *initid, UDF_ARGS *args, char *is_null,
		       char *error);
  my_bool queue_end_init(UDF_INIT *initid, UDF_ARGS *args, char *message);
  void queue_end_deinit(UDF_INIT *initid);
  long long queue_end(UDF_INIT *initid, UDF_ARGS *args, char *is_null,
		      char *error);
  my_bool queue_abort_init(UDF_INIT *initid, UDF_ARGS *args, char *message);
  void queue_abort_deinit(UDF_INIT *initid);
  long long queue_abort(UDF_INIT *initid, UDF_ARGS *args, char *is_null,
			char *error);
  my_bool queue_rowid_init(UDF_INIT *initid, UDF_ARGS *args, char *message);
  void queue_rowid_deinit(UDF_INIT *initid);
  long long queue_rowid(UDF_INIT *initid, UDF_ARGS *args, char *is_null,
			char *error);
  my_bool queue_set_srcid_init(UDF_INIT *initid, UDF_ARGS *args, char *message);
  void queue_set_srcid_deinit(UDF_INIT *initid);
  long long queue_set_srcid(UDF_INIT *initid, UDF_ARGS *args, char *is_null,
			    char *error);
};

#endif
