#include <vector>
#include <cstdint>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "binlog_undo.h"

using namespace binary_log;

static char magic[]= {'\xfe', '\x62', '\x69', '\x6e', '\x00'};


static const char *event_names[ENUM_END_EVENT] = {
"UNKNOWN_EVENT",
"START_EVENT_V3",
"QUERY_EVENT",
"STOP_EVENT",
"ROTATE_EVENT",
"INTVAR_EVENT",
"LOAD_EVENT",
"SLAVE_EVENT",
"CREATE_FILE_EVENT",
"APPEND_BLOCK_EVENT",
"EXEC_LOAD_EVENT",
"DELETE_FILE_EVENT",
"NEW_LOAD_EVENT",
"RAND_EVENT",
"USER_VAR_EVENT",
"FORMAT_DESCRIPTION_EVENT",
"XID_EVENT",
"BEGIN_LOAD_QUERY_EVENT",
"EXECUTE_LOAD_QUERY_EVENT",
"TABLE_MAP_EVENT",
"PRE_GA_WRITE_ROWS_EVENT",
"PRE_GA_UPDATE_ROWS_EVENT",
"PRE_GA_DELETE_ROWS_EVENT",
"WRITE_ROWS_EVENT_V1",
"UPDATE_ROWS_EVENT_V1",
"DELETE_ROWS_EVENT_V1",
"INCIDENT_EVENT",
"HEARTBEAT_LOG_EVENT",
"IGNORABLE_LOG_EVENT",
"ROWS_QUERY_LOG_EVENT",
"WRITE_ROWS_EVENT",
"UPDATE_ROWS_EVENT",
"DELETE_ROWS_EVENT",
"GTID_LOG_EVENT",
"ANONYMOUS_GTID_LOG_EVENT",
"PREVIOUS_GTIDS_LOG_EVENT",
"TRANSACTION_CONTEXT_EVENT",
"VIEW_CHANGE_EVENT",
"XA_PREPARE_LOG_EVENT"
};


void printhex(char *p, size_t n)
{
  for (size_t i = 0; i < n; ++i) {
    printf("%02x ", (uint8_t)(p[i]));
  }
  putchar('\n');
}

BinlogUndo::BinlogUndo(FILE *in_fd, FILE *out_fd, size_t max_event_size):
  in_fd(in_fd),out_fd(out_fd),
  max_event_size(max_event_size * 1048576),
  fde(NULL),
  has_checksum(false),
  is_rewrite_server_id(false),
  server_id(0),
  current_event_pos(0),
  current_event_len(0)
{
  event_buffer = new char[this->max_event_size];
  swap_buffer = new char[this->max_event_size];
} 

BinlogUndo::~BinlogUndo()
{
  delete fde;
  delete event_buffer;
  delete swap_buffer;
}

Result BinlogUndo::read_event_header()
{
  int n = fread(event_buffer, sizeof(char), LOG_EVENT_HEADER_LEN, in_fd);
  if (n != LOG_EVENT_HEADER_LEN) {
    if (n == 0 && feof(in_fd)) {
      return BU_EOF;
    } 
    return BU_IO_ERROR;
  }
  new (&current_header)Log_event_header(event_buffer, BINLOG_VERSION);
  if (!quiet) {
    unsigned int tmp_type_code = current_header.type_code;
    if (tmp_type_code >= ENUM_END_EVENT || tmp_type_code < 0) {
      tmp_type_code = 0;
    }
    printf("@%lu %s(%d) size: %lu; next pos: %llu\n", 
      current_event_pos,
      event_names[tmp_type_code], 
      current_header.type_code,
      current_header.data_written,
      current_header.log_pos);
  }
  if (current_header.type_code >= ENUM_END_EVENT ||
       current_header.type_code <= 0 ||
       current_header.log_pos - current_header.data_written != current_event_pos) {
    return BU_CORRUPT_EVENT; 
  }
  current_event_len = current_header.data_written;
  if (has_checksum) {
    current_event_len-= BINLOG_CHECKSUM_LEN;
  }
  return BU_OK;
}

