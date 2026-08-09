#include <math.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
static void L(const char*s){int fd=open("/tmp/exp2.log",O_WRONLY|O_CREAT|O_APPEND,0644);if(fd<0)return;write(fd,s,strlen(s));close(fd);}
volatile float g = 0.0f;
static sigset_t g_set;
static void* sigthr(void* a){ int s; while(1){ if(sigwait(&g_set,&s)==0){} } return 0; }
int main(void){
  char b[64]; pthread_t t;
  L("start\n");
  sigemptyset(&g_set); sigaddset(&g_set,SIGTERM); sigaddset(&g_set,SIGINT); sigaddset(&g_set,SIGUSR1); sigaddset(&g_set,SIGUSR2);
  pthread_sigmask(SIG_BLOCK,&g_set,NULL); L("sigmask blocked\n");
  pthread_create(&t,NULL,sigthr,NULL);   L("sigwait thread spawned\n");
  { unsigned f; __asm__ volatile("fmrx %0, fpscr":"=r"(f)); f&=~((1u<<24)|(1u<<25)); __asm__ volatile("fmxr fpscr, %0"::"r"(f)); } L("fpu ieee\n");
  L("before REAL expf (multithreaded)\n");
  volatile float e = expf(g);
  snprintf(b,sizeof b,"expf=%f OK\n",(double)e); L(b);
  L("done\n"); return 0;
}
