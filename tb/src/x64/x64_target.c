#include "x64.h"
#include <tb_x64.h>
#include "x64_emitter.h"
#include "x64_disasm.c"

enum {
    // register classes
    REG_CLASS_GPR,
    REG_CLASS_XMM,
    REG_CLASS_COUNT,
};

#include "../codegen_impl.h"

static const struct ParamDesc {
    int chkstk_limit;
    int gpr_count;
    int xmm_count;
    uint16_t caller_saved_xmms; // XMM0 - XMMwhatever
    uint16_t caller_saved_gprs; // bitfield

    GPR gprs[6];
} param_descs[] = {
    // win64
    { 4096,    4, 4, 6, WIN64_ABI_CALLER_SAVED,   { RCX, RDX, R8,  R9,  0,  0 } },
    // system v
    { INT_MAX, 6, 4, 5, SYSV_ABI_CALLER_SAVED,    { RDI, RSI, RDX, RCX, R8, R9 } },
    // syscall
    { INT_MAX, 6, 4, 5, SYSCALL_ABI_CALLER_SAVED, { RDI, RSI, RDX, R10, R8, R9 } },
};

enum {
    ALL_GPRS            = 0xFFFF & ~((1 << RBP) | (1 << RSP)),
    ALL_GPRS_NO_RAX_RDX = 0xFFFF & ~((1 << RBP) | (1 << RSP) | (1 << RAX) | (1 << RDX)),
};

// *out_mask of 0 means no mask
static TB_X86_DataType legalize_int(TB_DataType dt, uint64_t* out_mask) {
    assert(dt.type == TB_INT || dt.type == TB_PTR);
    if (dt.type == TB_PTR) return *out_mask = 0, TB_X86_TYPE_QWORD;

    TB_X86_DataType t = TB_X86_TYPE_NONE;
    int bits = 0;

    if (dt.data <= 8) bits = 8, t = TB_X86_TYPE_BYTE;
    else if (dt.data <= 16) bits = 16, t = TB_X86_TYPE_WORD;
    else if (dt.data <= 32) bits = 32, t = TB_X86_TYPE_DWORD;
    else if (dt.data <= 64) bits = 64, t = TB_X86_TYPE_QWORD;

    assert(bits != 0 && "TODO: large int support");
    uint64_t mask = dt.data == 0 ? 0 :  ~UINT64_C(0) >> (64 - dt.data);

    *out_mask = (dt.data == bits) ? 0 : mask;
    return t;
}

static TB_X86_DataType legalize_int2(TB_DataType dt) {
    uint64_t m;
    return legalize_int(dt, &m);
}

static TB_X86_DataType legalize_float(TB_DataType dt) {
    assert(dt.type == TB_FLOAT);
    return (dt.data == TB_FLT_64 ? TB_X86_TYPE_SSE_SD : TB_X86_TYPE_SSE_SS);
}

static TB_X86_DataType legalize(TB_DataType dt) {
    if (dt.type == TB_FLOAT) {
        return legalize_float(dt);
    } else {
        uint64_t m;
        return legalize_int(dt, &m);
    }
}

static bool try_for_imm32(int bits, TB_Node* n, int32_t* out_x) {
    if (n->type != TB_INTEGER_CONST) {
        return false;
    }

    TB_NodeInt* i = TB_NODE_GET_EXTRA(n);
    if (bits > 32) {
        bool sign = (i->value >> 31ull) & 1;
        uint64_t top = i->value >> 32ull;

        // if the sign matches the rest of the top bits, we can sign extend just fine
        if (top != (sign ? 0xFFFFFFFF : 0)) {
            return false;
        }
    }

    *out_x = i->value;
    return true;
}

