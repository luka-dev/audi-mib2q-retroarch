#include <math.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
static void L(const char*s){int fd=open("/tmp/exp.log",O_WRONLY|O_CREAT|O_APPEND,0644);if(fd<0)return;write(fd,s,strlen(s));close(fd);}
volatile float g = 0.0f;
int main(void){
  char b[96];
  L("start; expf addr="); { void*p=(void*)&expf; snprintf(b,sizeof b,"%p\n",p); L(b);} 
  L("before REAL expf\n");
  volatile float e = expf(g);
  snprintf(b,sizeof b,"expf(0)=%f OK\n",(double)e); L(b);
  L("before REAL powf\n");
  volatile float pw = powf(10.0f, g/20.0f);
  snprintf(b,sizeof b,"powf=%f OK\n",(double)pw); L(b);
  L("done\n"); return 0;
}