Result BinlogUndo::read_fde()
{
  current_event_pos = BIN_LOG_HEADER_SIZE;
  fseek(in_fd, current_event_pos, SEEK_SET); 
  read_event_header();
  if (current_header.type_code != FORMAT_DESCRIPTION_EVENT) {
    return BU_UNEXCEPTED_EVENT_TYPE;
  } 
  //printf("%d %lu %lld\n", current_header.type_code, current_header.data_written, current_header.log_pos);
  Result result = read_event_body();
  ASSERT_BU_OK(result);
  Format_description_event tmp(3, "");
  fde = new Format_description_event(event_buffer, current_header.data_written, &tmp);
  fde->footer()->checksum_alg = fde->footer()->get_checksum_alg(event_buffer, current_header.data_written);
  has_checksum = (fde->footer()->checksum_alg == BINLOG_CHECKSUM_ALG_CRC32);
  return BU_OK;
}

Result BinlogUndo::read_event_body()
{
  if (current_header.data_written > max_event_size) {
    return BU_EVENT_TOO_BIG;
  }
  int rest = current_header.data_written - LOG_EVENT_HEADER_LEN; 
  int n = fread(event_buffer + LOG_EVENT_HEADER_LEN, sizeof(char), rest, in_fd);
  if (n != rest) {
    return BU_IO_ERROR;
  }
  current_event_pos = current_header.log_pos;
  return BU_OK;
}

Result BinlogUndo::scan_begin()
{
  Result result;
  result = read_event_header();
  ASSERT_BU_OK(result);
  if (current_header.type_code == GTID_LOG_EVENT || current_header.type_code == ANONYMOUS_GTID_LOG_EVENT) {
    result = read_event_header_at(current_header.log_pos);
    ASSERT_BU_OK(result);
  }
  if (current_header.type_code == ROTATE_EVENT ||
      current_header.type_code == STOP_EVENT) {
    return BU_EOF;
  }
  if (current_header.type_code != QUERY_EVENT || current_header.data_written > 100) { // much bigger than begin event(79)
    return BU_UNEXCEPTED_EVENT_TYPE;
  }
  result = read_event_body();
  ASSERT_BU_OK(result);
  Query_event begin(event_buffer, current_event_len, fde, QUERY_EVENT);
  if (begin.q_len != 5 || memcmp(begin.query, "BEGIN", 5)) { 
    //printf("%ld %s\n", begin.q_len, begin.query);
    return BU_UNEXCEPTED_EVENT_TYPE;
  }
  //begin.print_event_info(std::cout);
  //printf("BEGIN\n");
  transactions.resize(transactions.size()+1);
  transactions.back().begin = {
    current_header.log_pos - current_header.data_written,
    current_header.data_written
  };
  return BU_OK;
}

Result BinlogUndo::scan_row_or_xid()
{
  // TODO: deal with multipal row events
  Result result = read_event_header();
  ASSERT_BU_OK(result);

  switch (current_header.type_code) {
  case TABLE_MAP_EVENT:
    if (current_header.data_written > MAX_TABLE_MAP_SIZE) {
      return BU_EVENT_TOO_BIG;
    }
    //printf("TABLE_MAP\n");
    transactions.back().rows.push_back(Event{
      current_event_pos,
      current_header.data_written
    });
    break;
  case WRITE_ROWS_EVENT:
  case UPDATE_ROWS_EVENT:
  case DELETE_ROWS_EVENT:
  case ROWS_QUERY_LOG_EVENT:
    //printf("ROW\n");
    break;
  case XID_EVENT:
    //printf("XID\n");
    result = BU_END_TRANSACTION;
    transactions.back().xid = {
      current_event_pos,
      current_header.data_written
    };
    break;
  default:
    return BU_UNEXCEPTED_EVENT_TYPE;
  }
  current_event_pos = current_header.log_pos;
  fseek(in_fd, current_event_pos, SEEK_SET); 
  return result; 
}

Result BinlogUndo::scan(size_t pos)
{
  Result result;
  result = read_fde();
  ASSERT_BU_OK(result);
  //fde->print_event_info(std::cout);
  //printf("\n");
  current_event_pos = pos;
  fseek(in_fd, pos, SEEK_SET);
  uint32_t n_trans = 0; 
  while (true) {
    result = scan_begin();
    if (result != BU_OK) {
      if (result == BU_EOF) {
        break;
      }
      return result;
    }
    while (true) {
      result = scan_row_or_xid();
      if (result == BU_END_TRANSACTION) {
        break;
      }
      ASSERT_BU_OK(result);
    } // rows
    ++n_trans;   
  } // transactions
  if (n_trans == 0) {
    return BU_NO_TRANSACTIONS;
  }
  log("transactions to undo: %u\n", n_trans);
  return BU_OK;
}