static void init_ctx(Ctx* restrict ctx, TB_ABI abi) {
    ctx->sched = greedy_scheduler;
    ctx->regalloc = tb__lsra;

    ctx->abi_index = abi == TB_ABI_SYSTEMV ? 1 : 0;

    // currently only using 16 GPRs and 16 XMMs, AVX gives us
    // 32 YMMs (which double as XMMs) and later on APX will do
    // 32 GPRs.
    ctx->num_regs[0] = 16;
    ctx->num_regs[1] = 16;

    // don't include RBP and RSP, those are special cases
    uint32_t callee_saved_gprs = ~param_descs[ctx->abi_index].caller_saved_gprs;
    callee_saved_gprs &= ~(1u << RBP);
    callee_saved_gprs &= ~(1u << RSP);
    ctx->callee_saved[0] = callee_saved_gprs;

    // mark XMM callees
    ctx->callee_saved[1] = 0;
    FOREACH_N(i, param_descs[ctx->abi_index].caller_saved_xmms, 16) {
        ctx->callee_saved[1] |= (1ull << i);
    }
}

static RegMask isel_node(Ctx* restrict ctx, Tile* dst, TB_Node* n) {
    switch (n->type) {
        // no inputs
        case TB_START:
        return REGEMPTY;

        case TB_END:
        tile_set_ins(ctx, dst, n, 3, n->input_count);
        return REGEMPTY;

        case TB_PROJ:
        int i = TB_NODE_GET_EXTRA_T(n, TB_NodeProj)->index;
        if (n->inputs[0]->type == TB_START) {
            // function params are ABI crap
            const struct ParamDesc* params = &param_descs[ctx->abi_index];
            return REGMASK(GPR, i >= 3 ? (1u << params->gprs[i - 3]) : 0);
        } else if (n->inputs[0]->type == TB_CALL) {
            if (i == 2) return REGMASK(GPR, 1 << RAX);
            else if (i == 3) return REGMASK(GPR, 1 << RDX);
            else return REGEMPTY;
        } else {
            tb_todo();
        }

        // binary ops
        case TB_AND:
        case TB_OR:
        case TB_XOR:
        case TB_ADD:
        case TB_SUB:
        case TB_MUL: {
            int32_t x;
            if (try_for_imm32(n->dt.data, n->inputs[2], &x)) {
                fold_node(ctx, n->inputs[2]);
                tile_set_ins(ctx, dst, n, 1, 2);
                dst->tag = TILE_FOLDED_IMM;
            } else {
                tile_set_ins(ctx, dst, n, 1, n->input_count);
            }
            return REGMASK(GPR, ALL_GPRS);
        }

        case TB_UDIV:
        case TB_SDIV:
        tile_set_ins(ctx, dst, n, 2, 3);
        return REGMASK(GPR, 1 << RAX);

        case TB_UMOD:
        case TB_SMOD:
        tile_set_ins(ctx, dst, n, 2, 3);
        return REGMASK(GPR, 1 << RDX);

        case TB_LOAD:
        return REGMASK(GPR, ALL_GPRS);

        case TB_STORE:
        tile_set_ins(ctx, dst, n, 2, n->input_count);
        return REGEMPTY;

        case TB_CALL: {
            int end_of_reg_params = n->input_count > 7 ? 7 : n->input_count;

            if (n->inputs[2]->type == TB_SYMBOL) {
                // CALL symbol
                fold_node(ctx, n->inputs[2]);
                tile_set_ins(ctx, dst, n, 3, end_of_reg_params);
            } else {
                // CALL r/m
                tile_set_ins(ctx, dst, n, 2, end_of_reg_params);
            }
            return REGEMPTY;
        }

        default:
        tb_todo();
        return REGEMPTY;
    }
}

