#include "defines.h"
#include "intr.h"
#include "interrupt.h"

/* ソフトウェア割込みベクタの初期化 */
int softvec_init(void) {
  int type;
  for (type = 0; type < SOFTVEC_TYPE_NUM; type++) {
    softvec_setintr(type, NULL);
  }
  return 0;
}

int softvec_setintr(softvec_type_t type, softvec_handler_t hander) {
  SOFTVECS[type] = hander;
  return 0;
}

/*
* 共通割込みハンドラ
* ソフトウェア割込みベクタを見て、各ハンドラに分岐する
*/
void interrupt(softvec_type_t type, unsigned long sp) {
  softvec_handler_t handler = SOFTVECS[type];
  if (handler) {
    handler(type, sp);
  }
}