Result BinlogUndo::output()
{
  //ftruncate(fd_out, 0); 
  Result result;
  fwrite(magic, sizeof(char), BIN_LOG_HEADER_SIZE, out_fd);
  result = copy_event_data({
    BIN_LOG_HEADER_SIZE, fde->header()->data_written
  });
  ASSERT_BU_OK(result);
  char table_map_buf[MAX_TABLE_MAP_SIZE];
  for (std::vector<Trans>::reverse_iterator trans_itr = transactions.rbegin();
       trans_itr != transactions.rend();
       ++trans_itr) {
    //printf("begin: %ld %ld\n", trans_itr->begin.pos, trans_itr->begin.size);
    result = copy_event_data(trans_itr->begin);
    if (result != BU_OK) {
      return result;
    }
    //printf("rs %d\n", trans_itr->rows.size());
    for (std::vector<Event>::reverse_iterator row_itr = trans_itr->rows.rbegin();
         row_itr != trans_itr->rows.rend();
         ++row_itr) {
      //printf("row_tm: %ld %ld\n", row_itr->pos, row_itr->size);
      result = read_event_at(row_itr->pos);
      ASSERT_BU_OK(result);

      memcpy(table_map_buf, event_buffer, row_itr->size);
      Table_map_event table_map(table_map_buf, current_event_len, fde);

      result = write_event_data(*row_itr);
      ASSERT_BU_OK(result);
      size_t row_pos = current_header.log_pos;
      do {
        result = read_event_header_at(row_pos);
        ASSERT_BU_OK(result);
        if (current_header.type_code == ROWS_QUERY_LOG_EVENT) {
          row_pos = current_header.log_pos; 
          continue;
        }
        if (current_header.type_code == TABLE_MAP_EVENT) {
          break;
        }
        result = read_event_body();
        ASSERT_BU_OK(result);
        result = revert_row_data(&table_map);
        ASSERT_BU_OK(result);
        result = write_event_data({ 
          row_pos, 
          current_header.data_written
        });
        ASSERT_BU_OK(result);
        row_pos = current_header.log_pos; 
        //printf("%ld %ld\n", row_pos, trans_itr->xid.pos);
      } while(row_pos < trans_itr->xid.pos);
    }
    //printf("xid: %ld %ld\n", trans_itr->xid.pos, trans_itr->xid.size);
    result = copy_event_data(trans_itr->xid);
    ASSERT_BU_OK(result);
  }
  return BU_OK;
}

Result BinlogUndo::read_event_data(Event e)
{
  return read_event_at(e.pos);
}

Result BinlogUndo::write_event_data(Event e)
{
  rewrite_server_id();
  rewrite_checksum(); 
  size_t n = fwrite(event_buffer, sizeof(char), e.size, out_fd);
  if (n != e.size) {
    return BU_IO_ERROR;
  }
  return BU_OK;
}

Result BinlogUndo::copy_event_data(Event e)
{
  Result result = read_event_data(e);
  ASSERT_BU_OK(result);
  return write_event_data(e);
}

Result BinlogUndo::revert_row_data(Table_map_event *table_map)
{
  Slice sl = calc_rows_body_slice();
  //printf("sz:%ld\n", sl.size);
  Slice dt_sl;
  uint32_t col_num;
  Slice mp_sl;
  Result result = calc_row_data(current_header.type_code, sl, &col_num, &mp_sl, &dt_sl);
  ASSERT_BU_OK(result);

  if (current_header.type_code == WRITE_ROWS_EVENT) {
    event_buffer[EVENT_TYPE_OFFSET] = DELETE_ROWS_EVENT;
  }
  else if (current_header.type_code == DELETE_ROWS_EVENT) {
    event_buffer[EVENT_TYPE_OFFSET] = WRITE_ROWS_EVENT;
  }
  else if (current_header.type_code == UPDATE_ROWS_EVENT) {
    result = swap_update_row(mp_sl, dt_sl, col_num, table_map);
    ASSERT_BU_OK(result)
  }
  else {
    return BU_UNEXCEPTED_EVENT_TYPE;
  }
  rewrite_server_id();
  rewrite_checksum(); 
  return BU_OK;
}

