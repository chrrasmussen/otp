/*
 * %CopyrightBegin%
 *
 * Copyright Ericsson AB 2020-2021. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * %CopyrightEnd%
 */
#include <algorithm>
#include "beam_asm.hpp"

using namespace asmjit;

extern "C"
{
#include "erl_map.h"
#include "beam_common.h"
}

static const Uint32 INTERNAL_HASH_SALT = 3432918353;
static const Uint32 HCONST_22 = 0x98C475E6UL;
static const Uint32 HCONST = 0x9E3779B9;

/* ARG3 = incoming hash
 * ARG4 = lower 32
 * ARG5 = upper 32
 * ARG6 = type constant
 *
 * Helper function for calculating the internal hash of keys before looking
 * them up in a map.
 *
 * This is essentially just a manual expansion of the `UINT32_HASH_2` macro.
 * Whenever the internal hash algorithm is updated, this and all of its users
 * must follow suit.
 *
 * Result is returned in ARG3. */
void BeamGlobalAssembler::emit_internal_hash_helper() {
    x86::Gp hash = ARG3d, lower = ARG4d, upper = ARG5d;

    a.add(lower, ARG6d);
    a.add(upper, ARG6d);

    using rounds =
            std::initializer_list<std::tuple<x86::Gp, x86::Gp, x86::Gp, int>>;
    for (const auto &round : rounds{{lower, upper, hash, 13},
                                    {upper, hash, lower, -8},
                                    {hash, lower, upper, 13},
                                    {lower, upper, hash, 12},
                                    {upper, hash, lower, -16},
                                    {hash, lower, upper, 5},
                                    {lower, upper, hash, 3},
                                    {upper, hash, lower, -10},
                                    {hash, lower, upper, 15}}) {
        const auto &[r_a, r_b, r_c, shift] = round;

        a.sub(r_a, r_b);
        a.sub(r_a, r_c);

        /* We have no use for the type constant anymore, reuse its register for
         * the `a ^= r_c << shift` expression. */
        a.mov(ARG6d, r_c);

        if (shift > 0) {
            a.shr(ARG6d, imm(shift));
        } else {
            a.shl(ARG6d, imm(-shift));
        }

        a.xor_(r_a, ARG6d);
    }

    a.ret();
}

/* ARG1 = hash map root, ARG2 = key, ARG3 = key hash, RETd = node header
 *
 * Result is returned in RET. ZF is set on success. */
