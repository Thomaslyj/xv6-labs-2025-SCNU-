
#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

int
main(int argc, char *argv[])
{
  // 申请 16 个页面的内存
  int size = 16 * 4096;
  char *p = sbrk(size);

  // 检查是否分配成功
  if(p == (char *)-1)
    exit(1);

  // 搜索残留在内存中的标记字符串
  for(int i = 0; i < size - 64; i++){

    if(memcmp(p + i, "This may help.", 14) == 0){

      // 秘密位于标记起始位置之后的 16 字节
      printf("%s\n", p + i + 16);
      exit(0);
    }
  }

  exit(1);
}