static RegMask in_reg_mask(Ctx* restrict ctx, Tile* tile, TB_Node* n, int i) {
    switch (n->type) {
        case TB_END: {
            if (i == 3) return REGMASK(GPR, 1 << RAX);
            else if (i == 4) return REGMASK(GPR, 1 << RDX);
            else return REGEMPTY;
        }

        case TB_CALL: {
            if (i >= 3) {
                // function parameters
                const struct ParamDesc* params = &param_descs[ctx->abi_index];
                int j = i - 3;

                if (j < 4) {
                    return REGMASK(GPR, 1u << params->gprs[j]);
                } else {
                    return REGMASK(GPR, ALL_GPRS);
                }
            } else {
                return REGEMPTY;
            }
        }

        case TB_LOAD:
        return REGMASK(GPR, ALL_GPRS);

        case TB_UDIV:
        case TB_SDIV:
        case TB_UMOD:
        case TB_SMOD:
        return REGMASK(GPR, i == 1 ? (1 << RAX) : ALL_GPRS_NO_RAX_RDX);

        case TB_AND:
        case TB_OR:
        case TB_XOR:
        case TB_ADD:
        case TB_SUB:
        case TB_MUL:
        return REGMASK(GPR, ALL_GPRS);

        default: tb_todo();
    }
}

static void emit_epilogue(Ctx* restrict ctx, TB_CGEmitter* e, int stack_usage) {
    // add rsp, N
    if (stack_usage > 0) {
        if (stack_usage == (int8_t)stack_usage) {
            EMIT1(&ctx->emit, rex(true, 0x00, RSP, 0));
            EMIT1(&ctx->emit, 0x83);
            EMIT1(&ctx->emit, mod_rx_rm(MOD_DIRECT, 0x00, RSP));
            EMIT1(&ctx->emit, (int8_t) stack_usage);
        } else {
            EMIT1(&ctx->emit, rex(true, 0x00, RSP, 0));
            EMIT1(&ctx->emit, 0x81);
            EMIT1(&ctx->emit, mod_rx_rm(MOD_DIRECT, 0x00, RSP));
            EMIT4(&ctx->emit, stack_usage);
        }
    }

    // pop rbp (if we even used the frameptr)
    if ((ctx->features.gen & TB_FEATURE_FRAME_PTR) && stack_usage > 0) {
        EMIT1(&ctx->emit, 0x58 + RBP);
    }
}

static Val val_at(Ctx* ctx, LiveInterval* l) {
    if (l->is_spill) {
        return val_stack(ctx->stack_usage - ctx->spills[l->id]);
    } else {
        return val_gpr(l->assigned);
    }
}

static Val val_indirect_at(LiveInterval* l) {
    assert(!l->is_spill);
    return val_base_disp(l->assigned, 0);
}

static bool clobbers(Ctx* restrict ctx, Tile* t, uint64_t clobbers[MAX_REG_CLASSES]) {
    if (t->n) switch (t->n->type) {
        case TB_UDIV:
        case TB_SDIV:
        case TB_UMOD:
        case TB_SMOD:
        clobbers[0] = 1u << RDX;
        clobbers[1] = 0;
        return true;
    }

    return false;
}

