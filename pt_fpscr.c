#include <math.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
static void L(const char*s){int fd=open("/tmp/fpscr_test.log",O_WRONLY|O_CREAT|O_APPEND,0644);if(fd<0)return;write(fd,s,strlen(s));close(fd);}
static unsigned rd(void){unsigned v;__asm__ volatile("fmrx %0, fpscr":"=r"(v));return v;}
static void wr(unsigned v){__asm__ volatile("fmxr fpscr, %0"::"r"(v));}
int main(int argc,char**argv){
  char b[128]; unsigned f=rd();
  snprintf(b,sizeof b,"fpscr=0x%08x  FZ=%u DN=%u\n",f,(f>>24)&1u,(f>>25)&1u); L(b);
  if(argc>1){ wr(f & ~((1u<<24)|(1u<<25))); L("cleared FZ+DN -> IEEE\n"); }
  L("before powf\n");
  volatile float r=powf(10.0f,0.0f);
  snprintf(b,sizeof b,"after powf=%f\n",(double)r); L(b);
  L("done\n"); return 0;
}
