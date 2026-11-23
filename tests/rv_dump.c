/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * rv_dump - HOST helper of tests/rv_protocol.py (adversarial protocol review, 23 Nov 2026).
 * Exposes the product code (js8enc, js8crc, js8msg, js8jsc, js8asm) on stdin/stdout so that an
 * independent Python model can be compared with it.  No product logic in here.
 *   rv_dump enc           lines "<12 chars> <i3>"  -> frame hex, both CRCs, checks, 79 tones
 *   rv_dump tab           LDPC Nm / Mn / row weights as compiled in
 *   rv_dump msg SD_DIR    lines "<hex22>"          -> js8_msg_decode fields, text as hex
 *   rv_dump asm SD_DIR MYCALL   lines "<hex22> <freq> <snr> <t>", "TICK <t>", "CK <text>", "# note"
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/js8_constants.h"
#include "../src/js8crc.h"
#include "../src/js8enc.h"
#include "../src/js8msg.h"
#include "../src/js8jsc.h"
#include "../src/js8asm.h"
static FILE *fds[8]; static const char *dir;
static int io_open(void *u, const char *name){ char p[512]; int i; (void)u; snprintf(p,sizeof p,"%s/%s",dir,name);
  for(i=0;i<8;i++) if(!fds[i]){ fds[i]=fopen(p,"rb"); if(!fds[i]) return -1; return i;} return -1; }
static int io_read(void *u,int fd,void *b,uint32_t n){(void)u;return (int)fread(b,1,n,fds[fd]);}
static int io_close(void *u,int fd){(void)u;fclose(fds[fd]);fds[fd]=0;return 0;}
static const js8_io_t io={io_open,io_read,io_close,0};
static uint8_t head[40000];
/* stdin: lines "<12 chars> <i3>" -> prints frame hex, crc, msgcrc, tones */
static int main_enc(int argc,char**argv){
  if(argc>1){ /* dump tables */
    int i,j;
    for(i=0;i<87;i++){ printf("NM"); for(j=0;j<7;j++) printf(" %d",kJS8_LDPC_Nm[i][j]); printf(" R %d\n",kJS8_LDPC_Num_rows[i]); }
    for(i=0;i<174;i++){ printf("MN %d %d %d\n",kJS8_LDPC_Mn[i][0],kJS8_LDPC_Mn[i][1],kJS8_LDPC_Mn[i][2]); }
    return 0; }
  char t[64]; int i3;
  while(scanf("%12s %d",t,&i3)==2){
    uint8_t f[11], tn[79]; int i;
    int r=js8_frame_build(t,i3,f);
    if(r){printf("ERR %d\n",r);continue;}
    js8_encode(f,js8_costas_normal,tn);
    for(i=0;i<11;i++)printf("%02x",f[i]);
    printf(" %03x %03x %d %d ",js8_crc12(f),js8_msg_crc12(f),js8_crc12_check(f),js8_msg_crc_ok(f));
    for(i=0;i<79;i++)printf("%d",tn[i]);
    printf("\n");
  }
  return 0;
}
static int main_msg(int argc,char**argv){
  js8jsc_t jsc; js8jsc_t *pj=0; char l[128];
  if(argc>1 && strcmp(argv[1],"-")){ dir=argv[1]; if(js8jsc_init(&jsc,&io,head,sizeof head)){fprintf(stderr,"nodict\n");return 1;} pj=&jsc; }
  while(fgets(l,sizeof l,stdin)){
    uint8_t f[11]; js8_msg_t m; int i; unsigned v;
    for(i=0;i<11;i++){ sscanf(l+2*i,"%2x",&v); f[i]=(uint8_t)v; }
    memset(&m,0x55,sizeof m);
    js8_msg_decode(f,pj,&m);
    printf("%d|%d|%d|%d|%d|%d|%d|%d|%s|%s|%s|",m.type,m.i3,m.is_cq,m.bits3,m.cmd,m.has_num,m.num,m.flags,m.from,m.to,m.grid);
    for(i=0;m.text[i];i++) printf("%02x",(unsigned char)m.text[i]);
    printf("\n");
  }
  return 0;
}
static void esc(const char*s){ for(;*s;s++){ unsigned char c=*s; if(c<32||c>126) printf("\\x%02x",c); else putchar(c);} }
static void emit(void *u,const js8asm_msg_t *m){ (void)u; printf("  MSG f=%d snr=%d cmd=%d flags=%02x frames=%d from=[%s] to=[%s] text=[",m->freq,m->snr,m->cmd,m->flags,m->frames,m->from,m->to); esc(m->text); printf("]\n"); }
static int main_asm(int argc,char**argv){
  js8jsc_t jsc; static js8asm_t a; char l[256];
  dir=argv[1]; if(js8jsc_init(&jsc,&io,head,sizeof head)){fprintf(stderr,"nodict\n");return 1;}
  js8asm_init(&a, argc>2?argv[2]:"OK1BT", argc>3?argv[3]:0, emit, 0);
  while(fgets(l,sizeof l,stdin)){
    if(l[0]=='#'){ printf("%s",l); continue; }
    if(!strncmp(l,"CK ",3)){ char o[4]={0}; l[strcspn(l,"\n")]=0; js8_checksum16(l+3,(int)strlen(l+3),o); printf("CK %s -> %s\n",l+3,o); continue; }
    if(!strncmp(l,"TICK",4)){ js8asm_tick(&a,(uint32_t)atoi(l+5)); continue; }
    uint8_t f[11]; js8_msg_t m; int i; unsigned v; int fr,snr,t;
    for(i=0;i<11;i++){ sscanf(l+2*i,"%2x",&v); f[i]=(uint8_t)v; }
    sscanf(l+22,"%d %d %d",&fr,&snr,&t);
    js8_msg_decode(f,&jsc,&m);
    printf(" frame t=%d f=%d type=%d i3=%d text=[",t,fr,m.type,m.i3); esc(m.text); printf("]\n");
    js8asm_feed(&a,&m,fr,snr,(uint32_t)t);
  }
  return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "enc")) return main_enc(1, argv + 1);
    if (!strcmp(argv[1], "tab")) return main_enc(2, argv);
    if (!strcmp(argv[1], "msg")) return main_msg(argc - 1, argv + 1);
    if (!strcmp(argv[1], "asm")) return main_asm(argc - 1, argv + 1);
    return 2;
}
