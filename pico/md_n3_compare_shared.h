#ifndef MD_N3_COMPARE_SHARED_H
#define MD_N3_COMPARE_SHARED_H
#include "microdos/native3.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static const uint8_t cmp_loop[]={0xB9,0,0x80,0x49,0x75,0xFD,0xF4};
static const uint8_t cmp_regmix[]={0xB8,1,0,0xBB,3,0,0xB9,0,0x80,0x31,0xD2,0x03,0xC3,0x33,0xD8,0x03,0xD0,0x49,0x75,0xF7,0xF4};
static const uint8_t cmp_callmix[]={0xB9,0,0x80,0xBB,0,0,0xE8,4,0,0x49,0x75,0xFA,0xF4,0x83,0xC3,3,0xC3};
typedef struct { const char *name; const uint8_t *program; size_t size; uint32_t retired; } CmpWork;
static const CmpWork cmp_work[]={
 {"loop",cmp_loop,sizeof(cmp_loop),65538u},
 {"regmix",cmp_regmix,sizeof(cmp_regmix),163845u},
 {"callmix",cmp_callmix,sizeof(cmp_callmix),163843u}
};
typedef struct { uint16_t r[8],cs,ds,es,ss,ip,flags; uint32_t memhash; } CmpSnapshot;
static uint32_t cmp_hash(const uint8_t *p,size_t n){uint32_t h=2166136261u;size_t i;for(i=0;i<n;i++)h=(h^p[i])*16777619u;return h;}
static void cmp_snapshot(CmpSnapshot *s,const MdRuntime *rt,const uint8_t *memory,size_t size){unsigned i;for(i=0;i<8;i++)s->r[i]=rt->cpu.r[i];s->cs=rt->cpu.cs;s->ds=rt->cpu.ds;s->es=rt->cpu.es;s->ss=rt->cpu.ss;s->ip=rt->cpu.ip;s->flags=md_x86_flags((MdX86 *)&rt->cpu);s->memhash=cmp_hash(memory,size);}
static int cmp_equal(const CmpSnapshot *a,const CmpSnapshot *b){unsigned i;for(i=0;i<8;i++)if(a->r[i]!=b->r[i])return 0;return a->cs==b->cs&&a->ds==b->ds&&a->es==b->es&&a->ss==b->ss&&a->ip==b->ip&&a->flags==b->flags&&a->memhash==b->memhash;}
static uint64_t cmp_median7(uint64_t *x){unsigned i,j;for(i=1;i<7;i++){uint64_t y=x[i];j=i;while(j&&x[j-1]>y){x[j]=x[j-1];j--;}x[j]=y;}return x[3];}
#endif