Result BinlogUndo::read_event_at(size_t pos)
{
  Result result = read_event_header_at(pos); 
  ASSERT_BU_OK(result);
  return read_event_body();
}

Result BinlogUndo::read_event_header_at(size_t pos)
{
  current_event_pos = pos;
  fseek(in_fd, pos, SEEK_SET);
  return read_event_header();
}

void BinlogUndo::rewrite_checksum() 
{
  if (!has_checksum) {
    return;
  }
  uint32_t checksum;
  checksum = checksum_crc32(0L, NULL, 0);
  checksum = checksum_crc32(checksum,
                       (const unsigned char*) event_buffer,
                       current_header.data_written - BINLOG_CHECKSUM_LEN);
  checksum = htole32(checksum);  
  *(uint32_t*)(&event_buffer[current_header.data_written - BINLOG_CHECKSUM_LEN]) = checksum;
}

void BinlogUndo::rewrite_server_id()
{
  if (!is_rewrite_server_id) {
    return;
  }
  *(uint32_t*)(event_buffer + SERVER_ID_OFFSET) = server_id;
}

/**
 * see rows_event.cpp
 */
Slice BinlogUndo::calc_rows_body_slice()
{
  Log_event_type event_type = current_header.type_code;
  uint8_t post_header_len = fde->post_header_len[event_type - 1];
  char *ptr_data = event_buffer + LOG_EVENT_HEADER_LEN + post_header_len;
  if (post_header_len == Binary_log_event::ROWS_HEADER_LEN_V2) {
    uint16_t var_header_len = *(uint16_t*)(ptr_data - 2);
    var_header_len = le16toh(var_header_len);
    ptr_data+= var_header_len;
  }
  ptr_data-= 2;
  size_t data_size = current_event_len - (ptr_data - event_buffer);
  return { p:ptr_data, size:data_size };
}

Result BinlogUndo::calc_row_data(Log_event_type event_type, Slice body, uint32_t *number_of_fields, Slice *field_bitset_slice, Slice *data_slice)
{
  char *pos = body.p; // copy a point, as get_field_length will change it
  *number_of_fields = (uint32_t)get_field_length((unsigned char**)&pos);
  // number_of_fields > 4096
  uint32_t bitmap_len = (*number_of_fields+7)/8;
  if (event_type == UPDATE_ROWS_EVENT) {
    bitmap_len*= 2;
  }
  for (size_t i = 0; i < bitmap_len; ++i) {
    if (pos[i] != '\xff') {
      //printf("b[%ld] %d\n", i, pos[i]);
      return BU_NOT_FULL_ROW_IMAGE;
    }
  }
  *field_bitset_slice = { p: pos, size: bitmap_len };
  *data_slice = {
    p:    pos + bitmap_len, 
    size: body.size - (pos-body.p) - bitmap_len
  };
  return BU_OK;
}


Result BinlogUndo::swap_update_row(Slice present, Slice data, uint32_t num_col, Table_map_event *table_map) 
{
  if (table_map->m_colcnt != num_col) {
    return BU_CORRUPT_EVENT;
  }
  //printhex(data.p, data.size);
  //printf("%ld\n", data.size);
  char *pos = data.p;
  //printf("bits:%d\n", 0xffff&(*(short*)pos));
  Bitset present_set(present.p);
  int present_bitmap_len = (num_col+7)/8; 
  Bitset null_set(pos);
  int null_bit_num = 0;
  for (uint32_t i = 0; i < num_col; ++i) {
    if (present_set.get(i)) {
      ++null_bit_num;
    }
  } 
  uint16_t field_metadata[num_col];
  fill_metadata(table_map, field_metadata); 
  //printhex((char *)field_metadata, num_col * sizeof(uint16_t));
  //uint32_t bitmap_len = (num_col+7)/8;
  pos+= (null_bit_num+7)/8;
  int null_i = -1;
  for (uint32_t i = 0; i < num_col; ++i) {
    //printf("col: %d %d %d\n", i, table_map->m_coltype[i], present_set.get(i));
    if (!present_set.get(i)) {
      continue;
    }
    ++null_i;
    if (null_set.get(null_i)) {
      continue;
    }
    //printf("type: %d\n", table_map->m_coltype[i]);
    size_t field_size = calc_field_size((uint8_t)(table_map->m_coltype[i]), (uint8_t*)pos, field_metadata[i]);

    //printf("fs: %lu\n", field_size);
    pos+= field_size;
  }
  size_t len_old = pos - data.p;
  size_t len_new = data.size - len_old;
  if (len_old + len_new > data.size) {
    return BU_CORRUPT_EVENT;
  }
  //printf("left: %ld; right: %ld\n", len_old, len_new);
  swap(present.p, present_bitmap_len, present_bitmap_len); 
  swap(data.p, len_old, len_new);
  return BU_OK;
  //printhex(data.p, data.size);
}

