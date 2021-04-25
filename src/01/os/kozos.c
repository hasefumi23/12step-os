#include "defines.h"
#include "kozos.h"
#include "intr.h"
#include "interrupt.h"
#include "syscall.h"
#include "lib.h"

#define THREAD_NUM 6 // TCBの個数
#define THREAD_NAME_SIZE 15 // スレッド名の最大長

/* スレッドコンテキスト */
// スレッドのコンテキスト保存用の構造体の定義
typedef struct _kz_context {
  // 汎用レジスタはスタックに保存されるので、TCBにコンテキストとして保存するのは
  // スタックkポインタのみ
  uint32 sp; // スタックポインタ
} kz_context;

/* タスクコントロールブロック(TCB) */
typedef struct _kz_thread {
  struct _kz_thread *next; // レディーキューへの接続に利用する next ポインタ
  char name[THREAD_NAME_SIZE + 1]; // スレッド名
  char *stack; // スタック

  /* スレッドのスタートアップ(thread_init())に渡すパラメータ */
  struct {
    kz_func_t func; // スレッドのメイン関数
    int argc; // スレッドのメイン関数にわたす argc
    char **argv; // スレッドのメイン関数にわたす argv
  } init;

  // システムコールの発行時に利用するパラメータ領域
  struct {
    kz_syscall_type_t type;
    kz_syscall_param_t *param;
  } syscall;

  kz_context context; // コンテキスト情報
} kz_thread;

/* スレッドのレディーキュー */
static struct {
  kz_thread *head; // 先頭のエントリ
  kz_thread *tail; // 末尾のエントリ
} readyque;

static kz_thread *current; // カレントスレッド
static kz_thread threads[THREAD_NUM]; // タスクコントロールブロック
static kz_handler_t handlers[SOFTVEC_TYPE_NUM]; // 割込みハンドラ

// スレッドのディスパッチ用関数(実態は startup.s にアセンブラで記述)
void dispatch(kz_context *context);

/* カレントスレッドをレディーキューから抜き出す */
static int getcurrent(void) {
  if (current == NULL) {
    return -1;
  }

  /* カレントスレッドは必ず先頭にあるはずなので、先頭から抜き出す */
  readyque.head = current->next;
  if (readyque.head == NULL) {
    readyque.tail = NULL;
  }
  current->next = NULL;

  return 0;
}

/* カレントスレッドをレディーキューに繋げる */
static int putcurrent(void) {
  if (current == NULL) {
    return -1;
  }

  // レディーキューの末尾に接続する
  if (readyque.tail) {
    readyque.tail->next = current;
  } else {
    readyque.head = current;
  }
  readyque.tail = current;

  return 0;
}

/* スレッドの終了 */
static void thread_end(void) {
  kz_exit();
}

/* スレッドのスタートアップ */
static void thread_init(kz_thread *thp) {
  // スレッドのメイン関数を呼び出す
  thp->init.func(thp->init.argc, thp->init.argv);
  // メイン関数から戻ってきたら、スレッドを終了する
  thread_end();
}

/* システムコールの処理(kz_run(): スレッドの起動) */
static kz_thread_id_t thread_run(kz_func_t func, char *name, int stacksize, int argc, char *argv[]) {
  int i;
  kz_thread *thp;
  uint32 *sp;
  extern char userstack; // リンカスクリプトで定義されるスタック領域
  static char *thread_stack = &userstack;

  /* 空いているタスクコントロールブロックを検索 */
  for (i = 0; i < THREAD_NUM; i++) {
    thp = &threads[i];
    if (!thp->init.func) {
      // 見つかった
      break;
    }
  }
  if (i == THREAD_NUM) {
    // 見つからなかった
    return -1;
  }

  // TCBを0クリアする
  memset(thp, 0, sizeof(*thp));

  /* TCBの設定 */
  strcpy(thp->name, name);
  thp->next = NULL;
  thp->init.func = func;
  thp->init.argc = argc;
  thp->init.argv = argv;

  /* スタック領域を獲得 */
  memset(thread_stack, 0, stacksize);
  thread_stack += stacksize;

  thp->stack = thread_stack;
  /* スタックの初期化 */
  // スタックに thread_init() からの戻り先として thread_end() を設定する
  sp = (uint32 *)thp->stack;
  *(--sp) = (uint32)thread_end;

  /*
  * プログラムカウンタを設定する
  */
  // ディスパッチ時にプログラムカウンタに格納される値として thread_init() を設定する
  // よってスレッドは thread_init() から動作を開始する
  *(--sp) = (uint32)thread_init;

  *(--sp) = 0; // ER6
  *(--sp) = 0; // ER5
  *(--sp) = 0; // ER4
  *(--sp) = 0; // ER3
  *(--sp) = 0; // ER2
  *(--sp) = 0; // ER1

  /* スレッドのスタートアップ(thread_init())に渡す引数 */
  *(--sp) = (uint32)thp; /* ER0 */

  /* スレッドのコンテキストを設定 */
  thp->context.sp = (uint32)sp;

  /* システムコールを呼び出したスレッドをレディーキューに戻す */
  putcurrent();

  /* 新規作成したスレッドをレディーキューに接続する */
  current = thp;
  putcurrent();

  // 新規作成したスレッドのスレッドIDを戻り値として返す
  return (kz_thread_id_t)current;
}