void BeamGlobalAssembler::emit_hashmap_get_element() {
    Label node_loop = a.newLabel();

    x86::Gp node = ARG1, key = ARG2, key_hash = ARG3d, header_val = RETd,
            index = ARG4d, depth = ARG5d;

    const int header_shift =
            (_HEADER_ARITY_OFFS + MAP_HEADER_TAG_SZ + MAP_HEADER_ARITY_SZ);

    /* Skip the root header. This is not required for child nodes. */
    a.add(node, imm(sizeof(Eterm)));
    mov_imm(depth, 0);

    a.bind(node_loop);
    {
        Label fail = a.newLabel(), leaf_node = a.newLabel(),
              skip_index_adjustment = a.newLabel(), update_hash = a.newLabel();

        /* Find out which child we should follow, and shift the hash for the
         * next round. */
        a.mov(index, key_hash);
        a.and_(index, imm(0xF));
        a.shr(key_hash, imm(4));
        a.inc(depth);

        /* The entry offset is always equal to the index on fully populated
         * nodes, so we'll skip adjusting them. */
        ERTS_CT_ASSERT(header_shift == 16);
        a.sar(header_val, imm(header_shift));
        a.cmp(header_val, imm(-1));
        a.short_().je(skip_index_adjustment);
        {
            /* If our bit isn't set on this node, the key can't be found.
             *
             * Note that we jump directly to a `RET` instruction, as `BT` only
             * affects CF, and ZF ("not found") is clear at this point. */
            a.bt(header_val, index);
            a.short_().jnc(fail);

            /* The actual offset of our entry is the number of bits set (in
             * essence "entries present") before our index in the bitmap. */
            a.bzhi(header_val, header_val, index);
            a.popcnt(index, header_val);
        }
        a.bind(skip_index_adjustment);

        a.mov(node,
              x86::qword_ptr(node,
                             index.r64(),
                             3,
                             sizeof(Eterm) - TAG_PRIMARY_BOXED));
        emit_ptr_val(node, node);

        /* Have we found our leaf? */
        a.test(node.r32(), imm(_TAG_PRIMARY_MASK - TAG_PRIMARY_LIST));
        a.short_().je(leaf_node);

        /* Nope, we have to search another node. */
        a.mov(header_val, emit_boxed_val(node, 0, sizeof(Uint32)));

        /* After 8 nodes we've run out of the 32 bits we started with, so we
         * need to update the hash to keep going. */
        a.test(depth, imm(0x7));
        a.short_().jz(update_hash);
        a.short_().jmp(node_loop);

        a.bind(leaf_node);
        {
            /* We've arrived at a leaf, set ZF according to whether its key
             * matches ours and speculatively place the element in RET. */
            a.cmp(getCARRef(node), key);
            a.mov(RET, getCDRRef(node));

            /* See comment at the jump. */
            a.bind(fail);
            a.ret();
        }

        /* After 8 nodes we've run out of the 32 bits we started with, so we
         * must calculate a new hash to continue.
         *
         * This is a manual expansion `make_map_hash` from utils.c, and all
         * changes to that function must be mirrored here. */
        a.bind(update_hash);
        {
            a.mov(TMP_MEM1d, depth);

            /* NOTE: ARG3d is always 0 at this point. */
            a.mov(ARG4d, depth);
            a.shr(ARG4d, imm(3));
            mov_imm(ARG5d, 1);
            a.mov(ARG6d, imm(HCONST_22));
            a.call(labels[internal_hash_helper]);

            a.xor_(ARG3d, imm(INTERNAL_HASH_SALT));
            a.mov(ARG4d, key.r32());
            a.mov(ARG5, key);
            a.shr(ARG5, imm(32));
            a.mov(ARG6d, imm(HCONST));
            a.call(labels[internal_hash_helper]);

            a.mov(depth, TMP_MEM1d);

            a.jmp(node_loop);
        }
    }
}

/* ARG1 = flat map, ARG2 = key
 *
 * Result is returned in RET. ZF is set on success. */
void BeamGlobalAssembler::emit_flatmap_get_element() {
    Label fail = a.newLabel(), loop = a.newLabel();

    a.mov(RETd, emit_boxed_val(ARG1, offsetof(flatmap_t, size), 4));
    a.mov(ARG4, emit_boxed_val(ARG1, offsetof(flatmap_t, keys)));

    emit_ptr_val(ARG4, ARG4);

    a.bind(loop);
    {
        a.dec(RETd);
        a.short_().jl(fail);

        a.cmp(ARG2,
              x86::qword_ptr(ARG4, RET, 3, sizeof(Eterm) - TAG_PRIMARY_BOXED));
        a.short_().jne(loop);
    }

    int value_offset = sizeof(flatmap_t) - TAG_PRIMARY_BOXED;
    a.mov(RET, x86::qword_ptr(ARG1, RET, 3, value_offset));

    a.bind(fail);
    a.ret();
}

void BeamGlobalAssembler::emit_new_map_shared() {
    emit_enter_frame();
    emit_enter_runtime<Update::eReductions | Update::eStack | Update::eHeap>();

    a.mov(ARG1, c_p);
    load_x_reg_array(ARG2);
    runtime_call<5>(erts_gc_new_map);

    emit_leave_runtime<Update::eReductions | Update::eStack | Update::eHeap>();
    emit_leave_frame();

    a.ret();
}

void BeamModuleAssembler::emit_new_map(const ArgRegister &Dst,
                                       const ArgWord &Live,
                                       const ArgWord &Size,
                                       const Span<ArgVal> &args) {
    Label data = embed_vararg_rodata(args, CP_SIZE);

    ASSERT(Size.get() == args.size());

    mov_imm(ARG3, Live.get());
    mov_imm(ARG4, args.size());
    a.lea(ARG5, x86::qword_ptr(data));
    fragment_call(ga->get_new_map_shared());

    mov_arg(Dst, RET);
}

