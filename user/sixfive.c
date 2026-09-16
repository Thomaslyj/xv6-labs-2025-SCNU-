#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

char *seps = " -\r\t\n./,";

static void
checknum(int cur)
{
  if(cur % 5 == 0 || cur % 6 == 0)
    printf("%d\n", cur);
}

static int
sixfive(int fd)
{
  char ch;
  int cur = 0;
  int have = 0;
  int bad = 0;
  int n;

  while((n = read(fd, &ch, 1)) > 0){
    if(ch >= '0' && ch <= '9'){
      // bad 为 1 表示当前字段已被非法字符污染。
      if(!bad){
        cur = cur * 10 + (ch - '0');
        have = 1;
      }
    } else if(strchr(seps, ch) != 0){
      // 分隔符结束当前字段。
      if(have && !bad)
        checknum(cur);

      cur = 0;
      have = 0;
      bad = 0;
    } else {
      // 非数字、非分隔符：整个字段无效。
      cur = 0;
      have = 0;
      bad = 1;
    }
  }

  if(n < 0){
    fprintf(2, "sixfive: read error\n");
    return -1;
  }

  // 文件末尾相当于一个隐式分隔符。
  if(have && !bad)
    checknum(cur);

  return 0;
}

int
main(int argc, char *argv[])
{
  int fd;
  int status = 0;

  if(argc < 2){
    fprintf(2, "usage: sixfive file...\n");
    exit(1);
  }

  for(int i = 1; i < argc; i++){
    fd = open(argv[i], O_RDONLY);

    if(fd < 0){
      fprintf(2, "sixfive: cannot open %s\n", argv[i]);
      status = 1;
      continue;
    }

    if(sixfive(fd) < 0)
      status = 1;

    close(fd);
  }

  exit(status);
}