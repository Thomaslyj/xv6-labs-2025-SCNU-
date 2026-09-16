#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"
#include "user/user.h"
static int matchhere(char *re, char *text);
static int matchstar(int c, char *re, char *text);

/*
 * 判断正则表达式re能否匹配text。
 */
static int
match(char *re, char *text)
{
  // ^要求从字符串开头匹配。
  if(re[0] == '^')
    return matchhere(re + 1, text);

  // 没有^时，从text的每个位置尝试匹配。
  do {
    if(matchhere(re, text))
      return 1;
  } while(*text++ != '\0');

  return 0;
}

/*
 * 从text当前位置开始匹配。
 */
static int
matchhere(char *re, char *text)
{
  // 正则表达式已经结束。
  if(re[0] == '\0')
    return 1;

  // 当前字符后面是*。
  if(re[1] == '*')
    return matchstar(re[0], re + 2, text);

  // $要求文本也正好结束。
  if(re[0] == '$' && re[1] == '\0')
    return *text == '\0';

  // 普通字符匹配，或者'.'匹配任意字符。
  if(*text != '\0' &&
     (re[0] == '.' || re[0] == *text))
    return matchhere(re + 1, text + 1);

  return 0;
}

/*
 * 处理c*，即字符c出现零次或多次。
 */
static int
matchstar(int c, char *re, char *text)
{
  do {
    // 先尝试让*匹配零个字符。
    if(matchhere(re, text))
      return 1;

    // 如果当前字符符合，就继续多匹配一个。
  } while(*text != '\0' &&
          (*text++ == c || c == '.'));

  return 0;
}

static char*
basename(char*path)
{
    char*p;
    p = path + strlen(path);

    while(p>path && *(p-1)!='/')
    p--;
    //从字符串的末尾向前在在寻找最后一个'/'
    return p;
}

static void
handle_match(char*path,char**cmdargv,int cmdargc)
{
    char*args[MAXARG];
    int pid;
    int i;
    //后面没有传echo的参数
    if(cmdargc==0){
        printf("%s\n",path);
        return;
    }

    for(i=0;i<cmdargc;i++)
        args[i]=cmdargv[i];
    
    args[cmdargc] = path;//把路径名字添加到指令的后面
    args[cmdargc+1] = 0;

    pid = fork();

    if(pid<0){
    fprintf(2, "find: fork failed\n");
    return;
    }
      if(pid == 0){
    // 子进程执行指定命令。
    exec(args[0], args);

    // exec成功不会返回，运行到这里说明exec失败。
    fprintf(2, "find: exec %s failed\n", args[0]);
    exit(1);
  }
    wait(0);
}


static void
find(char *path, char *target, char **cmdargv, int cmdargc)
{
    char buf[512];
    char *p;
    char name[DIRSIZ+1];
    int fd;
    int n;
    struct dirent de;//文件名字和inode号
    struct stat st;//文件类型等信息
//判断是否能打开文件
    fd = open(path,O_RDONLY);
    if(fd<0){
        fprintf(2,"find: cannot open %s\n", path);
        return;

    }
//统计文件基本信息
    if(fstat(fd,&st)<0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
    }

    if(match(target, basename(path)))
    handle_match(path, cmdargv, cmdargc);
  /*
   * 普通文件和设备文件不能继续向下遍历。
   * 只有目录中才包含目录项。
   */
    if(st.type != T_DIR){
        close(fd);
        return;
    }
// 过长，无法放进缓冲区
    if(strlen(path)+1+DIRSIZ+1 > sizeof(buf)){
        fprintf(2, "find: path too long\n");
        close(fd);
        return;
    }

    strcpy(buf,path);

    p = buf + strlen(buf);

    if(p > buf && *(p-1)!='/')
        *p++ = '/';

    while((n = read(fd, &de, sizeof(de))) == sizeof(de)){

        if(de.inum==0)  continue;
        memmove(name,de.name,DIRSIZ);
        name[DIRSIZ]='\0';
        //手动帮助结尾补充0

        if(strcmp(name,".")==0|| strcmp(name,"..")==0) 
        continue;
        
        memmove(p,name,strlen(name)+1);//路径拼接

        find(buf,target,cmdargv,cmdargc);//递归处理路径
    }
    if(n < 0)
        fprintf(2, "find: read error %s\n", path);

  
    close(fd);

}

int
main(int argc, char *argv[])
{
  int execpos = -1;
  int cmdargc;

  if(argc < 3){
    fprintf(2,
      "usage: find path name [-exec command args...]\n");
    exit(1);
  }

  // 寻找-exec所在位置
  for(int i = 3; i < argc; i++){
    if(strcmp(argv[i], "-exec") == 0){
      execpos = i;
      break;
    }
  }

  if(execpos < 0){
    // 不带-exec时，只允许find path name
    if(argc != 3){
      fprintf(2,
        "usage: find path name [-exec command args...]\n");
      exit(1);
    }

    find(argv[1], argv[2], 0, 0);
    exit(0);
  }

  // -exec位于path和name之后。
  if(execpos != 3 || execpos + 1 >= argc){
    fprintf(2, "find: missing command after -exec\n");
    exit(1);
  }

  cmdargc = argc - execpos - 1;

  /*
   * 还需要两个数组位置：
   * 一个保存找到的路径，一个保存结尾的0。
   */
  if(cmdargc + 2 > MAXARG){
    fprintf(2, "find: too many arguments\n");
    exit(1);
  }

  find(argv[1], argv[2],
       &argv[execpos + 1], cmdargc);

  exit(0);
}

