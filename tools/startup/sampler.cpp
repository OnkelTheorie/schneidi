// LD_PRELOAD sampling profiler: SIGPROF-like timer on the main thread only (timer_create with SIGEV_THREAD_ID),
// 0.5 ms wall clock; stops when SAMPLER_STOP_MS reached; dumps raw stacks + maps at exit.
#include <execinfo.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
static const int MAXS = 20000, DEPTH = 64;
static void* g_st[MAXS][DEPTH]; static int g_n[MAXS]; static long g_t[MAXS];
static std::atomic<int> g_count{0};
static timespec g_t0; static long g_stop = 1000;
static timer_t g_timer;
static long nowms(){ timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (t.tv_sec-g_t0.tv_sec)*1000+(t.tv_nsec-g_t0.tv_nsec)/1000000; }
static void handler(int){ int i=g_count.load(); if(i>=MAXS) return; long ms=nowms(); if(ms>g_stop) return; g_n[i]=backtrace(g_st[i],DEPTH); g_t[i]=ms; g_count.store(i+1); }
static void dump(){
  const char* out=getenv("SAMPLER_OUT"); if(!out) out="samples.txt";
  FILE* f=fopen(out,"w"); if(!f) return;
  FILE* m=fopen("/proc/self/maps","r"); char line[1024]; while(fgets(line,sizeof line,m)) fprintf(f,"M %s",line); fclose(m);
  for(int i=0;i<g_count.load();++i){ fprintf(f,"S %ld",g_t[i]); for(int j=0;j<g_n[i];++j) fprintf(f," %p",g_st[i][j]); fprintf(f,"\n"); }
  fclose(f);
}
__attribute__((constructor)) static void init(){
  clock_gettime(CLOCK_MONOTONIC,&g_t0); if(const char*s=getenv("SAMPLER_STOP_MS")) g_stop=atol(s);
  void* dummy[2]; backtrace(dummy,2); // load libgcc unwinder outside the handler
  struct sigaction sa{}; sa.sa_handler=handler; sa.sa_flags=SA_RESTART; sigaction(SIGRTMIN+3,&sa,nullptr);
  sigevent ev{}; ev.sigev_notify=SIGEV_THREAD_ID; ev.sigev_signo=SIGRTMIN+3; ev._sigev_un._tid=syscall(SYS_gettid);
  timer_create(CLOCK_MONOTONIC,&ev,&g_timer);
  itimerspec its{}; its.it_interval.tv_nsec=500000; its.it_value.tv_nsec=500000; timer_settime(g_timer,0,&its,nullptr);
  atexit(dump);
}