static void emit_tile(Ctx* restrict ctx, TB_CGEmitter* e, Tile* t) {
    if (t->tag == TILE_SPILL_MOVE) {
        Val dst = val_at(ctx, t->interval);
        Val src = val_at(ctx, t->ins[0].src);
        if (!is_value_match(&dst, &src)) {
            inst2(e, MOV, &dst, &src, TB_X86_TYPE_QWORD);
        }
    } else if (t->tag == TILE_NORMAL || t->tag == TILE_GOTO || t->tag >= TILE_FOLDED_IMM) {
        TB_Node* n = t->n;
        switch (n->type) {
            // prologue
            case TB_START: {
                int stack_usage = ctx->stack_usage;

                // save frame pointer (if applies)
                if ((ctx->features.gen & TB_FEATURE_FRAME_PTR) && stack_usage > 0) {
                    EMIT1(e, 0x50 + RBP);

                    // mov rbp, rsp
                    EMIT1(e, rex(true, RSP, RBP, 0));
                    EMIT1(e, 0x89);
                    EMIT1(e, mod_rx_rm(MOD_DIRECT, RSP, RBP));
                }

                // inserts a chkstk call if we use too much stack
                if (stack_usage >= param_descs[ctx->abi_index].chkstk_limit) {
                    assert(ctx->f->super.module->chkstk_extern);
                    Val sym = val_global(ctx->f->super.module->chkstk_extern, 0);
                    Val imm = val_imm(stack_usage);
                    Val rax = val_gpr(RAX);
                    Val rsp = val_gpr(RSP);

                    inst2(e, MOV, &rax, &imm, TB_X86_TYPE_DWORD);
                    inst1(e, CALL, &sym, TB_X86_TYPE_QWORD);
                    inst2(e, SUB, &rsp, &rax, TB_X86_TYPE_QWORD);
                } else if (stack_usage > 0) {
                    if (stack_usage == (int8_t)stack_usage) {
                        // sub rsp, stack_usage
                        EMIT1(e, rex(true, 0x00, RSP, 0));
                        EMIT1(e, 0x83);
                        EMIT1(e, mod_rx_rm(MOD_DIRECT, 0x05, RSP));
                        EMIT1(e, stack_usage);
                    } else {
                        // sub rsp, stack_usage
                        EMIT1(e, rex(true, 0x00, RSP, 0));
                        EMIT1(e, 0x81);
                        EMIT1(e, mod_rx_rm(MOD_DIRECT, 0x05, RSP));
                        EMIT4(e, stack_usage);
                    }
                }
                ctx->prologue_length = ctx->emit.count;
                break;
            }
            // epilogue
            case TB_END: {
                emit_epilogue(ctx, e, ctx->stack_usage);
                EMIT1(&ctx->emit, 0xC3);
                break;
            }
            // projections don't manage their own work, that's the
            // TUPLE node's job.
            case TB_PROJ: break;

            case TB_SYMBOL: {
                TB_Symbol* sym = TB_NODE_GET_EXTRA_T(n, TB_NodeSymbol)->sym;

                Val dst = val_at(ctx, t->interval);
                Val src = val_global(sym, 0);
                inst2(e, LEA, &dst, &src, TB_X86_TYPE_QWORD);
                break;
            }
            case TB_LOAD: {
                TB_Symbol* sym = TB_NODE_GET_EXTRA_T(n, TB_NodeSymbol)->sym;

                Val dst = val_at(ctx, t->interval);
                Val src = val_indirect_at(t->ins[0].src);
                inst2(e, MOV, &dst, &src, legalize_int2(n->dt));
                break;
            }
            case TB_AND:
            case TB_OR:
            case TB_XOR:
            case TB_ADD:
            case TB_SUB: {
                const static InstType ops[] = { AND, OR, XOR, ADD, SUB };
                InstType op = ops[n->type - TB_AND];
                TB_X86_DataType dt = legalize_int2(n->dt);

                Val dst = val_at(ctx, t->interval);
                Val lhs = val_at(ctx, t->ins[0].src);
                if (!is_value_match(&dst, &lhs)) {
                    inst2(e, MOV, &dst, &lhs, dt);
                }

                if (t->tag == TILE_FOLDED_IMM) {
                    assert(n->inputs[2]->type == TB_INTEGER_CONST);
                    TB_NodeInt* i = TB_NODE_GET_EXTRA(n->inputs[2]);

                    Val rhs = val_imm(i->value);
                    inst2(e, op, &dst, &rhs, dt);
                } else {
                    Val rhs = val_at(ctx, t->ins[1].src);
                    inst2(e, op, &dst, &rhs, dt);
                }
                break;
            }
            case TB_UDIV:
            case TB_SDIV:
            case TB_UMOD:
            case TB_SMOD: {
                bool is_signed = (n->type == TB_SDIV || n->type == TB_SMOD);
                bool is_div    = (n->type == TB_UDIV || n->type == TB_SDIV);

                TB_DataType dt = n->dt;

                // if signed:
                //   cqo/cdq (sign extend RAX into RDX)
                // else:
                //   xor rdx, rdx
                if (is_signed) {
                    if (n->dt.data > 32) {
                        EMIT1(e, 0x48);
                    }
                    EMIT1(e, 0x99);
                } else {
                    Val rdx = val_gpr(RDX);
                    inst2(e, XOR, &rdx, &rdx, TB_X86_TYPE_DWORD);
                }

                Val rhs = val_at(ctx, t->ins[0].src);
                inst1(e, is_signed ? IDIV : DIV, &rhs, TB_X86_TYPE_DWORD);
                break;
            }
            case TB_CALL: {
                // we've already placed the register params in their slots, now we're missing
                // stack params which go into [rsp + 0x20 + (i-4)*8] where i is the param index.
                int start = n->inputs[2]->type == TB_SYMBOL ? 1 : 0;
                FOREACH_N(i, start + 4, t->in_count) {
                    __debugbreak();
                }

                if (n->inputs[2]->type == TB_SYMBOL) {
                    TB_Symbol* sym = TB_NODE_GET_EXTRA_T(n->inputs[2], TB_NodeSymbol)->sym;

                    Val target = val_global(sym, 0);
                    inst1(e, CALL, &target, TB_X86_TYPE_QWORD);
                } else {
                    Val target = val_at(ctx, t->ins[0].src);
                    inst1(e, CALL, &target, TB_X86_TYPE_QWORD);
                }
                break;
            }

            default: tb_todo();
        }

        // if we were a TILE_GOTO this is the point where we jump
        if (t->tag == TILE_GOTO) {
            __debugbreak();
        }
    } else {
        tb_todo();
    }
}

