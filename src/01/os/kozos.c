#include "defines.h"
#include "memory.h"
#include "kozos.h"
#include "intr.h"
#include "interrupt.h"
#include "syscall.h"
#include "lib.h"

#define THREAD_NUM 6 // TCBの個数
#define PRIORITY_NUM 16 // 優先度の個数
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
  int priority; // 優先度
  char *stack; // スタック
  uint32 flags; // 各種フラグ
#define KZ_THREAD_FLAG_READY (1 << 0)

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

/* メッセージバッファ */
typedef struct _kz_msgbuf {
  struct _kz_msgbuf *next;
  kz_thread *sender; /* メッセージを送信したスレッド */
  struct {
    int size;
    char *p;
  } param;
} kz_msgbuf;

/* メッセージボックス */
typedef struct _kz_msgbox {
  kz_thread *receiver; /* 受信待ち状態のスレッド */
  kz_msgbuf *head;
  kz_msgbuf *tail;

  /*
  * H8は16ビットCPUなので、32ビット整数に対しての乗算命令がない。よって
  * 構造体のサイズが2の累乗になっていないと、構造体の配列のインデックス
  * 計算で乗算が使われて「__mulsi3がない」などのリンクエラになる場合が
  * ある。(2の累乗ならばシフト演算が利用されるので問題は出ない)
  * 対策として、サイズが2の累乗になるようにダミーメンバーで調整する
  * 他構造体で同様のエラーが出た場合には、同様の対処とすること
  */
  long dummy[1];
} kz_msgbox;

/* スレッドのレディーキュー */
static struct {
  kz_thread *head; // 先頭のエントリ
  kz_thread *tail; // 末尾のエントリ
} readyque[PRIORITY_NUM];

static kz_thread *current; // カレントスレッド
static kz_thread threads[THREAD_NUM]; // タスクコントロールブロック
static kz_handler_t handlers[SOFTVEC_TYPE_NUM]; // 割込みハンドラ
static kz_msgbox msgboxes[MSGBOX_ID_NUM]; /* メッセージボックス */

// スレッドのディスパッチ用関数(実態は startup.s にアセンブラで記述)
void dispatch(kz_context *context);

/* カレントスレッドをレディーキューから抜き出す */
static int getcurrent(void) {
  if (current == NULL) {
    return -1;
  }
  if (!(current->flags & KZ_THREAD_FLAG_READY)) {
    // 既にない場合は無視
    return 1;
  }

  /* カレントスレッドは必ず先頭にあるはずなので、先頭から抜き出す */
  readyque[current->priority].head = current->next;
  if (readyque[current->priority].head == NULL) {
    readyque[current->priority].tail = NULL;
  }
  current->flags &= ~KZ_THREAD_FLAG_READY;
  current->next = NULL;

  return 0;
}