/* システムコールの処理(kz_exit(): スレッドの終了) */
static int thread_exit(void) {
  /*
  * 本来ならスタックも開放して再利用できるようにすべきだが省略
  * このため、スレッドを頻繁に生成・消去するようなことは現状できない
  */
  puts(current->name);
  puts(" EXIT.\n");
  memset(current, 0, sizeof(*current));
  return 0;
}

/* 割込みハンドラの登録 */
static int setintr(softvec_type_t type, kz_handler_t handler) {
  static void thread_intr(softvec_type_t type, unsigned long sp);

  /*
  * 割込みを受け付けるために、ソフトウェア割込みベクタに
  * OSの割込み処理の入り口となる関数を登録する
  */
  softvec_setintr(type, thread_intr);

  handlers[type] = handler;

  return 0;
}

static void call_functions(kz_syscall_type_t type, kz_syscall_param_t *p) {
  /* システムコールの実行中に current が書き換わるので注意 */
  switch (type) {
    case KZ_SYSCALL_TYPE_RUN:
      p->un.run.ret = thread_run(
        p->un.run.func, p->un.run.name,
        p->un.run.stacksize,
        p->un.run.argc, p->un.run.argv
      );
      break;
    case KZ_SYSCALL_TYPE_EXIT:
      /* TCBが消去されるので戻り値を書き込んではいけない */
      thread_exit();
      break;
    default:
      break;
  }
}

/* システムコールの処理 */
static void syscall_proc(kz_syscall_type_t type, kz_syscall_param_t *p) {
  /*
  * システムコールを呼び出したスレッドをレディーキューから
  * 外した状態で処理関数を呼び出す。このためシステムコールを
  * 呼び出したスレッドをそのまま動作継続させたいばあいには、
  * 処理関数の内部で putcurrent() を行う必要がある
  */
  getcurrent(); // カレントスレッドをレディーキューから外す
  call_functions(type, p); // システムコールの処理関数を呼び出す
}

/* スレッドのスケジューリング */
static void schedule(void) {
  if (!readyque.head) {
    kz_sysdown();
  }
  current = readyque.head;
}

// システムコールの呼び出し
static void syscall_intr(void) {
  syscall_proc(current->syscall.type, current->syscall.param);
}

static void softerr_intr(void) {
  // ソフトウェアエラーが発生し場合は、スレッドを強制終了する
  puts(current->name);
  puts(" DOWN.\n");
  getcurrent();
  thread_exit();
}

/* 割込み処理の入り口関数 */
static void thread_intr(softvec_type_t type, unsigned long sp) {
  /* カレントスレッドのコンテキストを保存する */
  current->context.sp = sp;

  /*
  * 割込みごとの処理を実行する
  * SOFTVEC_TYPE_SYSCALL, SOFTVEC_TYPE_SOFTERR の場合は
  * syscall_intr(), softerr_intr() がハンドラに登録されているので、
  * それらが実行される
  */
  if (handlers[type]) {
    handlers[type]();
  }
  schedule();

  /*
  * スレッドのディスパッチ
  * (dispatch()関数の本体は startup.s にあり、アセンブラで記述されている)
  */
  dispatch(&current->context);
  /* ここには返ってこない */
}

void kz_start(kz_func_t func, char *name, int stacksize, int argc, char *argv[]) {
  /*
  * 以降で呼び出すスレッド関連のライブラリ関数の内部で current を
  * 見ている場合があるので、current を NULL に初期化しておく
  */
  current = NULL;

  readyque.head = readyque.tail = NULL;
  memset(threads, 0, sizeof(threads));
  memset(handlers, 0, sizeof(handlers));

  /* 割込みハンドラの登録 */
  setintr(SOFTVEC_TYPE_SYSCALL, syscall_intr); // システムコール
  setintr(SOFTVEC_TYPE_SOFTERR, softerr_intr); // ダウン要因発生

  /* システムコール発行不可なので直接呼び出してスレッド作成する */
  current = (kz_thread *)thread_run(func, name, stacksize, argc, argv);

  /* 最初のスレッドを起動 */
  dispatch(&current->context);

  /* ここには返ってこない */
}

void kz_sysdown(void) {
  puts("system error!\n");
  while (1);
}

void kz_syscall(kz_syscall_type_t type, kz_syscall_param_t *param) {
  current->syscall.type = type;
  current->syscall.param = param;
  // トラップ割込み発行
  asm volatile ("trapa #0");
}