static void emit_win64eh_unwind_info(TB_Emitter* e, TB_FunctionOutput* out_f, uint64_t stack_usage) {
    size_t patch_pos = e->count;
    UnwindInfo unwind = {
        .version = 1,
        .flags = 0, // UNWIND_FLAG_EHANDLER,
        .prolog_length = out_f->prologue_length,
        .code_count = 0,
        .frame_register = RBP,
        .frame_offset = 0,
    };
    tb_outs(e, sizeof(UnwindInfo), &unwind);

    size_t code_count = 0;
    if (stack_usage > 0) {
        UnwindCode codes[] = {
            // sub rsp, stack_usage
            { .code_offset = 8, .unwind_op = UNWIND_OP_ALLOC_SMALL, .op_info = (stack_usage / 8) - 1 },
            // mov rbp, rsp
            { .code_offset = 4, .unwind_op = UNWIND_OP_SET_FPREG, .op_info = 0 },
            // push rbp
            { .code_offset = 1, .unwind_op = UNWIND_OP_PUSH_NONVOL, .op_info = RBP },
        };
        tb_outs(e, sizeof(codes), codes);
        code_count += 3;
    }

    tb_patch1b(e, patch_pos + offsetof(UnwindInfo, code_count), code_count);
}

#define E(fmt, ...) tb_asm_print(e, fmt, ## __VA_ARGS__)
static void disassemble(TB_CGEmitter* e, Disasm* restrict d, int bb, size_t pos, size_t end) {
    if (bb >= 0) {
        E(".bb%d:\n", bb);
    }

    while (pos < end) {
        while (d->loc != d->end && d->loc->pos == pos) {
            E("  // %s : line %d\n", d->loc->file->path, d->loc->line);
            d->loc++;
        }

        TB_X86_Inst inst;
        if (!tb_x86_disasm(&inst, end - pos, &e->data[pos])) {
            E("  ERROR\n");
            pos += 1; // skip ahead once... cry
            continue;
        }

        const char* mnemonic = tb_x86_mnemonic(&inst);
        E("  ");
        if (inst.flags & TB_X86_INSTR_REP) {
            E("rep ");
        }
        if (inst.flags & TB_X86_INSTR_LOCK) {
            E("lock ");
        }
        E("%s", mnemonic);
        if (inst.data_type >= TB_X86_TYPE_SSE_SS && inst.data_type <= TB_X86_TYPE_SSE_PD) {
            static const char* strs[] = { "ss", "sd", "ps", "pd" };
            E(strs[inst.data_type - TB_X86_TYPE_SSE_SS]);
        }
        E(" ");

        bool mem = true, imm = true;
        for (int i = 0; i < 4; i++) {
            if (inst.regs[i] == -1) {
                if (mem && (inst.flags & TB_X86_INSTR_USE_MEMOP)) {
                    if (i > 0) E(", ");

                    mem = false;

                    if (inst.flags & TB_X86_INSTR_USE_RIPMEM) {
                        bool is_label = inst.opcode == 0xE8 || inst.opcode == 0xE9
                            || (inst.opcode >= 0x70   && inst.opcode <= 0x7F)
                            || (inst.opcode >= 0x0F80 && inst.opcode <= 0x0F8F);

                        if (!is_label) E("[");

                        if (d->patch && d->patch->pos == pos + inst.length - 4) {
                            const TB_Symbol* target = d->patch->target;

                            if (target->name[0] == 0) {
                                E("sym%p", target);
                            } else {
                                E("%s", target->name);
                            }
                            d->patch = d->patch->next;
                        } else {
                            uint32_t target = pos + inst.length + inst.disp;
                            int bb = tb_emit_get_label(e, target);
                            uint32_t landed = e->labels[bb] & 0x7FFFFFFF;

                            if (landed != target) {
                                E(".bb%d + %d", bb, (int)target - (int)landed);
                            } else {
                                E(".bb%d", bb);
                            }
                        }

                        if (!is_label) E("]");
                    } else {
                        E("%s [", tb_x86_type_name(inst.data_type));
                        if (inst.base != 255) {
                            E("%s", tb_x86_reg_name(inst.base, TB_X86_TYPE_QWORD));
                        }

                        if (inst.index != 255) {
                            E(" + %s*%d", tb_x86_reg_name(inst.index, TB_X86_TYPE_QWORD), 1 << inst.scale);
                        }

                        if (inst.disp > 0) {
                            E(" + %d", inst.disp);
                        } else if (inst.disp < 0) {
                            E(" - %d", -inst.disp);
                        }

                        E("]");
                    }
                } else if (imm && (inst.flags & (TB_X86_INSTR_IMMEDIATE | TB_X86_INSTR_ABSOLUTE))) {
                    if (i > 0) E(", ");

                    imm = false;
                    if (inst.flags & TB_X86_INSTR_ABSOLUTE) {
                        E("%#llx", inst.abs);
                    } else {
                        E("%#x", inst.imm);
                    }
                } else {
                    break;
                }
            } else {
                if (i > 0) {
                    E(", ");

                    // special case for certain ops with two data types
                    if (inst.flags & TB_X86_INSTR_TWO_DATA_TYPES) {
                        E("%s", tb_x86_reg_name(inst.regs[i], inst.data_type2));
                        continue;
                    }
                }

                E("%s", tb_x86_reg_name(inst.regs[i], inst.data_type));
            }
        }
        E("\n");

        pos += inst.length;
    }
}
#undef E

