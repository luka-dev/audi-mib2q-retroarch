#include <math.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
static void L(const char*s){int fd=open("/tmp/pow2.log",O_WRONLY|O_CREAT|O_APPEND,0644);if(fd<0)return;write(fd,s,strlen(s));close(fd);}
volatile float g_vol = 0.0f;   /* read from memory, like settings->floats.audio_volume */
int main(void){
  char b[96];
  L("start\n");
  { volatile float y = g_vol / 20.0f; snprintf(b,sizeof b,"y=%f\n",(double)y); L(b); }
  L("A) powf CONSTANT 2nd arg\n");   volatile float r1 = powf(10.0f, 0.0f);            snprintf(b,sizeof b,"  =%f OK\n",(double)r1); L(b);
  L("B) powf COMPUTED 2nd arg (g_vol/20)\n"); volatile float r2 = powf(10.0f, g_vol/20.0f); snprintf(b,sizeof b,"  =%f OK\n",(double)r2); L(b);
  L("done\n"); return 0;
}