void BinlogUndo::swap(char *str, size_t first, size_t second)
{
  //printhex(str, first+second);
  memcpy(swap_buffer, str, first);
  memcpy(str, str + first, second);
  memcpy(str + second, swap_buffer, first);
  /*
  size_t gcd;
  size_t a = first, b = second, c = 0;
  size_t len = first+second;
  while ( a != 0 ) {
     c = a; a = b%a;  b = c;
  }
  gcd = b;
  char t1, t2;
  size_t step = len/gcd;
  for (uint32_t i = 0; i < gcd; ++i) {
    size_t p1 = i;
    t1 = str[p1];
    for (uint32_t j = 0; j < step; ++j) {
      size_t p2 = p1 + second;
      if (p2 >= len) {
        p2-= len;
      }
      t2 = str[p2];
      str[p2] = t1;
      t1 = t2;
      p1 = p2;
    }
  }
  */
  //printhex(str, first+second);
}

void BinlogUndo::set_server_id(uint32_t server_id)
{
  //printf("%d\n", server_id);
  is_rewrite_server_id = (server_id != UINT32_MAX);
  this->server_id = server_id; 
}

Bitset::Bitset(char *ptr):p(ptr){}

bool Bitset::get(size_t n)
{
  char *ptr = p + (n / 8);
  char v = '\x01' << (n % 8);
  //printf("*ptr:%x v:%x ^:%x\n", 0xff&(*ptr), 0xff&v,(*ptr) & v);
  return ((*ptr) & v) != 0;
}

void BinlogUndo::log(const char* format, ...) {
  if (quiet) {
    return;
  }
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
}

void BinlogUndo::set_quiet(bool quiet) {
  this->quiet = quiet;
}

// copied from <MYSQL_SRC>/include/byte_order_generic_x86.h
static inline uint16_t uint2korr(const unsigned char *A) { return *((uint16_t*) A); }

void fill_metadata(Table_map_event *table_map,  uint16_t *metadata_out) {
  if (table_map->m_colcnt == 0 || table_map->m_field_metadata_size == 0) {
    return;
  }
  int index= 0;
  for (unsigned int i= 0; i < table_map->m_colcnt; i++) {
    switch (table_map->m_coltype[i]) {
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_JSON:
    {
      /*
        These types store a single byte.
      */
      metadata_out[i]= table_map->m_field_metadata[index];
      index++;
      break;
    }
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_STRING:
    {
      uint16_t x= table_map->m_field_metadata[index++] << 8U; // real_type
      x+= table_map->m_field_metadata[index++];            // pack or field length
      metadata_out[i]= x;
      break;
    }
    case MYSQL_TYPE_BIT:
    {
      uint16_t x= table_map->m_field_metadata[index++];
      x = x + (table_map->m_field_metadata[index++] << 8U);
      metadata_out[i]= x;
      break;
    }
    case MYSQL_TYPE_VARCHAR:
    {
      /*
        These types store two bytes.
      */
      unsigned char *ptr= &table_map->m_field_metadata[index];
      metadata_out[i]= uint2korr(ptr);
      index= index + 2;
      break;
    }
    case MYSQL_TYPE_NEWDECIMAL:
    {
      uint16_t x= table_map->m_field_metadata[index++] << 8U; // precision
      x+= table_map->m_field_metadata[index++];            // decimals
      metadata_out[i]= x;
      break;
    }
    case MYSQL_TYPE_TIME2:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP2:
      metadata_out[i]= table_map->m_field_metadata[index++];
      break;
    default:
      metadata_out[i]= 0;
      break;
    }
    //printf("md: %d\n", metadata_out[i]);
  }
}

