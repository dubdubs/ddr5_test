#include <svdpi.h>

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * fake_ddr5.c
 *
 * Minimal DPI-C DDR5 backing store for UVM co-simulation bring-up.
 *
 * Implemented DPI entry points (from ddr5_mm_bfm.sv):
 *   - h2s_ddr5_init_bfm
 *   - h2s_ddr5_write
 *   - h2s_ddr5_read
 *   - h2s_ddr5_read_request
 *   - h2s_ddr5_read_data_requeest
 *   - h2s_ddr5_read_data_blocking
 */

#ifdef __cplusplus
extern "C" {
#endif
void s2h_ddr5_read_data_reply(const svBitVecVal* data, unsigned char valid);
#ifdef __cplusplus
}
#endif

typedef struct ReadRequestNode {
  uint64_t addr;
  struct ReadRequestNode* next;
} ReadRequestNode;

typedef struct StorageNode {
  uint64_t addr;
  svBitVecVal* words;
  struct StorageNode* next;
} StorageNode;

typedef struct Ddr5Instance {
  uint16_t bfm_id;
  unsigned int addr_width;
  unsigned int data_width;
  unsigned int data_byte_width;
  StorageNode* storage_head;
  ReadRequestNode* request_head;
  ReadRequestNode* request_tail;
  struct Ddr5Instance* next;
} Ddr5Instance;

static Ddr5Instance* g_instances = NULL;
static int g_log_level = -1;
static unsigned int g_log_count = 0U;
static unsigned int g_log_max = 0U;

static unsigned int WordCount(unsigned int bit_width) {
  return (bit_width == 0U) ? 1U : ((bit_width + 31U) / 32U);
}

static unsigned int ParseUnsignedEnv(const char* name, unsigned int default_v) {
  const char* raw = getenv(name);
  char* endptr;
  unsigned long value;
  if (raw == NULL || raw[0] == '\0') {
    return default_v;
  }
  value = strtoul(raw, &endptr, 0);
  if (endptr == raw) {
    return default_v;
  }
  return (unsigned int)value;
}

static int ParseBoolEnv(const char* name, int default_v) {
  const char* raw = getenv(name);
  if (raw == NULL || raw[0] == '\0') {
    return default_v;
  }
  if ((strcmp(raw, "1") == 0) || (strcmp(raw, "true") == 0) ||
      (strcmp(raw, "TRUE") == 0) || (strcmp(raw, "on") == 0) ||
      (strcmp(raw, "ON") == 0) || (strcmp(raw, "yes") == 0) ||
      (strcmp(raw, "YES") == 0)) {
    return 1;
  }
  return 0;
}

static void EnsureLogConfig(void) {
  if (g_log_level >= 0) {
    return;
  }

  g_log_level = (int)ParseUnsignedEnv("DDR5_FAKE_LOG_LEVEL", 0U);
  if (g_log_level == 0 && ParseBoolEnv("DDR5_FAKE_LOG", 0)) {
    g_log_level = 1;
  }
  g_log_max = ParseUnsignedEnv("DDR5_FAKE_LOG_MAX", 1000U);
}