static size_t emit_call_patches(TB_Module* restrict m, TB_FunctionOutput* out_f) {
    size_t r = 0;
    uint32_t src_section = out_f->section;

    for (TB_SymbolPatch* patch = out_f->first_patch; patch; patch = patch->next) {
        if (patch->target->tag == TB_SYMBOL_FUNCTION) {
            uint32_t dst_section = ((TB_Function*) patch->target)->output->section;

            // you can't do relocations across sections
            if (src_section == dst_section) {
                assert(patch->pos < out_f->code_size);

                // x64 thinks of relative addresses as being relative
                // to the end of the instruction or in this case just
                // 4 bytes ahead hence the +4.
                size_t actual_pos = out_f->code_pos + patch->pos + 4;

                uint32_t p = ((TB_Function*) patch->target)->output->code_pos - actual_pos;
                memcpy(&out_f->code[patch->pos], &p, sizeof(uint32_t));

                r += 1;
                patch->internal = true;
            }
        }
    }

    return out_f->patch_count - r;
}

ICodeGen tb__x64_codegen = {
    .minimum_addressable_size = 8,
    .pointer_size = 64,
    .emit_win64eh_unwind_info = emit_win64eh_unwind_info,
    .emit_call_patches  = emit_call_patches,
    .get_data_type_size = get_data_type_size,
    .compile_function   = compile_function,
};