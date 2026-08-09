/* clear FZ+DN -> IEEE mode, using legacy FMRX/FMXR (gas 2.19 friendly) */
#define FPSCR_FZ (1u<<24)
#define FPSCR_DN (1u<<25)
static inline unsigned rd(void){unsigned v;__asm__ volatile("fmrx %0, fpscr":"=r"(v));return v;}
static inline void wr(unsigned v){__asm__ volatile("fmxr fpscr, %0"::"r"(v));}
int main(void){
    unsigned f = rd();
    wr(f & ~(FPSCR_FZ|FPSCR_DN));
    return (int)(rd() & 0xff);
}
