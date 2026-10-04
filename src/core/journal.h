/*
 * journal.h - 可回放输入日志
 *
 * 文件布局（全部小端）：
 *   固定文件头: magic "ISLOG" + version(1) + device_id[64] +
 *               generation(u64) + started_at[32] + 保留 7 字节
 *   帧序列:     LEN u32 | TYPE u8 | BODY[LEN-1] | CRC32 u32
 *               CRC32 覆盖 (TYPE | BODY)
 *
 * 日志同时记录"归一化输入"和"实际下发命令"。重放时只回放输入帧，
 * 因此命令是策略+核心重新计算出来的，而不是直接复制现场命令
 * （见 replay.h 的安全语义）。
 */
#ifndef INPUT_STRATEGY_JOURNAL_H
#define INPUT_STRATEGY_JOURNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "events.h"

#define JOURNAL_MAGIC "ISLOG"
#define JOURNAL_VERSION 1
#define JOURNAL_DEVICE_LEN 64
#define JOURNAL_TIME_LEN 32

typedef enum {
    R_INPUT = 1,
    R_COMMAND = 2,
    R_SYSTEM = 3,
    R_ACTIVATION = 4,
    R_REPLAY_BLOCKED = 5
} RecordType;

/* 系统事件子码（存于 body 首字节） */
#define SYS_FOCUS_LOST 1
#define SYS_FOCUS_GAINED 2
#define SYS_DISPLAY_LOST 3
#define SYS_DISPLAY_RESTORED 4

/* 文件线格式为显式序列化（不依赖结构体内存对齐）：
   magic(5) + version(1) + device_id(64) + generation u64(8) +
   started_at(32) + reserved(6) = 116 字节。
   JournalHeader 只是内存中的方便表示。 */
#define JOURNAL_HEADER_SIZE 116
typedef struct {
    char magic[5];
    uint8_t version;
    char device_id[JOURNAL_DEVICE_LEN];
    uint64_t generation;
    char started_at[JOURNAL_TIME_LEN];
} JournalHeader;

#define JOURNAL_MAX_FRAME 4104
typedef struct {
    FILE *fp;
    bool writable;
    JournalHeader header;
    /* 读取游标与当前帧缓冲：record.body 指向该缓冲，
       下次读取前保持有效。 */
    uint32_t frame_index;
    uint8_t framebuf[JOURNAL_MAX_FRAME];
} Journal;

/* 线格式头编解码（跨平台显式字节序） */
void encode_header(uint8_t out[JOURNAL_HEADER_SIZE], const JournalHeader *h);
bool decode_header(const uint8_t in[JOURNAL_HEADER_SIZE], JournalHeader *h);

/* 写模式（新建，截断） */
bool journal_open_write(Journal *j, const char *path, const char *device_id,
                        uint64_t generation, const char *started_at_iso);
/* 读模式：校验文件头 */
bool journal_open_read(Journal *j, const char *path);
void journal_close(Journal *j);

/* 写记录 */
bool journal_write_input(Journal *j, uint32_t rel_ms, const InputEvent *e);
bool journal_write_command(Journal *j, uint32_t rel_ms, const char *cmd_name,
                           int32_t arg, EventOrigin origin);
bool journal_write_system(Journal *j, uint32_t rel_ms, uint8_t sys_code,
                          const char *detail);
bool journal_write_activation(Journal *j, uint32_t rel_ms, uint64_t old_gen,
                              uint64_t new_gen, uint32_t released_keys);
bool journal_write_blocked(Journal *j, uint32_t rel_ms, const char *cmd_name,
                           EventOrigin origin, const char *reason);

/* 读：返回一条记录。type/rel_ms/body/body_len 输出。EOF 返回 false 且
   *eof=true；CRC 错误或截断返回 false 且 *corrupt=true。 */
typedef struct {
    uint8_t type;
    uint32_t rel_ms;
    const uint8_t *body;
    uint32_t body_len;
} JournalRecord;

bool journal_read_next(Journal *j, JournalRecord *rec, bool *eof, bool *corrupt);

/* R_INPUT body 解码为 InputEvent。成功返回 true。 */
bool journal_decode_input(const uint8_t *body, uint32_t len, InputEvent *out);

/* 测试/服务端便利：直接从字节缓冲写帧（用于生成测试日志） */
typedef struct {
    uint8_t *data;
    size_t len, cap;
} JournalMem;

void jm_init(JournalMem *m);
void jm_free(JournalMem *m);
bool jm_write_header(JournalMem *m, const char *device_id,
                     uint64_t generation, const char *started_at_iso);
bool jm_append_input(JournalMem *m, uint32_t rel_ms, const InputEvent *e);
bool jm_append_system(JournalMem *m, uint32_t rel_ms, uint8_t sys_code,
                      const char *detail);

#endif