/* カレントスレッドをレディーキューに繋げる */
static int putcurrent(void) {
  if (current == NULL) {
    return -1;
  }
  if (current->flags & KZ_THREAD_FLAG_READY) {
    // 既にある場合は無視
    return 1;
  }

  // レディーキューの末尾に接続する
  if (readyque[current->priority].tail) {
    readyque[current->priority].tail->next = current;
  } else {
    readyque[current->priority].head = current;
  }
  readyque[current->priority].tail = current;
  current->flags |= KZ_THREAD_FLAG_READY;

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
static kz_thread_id_t thread_run(kz_func_t func, char *name, int priority, int stacksize, int argc, char *argv[]) {
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
  thp->priority = priority;
  thp->flags = 0;
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
  * スレッドの優先度がゼロの場合には、割込み禁止すれどとする
  */
  // ディスパッチ時にプログラムカウンタに格納される値として thread_init() を設定する
  // よってスレッドは thread_init() から動作を開始する
  *(--sp) = (uint32)thread_init | ((uint32)(priority ? 0 : 0xc0) << 24);

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

/* システムコールの処理(kz_wait(): スレッドの実行破棄) */
static int thread_wait(void) {
  // レディーキューから一旦外して接続しなおすことで、ラウンドロビンで他のスレッドを動作させる
  putcurrent();
  return 0;
}

/* システムコールの処理(kz_sleep(): スレッドのスリープ) */
static int thread_sleep(void) {
  // レディーキューから外されたままになるので、スケジューリングされなくなる
  return 0;
}

/* システムコールの処理(kz_wakeup(): スレッドのウェイクアップ) */
static int thread_wakeup(kz_thread_id_t id) {
  /* ウェイクアップを呼び出したスレッドをレディーキューに戻す */
  putcurrent();

  /* 指定されたスレッドをレディーキューに接続してウェイクアップする */
  current = (kz_thread *)id;
  putcurrent();

  return 0;
}

/* システムコールの処理(kz_getid(): スレッドID取得) */
static kz_thread_id_t thread_getid(void) {
  putcurrent();
  // TCBのアドレスがスレッドIDとなる
  return (kz_thread_id_t)current;
}

/* システムコールの処理(kz_chpri(): スレッドの優先度変更) */
static int thread_chpri(int priority) {
  int old = current->priority;
  if (priority >= 0) {
    current->priority = priority;
  }
  putcurrent();
  return old;
}

/* システムコールの処理(kz_kmalloc(): 動的メモリ獲得) */
static void *thread_kmalloc(int size) {
  putcurrent();
  return kzmem_alloc(size);
}

/* システムコールの処理(kz_kfree(): メモリ解放) */
static int thread_kmfree(char *p) {
  kzmem_free(p);
  putcurrent();
  return 0;
}

/* メッセージの送信処理 */
static void sendmsg(kz_msgbox *mboxp, kz_thread *thp, int size, char *p) {
  kz_msgbuf *mp;

  /* メッセージバッファの作成 */
  mp = (kz_msgbuf *)kzmem_alloc(sizeof(*mp)); // メッセージバッファを獲得する
  if (mp == NULL) {
    kz_sysdown();
  }
  mp->next = NULL;
  mp->sender = thp;
  mp->param.size = size;
  mp->param.p = p;

  /* メッセージボックスの末尾にメッセージを接続する */
  if (mboxp->tail) {
    mboxp->tail->next = mp;
  } else {
    mboxp->head = mp;
  }
  mboxp->tail = mp;
}

static void recvmsg(kz_msgbox *mboxp) {
  kz_msgbuf *mp;
  kz_syscall_param_t *p;

  /* メッセージボックスの先頭にあるメッセージを抜き出す */
  mp = mboxp->head;
  mboxp->head = mp->next;
  if (mboxp->head == NULL) {
    mboxp->tail = NULL;
  }
  mp->next = NULL;

  /* メッセージを受信するスレッドに返す値を設定する */
  p = mboxp->receiver->syscall.param;
  p->un.recv.ret = (kz_thread_id_t)mp->sender;
  if (p->un.recv.sizep) {
    *(p->un.recv.sizep) = mp->param.size;
  }
  if (p->un.recv.pp) {
    *(p->un.recv.pp) = mp->param.p;
  }

  /* 受信待ちスレッドはいなくなったので、NULLに戻す */
  mboxp->receiver = NULL;

  /* メッセージバッファの開放 */
  kzmem_free(mp);
}

/* システムコールの処理(kz_send(): メッセージ送信) */
static int thread_send(kz_msgbox_id_t id, int size, char *p) {
  kz_msgbox *mboxp = &msgboxes[id];

  putcurrent();
  sendmsg(mboxp, current, size, p);

  /* 受信待ちスレッドが存在している場合には受信処理をおこなう */
  if (mboxp->receiver) {
    current = mboxp->receiver; // 受信待ちスレッド
    recvmsg(mboxp); // メッセージの受信処理
    putcurrent(); // 受信により動作可能になったので、ブロック解除する
  }

  return size;
}

static kz_thread_id_t thread_recv(kz_msgbox_id_t id, int *sizep, char **pp) {
  kz_msgbox *mboxp = &msgboxes[id];

  if (mboxp->receiver) {
    kz_sysdown();
  }

  mboxp->receiver = current; // 受信待ちスレッドに設定

  if (mboxp->head == NULL) {
    /*
    * メッセージボックスにメッセージがないので、スレッドを
    * スリープさせる。(システムコールをブロックする)
    */
    return -1;
  }

  recvmsg(mboxp); /* メッセージの受信処理 */
  putcurrent(); // メッセージを受信できたので、レディー状態にする

  return current->syscall.param->un.recv.ret;
}

/* システムコールの処理(kz_setintr(): 割込みハンドラの登録) */
static int thread_setintr(softvec_type_t type, kz_handler_t handler) {
  static void thread_intr(softvec_type_t type, unsigned long sp);

  /*
  * 割込みを受け付けるために、ソフトウェア割込みベクタに
  * OSの割込み処理の入り口となる関数を登録する
  */
  softvec_setintr(type, thread_intr);

  handlers[type] = handler;

  putcurrent();
  return 0;
}

static void call_functions(kz_syscall_type_t type, kz_syscall_param_t *p) {
  /* システムコールの実行中に current が書き換わるので注意 */
  switch (type) {
    case KZ_SYSCALL_TYPE_RUN:
      p->un.run.ret = thread_run(
        p->un.run.func, p->un.run.name,
        p->un.run.priority, p->un.run.stacksize,
        p->un.run.argc, p->un.run.argv
      );
      break;
    case KZ_SYSCALL_TYPE_EXIT:
      /* TCBが消去されるので戻り値を書き込んではいけない */
      thread_exit();
      break;
    case KZ_SYSCALL_TYPE_WAIT:
      p->un.wait.ret = thread_wait();
      break;
    case KZ_SYSCALL_TYPE_SLEEP:
      p->un.sleep.ret = thread_sleep();
      break;
    case KZ_SYSCALL_TYPE_WAKEUP:
      p->un.wakeup.ret = thread_wakeup(p->un.wakeup.id);
      break;
    case KZ_SYSCALL_TYPE_GETID:
      p->un.getid.ret = thread_getid();
      break;
    case KZ_SYSCALL_TYPE_CHPRI:
      p->un.chpri.ret = thread_chpri(p->un.chpri.priority);
      break;
    case KZ_SYSCALL_TYPE_KMALLOC:
      p->un.kmalloc.ret = thread_kmalloc(p->un.kmalloc.size);
      break;
    case KZ_SYSCALL_TYPE_KMFREE:
      p->un.kmfree.ret = thread_kmfree(p->un.kmfree.p);
      break;
    case KZ_SYSCALL_TYPE_SEND:
      p->un.send.ret = thread_send(p->un.send.id, p->un.send.size, p->un.send.p);
      break;
    case KZ_SYSCALL_TYPE_RECV:
      p->un.recv.ret = thread_recv(p->un.recv.id, p->un.recv.sizep, p->un.recv.pp);
      break;
    case KZ_SYSCALL_TYPE_SETINTR:
      p->un.setintr.ret = thread_setintr(p->un.setintr.type, p->un.setintr.handler);
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

/* サービスコールの処理 */
static void srvcall_proc(kz_syscall_type_t type, kz_syscall_param_t *p) {
  /*
  * システムコールとサービスコールの処理関数の内部で
  * システムコールの実行したスレッドIDを得るために current を
  * 参照している部分があり(たとえば thread_send() など)、
  * current が残っていると誤動作するため NULL に設定する。
  * サービスコールは thread_intrvec() 内部の割込みハンドラ呼び出しの
  * 延長で呼ばれているはずなので、呼び出し後に thread_intrvec() で
  * スケジューリング処理が行われ、 curret は再設定される
  */
  current = NULL;
  call_functions(type, p);
}

/* スレッドのスケジューリング */
static void schedule(void) {
  int i;
  /*
  * 優先順位の高い順(優先度の数値の小さい順)にレディーキューを見て、
  * 動作可能なスレッドを検索する
  */
  for (i = 0; i < PRIORITY_NUM; i++) {
    if (readyque[i].head) {
      break;
    }
  }
  if (i == PRIORITY_NUM) {
    kz_sysdown();
  }
  current = readyque[i].head;
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
  * それ以外の場合は、kz_setintr() によってユーザ登録されたハンドラが
  * 実行される
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

void kz_start(kz_func_t func, char *name, int priority, int stacksize, int argc, char *argv[]) {
  /* 動的メモリの初期化 */
  kzmem_init();

  /*
  * 以降で呼び出すスレッド関連のライブラリ関数の内部で current を
  * 見ている場合があるので、current を NULL に初期化しておく
  */
  current = NULL;

  memset(readyque, 0, sizeof(readyque)); // レディーキューが配列になったので、memset() でのゼロクリアに変更
  memset(threads, 0, sizeof(threads));
  memset(handlers, 0, sizeof(handlers));
  memset(msgboxes, 0, sizeof(msgboxes));

  /* 割込みハンドラの登録 */
  thread_setintr(SOFTVEC_TYPE_SYSCALL, syscall_intr); // システムコール
  thread_setintr(SOFTVEC_TYPE_SOFTERR, softerr_intr); // ダウン要因発生

  /* システムコール発行不可なので直接呼び出してスレッド作成する */
  current = (kz_thread *)thread_run(func, name, priority, stacksize, argc, argv);

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

/* サービスコール呼び出し用ライブラリ関数 */
void kz_srvcall(kz_syscall_type_t type, kz_syscall_param_t *param) {
  srvcall_proc(type, param);
}
