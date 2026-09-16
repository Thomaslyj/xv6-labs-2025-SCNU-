#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
int
main(int argc, char *argv[])
{
 if(argc != 2){
 fprintf(2, "usage: sleep ticks\n");
 exit(1);
 }
 // TODO: 把 argv[1] 转成整数，调用对应的系统调用
 int n = atoi(argv[1]);
 pause(n);
 exit(0);
}