void BeamGlobalAssembler::emit_i_new_small_map_lit_shared() {
    emit_enter_frame();
    emit_enter_runtime<Update::eReductions | Update::eStack | Update::eHeap>();

    a.mov(ARG1, c_p);
    load_x_reg_array(ARG2);
    runtime_call<5>(erts_gc_new_small_map_lit);

    emit_leave_runtime<Update::eReductions | Update::eStack | Update::eHeap>();
    emit_leave_frame();

    a.ret();
}

void BeamModuleAssembler::emit_i_new_small_map_lit(const ArgRegister &Dst,
                                                   const ArgWord &Live,
                                                   const ArgLiteral &Keys,
                                                   const ArgWord &Size,
                                                   const Span<ArgVal> &args) {
    Label data = embed_vararg_rodata(args, CP_SIZE);

    ASSERT(Size.get() == args.size());

    ASSERT(Keys.isLiteral());
    mov_arg(ARG3, Keys);
    mov_imm(ARG4, Live.get());
    a.lea(ARG5, x86::qword_ptr(data));

    fragment_call(ga->get_i_new_small_map_lit_shared());

    mov_arg(Dst, RET);
}

/* ARG1 = map, ARG2 = key
 *
 * Result is returned in RET. ZF is set on success. */
void BeamGlobalAssembler::emit_i_get_map_element_shared() {
    Label generic = a.newLabel(), hashmap = a.newLabel();

    a.mov(RETd, ARG2d);

    a.and_(RETb, imm(_TAG_PRIMARY_MASK));
    a.cmp(RETb, imm(TAG_PRIMARY_IMMED1));
    a.short_().jne(generic);

    emit_ptr_val(ARG1, ARG1);

    a.mov(RETd, emit_boxed_val(ARG1, 0, sizeof(Uint32)));
    a.mov(ARG4d, RETd);

    a.and_(ARG4d, imm(_HEADER_MAP_SUBTAG_MASK));
    a.cmp(ARG4d, imm(HAMT_SUBTAG_HEAD_FLATMAP));
    a.short_().jne(hashmap);

    emit_flatmap_get_element();

    a.bind(generic);
    {
        emit_enter_runtime();
        runtime_call<2>(get_map_element);
        emit_leave_runtime();

        emit_test_the_non_value(RET);

        /* Invert ZF, we want it to be set when RET is a value. */
        a.setnz(ARG1.r8());
        a.dec(ARG1.r8());

        a.ret();
    }

    a.bind(hashmap);
    {
        /* Calculate the internal hash of ARG2 before diving into the HAMT. */
        a.mov(ARG5, ARG2);
        a.shr(ARG5, imm(32));
        a.mov(ARG4d, ARG2d);

        a.mov(ARG3d, imm(INTERNAL_HASH_SALT));
        a.mov(ARG6d, imm(HCONST));
        a.call(labels[internal_hash_helper]);

        emit_hashmap_get_element();
    }
}

void BeamModuleAssembler::emit_i_get_map_element(const ArgLabel &Fail,
                                                 const ArgRegister &Src,
                                                 const ArgRegister &Key,
                                                 const ArgRegister &Dst) {
    mov_arg(ARG1, Src);
    mov_arg(ARG2, Key);

    if (masked_types(Key, BEAM_TYPE_MASK_IMMEDIATE) != BEAM_TYPE_NONE &&
        hasCpuFeature(CpuFeatures::X86::kBMI2)) {
        safe_fragment_call(ga->get_i_get_map_element_shared());
        a.jne(resolve_beam_label(Fail));
    } else {
        emit_enter_runtime();
        runtime_call<2>(get_map_element);
        emit_leave_runtime();

        emit_test_the_non_value(RET);
        a.je(resolve_beam_label(Fail));
    }

    /* Don't store the result if the destination is the scratch X register.
     * (This instruction was originally a has_map_fields instruction.) */
    if (!(Dst.isXRegister() && Dst.as<ArgXRegister>().get() == SCRATCH_X_REG)) {
        mov_arg(Dst, RET);
    }
}

