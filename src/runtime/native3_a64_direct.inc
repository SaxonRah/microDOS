/* Native-3 AArch64 direct lowering extension.
 * Included by jit_aarch64_backend.inc after MdA64Buf/a64_emit helpers exist.
 * No C semantic helper is called from emitted prefixes.
 */
#if defined(__aarch64__)

static uint32_t n3_a64_ldrh(unsigned wt, unsigned xn, unsigned off)
{
    return 0x79400000u | (((off >> 1) & 0x0fffu) << 10) |
           ((xn & 31u) << 5) | (wt & 31u);
}
static uint32_t n3_a64_add_w(unsigned wd,unsigned wn,unsigned wm)
{ return 0x0B000000u | ((wm&31u)<<16) | ((wn&31u)<<5) | (wd&31u); }
static uint32_t n3_a64_sub_w(unsigned wd,unsigned wn,unsigned wm)
{ return 0x4B000000u | ((wm&31u)<<16) | ((wn&31u)<<5) | (wd&31u); }
static uint32_t n3_a64_and_w(unsigned wd,unsigned wn,unsigned wm)
{ return 0x0A000000u | ((wm&31u)<<16) | ((wn&31u)<<5) | (wd&31u); }
static uint32_t n3_a64_orr_w(unsigned wd,unsigned wn,unsigned wm)
{ return 0x2A000000u | ((wm&31u)<<16) | ((wn&31u)<<5) | (wd&31u); }
static uint32_t n3_a64_eor_w(unsigned wd,unsigned wn,unsigned wm)
{ return 0x4A000000u | ((wm&31u)<<16) | ((wn&31u)<<5) | (wd&31u); }

static void n3_a64_lazy16(MdA64Buf *b, unsigned op,
                          unsigned oldr, unsigned rhsr, unsigned resr)
{
    a64_emit(b, a64_movz_w(13u, (uint16_t)op));
    a64_emit(b, a64_strb(13u, 0u, (unsigned)offsetof(MdX86,lazy_op)));
    a64_emit(b, a64_strh(oldr, 0u, (unsigned)offsetof(MdX86,lazy_a)));
    a64_emit(b, a64_strh(rhsr, 0u, (unsigned)offsetof(MdX86,lazy_b)));
    a64_emit(b, a64_strh(resr, 0u, (unsigned)offsetof(MdX86,lazy_res)));
}

/* Direct prefix. It is legal to stop before an unsupported op: commit IP to
 * that exact instruction and return the number of retired guest ops. */