static void LogEvent(int level, const char* fmt, ...) {
  va_list ap;
  EnsureLogConfig();
  if (g_log_level < level) {
    return;
  }
  if (g_log_max != 0U && g_log_count >= g_log_max) {
    if (g_log_count == g_log_max) {
      ++g_log_count;
      fprintf(stderr, "[fake_ddr5] log limit reached (%u), suppressing further logs\n",
              g_log_max);
    }
    return;
  }

  ++g_log_count;
  fprintf(stderr, "[fake_ddr5] ");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

static uint32_t GetWord(const svBitVecVal* value, unsigned int index) {
  if (value == NULL) {
    return 0U;
  }
  return (uint32_t)value[index];
}

static uint16_t ReadU16(const svBitVecVal* value) {
  return (uint16_t)(GetWord(value, 0U) & 0xFFFFU);
}

static uint64_t ReadU64(const svBitVecVal* value) {
  return ((uint64_t)GetWord(value, 0U)) |
         (((uint64_t)GetWord(value, 1U)) << 32U);
}

static int MaskBitSet(const svBitVecVal* mask, unsigned int bit_index) {
  const unsigned int word_index = bit_index / 32U;
  const unsigned int bit_offset = bit_index % 32U;
  return ((GetWord(mask, word_index) >> bit_offset) & 1U) != 0U;
}

static uint8_t GetByte(const svBitVecVal* words, unsigned int byte_index) {
  const unsigned int bit_index = byte_index * 8U;
  const unsigned int word_index = bit_index / 32U;
  const unsigned int bit_offset = bit_index % 32U;
  return (uint8_t)((words[word_index] >> bit_offset) & 0xFFU);
}

static void SetByte(svBitVecVal* words, unsigned int byte_index, uint8_t value) {
  const unsigned int bit_index = byte_index * 8U;
  const unsigned int word_index = bit_index / 32U;
  const unsigned int bit_offset = bit_index % 32U;
  const svBitVecVal clear_mask =
      (svBitVecVal)~(((svBitVecVal)0xFFU) << bit_offset);

  words[word_index] =
      (words[word_index] & clear_mask) | (((svBitVecVal)value) << bit_offset);
}

static void FreeStorageList(StorageNode* head) {
  StorageNode* node = head;
  while (node != NULL) {
    StorageNode* next = node->next;
    free(node->words);
    free(node);
    node = next;
  }
}

static void FreeRequestList(ReadRequestNode* head) {
  ReadRequestNode* node = head;
  while (node != NULL) {
    ReadRequestNode* next = node->next;
    free(node);
    node = next;
  }
}

static Ddr5Instance* FindOrCreateInstance(uint16_t bfm_id) {
  Ddr5Instance* instance = g_instances;
  while (instance != NULL) {
    if (instance->bfm_id == bfm_id) {
      return instance;
    }
    instance = instance->next;
  }

  instance = (Ddr5Instance*)calloc(1, sizeof(*instance));
  if (instance == NULL) {
    return NULL;
  }
  instance->bfm_id = bfm_id;
  instance->addr_width = 64U;
  instance->data_width = 256U;
  instance->data_byte_width = 32U;
  instance->next = g_instances;
  g_instances = instance;
  return instance;
}

static StorageNode* FindStorageNode(StorageNode* head, uint64_t addr) {
  StorageNode* node = head;
  while (node != NULL) {
    if (node->addr == addr) {
      return node;
    }
    node = node->next;
  }
  return NULL;
}

static StorageNode* FindOrCreateStorageNode(Ddr5Instance* instance,
                                            uint64_t addr) {
  StorageNode* node;
  unsigned int words;

  if (instance == NULL) {
    return NULL;
  }

  node = FindStorageNode(instance->storage_head, addr);
  if (node != NULL) {
    return node;
  }

  words = WordCount(instance->data_width);
  node = (StorageNode*)calloc(1, sizeof(*node));
  if (node == NULL) {
    return NULL;
  }
  node->words = (svBitVecVal*)calloc(words, sizeof(svBitVecVal));
  if (node->words == NULL) {
    free(node);
    return NULL;
  }
  node->addr = addr;
  node->next = instance->storage_head;
  instance->storage_head = node;
  return node;
}

static void PushReadRequest(Ddr5Instance* instance, uint64_t addr) {
  ReadRequestNode* node;
  if (instance == NULL) {
    return;
  }

  node = (ReadRequestNode*)calloc(1, sizeof(*node));
  if (node == NULL) {
    return;
  }
  node->addr = addr;
  if (instance->request_tail == NULL) {
    instance->request_head = node;
    instance->request_tail = node;
  } else {
    instance->request_tail->next = node;
    instance->request_tail = node;
  }
}

static int PopReadRequest(Ddr5Instance* instance, uint64_t* addr_out) {
  ReadRequestNode* node;

  if (instance == NULL || instance->request_head == NULL) {
    return 0;
  }

  node = instance->request_head;
  instance->request_head = node->next;
  if (instance->request_head == NULL) {
    instance->request_tail = NULL;
  }
  if (addr_out != NULL) {
    *addr_out = node->addr;
  }
  free(node);
  return 1;
}

static void ReplyReadData(Ddr5Instance* instance, uint64_t addr, int valid) {
  const unsigned int words = (instance == NULL) ? 1U : WordCount(instance->data_width);
  svBitVecVal* out_data = (svBitVecVal*)calloc(words, sizeof(svBitVecVal));

  if (out_data == NULL) {
    return;
  }

  if (valid && instance != NULL) {
    StorageNode* node = FindStorageNode(instance->storage_head, addr);
    if (node != NULL && node->words != NULL) {
      memcpy(out_data, node->words, words * sizeof(svBitVecVal));
    }
  }

  s2h_ddr5_read_data_reply(out_data, valid ? (unsigned char)1U : (unsigned char)0U);
  free(out_data);
}

static void InitBfmState(const svBitVecVal* bfm_id,
                         unsigned int addr_width,
                         unsigned int data_width,
                         unsigned int data_byte_width) {
  Ddr5Instance* instance = FindOrCreateInstance(ReadU16(bfm_id));

  if (instance == NULL) {
    return;
  }

  /* Re-init to a clean state for deterministic UVM regression behavior. */
  FreeStorageList(instance->storage_head);
  FreeRequestList(instance->request_head);
  instance->storage_head = NULL;
  instance->request_head = NULL;
  instance->request_tail = NULL;
  instance->addr_width = (addr_width == 0U) ? 64U : addr_width;
  instance->data_width = (data_width == 0U) ? 256U : data_width;
  instance->data_byte_width =
      (data_byte_width == 0U) ? ((instance->data_width + 7U) / 8U)
                              : data_byte_width;
  LogEvent(1,
           "init bfm_id=0x%04x addr_width=%u data_width=%u data_byte_width=%u",
           (unsigned int)instance->bfm_id, instance->addr_width,
           instance->data_width, instance->data_byte_width);
}

void h2s_ddr5_init_bfm(const svBitVecVal* bfm_id, unsigned int data_width) {
  const unsigned int dw = (data_width == 0U) ? 256U : data_width;
  InitBfmState(bfm_id, 64U, dw, (dw + 7U) / 8U);
}

void h2s_ddr5_write(const svBitVecVal* bfm_id,
                    const svBitVecVal* addr,
                    const svBitVecVal* data,
                    const svBitVecVal* mask) {
  const uint16_t id = ReadU16(bfm_id);
  const uint64_t address = ReadU64(addr);
  Ddr5Instance* instance = FindOrCreateInstance(id);
  StorageNode* node;
  unsigned int byte_index;

  if (instance == NULL) {
    return;
  }

  node = FindOrCreateStorageNode(instance, address);
  if (node == NULL || node->words == NULL) {
    return;
  }

  for (byte_index = 0; byte_index < instance->data_byte_width; ++byte_index) {
    if (MaskBitSet(mask, byte_index)) {
      SetByte(node->words, byte_index, GetByte(data, byte_index));
    }
  }
  LogEvent(2, "write bfm_id=0x%04x addr=0x%016llx", (unsigned int)id,
           (unsigned long long)address);
}

void h2s_ddr5_read(const svBitVecVal* bfm_id,
                   const svBitVecVal* addr,
                   svBitVecVal* data) {
  const uint16_t id = ReadU16(bfm_id);
  const uint64_t address = ReadU64(addr);
  Ddr5Instance* instance = FindOrCreateInstance(id);
  const unsigned int words = (instance == NULL) ? 1U : WordCount(instance->data_width);
  StorageNode* node;

  if (data == NULL) {
    return;
  }

  memset(data, 0, words * sizeof(svBitVecVal));
  if (instance == NULL) {
    return;
  }

  node = FindStorageNode(instance->storage_head, address);
  if (node != NULL && node->words != NULL) {
    memcpy(data, node->words, words * sizeof(svBitVecVal));
  }
  LogEvent(2, "read bfm_id=0x%04x addr=0x%016llx hit=%u", (unsigned int)id,
           (unsigned long long)address, (node != NULL) ? 1U : 0U);
}

void h2s_ddr5_read_request(const svBitVecVal* bfm_id, const svBitVecVal* addr) {
  const uint16_t id = ReadU16(bfm_id);
  const uint64_t address = ReadU64(addr);
  Ddr5Instance* instance = FindOrCreateInstance(id);
  PushReadRequest(instance, address);
  LogEvent(2, "read_request bfm_id=0x%04x addr=0x%016llx", (unsigned int)id,
           (unsigned long long)address);
}

void h2s_ddr5_read_data_requeest(const svBitVecVal* bfm_id,
                                 const svBitVecVal* left_space) {
  Ddr5Instance* instance = FindOrCreateInstance(ReadU16(bfm_id));
  uint64_t address = 0;

  if (instance == NULL) {
    return;
  }

  if (ReadU16(left_space) == 0U) {
    ReplyReadData(instance, 0U, 0);
    LogEvent(2, "read_data_requeest bfm_id=0x%04x left_space=0 -> invalid",
             (unsigned int)instance->bfm_id);
    return;
  }

  if (PopReadRequest(instance, &address)) {
    ReplyReadData(instance, address, 1);
    LogEvent(2, "read_data_requeest bfm_id=0x%04x addr=0x%016llx valid=1",
             (unsigned int)instance->bfm_id, (unsigned long long)address);
  } else {
    ReplyReadData(instance, 0U, 0);
    LogEvent(2, "read_data_requeest bfm_id=0x%04x no_pending valid=0",
             (unsigned int)instance->bfm_id);
  }
}

void h2s_ddr5_read_data_blocking(const svBitVecVal* bfm_id) {
  Ddr5Instance* instance = FindOrCreateInstance(ReadU16(bfm_id));
  uint64_t address = 0;

  if (instance == NULL) {
    return;
  }

  if (PopReadRequest(instance, &address)) {
    ReplyReadData(instance, address, 1);
    LogEvent(2, "read_data_blocking bfm_id=0x%04x addr=0x%016llx valid=1",
             (unsigned int)instance->bfm_id, (unsigned long long)address);
  } else {
    ReplyReadData(instance, 0U, 0);
    LogEvent(2, "read_data_blocking bfm_id=0x%04x no_pending valid=0",
             (unsigned int)instance->bfm_id);
  }
}