void BeamModuleAssembler::emit_i_get_map_elements(const ArgLabel &Fail,
                                                  const ArgSource &Src,
                                                  const ArgWord &Size,
                                                  const Span<ArgVal> &args) {
    Label generic = a.newLabel(), next = a.newLabel();
    Label data = embed_vararg_rodata(args, 0);

    /* We're not likely to gain much from inlining huge extractions, and the
     * resulting code is quite large, so we'll cut it off after a handful
     * elements.
     *
     * Note that the arguments come in flattened triplets of
     * `{Key, Dst, KeyHash}` */
    bool can_inline = args.size() < (8 * 3);

    ASSERT(Size.get() == args.size());
    ASSERT((Size.get() % 3) == 0);

    for (size_t i = 0; i < args.size(); i += 3) {
        can_inline &= args[i].isImmed();
    }

    mov_arg(ARG1, Src);

    if (can_inline) {
        comment("simplified multi-element lookup");

        emit_ptr_val(ARG1, ARG1);

        a.mov(RETd, emit_boxed_val(ARG1, 0, sizeof(Uint32)));
        a.and_(RETb, imm(_HEADER_MAP_SUBTAG_MASK));
        a.cmp(RETb, imm(HAMT_SUBTAG_HEAD_FLATMAP));
        a.jne(generic);

        ERTS_CT_ASSERT(MAP_SMALL_MAP_LIMIT <= ERTS_UINT32_MAX);
        a.mov(RETd,
              emit_boxed_val(ARG1, offsetof(flatmap_t, size), sizeof(Uint32)));
        a.mov(ARG2, emit_boxed_val(ARG1, offsetof(flatmap_t, keys)));

        emit_ptr_val(ARG2, ARG2);

        for (ssize_t i = args.size() - 3; i >= 0; i -= 3) {
            Label loop = a.newLabel();

            a.bind(loop);
            {
                x86::Mem candidate =
                        x86::qword_ptr(ARG2,
                                       RET,
                                       3,
                                       sizeof(Eterm) - TAG_PRIMARY_BOXED);

                a.dec(RETd);
                a.jl(resolve_beam_label(Fail));

                const auto &Comparand = args[i];
                cmp_arg(candidate, Comparand, ARG3);
                a.short_().jne(loop);
            }

            /* Don't store the result if the destination is the scratch X
             * register. (This instruction was originally a has_map_fields
             * instruction.) */
            const auto &Dst = args[i + 1];
            if (!(Dst.isXRegister() &&
                  Dst.as<ArgXRegister>().get() == SCRATCH_X_REG)) {
                const int value_offset = sizeof(flatmap_t) - TAG_PRIMARY_BOXED;
                mov_arg(Dst, x86::qword_ptr(ARG1, RET, 3, value_offset), ARG3);
            }
        }

        a.short_().jmp(next);
    }

    a.bind(generic);
    {
        mov_imm(ARG4, args.size() / 3);
        a.lea(ARG5, x86::qword_ptr(data));
        a.mov(ARG3, E);

        emit_enter_runtime();

        load_x_reg_array(ARG2);
        runtime_call<5>(beam_jit_get_map_elements);

        emit_leave_runtime();

        a.test(RET, RET);
        a.je(resolve_beam_label(Fail));
    }

    a.bind(next);
}

/* ARG1 = map, ARG2 = key, ARG3 = key hash
 *
 * Result is returned in RET. ZF is set on success. */
void BeamGlobalAssembler::emit_i_get_map_element_hash_shared() {
    Label hashmap = a.newLabel();

    emit_ptr_val(ARG1, ARG1);

    a.mov(RETd, emit_boxed_val(ARG1, 0, sizeof(Uint32)));
    a.mov(ARG4d, RETd);

    a.and_(ARG4d, imm(_HEADER_MAP_SUBTAG_MASK));
    a.cmp(ARG4d, imm(HAMT_SUBTAG_HEAD_FLATMAP));
    a.short_().jne(hashmap);

    emit_flatmap_get_element();

    a.bind(hashmap);
    emit_hashmap_get_element();
}