static int md_n3_a64_emit_direct_ext(MdJit *jit, MdJitBlock *block)
{
    MdA64Buf b;
    unsigned i, direct = 0u;
    uint16_t exit_ip;

    if (jit == NULL || block == NULL || block->ops == NULL || block->op_count == 0u)
        return 0;

    memset(&b,0,sizeof(b));
    exit_ip = block->ip;

    for (i=0u;i<block->op_count;i++) {
        const MdJitOp *op=&block->ops[i];
        unsigned roff;
        switch ((MdJitOpKind)op->kind) {
            case MD_JIT_OP_MOV_R16_IMM:
                roff=(unsigned)(offsetof(MdX86,r)+(size_t)(op->reg&7u)*2u);
                a64_emit(&b,a64_movz_w(9u,op->imm));
                a64_emit(&b,a64_strh(9u,0u,roff));
                exit_ip=op->next_ip;++direct;
                break;

            case MD_JIT_OP_ALU_RR16: {
                unsigned soff=(unsigned)(offsetof(MdX86,r)+(size_t)(op->imm&7u)*2u);
                roff=(unsigned)(offsetof(MdX86,r)+(size_t)(op->reg&7u)*2u);
                a64_emit(&b,n3_a64_ldrh(9u,0u,roff));
                a64_emit(&b,n3_a64_ldrh(10u,0u,soff));
                switch(op->aux&7u){
                    case 0u: a64_emit(&b,n3_a64_add_w(11u,9u,10u)); n3_a64_lazy16(&b,MD_LAZY_ADD16,9u,10u,11u); break;
                    case 1u: a64_emit(&b,n3_a64_orr_w(11u,9u,10u)); n3_a64_lazy16(&b,MD_LAZY_LOGIC16,9u,10u,11u); break;
                    case 4u: a64_emit(&b,n3_a64_and_w(11u,9u,10u)); n3_a64_lazy16(&b,MD_LAZY_LOGIC16,9u,10u,11u); break;
                    case 5u: a64_emit(&b,n3_a64_sub_w(11u,9u,10u)); n3_a64_lazy16(&b,MD_LAZY_SUB16,9u,10u,11u); break;
                    case 6u: a64_emit(&b,n3_a64_eor_w(11u,9u,10u)); n3_a64_lazy16(&b,MD_LAZY_LOGIC16,9u,10u,11u); break;
                    case 7u: a64_emit(&b,n3_a64_sub_w(11u,9u,10u)); n3_a64_lazy16(&b,MD_LAZY_SUB16,9u,10u,11u); break;
                    default: goto done;
                }
                if((op->aux&7u)!=7u) a64_emit(&b,a64_strh(11u,0u,roff));
                exit_ip=op->next_ip;++direct;
                break;
            }

            case MD_JIT_OP_ALU_ACC_IMM:
                if(op->reg!=1u) goto done; /* 16-bit AX form */
                roff=(unsigned)(offsetof(MdX86,r)+MD_X86_AX*2u);
                a64_emit(&b,n3_a64_ldrh(9u,0u,roff));
                a64_emit(&b,a64_movz_w(10u,op->imm));
                switch(op->aux&7u){
                    case 0u:a64_emit(&b,n3_a64_add_w(11u,9u,10u));n3_a64_lazy16(&b,MD_LAZY_ADD16,9u,10u,11u);break;
                    case 1u:a64_emit(&b,n3_a64_orr_w(11u,9u,10u));n3_a64_lazy16(&b,MD_LAZY_LOGIC16,9u,10u,11u);break;
                    case 4u:a64_emit(&b,n3_a64_and_w(11u,9u,10u));n3_a64_lazy16(&b,MD_LAZY_LOGIC16,9u,10u,11u);break;
                    case 5u:a64_emit(&b,n3_a64_sub_w(11u,9u,10u));n3_a64_lazy16(&b,MD_LAZY_SUB16,9u,10u,11u);break;
                    case 6u:a64_emit(&b,n3_a64_eor_w(11u,9u,10u));n3_a64_lazy16(&b,MD_LAZY_LOGIC16,9u,10u,11u);break;
                    case 7u:a64_emit(&b,n3_a64_sub_w(11u,9u,10u));n3_a64_lazy16(&b,MD_LAZY_SUB16,9u,10u,11u);break;
                    default:goto done;
                }
                if((op->aux&7u)!=7u)a64_emit(&b,a64_strh(11u,0u,roff));
                exit_ip=op->next_ip;++direct;
                break;

            case MD_JIT_OP_GRP1_R16_IMM:
                roff=(unsigned)(offsetof(MdX86,r)+(size_t)(op->reg&7u)*2u);
                a64_emit(&b,n3_a64_ldrh(9u,0u,roff));
                a64_emit(&b,a64_movz_w(10u,op->imm));
                switch(op->aux&7u){
                    case 0u:a64_emit(&b,n3_a64_add_w(11u,9u,10u));n3_a64_lazy16(&b,MD_LAZY_ADD16,9u,10u,11u);break;
                    case 1u:a64_emit(&b,n3_a64_orr_w(11u,9u,10u));n3_a64_lazy16(&b,MD_LAZY_LOGIC16,9u,10u,11u);break;
                    case 4u:a64_emit(&b,n3_a64_and_w(11u,9u,10u));n3_a64_lazy16(&b,MD_LAZY_LOGIC16,9u,10u,11u);break;
                    case 5u:a64_emit(&b,n3_a64_sub_w(11u,9u,10u));n3_a64_lazy16(&b,MD_LAZY_SUB16,9u,10u,11u);break;
                    case 6u:a64_emit(&b,n3_a64_eor_w(11u,9u,10u));n3_a64_lazy16(&b,MD_LAZY_LOGIC16,9u,10u,11u);break;
                    case 7u:a64_emit(&b,n3_a64_sub_w(11u,9u,10u));n3_a64_lazy16(&b,MD_LAZY_SUB16,9u,10u,11u);break;
                    default:goto done;
                }
                if((op->aux&7u)!=7u)a64_emit(&b,a64_strh(11u,0u,roff));
                exit_ip=op->next_ip;++direct;
                break;

            case MD_JIT_OP_NOP:
                exit_ip=op->next_ip;++direct;break;
            case MD_JIT_OP_JMP:
                exit_ip=op->target;++direct;goto done;
            default:
                goto done;
        }
    }

done:
    if(!direct||b.failed)return 0;
    a64_emit(&b,a64_movz_w(9u,exit_ip));
    a64_emit(&b,a64_strh(9u,0u,(unsigned)offsetof(MdX86,ip)));
    a64_emit(&b,a64_movz_w(0u,(uint16_t)direct));
    a64_ret(&b);
    if(!md_jit_a64_install(jit,block,&b))return 0;
    block->direct_prefix_ops=(uint8_t)direct;
    return 1;
}
#endif
