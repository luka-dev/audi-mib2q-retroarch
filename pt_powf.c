#include <math.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <dlfcn.h>
static void L(const char*s){int fd=open("/tmp/powf_test.log",O_WRONLY|O_CREAT|O_APPEND,0644);if(fd<0)return;write(fd,s,strlen(s));close(fd);}
int main(void){
  char b[96];
  L("start; powf before GL: ");
  volatile float r0=powf(10.0f,0.0f); snprintf(b,sizeof b,"%f\n",(double)r0); L(b);
  L("dlopen libEGL...\n");     void*h1=dlopen("libEGL.so.1",RTLD_NOW|RTLD_GLOBAL);     snprintf(b,sizeof b,"libEGL=%p\n",h1);L(b);
  L("dlopen libGLESv2...\n");  void*h2=dlopen("libGLESv2.so.1",RTLD_NOW|RTLD_GLOBAL); snprintf(b,sizeof b,"libGLESv2=%p\n",h2);L(b);
  L("dlopen libstdc++...\n");  void*h3=dlopen("libstdc++.so.6",RTLD_NOW|RTLD_GLOBAL); snprintf(b,sizeof b,"libstdc++=%p\n",h3);L(b);
  L("before powf AFTER GL\n");
  volatile float r=powf(10.0f,0.0f);
  snprintf(b,sizeof b,"after powf=%f\n",(double)r); L(b);
  L("done\n"); return 0;
}