void BeamModuleAssembler::emit_i_get_map_element_hash(const ArgLabel &Fail,
                                                      const ArgRegister &Src,
                                                      const ArgConstant &Key,
                                                      const ArgWord &Hx,
                                                      const ArgRegister &Dst) {
    mov_arg(ARG1, Src);
    mov_arg(ARG2, Key);
    mov_arg(ARG3, Hx);

    if (Key.isImmed() && hasCpuFeature(CpuFeatures::X86::kBMI2)) {
        safe_fragment_call(ga->get_i_get_map_element_hash_shared());
        a.jne(resolve_beam_label(Fail));
    } else {
        emit_enter_runtime();
        runtime_call<3>(get_map_element_hash);
        emit_leave_runtime();

        emit_test_the_non_value(RET);
        a.je(resolve_beam_label(Fail));
    }

    /* Don't store the result if the destination is the scratch X register.
     * (This instruction was originally a has_map_fields instruction.) */
    if (!(Dst.isXRegister() && Dst.as<ArgXRegister>().get() == SCRATCH_X_REG)) {
        mov_arg(Dst, RET);
    }
}

/* ARG3 = live registers, ARG4 = update vector size, ARG5 = update vector. */
void BeamGlobalAssembler::emit_update_map_assoc_shared() {
    emit_enter_frame();
    emit_enter_runtime<Update::eReductions | Update::eStack | Update::eHeap>();

    a.mov(ARG1, c_p);
    load_x_reg_array(ARG2);
    runtime_call<5>(erts_gc_update_map_assoc);

    emit_leave_runtime<Update::eReductions | Update::eStack | Update::eHeap>();
    emit_leave_frame();

    a.ret();
}

void BeamModuleAssembler::emit_update_map_assoc(const ArgSource &Src,
                                                const ArgRegister &Dst,
                                                const ArgWord &Live,
                                                const ArgWord &Size,
                                                const Span<ArgVal> &args) {
    Label data = embed_vararg_rodata(args, CP_SIZE);

    ASSERT(Size.get() == args.size());

    mov_arg(getXRef(Live.get()), Src);

    mov_imm(ARG3, Live.get());
    mov_imm(ARG4, args.size());
    a.lea(ARG5, x86::qword_ptr(data));
    fragment_call(ga->get_update_map_assoc_shared());

    mov_arg(Dst, RET);
}

/* ARG3 = live registers, ARG4 = update vector size, ARG5 = update vector.
 *
 * Result is returned in RET, error is indicated by ZF. */
void BeamGlobalAssembler::emit_update_map_exact_guard_shared() {
    emit_enter_frame();
    emit_enter_runtime<Update::eReductions | Update::eStack | Update::eHeap>();

    a.mov(ARG1, c_p);
    load_x_reg_array(ARG2);
    runtime_call<5>(erts_gc_update_map_exact);

    emit_leave_runtime<Update::eReductions | Update::eStack | Update::eHeap>();
    emit_leave_frame();

    emit_test_the_non_value(RET);
    a.ret();
}

/* ARG3 = live registers, ARG4 = update vector size, ARG5 = update vector.
 *
 * Does not return on error. */
void BeamGlobalAssembler::emit_update_map_exact_body_shared() {
    Label error = a.newLabel();

    emit_enter_frame();
    emit_enter_runtime<Update::eReductions | Update::eStack | Update::eHeap>();

    a.mov(ARG1, c_p);
    load_x_reg_array(ARG2);
    runtime_call<5>(erts_gc_update_map_exact);

    emit_leave_runtime<Update::eReductions | Update::eStack | Update::eHeap>();
    emit_leave_frame();

    emit_test_the_non_value(RET);
    a.short_().je(error);

    a.ret();

    a.bind(error);
    {
        mov_imm(ARG4, 0);
        a.jmp(labels[raise_exception]);
    }
}

void BeamModuleAssembler::emit_update_map_exact(const ArgSource &Src,
                                                const ArgLabel &Fail,
                                                const ArgRegister &Dst,
                                                const ArgWord &Live,
                                                const ArgWord &Size,
                                                const Span<ArgVal> &args) {
    Label data = embed_vararg_rodata(args, CP_SIZE);

    ASSERT(Size.get() == args.size());

    /* We _KNOW_ Src is a map */
    mov_arg(getXRef(Live.get()), Src);

    mov_imm(ARG3, Live.get());
    mov_imm(ARG4, args.size());
    a.lea(ARG5, x86::qword_ptr(data));

    if (Fail.get() != 0) {
        fragment_call(ga->get_update_map_exact_guard_shared());
        a.je(resolve_beam_label(Fail));
    } else {
        fragment_call(ga->get_update_map_exact_body_shared());
    }

    mov_arg(Dst, RET);
}
