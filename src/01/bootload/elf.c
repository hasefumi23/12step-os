#include "elf.h"

#include "defines.h"
#include "lib.h"

struct elf_header {
    struct {
        unsigned char magic[4];     // マジックナンバー
        unsigned char class;        // 32/64ビットの区別
        unsigned char format;       // エンディアン情報
        unsigned char version;      // ELFフォーマットのバージョン
        unsigned char abi;          // OSの種別
        unsigned char abi_version;  // OSのバージョン
        unsigned char reserve[7];   // 予約(未使用)
    } id;
    short type;                  // ファイルの種別
    short arch;                  // CPUの種類
    long version;                // ELF形式のバージョン
    long entry_point;            // 実行開始アドレス
    long program_header_offset;  // プログラムヘッダテーブルの位置
    long section_header_offset;  // セクションヘッダテーブルの位置
    long flags;                  // 各種フラグ
    short header_size;           // ELFヘッダのサイズ
    short program_header_size;   // プログラムヘッダのサイズ
    short program_header_num;    // プログラムヘッダの個数
    short section_header_size;   // セクションヘッダのサイズ
    short section_header_num;    // セクションヘッダの個数
    short section_name_index;  // セクション名を格納するセクション
};

struct elf_program_header {
    long type;           // セグメントの種別
    long offset;         // ファイル中の位置
    long virtual_addr;   // 論理アドレス(VA)
    long physical_addr;  // 物理アドレス(PA)
    long file_size;      // ファイル中のサイズ
    long memory_size;    // メモリ上でのサイズ
    long flags;          // 各種フラグ
    long align;          // アラインメント
};

/* ELFヘッダのチェック */
static int elf_check(struct elf_header *header) {
    if (memcmp(header->id.magic,
               "\x7f"
               "ELF",
               4)) {
        return -1;
    }

    if (header->id.class != 1) return -1;   /* ELF32 */
    if (header->id.format != 2) return -1;  /* Big endian */
    if (header->id.version != 1) return -1; /* version 1 */
    if (header->type != 2) return -1;       /* Executable file */
    if (header->version != 1) return -1;    /* version 1 */

    /* Hitachi H8/300 or H8/300H */
    if ((header->arch != 46) && (header->arch != 47)) return -1;

    return 0;
}

static int elf_load_program(struct elf_header *header) {
    int i;
    struct elf_program_header *phdr;

    for (i = 0; i < header->program_header_num; i++) {
      /* プログラムヘッダを取得 */
      phdr = (struct elf_program_header *)((char *)header +
                                            header->program_header_offset +
                                            header->program_header_size * i);

      if (phdr->type != 1) continue; // ロード可能なセグメントか？

      memcpy((char *)phdr->physical_addr, (char *)header + phdr->offset,
        phdr->file_size);
      memset((char *)phdr->physical_addr + phdr->file_size, 0,
        phdr->memory_size - phdr->file_size);
    }

    return 0;
}

char *elf_load(char *buf) {
  struct elf_header *header = (struct elf_header *)buf;

  if (elf_check(header) < 0) return NULL;
  if (elf_load_program(header) < 0) return NULL;

  return (char *)header->entry_point;
}
