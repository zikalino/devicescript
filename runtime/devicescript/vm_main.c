#include "devs_internal.h"
#include "devs_vm_internal.h"

static inline uint8_t devs_vm_fetch_byte(devs_activation_t *frame, devs_ctx_t *ctx) {
    if (frame->pc < frame->maxpc)
        return ctx->img.data[frame->pc++];
    devs_runtime_failure(ctx, 60100);
    return 0;
}

uint8_t devs_fetch_opcode(devs_activation_t *frame, devs_ctx_t *ctx) {
    return devs_vm_fetch_byte(frame, ctx);
}

static inline int32_t devs_vm_fetch_int(devs_activation_t *frame, devs_ctx_t *ctx) {
    uint8_t v = devs_vm_fetch_byte(frame, ctx);
    if (v < DEVS_FIRST_MULTIBYTE_INT)
        return v;

    int32_t r = 0;
    bool n = !!(v & 4);
    int len = (v & 3) + 1;
    for (int i = 0; i < len; ++i) {
        uint8_t b = devs_vm_fetch_byte(frame, ctx);
        r <<= 8;
        r |= b;
    }

    return n ? -r : r;
}

static inline void devs_vm_push(devs_ctx_t *ctx, value_t v) {
    if (ctx->stack_top >= DEVS_MAX_STACK_DEPTH)
        devs_runtime_failure(ctx, 60101);
    else
        ctx->the_stack[ctx->stack_top++] = v;
}

void devs_dump_stackframe(devs_ctx_t *ctx, devs_activation_t *fn) {
    int idx = fn->func - devs_img_get_function(ctx->img, 0);
    DMESG("pc=%d @ %s_F%d st=%d", (int)(fn->pc - fn->func->start), devs_img_fun_name(ctx->img, idx),
          idx, ctx->stack_top);
}

int devs_vm_resume(devs_ctx_t *ctx) {
    if (!devs_is_suspended(ctx))
        return -1;
    ctx->suspension = JD_DEVS_DBG_SUSPENSION_TYPE_NONE;
    TODO();
}

void devs_vm_suspend(devs_ctx_t *ctx, unsigned cause) {
    ctx->suspension = cause;
    TODO();
}

static inline unsigned brk_hash(unsigned pc) {
    return pc & (DEVS_BRK_HASH_SIZE - 1);
}

static void recompute_brk_jump_tbl(devs_ctx_t *ctx) {
    memset(ctx->brk_jump_tbl, 0, DEVS_BRK_HASH_SIZE);
    devs_pc_t *l = ctx->brk_list;
    for (unsigned i = 0; i < ctx->brk_count; ++i) {
        if (l[i] && !ctx->brk_jump_tbl[brk_hash(l[i])])
            ctx->brk_jump_tbl[brk_hash(l[i])] = i;
    }
}

void devs_vm_clear_breakpoints(devs_ctx_t *ctx) {
    jd_free(ctx->brk_list);
    ctx->brk_list = NULL;
    ctx->brk_count = 0;
    recompute_brk_jump_tbl(ctx);
}

bool devs_vm_clear_breakpoint(devs_ctx_t *ctx, unsigned pc) {
    if (pc == 0)
        return false;
    JD_ASSERT(pc == (devs_pc_t)pc);
    devs_pc_t *l = ctx->brk_list;
    for (unsigned i = 0; i < ctx->brk_count; ++i) {
        if (l[i] == pc) {
            memmove(l + i, l + i + 1, (ctx->brk_count - i - 1) * sizeof(devs_pc_t));
            ctx->brk_list[ctx->brk_count - 1] = 0;
            recompute_brk_jump_tbl(ctx);
            return true;
        }
    }
    return false;
}

int devs_vm_set_breakpoint(devs_ctx_t *ctx, unsigned pc) {
    JD_ASSERT(pc == (devs_pc_t)pc);
    if (pc == 0)
        return -2;
    devs_pc_t *l = ctx->brk_list;
    unsigned cnt = ctx->brk_count;
    if (cnt == 0 || l[cnt - 1] != 0) {
        if (cnt >= DEVS_BRK_MAX_COUNT)
            return -1;

        cnt = cnt * 2 + 8;
        if (cnt > DEVS_BRK_MAX_COUNT)
            cnt = DEVS_BRK_MAX_COUNT;

        l = jd_alloc(cnt * sizeof(devs_pc_t));
        memcpy(l, ctx->brk_list, ctx->brk_count * sizeof(devs_pc_t));
        jd_free(ctx->brk_list);
        ctx->brk_list = l;
        ctx->brk_count = cnt;
    }

    bool was_in_section = false;
    for (unsigned i = 0; i < cnt; ++i) {
        bool in_section = brk_hash(l[i]) == brk_hash(pc);
        if (l[i] == 0) {
            l[i] = pc;
            break;
        } else if ((was_in_section && !in_section) || (in_section && l[i] >= pc)) {
            if (l[i] == pc)
                return 0;
            memmove(l + i + 1, l + i, (cnt - i - 1) * sizeof(devs_pc_t));
            l[i] = pc;
            break;
        }
        was_in_section = in_section;
    }

    recompute_brk_jump_tbl(ctx);
    return 1;
}

static inline bool devs_vm_chk_brk(devs_ctx_t *ctx, devs_activation_t *frame) {
    devs_pc_t pc = frame->pc;
    unsigned i = ctx->brk_jump_tbl[brk_hash(pc)];

    if (i) {
        devs_pc_t *l = ctx->brk_list;
        for (; pc >= l[i]; ++i) {
            if (pc == l[i]) {
                devs_vm_suspend(ctx, JD_DEVS_DBG_SUSPENSION_TYPE_BREAKPOINT);
                return true;
            }
        }
    }

    return false;
}

static void devs_vm_exec_opcode(devs_ctx_t *ctx, devs_activation_t *frame) {
    if (devs_vm_chk_brk(ctx, frame))
        return;

    uint8_t op = devs_vm_fetch_byte(frame, ctx);

    if (op >= DEVS_DIRECT_CONST_OP) {
        int v = op - DEVS_DIRECT_CONST_OP - DEVS_DIRECT_CONST_OFFSET;
        devs_vm_push(ctx, devs_value_from_int(v));
        return;
    }

    if (op >= DEVS_OP_PAST_LAST) {
        devs_runtime_failure(ctx, 60102);
    } else {
        uint8_t flags = DEVS_OP_PROPS[op];

        if (flags & DEVS_BYTECODEFLAG_TAKES_NUMBER) {
            ctx->jmp_pc = frame->pc - 1;
            ctx->literal_int = devs_vm_fetch_int(frame, ctx);
        }

        ctx->stack_top_for_gc = ctx->stack_top;

        // devs_dump_stackframe(ctx, frame);

        if (flags & DEVS_BYTECODEFLAG_IS_STMT) {
            ((devs_vm_stmt_handler_t)devs_vm_op_handlers[op])(frame, ctx);
            if (ctx->stack_top)
                devs_runtime_failure(ctx, 60103);
        } else {
            value_t v = ((devs_vm_expr_handler_t)devs_vm_op_handlers[op])(frame, ctx);
            devs_vm_push(ctx, v);
        }

        if (ctx->in_throw) {
            ctx->stack_top = 0;
            ctx->in_throw = 0;
            if (ctx->curr_fiber)
                ctx->curr_fiber->ret_val = ctx->exn_val;
            ctx->exn_val = devs_undefined;
        }
    }
}

void devs_vm_exec_opcodes(devs_ctx_t *ctx) {
    unsigned maxsteps = DEVS_MAX_STEPS;

    while (ctx->curr_fn && --maxsteps)
        devs_vm_exec_opcode(ctx, ctx->curr_fn);

    if (maxsteps == 0)
        devs_panic(ctx, DEVS_PANIC_TIMEOUT);
}

static const char *builtin_strings[DEVS_BUILTIN_STRING___MAX + 1] = {DEVS_BUILTIN_STRING__VAL};

const char *devs_builtin_string_by_idx(unsigned idx) {
    if (idx > DEVS_BUILTIN_STRING___MAX)
        return NULL;
    return builtin_strings[idx];
}

const char *devs_img_get_utf8(devs_img_t img, uint32_t idx, unsigned *size) {
    if (!devs_img_stridx_ok(img, idx)) {
        if (size)
            *size = 0;
        return NULL;
    }

    unsigned tp = (uint16_t)idx >> DEVS_STRIDX__SHIFT;
    idx &= (1 << DEVS_STRIDX__SHIFT) - 1;

    const char *r = NULL;
    const devs_img_section_t *sect = NULL;

    switch (tp) {
    case DEVS_STRIDX_UTF8:
        sect = (const devs_img_section_t *)(img.data + img.header->utf8_strings.start +
                                            idx * sizeof(devs_img_section_t));
        break;
    case DEVS_STRIDX_BUFFER:
        sect = (const devs_img_section_t *)(img.data + img.header->buffers.start +
                                            idx * sizeof(devs_img_section_t));
        break;
    case DEVS_STRIDX_BUILTIN:
        r = builtin_strings[idx];
        break;
    case DEVS_STRIDX_ASCII: {
        JD_ASSERT(DEVS_ASCII_HEADER_SIZE == sizeof(uint16_t));
        uint16_t off = *(uint16_t *)(img.data + img.header->ascii_strings.start +
                                     idx * DEVS_ASCII_HEADER_SIZE);
        r = (const char *)(img.data + img.header->string_data.start + off);
    } break;
    default:
        JD_PANIC();
    }

    if (sect) {
        if (size)
            *size = sect->length;
        return (const char *)(img.data + img.header->string_data.start + sect->start);
    } else if (r) {
        if (size)
            *size = strlen(r);
        return r;
    } else {
        JD_PANIC();
    }
}

const char *devs_get_static_utf8(devs_ctx_t *ctx, uint32_t idx, unsigned *size) {
    const char *r = devs_img_get_utf8(ctx->img, idx, size);
    if (r == NULL) {
        devs_runtime_failure(ctx, 60104);
        return "";
    }
    return r;
}